#ifndef BSP_BUTTON_EVENTS_H
#define BSP_BUTTON_EVENTS_H

#include "button.h"
#include "msg.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*Button_EventCallback)(uint16_t message_id, const ProtoButtonEvent *event, void *ctx);

void Button_SetEventCallback(Button_EventCallback callback, void *ctx);

void Button_EXTI_Callback(uint16_t gpio_pin);

uint8_t Button_IdFromConfig(const Button_Config *config);

#ifdef __cplusplus
}
#endif

#endif
