# DIY Stream Deck with Environment Monitor

A small desk device built with two microcontrollers and a PC program.

- **STM32** reads two buttons, a BME280 sensor and a DS1307 clock. It shows the data on an OLED screen.
- **ESP32-S3** connects the device to Wi-Fi and MQTT. It also gets the real time from the internet (SNTP).
- **`deckd` (PC daemon)** receives button events and presses keyboard shortcuts in the active Windows application.

```
Button -> STM32 --UART--> ESP32 --Wi-Fi/MQTT--> deckd (PC) --hotkey--> active application
```

The device only reports events. All binding logic (which button does what in which application) lives on the PC.

## The problem we solve

A normal keyboard has no free buttons for shortcuts that you use every day. This project gives you
physical buttons whose action **changes with the active application**. The same device also measures
temperature, humidity and pressure, and it keeps accurate time.

## Features

- Two buttons with debounce, **single/double/triple clicks**, and **long press** detection (state machine per button).
- Temperature, humidity and pressure from a BME280 (SPI). Data is sent only when it changes enough.
- Real-time clock DS1307 (I2C). Time can be set from the internet or by an MQTT command.
- 128x64 OLED (I2C + DMA) shows date, time and sensor values.
- Binary UART protocol between STM32 and ESP32 with **CRC16**, resynchronization and error counters.
- MQTT bridge with retained topics and a "last will" message (online/offline status).
- Windows daemon that maps `(application, button, event)` to a hotkey, a command or a delay.
- Reliability: hardware watchdog (IWDG) + software supervisor, stack checks, automatic re-init of sensors.

## Hardware

| Part | Role | Interface |
|---|---|---|
| STM32F4 board (Black Pill) | Real-time part: buttons, sensors, display | - |
| ESP32-S3-WROOM-1 board | Wi-Fi, MQTT, SNTP | UART to STM32 |
| BME280 module | Temperature, humidity, pressure | SPI |
| DS1307 RTC module | Date and time | I2C (0x68) |
| SSD1306 OLED 128x64 | Local display | I2C (0x3C) + DMA |
| 2 x button module | User input | GPIO + EXTI interrupt |

See [docs/hardware.md](docs/hardware.md) for wiring and the PCB plan.

## Repository structure

```
.
├── common/proto/            Shared packet protocol (C). Used by STM32 and ESP32.
├── stm32/                   STM32 firmware (STM32CubeMX + CMake + FreeRTOS)
│   ├── Common/              FSM, Link (UART), Logger, Supervisor, Memstat, result.h
│   ├── BSP/                 Drivers: Button, BME280, DS1307, SSD1306, I2C bus lock
│   ├── Cmd/App/             Application: connects all modules
│   ├── Core/                CubeMX generated code (main.c, ...)
│   └── ThirdParty/u8g2/     Graphics library for the OLED
├── esp32/                   ESP-IDF project
│   ├── components/          link, bridge (Wi-Fi + MQTT + SNTP), proto
│   └── main/                app_main
├── pc/                      deckd.py (daemon), deckctl.py (command line tool)
└── docs/                    Documentation
```

## Quick start

1. Wire the hardware. See [docs/hardware.md](docs/hardware.md).
2. Build and flash the STM32 and the ESP32. See [docs/getting-started.md](docs/getting-started.md).
3. Start an MQTT broker and the PC daemon:
   ```
   cd pc
   pip install -r requirements.txt
   python deckd.py
   ```
4. Bind a button to a hotkey:
   ```
   python deckctl.py bind add --exe Code.exe --keys ctrl+shift+t
   ```
   Then press a button on the device when `deckctl` asks you.

## Documentation

| File | Content |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Block diagrams, tasks, state machines, synchronization, reliability |
| [docs/interfaces.md](docs/interfaces.md) | UART, I2C, SPI, DMA, interrupts, packet format, MQTT topics |
| [docs/hardware.md](docs/hardware.md) | Wiring table, test points, PCB plan |
| [docs/getting-started.md](docs/getting-started.md) | How to build, configure and run everything |
| [docs/testing.md](docs/testing.md) | Timing and CPU measurements, stability test, boundary-mode demo |
| [docs/checkpoints.md](docs/checkpoints.md) | Course checklist with status of every point |

## Known limitations

- `deckd` works only on Windows 10/11 (it uses Win32 API through `ctypes`).
- The firmware supports one BME280, one DS1307 and one logger (static objects).
- `MSG_ID_SYS_PING` and `MSG_ID_SYS_PONG` are defined in the protocol but are not used yet.
- The BME280 is read with blocking SPI (no DMA). The transfer is very short, so this is acceptable.
- `deckd` can send a short hint for the OLED over MQTT, but the ESP32 does not subscribe to it yet.

## Security note

Wi-Fi and MQTT settings are in `esp32` Kconfig (`menuconfig` -> *App Configuration*).
**Do not commit your real Wi-Fi password.** Keep it in a local `sdkconfig` that is listed in `.gitignore`,
and change the default values before you publish the repository.
