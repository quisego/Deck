# Interfaces

This document explains every interface used in the project: what it connects, how it is configured
and why we chose it.

## 1. Summary

| Interface | Where | Connects | Settings | DMA |
|---|---|---|---|---|
| UART1 | STM32 <-> ESP32 | Binary packets | 115200, 8N1, no flow control | TX normal, RX circular |
| UART2 | STM32 -> PC | Debug log (TX only) | 115200 | TX |
| I2C1 | STM32 | SSD1306 OLED, DS1307 RTC | 100 kHz, 7-bit | OLED TX only |
| SPI1 | STM32 | BME280 | Master, prescaler 16, CS on PA4 (GPIO) | No |
| EXTI | STM32 | 2 buttons (PB0, PB1) | Rising + falling edge | - |
| IWDG | STM32 | Watchdog | Prescaler 16, reload 4095 (about 2 s) | - |
| UART1 | ESP32 | STM32 link | GPIO4 = TX, GPIO5 = RX, 115200 | Driver buffers (1024 B) |
| Wi-Fi + MQTT | ESP32 <-> broker | JSON topics | MQTT keepalive 30 s | - |
| TCP (localhost) | `deckctl` <-> `deckd` | IPC commands | `127.0.0.1:47800` | - |

## 2. UART link between STM32 and ESP32

### Why UART?

It is simple, it needs only two wires, and both chips have good support. The speed (115200 baud, about 11 KB/s)
is more than enough: our messages are small and rare.

### STM32 side

- **RX**: DMA in **circular** mode writes into a 256-byte ring buffer all the time. The CPU does not read byte by byte.
- The `LinkRx` task wakes up on three events: *half buffer full*, *buffer full*, and the **UART IDLE interrupt**
  (the line was quiet for one byte time, which usually means "the packet ended").
- DMA does not tell how many laps it made, so the driver counts half-buffer interrupts and calculates the
  total number of received bytes. If the task is too slow and DMA overwrites unread data, the driver detects it
  and increments `rx_overflow`.
- **TX**: `Link_Send()` puts a message in a free slot and returns at once (it does not wait if `wait = 0`).
  The `LinkTx` task builds the packet and starts a DMA transfer, then sleeps until the TX-complete interrupt.
  There is a timeout, so it never waits forever.

### ESP32 side

The ESP-IDF UART driver has its own RX/TX buffers, so we do not need our own DMA code. The `Link` API is the same
as on the STM32 (`Link_Init`, `Link_Send`, `Link_Subscribe`), so the application code looks the same on both chips.

### Statistics

`LinkStats` counts: `tx_messages`, `tx_queue_full`, `tx_timeouts`, `tx_errors`, `rx_messages`, `rx_unknown_id`,
`rx_overflow`, `rx_uart_errors`, `rx_invalid_sof`, `rx_invalid_version`, `rx_invalid_length`, `rx_crc_errors`.
They help to see problems during tests.

## 3. Packet format (`common/proto`)

All numbers are **little-endian** (low byte first).

```
+------+------+---------+-------------+-----+-------------+-----------------+-------------+
| 0xA5 | 0x5A | version | message id  | seq | length      | payload         | CRC16       |
| 1 B  | 1 B  | 1 B (=1)| 2 B         | 1 B | 2 B         | 0..256 B        | 2 B         |
+------+------+---------+-------------+-----+-------------+-----------------+-------------+
                |<------------------ CRC is calculated over these bytes ----------------->|
```

- **SOF** (two start bytes) lets the receiver find the start of a packet in a stream.
- **seq** is a packet counter.
- **CRC16-CCITT**: polynomial `0x1021`, start value `0xFFFF`. It covers the header (without SOF) and the payload.
- The receiver accepts only payloads up to `max_payload` (128 bytes in this project).
- Payloads are written byte by byte (no `struct` copy), so the format is the same on both chips.

### Messages

| ID | Name | Direction | Payload size | Content |
|---|---|---|---|---|
| `0x0001` | `SYS_PING` | - | 4 | uptime (defined, not used yet) |
| `0x0002` | `SYS_PONG` | - | - | (defined, not used yet) |
| `0x0003` | `SYS_STATUS` | STM32 -> ESP32 | 16 | uptime ms, free heap, minimum free heap, device flags |
| `0x0101` | `BUTTON_PRESSED` | STM32 -> ESP32 | 8 | button id, clicks, duration, timestamp |
| `0x0102` | `BUTTON_RELEASED` | STM32 -> ESP32 | 8 | same |
| `0x0103` | `BUTTON_CLICKS` | STM32 -> ESP32 | 8 | same (click count is valid) |
| `0x0104` | `BUTTON_LONG_PRESS` | STM32 -> ESP32 | 8 | same (duration is valid) |
| `0x0201` | `RTC_TIME` | STM32 -> ESP32 | 8 | year (2 B), month, date, weekday, hours, minutes, seconds |
| `0x0202` | `RTC_SET` | ESP32 -> STM32 | 8 | same layout; weekday is calculated by the STM32 |
| `0x0301` | `ENV_DATA` | STM32 -> ESP32 | 8 | temperature x100 (int16), humidity x100 (uint16), pressure in Pa (uint32) |

Sensor values are multiplied by 100 and sent as integers, so the firmware does not need floating point.
Status flags: bit 0 = OLED online, bit 1 = RTC online, bit 2 = BME280 online.

Button id = `(port index << 4) | pin number`. So `PB0` = `0x10` (button A) and `PB1` = `0x11` (button B).

## 4. I2C (OLED and RTC)

