#include "button.h"
#include "result.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_gpio.h"
#include <stdint.h>

static const Button_Ops button_ops = {
	.is_pressed = Button_IsPressed,
	.is_pressed_with_delay = Button_IsPressedWithDelay,
	.is_clicked_times = Button_IsClickedTimes,
	.update = Button_Update,
};

static inline bool elapsed_at_least(uint32_t now, uint32_t since, uint32_t interval)
{
	return (now - since) >= interval;
}

static inline bool elapsed_more_than(uint32_t now, uint32_t since, uint32_t interval)
{
	return (now - since) > interval;
}

static Button_State released_state(const Button *self, uint32_t now)
{
	if (self->click_count == 0)
	{
		return BUTTON_STATE_IDLE;
	}

	if (elapsed_more_than(now, self->last_click_timestamp_ms, self->config.click_timeout_ms))
	{
		return BUTTON_STATE_CLICKS_READY;
	}

	return BUTTON_STATE_WAIT_CLICK;
}

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

	Button button = {
		.config = *config,
		.ops = &button_ops,
	};

	GPIO_PinState gpio_state = HAL_GPIO_ReadPin(button.config.port, button.config.pin);

	button.pressed_raw_state = (gpio_state == (GPIO_PinState)button.config.active_level);
	button.pressed_state = button.pressed_raw_state;
	button.previous_pressed_state = button.pressed_state;

	uint32_t now = HAL_GetTick();

	button.debounce_timestamp_ms = now;

	if (button.pressed_state)
	{
		button.press_timestamp_ms = now;
		button.state = BUTTON_STATE_PRESSED;
	}
	else
	{
		button.state = BUTTON_STATE_IDLE;
	}

	return RESULT_OK(Button, Button_Error, button);
}

bool Button_IsPressed(const Button *self)
{
	if (self == NULL)
	{
		return false;
	}

	return self->pressed_state;
}

bool Button_IsPressedWithDelay(const Button *self, uint32_t delay_ms)
{
	if (self == NULL || !self->pressed_state)
	{
		return false;
	}

	return elapsed_at_least(HAL_GetTick(), self->press_timestamp_ms, delay_ms);
}

bool Button_IsClickedTimes(const Button *self, uint8_t times)
{
	if (self == NULL || times == 0)
	{
		return false;
	}

	return self->state == BUTTON_STATE_CLICKS_READY && self->click_count == times;
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

	const uint32_t now = HAL_GetTick();
	const GPIO_PinState gpio_state = HAL_GPIO_ReadPin(self->config.port, self->config.pin);
	const bool raw = (gpio_state == (GPIO_PinState)self->config.active_level);

	self->pressed_raw_state = raw;
	self->previous_pressed_state = self->pressed_state;

	switch (self->state)
	{
	case BUTTON_STATE_IDLE:
	{
		if (raw)
		{
			self->debounce_timestamp_ms = now;
			self->state = BUTTON_STATE_DEBOUNCE_PRESS;
		}

		break;
	}
	case BUTTON_STATE_DEBOUNCE_PRESS:
	{
		if (!raw)
		{
			self->state = released_state(self, now);
		}
		else if (elapsed_at_least(now, self->debounce_timestamp_ms, self->config.debounce_ms))
		{
			if (self->click_count > 0 && elapsed_more_than(self->debounce_timestamp_ms, self->last_click_timestamp_ms, self->config.click_timeout_ms))
			{
				self->click_count = 0;
			}

			self->pressed_state = true;
			self->press_timestamp_ms = now;
			self->state = BUTTON_STATE_PRESSED;
		}

		break;
	}
	case BUTTON_STATE_PRESSED:
	{
		if (!raw)
		{
			self->debounce_timestamp_ms = now;
			self->state = BUTTON_STATE_DEBOUNCE_RELEASE;
		}

		break;
	}
	case BUTTON_STATE_DEBOUNCE_RELEASE:
	{
		if (raw)
		{
			self->state = BUTTON_STATE_PRESSED;
		}
		else if (elapsed_at_least(now, self->debounce_timestamp_ms, self->config.debounce_ms))
		{
			const bool is_long_press = elapsed_more_than(self->debounce_timestamp_ms, self->press_timestamp_ms, self->config.click_timeout_ms);

			self->pressed_state = false;

			if (is_long_press)
			{
				self->click_count = 0;
				self->state = BUTTON_STATE_IDLE;
			}
			else
			{
				if (self->click_count < UINT8_MAX)
				{
					self->click_count++;
				}

				
				self->last_click_timestamp_ms = now;
				self->state = BUTTON_STATE_WAIT_CLICK;
			}
		}

		break;
	}
	case BUTTON_STATE_WAIT_CLICK:
	{
		if (raw)
		{
			self->debounce_timestamp_ms = now;
			self->state = BUTTON_STATE_DEBOUNCE_PRESS;
		}
		else if (elapsed_more_than(now, self->last_click_timestamp_ms, self->config.click_timeout_ms))
		{
			self->state = BUTTON_STATE_CLICKS_READY;
		}

		break;
	}
	case BUTTON_STATE_CLICKS_READY:
	{
		if (raw)
		{
			self->debounce_timestamp_ms = now;
			self->state = BUTTON_STATE_DEBOUNCE_PRESS;
		}

		break;
	}
	default:
	{
		self->state = BUTTON_STATE_IDLE;

		break;
	}
	}

	Button_State btn_state = self->state;

	return RESULT_OK(Button_State, Button_Error, btn_state);
}
