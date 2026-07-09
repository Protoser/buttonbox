## What's new since v2.0

### Dimmable screen (new)

- **PWM backlight dimming** — the display backlight is now driven from a dedicated pin, with a **gamma-corrected** brightness curve so equal steps look evenly spaced to the eye.
- **Two brightness levels** — a normal (active) level and a dimmed **idle** level, both saved on the box.
- **Auto-dim on idle** — after a configurable delay of no input the screen dims to the idle level; any button press brings it straight back to full. When the screen fully blanks, the backlight turns off entirely.
- **Adjust on the fly** — assign a **chord** to the new *Brightness overlay*: press it on any page to pop a brightness bar, then Up/Down to adjust, Select/Back (or a short timeout) to dismiss.

### Companion

- New **Display** pane controls: Brightness, Idle brightness, and Auto-dim delay — synced both ways with the box.
- The **Chords** tab can now assign *Brightness overlay* as a chord output.

### On-device

- **Settings** (Menu → Settings) gains Brightness, Idle brightness, and Auto-dim rows.
- The chord output picker can select the *Brightness overlay* action directly on the box.

## Install

- **Firmware:** flash `firmware.bin` to the ESP32-S3 (PlatformIO, or the on-device Flash Mode + esptool).
- **Companion (Windows only):** run `ButtonboxCompanion.exe`.
