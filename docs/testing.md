# Testing and measurements

This document is the plan for the measurements and the demonstration. Empty cells (`TBD`) must be filled
with real results from your board. Do not use numbers that you did not measure.

## 1. Execution time of critical code (checkpoint 6.1)

### Method

Use the Cortex-M4 cycle counter (DWT). At 84 MHz, `time_us = cycles / 84`.

```c
/* once, at start */
CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
DWT->CYCCNT = 0;
DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

/* around the code under test */
uint32_t t0 = DWT->CYCCNT;
/* ... code under test ... */
uint32_t cycles = DWT->CYCCNT - t0;
```

Print the result with `LOG_I()` (not inside the measured code). Repeat 100 times and write min / max / average.
Use the **maximum** as the worst case. A task can be interrupted by a higher-priority task or an interrupt,
so for long code measure the time with interrupts and tasks as they really run.

### What to measure

| # | Code | Why it is critical | min | avg | max |
|---|---|---|---|---|---|
| 1 | `Protocol_Encode()` for an 8-byte payload | Runs for every sent message | TBD | TBD | TBD |
| 2 | `Protocol_ParserProcess()` for one 18-byte packet | Runs for every received message | TBD | TBD | TBD |
| 3 | `Protocol_Crc16()` over 256 bytes | CRC is the biggest cost in the protocol | TBD | TBD | TBD |
| 4 | One `step()` of the button FSM | Must finish before the next edge | TBD | TBD | TBD |
| 5 | BME280 `compensate_*()` (all three) | Integer math | TBD | TBD | TBD |
| 6 | BME280 `measure()` without the 12 ms wait | SPI time | TBD | TBD | TBD |
| 7 | One OLED frame (`u8g2_SendBuffer`), CPU time and total time | Biggest transfer on the I2C bus | TBD | TBD | TBD |
| 8 | `LOG_I()` call (producer side only) | Must be very short | TBD | TBD | TBD |
| 9 | Button press -> MQTT message on the broker (end-to-end) | User-visible latency. Measure with a logic analyzer on PB0 and a timestamped `mosquitto_sub`. | - | TBD | TBD |

## 2. CPU load (checkpoint 6.4)

Goal: the CPU load must be **below 70 %**.

### Method

Use FreeRTOS run-time statistics. In `FreeRTOSConfig.h` set:

```c
#define configGENERATE_RUN_TIME_STATS            1
#define configUSE_TRACE_FACILITY                 1
#define configUSE_STATS_FORMATTING_FUNCTIONS     1
/* a free-running timer that is 10-20 times faster than the tick */
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() /* start the timer */
#define portGET_RUN_TIME_COUNTER_VALUE()         /* return the timer value */
```

Then call `vTaskGetRunTimeStats(buffer)` every few seconds and print it.
**CPU load = 100 % - (IDLE task percentage).**

Measure in three situations:

| Situation | Idle % | CPU load % |
|---|---|---|
| Normal work (no button presses) | TBD | TBD |
| Fast button pressing (both buttons) | TBD | TBD |
| Worst case: fast pressing + UART error flood (see section 4) | TBD | TBD |

Also write down from `MemStat_Log()` and the supervisor: minimum free heap and the stack high-water mark of each task.

## 3. Stability test (checkpoint 9.1)

**Requirement:** the project works without errors for at least **5 minutes**. We recommend 30 minutes before the demo.

Steps:

1. Reset the STM32 and the ESP32. Start `mosquitto_sub -t "stm32bridge/#" -v` and `deckd`.
2. Press buttons from time to time (single, double, long press).
3. Leave the device running.

What must be true during the whole test:

- [ ] No watchdog reset (`reset reason - IWDG` does not appear in the log)
- [ ] The OLED time changes every second, values are updated
- [ ] `stm32bridge/rtc/time` arrives about every 10 s, `stm32bridge/stm/state` about every 5 s
- [ ] `stm32bridge/stm/online` stays `1`
- [ ] `rx_crc_errors`, `rx_overflow`, `tx_timeouts` do not grow (or grow very slowly)
- [ ] `heap_min` does not go down all the time (no memory leak)
- [ ] Hotkeys work in the target application

Result: duration `TBD`, resets `TBD`, final `heap_min` `TBD`.

## 4. Boundary modes to demonstrate (checkpoint 9.2)

"Expected result" is what the code should do. Confirm it on the real board.

| # | Scenario | How to do it | Expected result |
|---|---|---|---|
| 1 | Wrong data on the UART | Send random bytes to the STM32 RX with a USB-UART adapter (or a small script) | The parser resyncs. Counters `rx_invalid_sof` / `rx_crc_errors` grow. The system keeps working. |
| 2 | Broken packet | Send a valid packet with one changed byte | `rx_crc_errors` + 1, the message is ignored |
| 3 | Too long payload | Send a packet with length field larger than 128 | `rx_invalid_length` + 1, resync |
| 4 | Button bounce and fast pressing | Press the button very fast many times | Debounce removes the noise. Click count stops at 255. If the TX queue is full, a warning is logged and the message is dropped, but nothing hangs. |
| 5 | Very long press | Hold a button for 10 s | One `LONG_PRESS` event. Duration is limited to 65535 ms. No click is counted after a long press. |
| 6 | Invalid time command | `mosquitto_pub -t stm32bridge/cmd/rtc/set -m "2026-02-30 25:61:00"` | Rejected. `cmd/rtc/result` = `error`. The clock does not change. |
| 7 | Leap-year date | Set `2028-02-29 12:00:00` | Accepted (valid date) |
| 8 | Sensor disconnected | Remove the BME280 CS or MISO wire | After 3 failed reads the sensor is re-initialized. Flag `bme` = 0 in `stm/state`. After you reconnect, it recovers by itself. |
| 9 | OLED disconnected | Remove the OLED SDA wire | Display is marked offline, retried. The rest (RTC, buttons, ESP32) continues. Flag `oled` = 0. |
| 10 | RTC disconnected | Remove the DS1307 SDA wire | Flag `rtc` = 0. No crash. The RTC recovers after reconnect. |
| 11 | Wi-Fi or broker lost | Stop the broker or switch off the router | ESP32 reconnects by itself. After reconnect the last values are published again. The last-will message shows `offline`. |
| 12 | STM32 silent | Hold the STM32 in reset for more than 30 s | `stm/online` = `0`. It becomes `1` again after the STM32 starts. |
| 13 | Environment limits | Breathe on the sensor (humidity), use a hair dryer on low (temperature) | Values change. Humidity stays in 0..100 %. Negative temperatures are shown with the right sign. |
| 14 | Watchdog (optional, needs a temporary code change) | Make one task stop checking in (for example, an endless loop in a test build) | Supervisor stops feeding the IWDG. The MCU resets in about 2 s. The log shows the reason and the silent task. |
