---
name: codebase-map
description: Build a full mental model of the buttonbox codebase before fixing a bug or adding a feature. A map of what each part does and where to find it (ESP32-S3 firmware in src/, Python companion in host/, joined by a USB-serial protocol). Use at the start of any non-trivial change.
---

# Buttonbox codebase map

Read this first whenever you're about to fix a bug or add a feature and you don't
already have the relevant files loaded. It tells you **what the system is**, **how
the two halves talk**, **which file owns what**, and **where to make a given kind
of change**. Then open only the files the task touches.

## How to use this skill

1. Read the **System shape** and **Serial protocol** sections below — that's the
   architecture you can't get from any single file.
2. Find your task in **Where to make changes** to jump to the owning module.
3. Open that module's `.h` first (the headers are documented), then the `.cpp`.
4. Verify before asserting: this map can drift. Cross-check pin numbers, the HID
   mask, page enums, and the protocol token names against the live code — the
   firmware and the companion **must agree** on protocol and counts, so a change
   on one side almost always needs the mirror change on the other.
5. For build/flash specifics, see the memory note `buttonbox-build-upload`
   (embedded `pio.exe`, nested project dir, Flash Mode for hands-free upload).

## System shape

Two programs, one cable:

```
   ESP32-S3 firmware  (src/, C++/Arduino)            PC companion (host/, Python/PySide6)
   ┌─────────────────────────────────┐               ┌──────────────────────────────────┐
   │ buttons → chords/HID → USB HID   │  USB HID  ───▶│ games / OS see a 32-button gamepad │
   │ ST7920 display ← ui pages        │               │                                    │
   │ hostlink (CDC serial) ◀────────▶ │  USB CDC  ◀──▶ │ link.py serial thread             │
   │   telemetry in, config out       │   serial      │   sensors / media / shelly polling │
   └─────────────────────────────────┘               │   app.py tray GUI (config + chords)│
                                                      └──────────────────────────────────┘
```

- The box is a **native USB HID gamepad** (32-button mask) — that's the input the
  PC games consume. HID is input-only.
- A second **USB CDC serial** channel (`hostlink` ⇄ `link.py`) is the two-way
  control link: the PC pushes telemetry (PC stats, now-playing media, Shelly
  state) for the box to display, and the GUI reads/writes settings and chords.
- The box can run **without** the companion (HID + on-device menu still work);
  the companion only adds telemetry display and live config.

## Serial protocol — the contract between the halves

Defined firmware-side in [hostlink.h](src/hostlink.h)/[hostlink.cpp](src/hostlink.cpp),
mirrored PC-side in [host/link.py](host/link.py). Newline-terminated text lines,
dispatched on the first token. **Change one side, change the other.**

