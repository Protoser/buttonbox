# Buttonbox

Custom USB button box built on an **ESP32-S3 N16R8** with 14 physical buttons and a CR-10 ST7920 display (128x64). The device enumerates as a native USB HID gamepad and includes a companion PC app for live configuration, monitoring, and button chord management.

## Hardware

- **MCU:** ESP32-S3 N16R8 DevKitC-1 (16 MB flash + 8 MB OPI PSRAM)
- **Display:** CR-10 module with ST7920 controller (128x64, software SPI)
- **Buttons:** 14 total — 10 always-on HID buttons, 4 multiplexed nav/menu buttons via a mode toggle
- **USB:** Native TinyUSB CDC + gamepad HID

## Project structure

```
platformio.ini        PlatformIO config (ESP32-S3, Arduino framework)
src/                  ESP32 firmware source code
  main.cpp            Entry point — init in setup(), tick in loop()
  config.h            Pin map and constants
  settings.h/.cpp     NVS-persisted device settings
  buttons.h/.cpp      Debounced button input handling
  hid.h/.cpp          USB HID gamepad report generation
  chords.h/.cpp       Multi-button combos -> output keypresses
  display.h/.cpp      ST7920 LCD drawing (U8g2)
  ui.h/.cpp           On-screen menu pages, navigation and rendering
  hostlink.h/.cpp     Serial protocol: PC telemetry + config commands
  pcstats.h/.cpp      PC stats display page (CPU/RAM/GPU/temps)
  shelly.h/.cpp       Shelly smart switch integration
  stopwatch.h/.cpp    Lap timer utility

host/                 Windows companion app (Python / PySide6)
  app.py              System-tray GUI with Monitor, Device, and Chords tabs
  link.py             Background serial thread — discovery, telemetry, auto-reconnect
  sensors.py          Hardware sensor reads via psutil + LibreHardwareMonitor
  pcstats.py          Headless CLI fallback (no GUI), streams stats only
  companion.spec      PyInstaller build spec
  build.bat           One-shot build script -> dist/ButtonboxCompanion.exe

include/              Shared project header files
lib/                  Private PlatformIO libraries
test/                 PlatformIO test runner directory
```

## Quick start — firmware

Requires [PlatformIO](https://platformio.org).

```bash
pio run -t upload    # flash to device
pio monitor          # serial console at 115200 baud
```

The build requires `espressif32` platform and the U8g2 library (pulled automatically via PlatformIO lib_deps).

## Quick start — companion app

```powershell
cd host
pip install PySide6 pyserial psutil
pythonw app.py       # tray app (no console window)
```

### Prerequisites for full sensor support

GPU load/temp and CPU temperature require [LibreHardwareMonitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor). Download the release zip, copy all `.dll` files into `host/`, and run the app as Administrator.

### Build standalone executable

```powershell
cd host
build.bat            # outputs dist/ButtonboxCompanion.exe
```

## Features

- **Native USB gamepad** — all 14 buttons report as HID keys, works in any game or app without drivers
- **On-screen menu** — navigate settings with the 4 multiplexed buttons and LCD display
- **Button chords** — combine 2+ physical buttons into a single output keypress (configured via PC app)
- **PC telemetry overlay** — live CPU/RAM/GPU load, temperatures, power, and VRAM displayed on the box screen
- **Device configuration** — rotate display, set idle blank timeout, boot screen selection, chord editing, and more — all persisted in NVS flash
- **Auto-reconnect** — companion app reconnects automatically if the USB cable is unplugged/replugged
