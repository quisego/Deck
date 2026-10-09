#ifndef BSP_BUTTON_H
#define BSP_BUTTON_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>
#include "result.h"

typedef struct Button Button;
typedef struct Button_Config Button_Config;
typedef struct Button_Ops Button_Ops;
typedef enum Button_ActiveState Button_ActiveState;
typedef enum Button_State Button_State;
typedef enum Button_Error Button_Error;

enum Button_ActiveState
{
	BUTTON_ACTIVE_LOW = GPIO_PIN_RESET,
	BUTTON_ACTIVE_HIGH = GPIO_PIN_SET,
};

enum Button_State
{
	BUTTON_STATE_IDLE = 0,
	BUTTON_STATE_DEBOUNCE_PRESS,
	BUTTON_STATE_PRESSED,
	BUTTON_STATE_DEBOUNCE_RELEASE,
	BUTTON_STATE_WAIT_CLICK,
	BUTTON_STATE_CLICKS_READY,
};

struct Button_Config
{
	GPIO_TypeDef *port;
	uint16_t pin;

	uint32_t debounce_ms;
	uint32_t click_timeout_ms;

	Button_ActiveState active_level;
};

struct Button
{
	Button_Config config;

	Button_State state;

	bool pressed_raw_state;
	bool pressed_state;
	bool previous_pressed_state;

	uint8_t click_count;

	uint32_t debounce_timestamp_ms;
	uint32_t press_timestamp_ms;
	uint32_t last_click_timestamp_ms;

	const Button_Ops *ops;
};

enum Button_Error
{
	BUTTON_ERROR_NONE = 0,
	BUTTON_ERROR_NULLPTR,
	BUTTON_ERROR_UNINITIALIZED_CONFIG,
	BUTTON_ERROR_GPIO_PORT_NULLPTR,
};

DEFINE_RESULT(Button, Button_Error);
DEFINE_RESULT(Button_State, Button_Error);

struct Button_Ops
{
	bool (*is_pressed)(const Button *self);
	bool (*is_pressed_with_delay)(const Button *self, uint32_t delay_ms);
	bool (*is_clicked_times)(const Button *self, uint8_t times);
	Result(Button_State, Button_Error) (*update)(Button *self);
};

Result(Button, Button_Error) Button_Init(const Button_Config *config);

bool Button_IsPressed(const Button *self);

bool Button_IsPressedWithDelay(const Button *self, uint32_t delay_ms);

bool Button_IsClickedTimes(const Button *self, uint8_t times);

Result(Button_State, Button_Error) Button_Update(Button *self);

#ifdef __cplusplus
}
#endif

#endif
