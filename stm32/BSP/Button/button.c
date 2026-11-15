/*
*	Button driver. Each button has its own task and its own state machine (FSM).
*	It does debounce, counts clicks, and finds a long press.
*	An EXTI interrupt only wakes the task. The logic runs in the task.
*/

#include "button.h"
#include "button_events.h"
#include "fsm.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "stm32f4xx_hal.h"
#include "supervisor.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef BUTTON_MAX_INSTANCES
#define BUTTON_MAX_INSTANCES 8U
#endif

#define BUTTON_QUEUE_LENGTH          8U
#define BUTTON_SUPERVISOR_TIMEOUT_MS 1000U

/*
*	The task wakes up at least every 500 ms, so it can check in with the supervisor in time.
*/
#define BUTTON_MAX_WAIT_MS           (BUTTON_SUPERVISOR_TIMEOUT_MS / 2U)
#define BUTTON_TASK_STACK_WORDS      (configMINIMAL_STACK_SIZE * 3U)
#define BUTTON_STATE_COUNT           6U

/*
*	Everything one button needs while it works (we call this the "runtime").
*/
typedef struct
{
	Button button;
	FSM fsm;
	uint32_t now;
	uint8_t id;
	QueueHandle_t queue;
	volatile bool in_use;
	bool long_reported;
	bool clicks_reported;
	volatile SupervisorTaskID supervisor_id;
} Button_Runtime;

/*
*	The interrupt puts this small signal in the queue to wake the task.
*/
typedef uint8_t Button_Signal;
#define BUTTON_SIGNAL_EDGE 1U

/*
*	A fixed array of buttons. We do not use malloc.
*/
static Button_Runtime s_pool[BUTTON_MAX_INSTANCES];
static Button_EventCallback s_event_cb;
static void *s_event_ctx;

/*
*	Time helpers. "now - since" works correctly even when the 32-bit timer wraps around.
*/
static inline bool elapsed_at_least(uint32_t now, uint32_t since, uint32_t interval)
{
	return (now - since) >= interval;
}

static inline bool elapsed_more_than(uint32_t now, uint32_t since, uint32_t interval)
{
	return (now - since) > interval;
}

/*
*	If the value is too big for 16 bits, use the maximum instead of overflow.
*/
static inline uint16_t saturate_u16(uint32_t v)
{
	return (v > UINT16_MAX) ? UINT16_MAX : (uint16_t)v;
}

/*
*	Make an id: high 4 bits = port number, low 4 bits = pin number. GPIOB pin 0 -> 0x10.
*/
uint8_t Button_IdFromConfig(const Button_Config *config)
{
	if (config == NULL || config->port == NULL || config->pin == 0U)
	{
		return 0xFFU;
	}

	const uint32_t port_index = ((uint32_t)(uintptr_t)config->port - (uint32_t)GPIOA_BASE) / ((uint32_t)GPIOB_BASE - (uint32_t)GPIOA_BASE);
	const uint32_t pin_index = (uint32_t)__builtin_ctz((unsigned int)config->pin);

	return (uint8_t)((port_index << 4) | pin_index);
}

void Button_SetEventCallback(Button_EventCallback callback, void *ctx)
{
	s_event_cb = callback;
	s_event_ctx = ctx;
}

/*
*	Build an event and send it to the callback (if there is one).
*/
static void emit(const Button_Runtime *rt, ProtoMessageId msg_id, uint8_t clicks, uint32_t duration_ms)
{
	if (s_event_cb == NULL)
	{
		return;
	}

	const ProtoButtonEvent event = {
		.button_id = rt->id,
		.click_count = clicks,
		.duration_ms = saturate_u16(duration_ms),
		.timestamp_ms = rt->now,
	};

	s_event_cb((uint16_t)msg_id, &event, s_event_ctx);
}

/*
*	Find the internal object that matches this button (same port and pin).
*/
static Button_Runtime *find_runtime(const Button *self)
{
	for (size_t i = 0; i < BUTTON_MAX_INSTANCES; i++)
	{
		Button_Runtime *rt = &s_pool[i];

		if (rt->in_use && rt->button.config.port == self->config.port && rt->button.config.pin == self->config.pin)
		{
			return rt;
		}
	}

	return NULL;
}

