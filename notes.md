## What's new since v2.2

### Keyboard macro mode (major change)

- **The box is now a USB keyboard, not a gamepad.** Every button — and every chord — sends a real keystroke, so it works as a universal macro pad in apps that don't accept a controller: OBS, VS Code, DaVinci Resolve, and so on. **Games bound to the old gamepad buttons must be rebound to the new keys.**
- **N-key rollover (NKRO)** — a custom HID report removes the usual 6-key limit, so any number of buttons held at once all register.
- **Bind any key (+ modifiers) to any output.** All 32 outputs — the 14 physical buttons and the 18 chord outputs — map to a HID key with optional Ctrl / Shift / Alt / Win.
- **Default map:** the 10 always-on buttons come pre-bound to **F13–F22** — "phantom" keys no normal keyboard has, so they never clash with typing or shortcuts and are safe to bind in any app. Nav buttons and chords start unbound.

### Editing your bindings

- **Companion → Keys pane** — one row per output with modifier checkboxes and a **press-to-set** capture (just press the key you want it to send), plus a one-click **Preset F13–F24** for the physical buttons.
- **On the box → Menu → Key Binds** — press a button, then set its modifiers and key with the nav buttons; no PC needed.

### Fixes / polish

- **Header clock now shows seconds** (HH:MM:SS), synced from the PC or NTP.
- Shortened a few page titles so they no longer overlap the (now wider) clock.

## Install

- **Firmware:** flash `firmware.bin` to the ESP32-S3 (PlatformIO, or the on-device Flash Mode + esptool). After flashing, the box enumerates as a keyboard — open the companion's **Keys** pane to set your bindings.
- **Companion (Windows only):** run `ButtonboxCompanion.exe`. The Keys pane reads and writes your bindings live over USB.
