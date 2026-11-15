#ifndef APP_BRIDGE_H
#define APP_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include "link.h"
#include "msg.h"

#ifdef __cplusplus
extern "C" {
#endif

void Bridge_Init(Link *link);

void Bridge_OnButton(uint16_t msg_id, const char *name, const ProtoButtonEvent *ev);
void Bridge_OnRtc(const ProtoRtcTime *t);
void Bridge_OnEnv(const ProtoEnvData *e);
void Bridge_OnStmStatus(const ProtoSysStatus *s);

bool Bridge_SendRtcSet(uint16_t year, uint8_t month, uint8_t date, uint8_t hours, uint8_t minutes, uint8_t seconds);

#ifdef __cplusplus
}
#endif

#endif