/*
*	The user gets a COPY of the button. This returns the real, up-to-date data if it exists.
*/
static const Button *live_button(const Button *self)
{
	const Button_Runtime *rt = find_runtime(self);

	return (rt != NULL) ? &rt->button : self;
}

/*
*	Change the FSM state (and remember it in the button). Nothing happens if the state is the same.
*/
static void go(Button_Runtime *rt, Button_State next)
{
	if ((size_t)next == rt->fsm.current_state)
	{
		return;
	}

	(void)FSM_Transition(&rt->fsm, (size_t)next);
	rt->button.state = next;
}

/*
*	The button was released early. Where do we go next? No clicks: IDLE.
*	Click timeout passed: CLICKS_READY. Otherwise: wait for another click.
*/
static Button_State released_state(const Button_Runtime *rt)
{
	const Button *b = &rt->button;

	if (b->click_count == 0U)
	{
		return BUTTON_STATE_IDLE;
	}

	if (elapsed_more_than(rt->now, b->last_click_timestamp_ms, b->config.click_timeout_ms))
	{
		return BUTTON_STATE_CLICKS_READY;
	}

	return BUTTON_STATE_WAIT_CLICK;
}

/*
*	enter: DEBOUNCE_PRESS / DEBOUNCE_RELEASE
*	Remember when the debounce time started.
*/
static void on_enter_debounce(FSM *fsm, void *ctx)
{
	(void)fsm;
	Button_Runtime *rt = (Button_Runtime *)ctx;

	rt->button.debounce_timestamp_ms = rt->now;
}

/*
*	enter: CLICKS_READY
*	Send the "clicks" event, but only one time.
*/
static void on_enter_clicks_ready(FSM *fsm, void *ctx)
{
	(void)fsm;
	Button_Runtime *rt = (Button_Runtime *)ctx;

	if (!rt->clicks_reported)
	{
		rt->clicks_reported = true;
		emit(rt, MSG_ID_BUTTON_CLICKS, rt->button.click_count, 0U);
	}
}

/*
*	IDLE: wait for the first press.
*/
static void on_update_idle(FSM *fsm, void *ctx)
{
	(void)fsm;
	Button_Runtime *rt = (Button_Runtime *)ctx;

	if (rt->button.pressed_raw_state)
	{
		go(rt, BUTTON_STATE_DEBOUNCE_PRESS);
	}
}

/*
*	DEBOUNCE_PRESS: the pin must stay pressed for the whole debounce time.
*	If it is released earlier, it was only noise.
*/
static void on_update_debounce_press(FSM *fsm, void *ctx)
{
	(void)fsm;
	Button_Runtime *rt = (Button_Runtime *)ctx;
	Button *b = &rt->button;

	if (!b->pressed_raw_state)
	{
		go(rt, released_state(rt));
		return;
	}

	if (!elapsed_at_least(rt->now, b->debounce_timestamp_ms, b->config.debounce_ms))
	{
		return;
	}

	// Too much time since the last click: start counting clicks from zero.
	if (b->click_count > 0U && elapsed_more_than(b->debounce_timestamp_ms, b->last_click_timestamp_ms, b->config.click_timeout_ms))
	{
		b->click_count = 0U;
	}

	// This is a real press now. Send the "pressed" event.
	b->pressed_state = true;
	b->press_timestamp_ms = rt->now;
	rt->long_reported = false;

	emit(rt, MSG_ID_BUTTON_PRESSED, 0U, 0U);
	go(rt, BUTTON_STATE_PRESSED);
}

/*
*	PRESSED: if the button is held longer than the timeout, send "long press" once.
*/
static void on_update_pressed(FSM *fsm, void *ctx)
{
	(void)fsm;
	Button_Runtime *rt = (Button_Runtime *)ctx;
	Button *b = &rt->button;

	if (!rt->long_reported && elapsed_more_than(rt->now, b->press_timestamp_ms, b->config.click_timeout_ms))
	{
		rt->long_reported = true;
		emit(rt, MSG_ID_BUTTON_LONG_PRESS, 0U, rt->now - b->press_timestamp_ms);
	}

	if (!b->pressed_raw_state)
	{
		go(rt, BUTTON_STATE_DEBOUNCE_RELEASE);
	}
}