- I2C1 at 100 kHz. Two devices share the bus: SSD1306 (`0x3C`) and DS1307 (`0x68`).
- **Bus lock:** a FreeRTOS mutex (`I2C_Bus_Lock`). A task takes it before any transfer and gives it back after.
- **OLED** uses `HAL_I2C_Master_Transmit_DMA`. The task waits for a semaphore that the I2C interrupt gives.
  If it times out (100 ms), the I2C peripheral is reset and the screen is marked offline.
  The display task only redraws when something changed.
- **RTC** uses blocking `HAL_I2C_Mem_Read/Write` (7 bytes). It is a very short transfer.
- The DS1307 stores numbers in **BCD**. The driver converts them and checks that every digit is 0..9.
  It also handles 12h/24h mode and starts the oscillator if the "clock halt" bit is set.

## 5. SPI (BME280)

- SPI1 in master mode. The chip select (CS) is a normal GPIO (PA4).
- Reading: send the register address with bit 7 set, then read the answer. Writing: bit 7 cleared.
- Steps at start: check the chip id (`0x60`), soft reset, read the **calibration data** (every chip has its own).
- Each measurement uses **forced mode**: the sensor measures once and goes back to sleep.
- Raw values are converted with the **integer formulas from the Bosch datasheet**. Humidity is limited to 0..100 %RH.
- SPI is used for the sensor because it is simple and it does not share the I2C bus with the OLED and RTC.

## 6. Interrupts

| Interrupt | Priority (NVIC) | What it does | Rule |
|---|---|---|---|
| EXTI0, EXTI1 (buttons) | 5 | Puts one signal in the button's queue | Very short. No logic. |
| UART1 IDLE / DMA RX half / full (DMA2 Stream2) | 0 in CubeMX (see note below) | Wakes the `LinkRx` task (`vTaskNotifyGiveFromISR`) | Only a notification |
| DMA TX complete (UART1, UART2) | 5 | Wakes the TX or logger task | Only a notification |
| I2C DMA complete / error | - | Gives a semaphore to the OLED task | Only a semaphore |
| UART error | - | Sets a flag, wakes tasks | The task restarts the DMA |

All real work is done in tasks. The interrupts only wake a task.

> **Note to check:** in `main.c` the DMA stream for UART1 RX (`DMA2_Stream2`) has priority 0, but the other
> interrupts that use FreeRTOS have priority 5. FreeRTOS `...FromISR()` functions must only be called from interrupts
> whose priority number is **not lower** than `configMAX_SYSCALL_INTERRUPT_PRIORITY`. Please check that value in
> `FreeRTOSConfig.h` and set this DMA priority to 5 in CubeMX if needed.

## 7. Watchdog and timers

- **IWDG** (hardware, independent clock). Only the `Supervisor` task refreshes it, and only if **all**
  registered tasks have checked in on time. Each task has its own timeout (for example buttons 1 s, RTC 3 s).
- **FreeRTOS tick / SysTick** gives all delays and timeouts (`vTaskDelay`, `vTaskDelayUntil`, queue timeouts).
- **Hardware events:** EXTI edge (button), UART IDLE line, DMA half/full transfer, I2C DMA complete.
- On the ESP32: `esp_timer` for time since boot, and SNTP for the real time.

## 8. MQTT (ESP32 <-> PC)

Base topic is `stm32bridge` (configurable). Payloads are JSON or plain text.

| Topic | Direction | Retained | QoS | Payload |
|---|---|---|---|---|
| `BASE/status` | ESP32 -> broker | yes | - | `online` / `offline` (last will) |
| `BASE/esp/state` | ESP32 -> | yes | - | `{"uptime_s","heap_free","heap_min","rssi"}` every 30 s |
| `BASE/stm/online` | ESP32 -> | yes | - | `1` / `0` (STM32 silent for more than 30 s) |
| `BASE/stm/state` | ESP32 -> | yes | 1 | `{"uptime_s","heap_free","heap_min","oled","rtc","bme"}` |
| `BASE/sensor/env` | ESP32 -> | yes | 1 | `{"temperature_c","humidity_pct","pressure_hpa"}` |
| `BASE/sensor/temperature` `humidity` `pressure` | ESP32 -> | yes | - | single value |
| `BASE/rtc/time` | ESP32 -> | yes | 0 | `{"iso":"2026-11-12T12:00:00","weekday":5}` every 10 s |
| `BASE/button/<A\|B>/event` | ESP32 -> | no | 1 | `{"event","button_id","name","timestamp_ms","duration_ms","click_count",...}` |
| `BASE/button/<A\|B>/state` | ESP32 -> | yes | - | `1` pressed / `0` released |
| `BASE/cmd/rtc/set` | PC -> ESP32 | - | - | `YYYY-MM-DD HH:MM:SS` |
| `BASE/cmd/rtc/sync` | PC -> ESP32 | - | - | any payload: copy the SNTP time to the DS1307 |
| `BASE/cmd/rtc/result` | ESP32 -> | - | - | `ok` / `error` |

Button events are **not** retained (they are one-time events). State topics are retained so a new subscriber
gets the last value at once.

### SNTP and the RTC loop

When SNTP gets the real time, the ESP32 can write it to the DS1307 (option `APP_RTC_SYNC_ON_SNTP`).
It does not send a time before 2024, because that means SNTP is not ready yet.

## 9. IPC between `deckctl` and `deckd`

A JSON request/answer over TCP on `127.0.0.1` only (not reachable from the network). Default port is `47800`.
Commands: list windows, show status, learn a button, and add / list / remove bindings.
In **learn mode** `deckd` waits (15 s by default) for the next button press and returns its id.
