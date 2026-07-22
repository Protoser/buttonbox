## What's new since v2.3

### Clock app

- **New Clock app in the launcher** — a full-screen wall clock driven by the box's synced time (NTP over WiFi, or pushed from the PC companion; no RTC needed).
- **Analog face** inspired by a De Bethune dial: hollow **lance hands** (shaft → hollow lozenge → pointed tip) for the hour and minute, a thin sweeping second hand, and a full ring of **12 numerals** with an hour pip on the rim.
- **Digital style** as an alternative — big `HH:MM` with optional seconds and AM/PM.
- **Weekday + date** line along the bottom.
- The face uses the whole panel (no header bar) and keeps the display awake so the seconds hand keeps ticking.

### Customizing the clock

- **On the box → open Clock → Select (►)** opens a per-clock settings page. Toggle each option with Select:
  - **Style** — Analog / Digital
  - **Seconds** — show the seconds hand (or digital seconds)
  - **Numerals** — all-12 numerals vs. a plain tick ring
  - **Date** — show the weekday/date line
  - **Hour fmt** — 24h / 12h
- Settings persist across reboots.

### Fixes

- Fixed a launcher bug where adding an app could write one entry past the app-order list and corrupt adjacent settings.
- Companion app list and Boot-screen dropdown now include the **Volume** and **Clock** apps, keeping them index-aligned with the firmware.

## Install

- **Firmware:** flash `firmware.bin` to the ESP32-S3 (PlatformIO, or the on-device Flash Mode + esptool). The Clock app appears in the launcher; open it and press Select to customize.
- **Companion (Windows only):** run `ButtonboxCompanion.exe`.