/*
*	DEBOUNCE_RELEASE: if the pin is pressed again, it was noise (go back to PRESSED).
*	Otherwise the release is real.
*/
static void on_update_debounce_release(FSM *fsm, void *ctx)
{
	(void)fsm;
	Button_Runtime *rt = (Button_Runtime *)ctx;
	Button *b = &rt->button;

	if (b->pressed_raw_state)
	{
		go(rt, BUTTON_STATE_PRESSED);
		return;
	}

	if (!elapsed_at_least(rt->now, b->debounce_timestamp_ms, b->config.debounce_ms))
	{
		return;
	}

	b->pressed_state = false;

	emit(rt, MSG_ID_BUTTON_RELEASED, 0U, b->debounce_timestamp_ms - b->press_timestamp_ms);

	// After a long press we do not count it as a click.
	if (rt->long_reported)
	{
		b->click_count = 0U;
		go(rt, BUTTON_STATE_IDLE);
		return;
	}

	// Count one more click (but not more than 255).
	if (b->click_count < UINT8_MAX)
	{
		b->click_count++;
	}

	rt->clicks_reported = false;
	b->last_click_timestamp_ms = rt->now;
	go(rt, BUTTON_STATE_WAIT_CLICK);
}

/*
*	WAIT_CLICK: wait for another click, or for the timeout (then the clicks are ready).
*/
static void on_update_wait_click(FSM *fsm, void *ctx)
{
	(void)fsm;
	Button_Runtime *rt = (Button_Runtime *)ctx;
	Button *b = &rt->button;

	if (b->pressed_raw_state)
	{
		go(rt, BUTTON_STATE_DEBOUNCE_PRESS);
	}
	else if (elapsed_more_than(rt->now, b->last_click_timestamp_ms, b->config.click_timeout_ms))
	{
		go(rt, BUTTON_STATE_CLICKS_READY);
	}
}

/*
*	CLICKS_READY: the click count was sent. A new press starts a new round.
*/
static void on_update_clicks_ready(FSM *fsm, void *ctx)
{
	(void)fsm;
	Button_Runtime *rt = (Button_Runtime *)ctx;

	if (rt->button.pressed_raw_state)
	{
		go(rt, BUTTON_STATE_DEBOUNCE_PRESS);
	}
}

/*
*	State table: which functions each state uses.
*/
static const FSM_StateOps button_states[BUTTON_STATE_COUNT] = {
	[BUTTON_STATE_IDLE] = { .update = on_update_idle },
	[BUTTON_STATE_DEBOUNCE_PRESS] = { .enter = on_enter_debounce, .update = on_update_debounce_press },
	[BUTTON_STATE_PRESSED] = { .update = on_update_pressed },
	[BUTTON_STATE_DEBOUNCE_RELEASE] = { .enter = on_enter_debounce, .update = on_update_debounce_release },
	[BUTTON_STATE_WAIT_CLICK] = { .update = on_update_wait_click },
	[BUTTON_STATE_CLICKS_READY] = { .enter = on_enter_clicks_ready, .update = on_update_clicks_ready },
};

/*
*	One step: read the pin, then run the FSM.
*/
static void step(Button_Runtime *rt)
{
	Button *b = &rt->button;

	rt->now = HAL_GetTick();

	const GPIO_PinState gpio = HAL_GPIO_ReadPin(b->config.port, b->config.pin);

	b->pressed_raw_state = (gpio == (GPIO_PinState)b->config.active_level);
	b->previous_pressed_state = b->pressed_state;

	(void)FSM_Update(&rt->fsm);
}

/*
*	How many ms are left until "interval" is over (0 if it is already over).
*/
static uint32_t ms_until(uint32_t now, uint32_t since, uint32_t interval)
{
	const uint32_t elapsed = now - since;

	return (elapsed >= interval) ? 0U : (interval - elapsed);
}