PC → box:
- `<key:value ...>` — PC telemetry → `pcStatsApply` (CPU/RAM/GPU/temps/power)
- `music <state> <title>` — now-playing → `musicApply` (state 0 none/1 play/2 pause)
- `get` — request current config; box replies `cfg` + `chd` lines
- `set <key>:<val>` — change & persist a setting, box replies `cfg`
- `chord add <mask>:<output>` / `chord del <index>` — edit chords, box replies
- `flash` — enter bootloader (Flash Mode)
- (Shelly state pushed from `shelly_poll.py` so the box needn't join WiFi)

Box → PC:
- `cfg flip:.. labels:.. idle:.. chord:.. boot:.. pcorder:.. nchords:..`
- `chd <i>:<membersMask>:<output>` — one per configured chord
- `mctl prev|playpause|next` — media control requests (Music page → companion)

## Firmware map (`src/`) — ESP32-S3, PlatformIO + Arduino

Wiring/orchestration only lives in [main.cpp](src/main.cpp): `setup()` inits each
module; `loop()` order is **buttonsUpdate → hostlinkUpdate → uiHandleMenuButton →
(non-Buttons page: uiHandlePageInput claims its buttons) → updateChords (every
page) → timer lap → uiTickDisplay**. Understanding that order explains most
input-routing behavior.

| File | Owns | Notes |
|------|------|-------|
| [config.h](src/config.h) | Pin map, button counts, `NavAction` enum, debounce | **Single source of truth for pins.** 10 always-on HID pins + 4 nav pins + 1 mode-toggle. `NUM_HID = 14`. |
| [buttons.h](src/buttons.h)/.cpp | Debounced reads, edge detection, `uiSuppressedMask` | UI "claims" buttons by suppressing them so chords/HID skip them. |
| [hid.h](src/hid.h)/.cpp | USB gamepad report (32-bit button mask) | Thin wrapper over `USBHIDGamepad`. |
| [chords.h](src/chords.h)/.cpp | 2+ button combos → one virtual button; `updateChords` | Runs **every page**; hold-window timing model; up to 18 chords; persisted to NVS. Physical buttons = HID 1–14, chord outputs = 15–32. |
| [settings.h](src/settings.h)/.cpp | NVS-persisted options via Preferences | orientation, label style, idle-blank, chord window, `bootSel` (boot screen). |
| [display.h](src/display.h)/.cpp | The U8g2 ST7920 panel object | SW SPI on 17/18/21, rotated `U8G2_R2`. |
| [ui.h](src/ui.h)/[ui.cpp](src/ui.cpp) | **All pages, nav, rendering** (~820 lines, biggest file) | Apps launcher + pages. `APPS[]` table drives the launcher grid. Page enum: `PAGE_LAUNCHER`, `PAGE_BUTTONS`, `PAGE_TIMER`, etc. Owns every `u8g2` draw call. |
| [stopwatch.h](src/stopwatch.h)/.cpp | Lap timer logic | Background-running; ring of 30 laps. |
| [hostlink.h](src/hostlink.h)/.cpp | Serial protocol parse/dispatch | See protocol section above. |
| [pcstats.h](src/pcstats.h)/.cpp | PC-telemetry state + page (`pcStatsApply`) | Fed by `<key:value>` serial lines. |
| [music.h](src/music.h)/.cpp | Now-playing state + Music page (`musicApply`) | Title is ASCII-sanitized PC-side; box emits `mctl`. *(new, untracked)* |
| [shelly.h](src/shelly.h)/.cpp | Shelly smart-switch integration | Largest non-UI module; WiFi-auto vs. companion-poll modes. |

To add an **on-screen app/page**: add a row to `APPS[]` in `ui.cpp`, a small
primitive-drawn icon fn, and a `Page` enum value — see the project memory note for
the launcher/nav conventions (legend glyphs, ~107px content column, etc.).

## Host map (`host/`) — Windows companion, Python + PySide6

| File | Owns | Notes |
|------|------|-------|
| [app.py](host/app.py) | Tray GUI: Monitor / Device / Chords tabs | Mirrors firmware constants at top (`NUM_HID`, chord output range, `MAX_CHORDS`, idle/chord opts) — keep in sync with `config.h`/settings. |
| [link.py](host/link.py) | `DeviceLink` serial thread | Discovers box by USB VID `0x303A`; streams telemetry; queues GUI commands; auto-reconnect on unplug. Pulls from `sensors`/`media`/`shelly_poll`. |
| [sensors.py](host/sensors.py) | PC sensor reads | psutil (cpu/ram) + LibreHardwareMonitor via pythonnet (gpu/temps/power/vram, needs admin + DLLs in folder). |
| [media.py](host/media.py) | Now-playing via Windows GSMTC | Optional `winsdk`; degrades gracefully if absent. Sends `music` lines, handles `mctl`. |
| [shelly_poll.py](host/shelly_poll.py) | Shelly Gen2 HTTP polling | SHA-256 Digest auth, stdlib only; pushes state to box over serial. |
| [secrets_store.py](host/secrets_store.py) | Encrypted secret store (Shelly pw) | Windows DPAPI, stdlib only; no-op off-Windows. |
| [pcstats.py](host/pcstats.py) | Headless CLI fallback | Streams stats only, no GUI. |
| [companion.spec](host/companion.spec) / build.bat | PyInstaller build → `dist/ButtonboxCompanion.exe` | Bundles DLLs under `sys._MEIPASS`. |

Build artifacts (`host/build/`, `__pycache__/`, bundled `.dll`/`.exe`) are not
source — ignore them when mapping or searching.

## Where to make changes (task → file)

- **Re-wire a pin / change button count** → [config.h](src/config.h) (then check
  `app.py` constants and any HID-index assumptions).
- **Change HID button behavior / mask** → [hid.cpp](src/hid.cpp) + [chords.cpp](src/chords.cpp).
- **Add/alter a chord rule or timing** → [chords.cpp](src/chords.cpp) + the Chords
  tab in [app.py](host/app.py) + protocol in [hostlink.cpp](src/hostlink.cpp).
- **Add a screen / menu page / app** → [ui.cpp](src/ui.cpp) (`APPS[]`, `Page` enum,
  icon fn, render + nav handlers).
- **Add a persisted setting** → [settings.h](src/settings.h)/.cpp, surface it in
  `hostlink` `cfg`/`set`, and add the control in `app.py`'s Device tab.
- **Add a new telemetry source on screen** → new state+page module (model
  `pcstats`/`music`), a serial line in `hostlink`, and a producer in `host/`.
- **Change the serial protocol** → [hostlink.cpp](src/hostlink.cpp) **and**
  [link.py](host/link.py) together; update the protocol section above.
- **Companion GUI / reconnect / discovery** → [app.py](host/app.py) / [link.py](host/link.py).
- **Display orientation / drawing primitives** → [display.cpp](src/display.cpp) +
  the relevant draw fn in `ui.cpp`.

## Gotchas

- **Button identity confusion:** the user refers to buttons by the **number shown
  on the screen grid (HID#)**, not GPIO. Those renumber when the layout changes.
  Confirm which numbering before remapping. (The "Labels" setting can show GPIO.)
- **Two halves must agree:** counts (`NUM_HID`, chord output range, `MAX_CHORDS`)
  and protocol tokens are duplicated in firmware and `app.py`/`link.py`.
- **Input routing depends on `loop()` order and `uiSuppressedMask`** — chords/HID
  run on every page; the UI only suppresses the buttons it claims. Don't "fix"
  routing without understanding this.
- **Upload is finicky** (TinyUSB, native USB only): use the on-device Flash Mode,
  then upload immediately. See memory `buttonbox-build-upload`.
- `src/music.*` and `host/media.py` may be untracked/new — check `git status`.
