# Buttonbox Companion (PC app)

A system-tray app that streams PC stats to the buttonbox **and** reads/edits the
device's settings and chords live over its USB serial link. Starts minimized to
the tray and **auto-reconnects** whenever the box is unplugged/replugged.

```
host/
  app.py          the companion app (tray + Monitor / Device / Chords tabs)
  link.py         background serial thread: discovery, telemetry, commands, auto-reconnect
  sensors.py      sensor reading (psutil + LibreHardwareMonitor / nvidia-smi)
  pcstats.py      headless CLI fallback (no GUI), shares sensors.py
  companion.spec  PyInstaller build -> standalone .exe
  build.bat       one-shot build script
```

## Run from source

```powershell
pip install PySide6 pyserial psutil
python app.py            # tray app (use pythonw app.py for no console flash)
```

It appears in the system tray (blue "BB" icon). **Left-click** the tray icon to
open the window, **right-click** for the menu (Show / Start at login / Flash
device / Quit). Closing the window hides it back to the tray.

- **Monitor** tab — connection status + live CPU/RAM/GPU/temp bars.
- **Device** tab — Rotate, grid Labels, Idle-blank, Chord window, Boot screen,
  and which PC stats the box shows. Changing any control writes it to the box and
  it persists in the box's flash. There's also a **Flash Mode** button (handy for
  firmware uploads).
- **Chords** tab — see/add/delete button chords (pick 2+ member buttons + an
  output button); changes apply on the box immediately.

## Temperatures (LibreHardwareMonitor)

CPU load + RAM need nothing extra. GPU load/temp and **CPU temp** need a sensor
source:

1. `pip install pythonnet`, then copy **all `.dll` files** from the
   LibreHardwareMonitor release zip into this `host/` folder
   (`LibreHardwareMonitorLib.dll` **and** its siblings `System.Memory.dll`,
   `System.Buffers.dll`, `System.Runtime.CompilerServices.Unsafe.dll`,
   `System.Numerics.Vectors.dll`, `System.Threading.Tasks.Extensions.dll`,
   `HidSharp.dll`). The app resolves them from this folder automatically.
2. **Run the app as Administrator** — CPU temperature comes from MSR/SuperIO,
   which needs the kernel sensor driver. Without admin you'll still get CPU/RAM,
   GPU load/temp (via NVAPI/nvidia-smi), just not CPU temp.

NVIDIA-only machines can skip LHM for GPU (nvidia-smi is used as a fallback), but
CPU temp still needs LHM + admin.

## Build a standalone .exe

Put the LHM DLLs in `host/` first (they get bundled), then:

```powershell
build.bat            # or:  py -m PyInstaller --noconfirm companion.spec
```

Output: `dist\ButtonboxCompanion.exe` — runs without Python. Run it as admin for
temps.

## Start at login

Toggle **Start at login** in the tray menu — it adds/removes an entry under
`HKCU\…\CurrentVersion\Run` pointing at the exe (or `pythonw app.py` when run
from source), so the app launches minimized to the tray on sign-in.

## Headless fallback

`python pcstats.py` just streams stats (no GUI, no config), also auto-reconnecting.