/*
*	How long can the task sleep? Until the next time event (debounce end, long press, click timeout),
*	but never longer than BUTTON_MAX_WAIT_MS.
*/
static uint32_t next_wake_ms(const Button_Runtime *rt)
{
	const Button *b = &rt->button;
	const uint32_t now = HAL_GetTick();
	uint32_t due = BUTTON_MAX_WAIT_MS;
	uint32_t ms = BUTTON_MAX_WAIT_MS;

	switch (b->state)
	{
	case BUTTON_STATE_DEBOUNCE_PRESS:
	case BUTTON_STATE_DEBOUNCE_RELEASE:
		ms = ms_until(now, b->debounce_timestamp_ms, b->config.debounce_ms);
		break;
	case BUTTON_STATE_PRESSED:
		if (!rt->long_reported)
		{
			ms = ms_until(now, b->press_timestamp_ms, b->config.click_timeout_ms + 1U);
		}
		break;
	case BUTTON_STATE_WAIT_CLICK:
		ms = ms_until(now, b->last_click_timestamp_ms, b->config.click_timeout_ms + 1U);
		break;
	default:
		break;
	}

	if (ms < due)
	{
		due = ms;
	}

	return due;
}

/*
*	Runs in an interrupt. Find the button with this pin and wake its task by sending a signal.
*/
void Button_EXTI_Callback(uint16_t gpio_pin)
{
	BaseType_t woken = pdFALSE;
	const Button_Signal signal = BUTTON_SIGNAL_EDGE;

	for (size_t i = 0; i < BUTTON_MAX_INSTANCES; i++)
	{
		Button_Runtime *rt = &s_pool[i];

		if (rt->in_use && rt->button.config.pin == gpio_pin)
		{
			(void)xQueueSendFromISR(rt->queue, &signal, &woken);
			break;
		}
	}

	portYIELD_FROM_ISR(woken);
}

/*
*	Loop: update the FSM -> check in -> sleep until an interrupt or a time event.
*/
static void button_task(void *arg)
{
	Button_Runtime *rt = (Button_Runtime *)arg;

	for (;;)
	{
		step(rt);

		if (rt->supervisor_id != SUPERVISOR_INVALID_ID)
		{
			Supervisor_CheckIn(rt->supervisor_id);
		}

		uint32_t wait_ms = next_wake_ms(rt);
		TickType_t wait_ticks = pdMS_TO_TICKS(wait_ms);

		if (wait_ms > 0U && wait_ticks == 0U)
		{
			wait_ticks = 1U;
		}

		Button_Signal signal;

		// Sleep here. An edge interrupt wakes us earlier.
		(void)xQueueReceive(rt->queue, &signal, wait_ticks);

		// Remove extra signals. One run of step() reads the real pin state anyway.
		while (xQueueReceive(rt->queue, &signal, 0) == pdPASS)
		{
		}
	}
}

static const Button_Ops button_ops = {
	.is_pressed = Button_IsPressed,
	.is_pressed_with_delay = Button_IsPressedWithDelay,
	.is_clicked_times = Button_IsClickedTimes,
	.update = Button_Update,
};

