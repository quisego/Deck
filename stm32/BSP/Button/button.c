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

#define BUTTON_MAX_WAIT_MS           (BUTTON_SUPERVISOR_TIMEOUT_MS / 2U)
#define BUTTON_TASK_STACK_WORDS      (configMINIMAL_STACK_SIZE * 3U)
#define BUTTON_STATE_COUNT           6U

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

typedef uint8_t Button_Signal;
#define BUTTON_SIGNAL_EDGE 1U

static Button_Runtime s_pool[BUTTON_MAX_INSTANCES];
static Button_EventCallback s_event_cb;
static void *s_event_ctx;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static inline bool elapsed_at_least(uint32_t now, uint32_t since, uint32_t interval)
{
	return (now - since) >= interval;
}

static inline bool elapsed_more_than(uint32_t now, uint32_t since, uint32_t interval)
{
	return (now - since) > interval;
}

static inline uint16_t saturate_u16(uint32_t v)
{
	return (v > UINT16_MAX) ? UINT16_MAX : (uint16_t)v;
}

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

static const Button *live_button(const Button *self)
{
	const Button_Runtime *rt = find_runtime(self);

	return (rt != NULL) ? &rt->button : self;
}

/* ------------------------------------------------------------------ */
/* FSM                                                                 */
/* ------------------------------------------------------------------ */

static void go(Button_Runtime *rt, Button_State next)
{
	if ((size_t)next == rt->fsm.current_state)
	{
		return;
	}

	(void)FSM_Transition(&rt->fsm, (size_t)next);
	rt->button.state = next;
}

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

/* enter: DEBOUNCE_PRESS / DEBOUNCE_RELEASE */
static void on_enter_debounce(FSM *fsm, void *ctx)
{
	(void)fsm;
	Button_Runtime *rt = (Button_Runtime *)ctx;

	rt->button.debounce_timestamp_ms = rt->now;
}

/* enter: CLICKS_READY */
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

static void on_update_idle(FSM *fsm, void *ctx)
{
	(void)fsm;
	Button_Runtime *rt = (Button_Runtime *)ctx;

	if (rt->button.pressed_raw_state)
	{
		go(rt, BUTTON_STATE_DEBOUNCE_PRESS);
	}
}

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

	if (b->click_count > 0U && elapsed_more_than(b->debounce_timestamp_ms, b->last_click_timestamp_ms, b->config.click_timeout_ms))
	{
		b->click_count = 0U;
	}

	b->pressed_state = true;
	b->press_timestamp_ms = rt->now;
	rt->long_reported = false;

	emit(rt, MSG_ID_BUTTON_PRESSED, 0U, 0U);
	go(rt, BUTTON_STATE_PRESSED);
}

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

	if (rt->long_reported)
	{
		b->click_count = 0U;
		go(rt, BUTTON_STATE_IDLE);
		return;
	}

	if (b->click_count < UINT8_MAX)
	{
		b->click_count++;
	}

	rt->clicks_reported = false;
	b->last_click_timestamp_ms = rt->now;
	go(rt, BUTTON_STATE_WAIT_CLICK);
}

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

static void on_update_clicks_ready(FSM *fsm, void *ctx)
{
	(void)fsm;
	Button_Runtime *rt = (Button_Runtime *)ctx;

	if (rt->button.pressed_raw_state)
	{
		go(rt, BUTTON_STATE_DEBOUNCE_PRESS);
	}
}

static const FSM_StateOps button_states[BUTTON_STATE_COUNT] = {
	[BUTTON_STATE_IDLE] = { .update = on_update_idle },
	[BUTTON_STATE_DEBOUNCE_PRESS] = { .enter = on_enter_debounce, .update = on_update_debounce_press },
	[BUTTON_STATE_PRESSED] = { .update = on_update_pressed },
	[BUTTON_STATE_DEBOUNCE_RELEASE] = { .enter = on_enter_debounce, .update = on_update_debounce_release },
	[BUTTON_STATE_WAIT_CLICK] = { .update = on_update_wait_click },
	[BUTTON_STATE_CLICKS_READY] = { .enter = on_enter_clicks_ready, .update = on_update_clicks_ready },
};

/* ------------------------------------------------------------------ */
/* Task                                                                */
/* ------------------------------------------------------------------ */

static void step(Button_Runtime *rt)
{
	Button *b = &rt->button;

	rt->now = HAL_GetTick();

	const GPIO_PinState gpio = HAL_GPIO_ReadPin(b->config.port, b->config.pin);

	b->pressed_raw_state = (gpio == (GPIO_PinState)b->config.active_level);
	b->previous_pressed_state = b->pressed_state;

	(void)FSM_Update(&rt->fsm);
}

static uint32_t ms_until(uint32_t now, uint32_t since, uint32_t interval)
{
	const uint32_t elapsed = now - since;

	return (elapsed >= interval) ? 0U : (interval - elapsed);
}

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

		(void)xQueueReceive(rt->queue, &signal, wait_ticks);

		while (xQueueReceive(rt->queue, &signal, 0) == pdPASS)
		{
		}
	}
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static const Button_Ops button_ops = {
	.is_pressed = Button_IsPressed,
	.is_pressed_with_delay = Button_IsPressedWithDelay,
	.is_clicked_times = Button_IsClickedTimes,
	.update = Button_Update,
};

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

	if (config->pin == 0U || (config->pin & (uint16_t)(config->pin - 1U)) != 0U)
	{
		return RESULT_ERROR(Button, Button_Error, BUTTON_ERROR_UNINITIALIZED_CONFIG);
	}

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

bool Button_IsPressed(const Button *self)
{
	if (self == NULL)
	{
		return false;
	}

	return live_button(self)->pressed_state;
}

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

bool Button_IsClickedTimes(const Button *self, uint8_t times)
{
	if (self == NULL || times == 0U)
	{
		return false;
	}

	const Button *b = live_button(self);

	return b->state == BUTTON_STATE_CLICKS_READY && b->click_count == times;
}

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
