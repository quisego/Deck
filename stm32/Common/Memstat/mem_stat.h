#ifndef BSP_MEMSTAT_H
#define BSP_MEMSTAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void MemStat_Log(void);

bool MemStat_Start(uint32_t period_ms);

#ifdef __cplusplus
}
#endif

#endif