/*
*	Create a button. Steps: check config, reuse if it exists, find a free slot, set start state,
*	create FSM, queue and task.
*/
Result(Button, Button_Error) Button_Init(const Button_Config *config)
{
	if (config == NULL)
	{
		return RESULT_ERROR(Button, Button_Error, BUTTON_ERROR_UNINITIALIZED_CONFIG);
	}

	if (config->port == NULL)
	{
		return RESULT_ERROR(Button, Button_Error, BUTTON_ERROR_GPIO_PORT_NULLPTR);
	}

	// The pin must have exactly ONE bit set (a single pin).
	if (config->pin == 0U || (config->pin & (uint16_t)(config->pin - 1U)) != 0U)
	{
		return RESULT_ERROR(Button, Button_Error, BUTTON_ERROR_UNINITIALIZED_CONFIG);
	}

	// If this button already exists, return it (no double init).
	const Button probe = { .config = *config };
	const Button_Runtime *existing = find_runtime(&probe);
	if (existing != NULL)
	{
		return RESULT_OK(Button, Button_Error, existing->button);
	}

	for (size_t i = 0; i < BUTTON_MAX_INSTANCES; i++)
	{
		if (s_pool[i].in_use && s_pool[i].button.config.pin == config->pin)
		{
			return RESULT_ERROR(Button, Button_Error, BUTTON_ERROR_UNINITIALIZED_CONFIG);
		}
	}

	// Look for a free place in the pool.
	Button_Runtime *rt = NULL;
	for (size_t i = 0; i < BUTTON_MAX_INSTANCES; i++)
	{
		if (!s_pool[i].in_use)
		{
			rt = &s_pool[i];
			break;
		}
	}

	if (rt == NULL)
	{
		return RESULT_ERROR(Button, Button_Error, BUTTON_ERROR_TASK_CREATE_FAILED);
	}

	memset(rt, 0, sizeof(*rt));

	rt->button.config = *config;
	rt->button.ops = &button_ops;
	rt->id = Button_IdFromConfig(config);
	rt->supervisor_id = SUPERVISOR_INVALID_ID;
	rt->now = HAL_GetTick();

	// Read the pin now. If the button is already held at startup, we start in PRESSED.
	const GPIO_PinState gpio = HAL_GPIO_ReadPin(config->port, config->pin);

	rt->button.pressed_raw_state = (gpio == (GPIO_PinState)config->active_level);
	rt->button.pressed_state = rt->button.pressed_raw_state;
	rt->button.previous_pressed_state = rt->button.pressed_state;
	rt->button.debounce_timestamp_ms = rt->now;

	if (rt->button.pressed_state)
	{
		rt->button.press_timestamp_ms = rt->now;
		rt->button.state = BUTTON_STATE_PRESSED;
	}
	else
	{
		rt->button.state = BUTTON_STATE_IDLE;
	}

	const Result(FSM, FSM_Error) fsm_result = FSM_Init(button_states, BUTTON_STATE_COUNT, (size_t)rt->button.state, rt);
	if (fsm_result.is_error)
	{
		return RESULT_ERROR(Button, Button_Error, BUTTON_ERROR_UNINITIALIZED_CONFIG);
	}

	rt->fsm = fsm_result.value;

	rt->queue = xQueueCreate(BUTTON_QUEUE_LENGTH, sizeof(Button_Signal));
	if (rt->queue == NULL)
	{
		return RESULT_ERROR(Button, Button_Error, BUTTON_ERROR_TASK_CREATE_FAILED);
	}

	rt->in_use = true;

	TaskHandle_t handle = NULL;

	if (xTaskCreate(button_task, "button", BUTTON_TASK_STACK_WORDS, rt, tskIDLE_PRIORITY + 1U, &handle) != pdPASS)
	{
		rt->in_use = false;
		vQueueDelete(rt->queue);
		rt->queue = NULL;
		return RESULT_ERROR(Button, Button_Error, BUTTON_ERROR_TASK_CREATE_FAILED);
	}

	rt->supervisor_id = Supervisor_Register(handle, BUTTON_SUPERVISOR_TIMEOUT_MS);

	return RESULT_OK(Button, Button_Error, rt->button);
}

/*
*	Public functions: ask the button for its state.
*/
bool Button_IsPressed(const Button *self)
{
	if (self == NULL)
	{
		return false;
	}

	return live_button(self)->pressed_state;
}

/*
*	True if the button is held for at least delay_ms.
*/
bool Button_IsPressedWithDelay(const Button *self, uint32_t delay_ms)
{
	if (self == NULL)
	{
		return false;
	}

	const Button *b = live_button(self);

	if (!b->pressed_state)
	{
		return false;
	}

	return elapsed_at_least(HAL_GetTick(), b->press_timestamp_ms, delay_ms);
}

/*
*	True if the click series is finished and has exactly this number of clicks.
*/
bool Button_IsClickedTimes(const Button *self, uint8_t times)
{
	if (self == NULL || times == 0U)
	{
		return false;
	}

	const Button *b = live_button(self);

	return b->state == BUTTON_STATE_CLICKS_READY && b->click_count == times;
}

/*
*	Copy the newest data into "self". The critical section keeps the copy consistent.
*/
Result(Button_State, Button_Error) Button_Update(Button *self)
{
	if (self == NULL)
	{
		return RESULT_ERROR(Button_State, Button_Error, BUTTON_ERROR_NULLPTR);
	}

	if (self->config.port == NULL)
	{
		return RESULT_ERROR(Button_State, Button_Error, BUTTON_ERROR_GPIO_PORT_NULLPTR);
	}

	const Button_Runtime *rt = find_runtime(self);

	if (rt == NULL)
	{
		return RESULT_ERROR(Button_State, Button_Error, BUTTON_ERROR_UNINITIALIZED_CONFIG);
	}

	taskENTER_CRITICAL();
	*self = rt->button;
	taskEXIT_CRITICAL();

	const Button_State state = self->state;

	return RESULT_OK(Button_State, Button_Error, state);
}
