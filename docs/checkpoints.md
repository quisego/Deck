# Course checklist

This file shows how the project covers the course checkpoints.

**Status legend**

| Mark | Meaning |
|---|---|
| ✅ Done | Implemented in the code |
| 📄 Done (docs) | Delivered as documentation in this repository |
| 🟡 Partial | Started, but something is missing |
| ⬜ TODO | Selected for the project, not finished yet |
| ➖ Skipped | Skipped on purpose |
| ❔ Not selected | No decision made |

## Summary

| Item | Count |
|---|---|
| All checkpoints | 42 |
| Selected for the project | 34 |
| Skipped | 7 (2.1, 4.2, 7.2, 7.3, 7.4, 7.5, 7.7) |
| Not selected | 1 (9.4) |
| Planned coverage | 34 / 42 = **81 %** |
| ✅ / 📄 Done now | 34 |
| 🟡 Partial | 2 (6.4, 7.1) |

> The goal is at least 80 %. That means 34 of 42 points, so **all selected points must be finished**.
> If one selected point is missing, the coverage is 33 / 42 = 78.6 %.

## 1. Architecture

| # | Checkpoint | Status | How it is done |
|---|---|---|---|
| 1.1 | Super loop or RTOS | ✅ Done | FreeRTOS on STM32 (CMSIS-RTOS2 + FreeRTOS) and on ESP32 (ESP-IDF). See [architecture.md](architecture.md#1-why-rtos-and-not-a-super-loop). |
| 1.2 | No blocking `delay()` in the main logic | ✅ Done | No `HAL_Delay()` in the project. Tasks wait on queues, task notifications, semaphores or `vTaskDelay` / `vTaskDelayUntil`. Button task sleeps until the next time event or an EXTI signal. |
| 1.3 | State machine | ✅ Done | Own FSM library (`Common/FSM/fsm.c`). Used by the button driver (6 states: IDLE, DEBOUNCE_PRESS, PRESSED, DEBOUNCE_RELEASE, WAIT_CLICK, CLICKS_READY). The protocol parser is a 4-state machine. Link TX has states IDLE / PENDING / DONE / FAILED. |
| 1.4 | Architecture block diagram | 📄 Done (docs) | Mermaid diagrams in [architecture.md](architecture.md). |
| 1.5 | Logic split into modules | ✅ Done | `common/proto`, `stm32/Common/*`, `stm32/BSP/*`, `stm32/Cmd/App`, `esp32/components/*`. Each module has its own `.c/.h` and API. |

## 2. Peripherals and interfaces

| # | Checkpoint | Status | How it is done |
|---|---|---|---|
| 2.1 | Analog subsystem (ADC / DAC / PWM / I2S / PDM) | ➖ Skipped | Not needed for this project. |
| 2.2 | Digital interface (UART / I2C / SPI) | ✅ Done | UART1 (STM32 <-> ESP32), UART2 (log), I2C1 (OLED, RTC), SPI1 (BME280). See [interfaces.md](interfaces.md). |
| 2.3 | Interrupts | ✅ Done | EXTI0/EXTI1 for buttons, UART IDLE, UART DMA half/full/TX complete, I2C DMA complete and error, UART error. Interrupts only wake tasks. |
| 2.4 | Asynchronous data transfer (buffer / queue) | ✅ Done | UART RX circular DMA ring buffer, TX slot pool with two queues, logger queue, RTC "set time" queue, button signal queue. |
| 2.5 | Timers or hardware events | ✅ Done | Hardware watchdog IWDG, SysTick for FreeRTOS delays and timeouts (`vTaskDelayUntil` in the supervisor), EXTI edge events, UART IDLE-line event, DMA events. |

## 3. RTOS

| # | Checkpoint | Status | How it is done |
|---|---|---|---|
| 3.1 | At least 2 tasks | ✅ Done | STM32: 2 x button, rtc, bme280, display, LinkTx, LinkRx, status, logger, supervisor. ESP32: LinkTx, LinkRx, bridge task, MQTT / Wi-Fi tasks. |
| 3.2 | Queue | ✅ Done | Link TX queues (free / ready), logger queue, button signal queue, RTC set queue (STM32). Link queues on the ESP32. |
| 3.3 | Mutex or other synchronization | ✅ Done | I2C bus mutex, Link mutex, ESP32 bridge mutex, semaphore for the OLED DMA, task notifications, critical sections. |
| 3.4 | No race conditions | ✅ Done | Table "Synchronization and no race conditions" in [architecture.md](architecture.md#35-synchronization-and-no-race-conditions). One owner per resource, atomic counters, callbacks called after unlock, `is_active` flag set last. **One item to check:** the NVIC priority of the UART1 RX DMA stream (see [interfaces.md](interfaces.md#6-interrupts)). |

## 4. Control and algorithms

| # | Checkpoint | Status | How it is done |
|---|---|---|---|
| 4.1 | Feedback | ✅ Done | (1) Supervisor loop: tasks report "alive", the supervisor feeds the IWDG only if all are alive. (2) Time loop: SNTP -> ESP32 -> DS1307 -> read back -> OLED and MQTT. (3) Health loop: device flags and counters go to the ESP32 and to MQTT. (4) Sensor loop: after failed reads the sensor is initialized again. |
| 4.2 | PID or other control algorithm | ➖ Skipped | Not needed for this project. |
| 4.3 | Parameters can be configured | ✅ Done | Compile-time: `LinkConfig`, button debounce/click time, sensor steps and periods (`app.c`). ESP32: `menuconfig` (Wi-Fi, MQTT, time zone, periods). PC: `config.yaml` + `deckctl`. Run-time: MQTT command `cmd/rtc/set`. |
| 4.4 | Boundary conditions (overflow, saturation) | ✅ Done | Button duration saturates at 65535 ms, click count stops at 255, humidity limited to 0..100 %, payload size limits, tick counter wrap-around safe math (`now - since`), ring buffer overflow detection, BCD and date validation (also leap years), log message length limits. |

## 5. Reliability

| # | Checkpoint | Status | How it is done |
|---|---|---|---|
| 5.1 | Watchdog | ✅ Done | Hardware IWDG (about 2 s) + software `Supervisor` with per-task timeouts, stack high-water mark check, stack overflow hook, logged reset reason. |
| 5.2 | Peripheral error handling | ✅ Done | UART error callback + DMA restart, TX timeout and abort, I2C DMA timeout + peripheral re-init, OLED and BME280 re-init, online flags for each device, `Result` type for init errors. |
| 5.3 | Check of incorrect data | ✅ Done | CRC16, version check, length check, exact payload size check on decode, BCD check, range check of RTC values, validation of "set time", `sscanf` result check for the MQTT time command. |
| 5.4 | No hang on wrong input | ✅ Done | The parser resyncs and never blocks. All waits have timeouts. Bad packets only increase counters. Wrong MQTT commands answer `error`. |

## 6. Performance and optimization

| # | Checkpoint | Status | How it is done / what is missing |
|---|---|---|---|
| 6.1 | Execution time of critical code is measured | ⬜ TODO | Measurement plan and result table in [testing.md](testing.md#1-execution-time-of-critical-code-checkpoint-61). Use the DWT cycle counter. |
| 6.2 | DMA where it makes sense | ✅ Done | UART1 RX (circular) and TX, UART2 TX (logger), I2C1 TX (OLED frame). SPI is not on DMA, because the BME280 transfer is only a few bytes. |
| 6.3 | No extra data copies | ✅ Done | The parser returns a pointer into its own buffer. The logger queues a pointer and arguments, not text. TX uses a fixed slot pool. Remaining copies are small and needed: payload -> TX slot -> packet buffer, and ring buffer -> 64-byte chunk for the parser. |
| 6.4 | CPU load below 70 % | 🟡 Partial | The design is event-driven (tasks sleep most of the time), but the load is **not measured yet**. Method and table in [testing.md](testing.md#2-cpu-load-checkpoint-64). |

## 7. PCB

| # | Checkpoint | Status | How it is done / what is missing |
|---|---|---|---|
| 7.1 | Schematic | 🟡 Partial | There is a wiring diagram (`circuit_image.png`, Cirkit Designer) and a wiring table in [hardware.md](hardware.md). A real schematic (KiCad) is still needed. |
| 7.2 | 2-layer PCB | ➖ Skipped | |
| 7.3 | Power filtering | ➖ Skipped | |
| 7.4 | Power and logic separation | ➖ Skipped | |
| 7.5 | High-speed signal routing | ➖ Skipped | |
| 7.6 | Test points | 🟡 Partial | The list of test points is ready in [hardware.md](hardware.md#4-test-points). They must be added to the schematic. |
| 7.7 | Board made and tested | ➖ Skipped | |

## 8. Documentation

| # | Checkpoint | Status | Where |
|---|---|---|---|
| 8.1 | README with the task description | 📄 Done (docs) | [README.md](../README.md) |
| 8.2 | Block diagram | 📄 Done (docs) | [architecture.md](architecture.md#2-system-block-diagram) |
| 8.3 | Architecture description | 📄 Done (docs) | [architecture.md](architecture.md) |
| 8.4 | Run instructions | 📄 Done (docs) | [getting-started.md](getting-started.md) |
| 8.5 | Explanation of the interfaces | 📄 Done (docs) | [interfaces.md](interfaces.md) |

## 9. Demonstration

| # | Checkpoint | Status | Plan |
|---|---|---|---|
| 9.1 | Stable work for at least 5 minutes | ✅ Done | Procedure and checklist in [testing.md](testing.md#3-stability-test-checkpoint-91) |
| 9.2 | Boundary modes shown | ✅ Done | 14 scenarios in [testing.md](testing.md#4-boundary-modes-to-demonstrate-checkpoint-92) |
| 9.3 | Architecture explained | ✅ Done | Talk plan in [testing.md](testing.md#5-architecture-explanation-checkpoint-93) |
| 9.4 | PCB decisions explained | ❔ Not selected | Decide if you want to include it. Only the schematic and test points exist, so there is little to explain. |
