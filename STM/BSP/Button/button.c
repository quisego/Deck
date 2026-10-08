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
	};

	GPIO_PinState gpio_state = HAL_GPIO_ReadPin(button.config.port, button.config.pin);

	button.pressed_raw_state = (gpio_state == button.config.active_level);
	button.pressed_state = button.pressed_raw_state;
	button.previous_pressed_state = button.pressed_state;

	uint32_t now = HAL_GetTick();

	button.debounce_timestamp_ms = now;

	if (button.pressed_state)
	{
		button.press_timestamp_ms = now;
	}

	return RESULT_OK(Button, Button_Error, button);
}

bool Button_IsPressed(const Button *self)
{
	return false;
}

bool Button_IsPressedWithDelay(const Button *self, uint32_t delay_ms)
{
	return false;
}

bool Button_IsClickedTimes(const Button *self, uint8_t times)
{
	return false;
}

void Button_Update(Button *self)
{
	
}
