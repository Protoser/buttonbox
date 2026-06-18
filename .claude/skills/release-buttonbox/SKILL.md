---
name: release-buttonbox
description: Cut and publish a Buttonbox GitHub release — build the companion .exe and firmware.bin, tag vX.Y-windows-only, and create the gh release with both assets attached. Use when asked to release, publish a new version, ship a build, tag a release, or update the GitHub release.
---

Cuts a versioned GitHub release for the buttonbox repo. Releases ship **two
assets** — the Windows companion `ButtonboxCompanion.exe` and the ESP32-S3
`firmware.bin` — under an annotated tag `vX.Y-windows-only`, matching v1.0/v1.1/v1.2.

The mechanical half (build both artifacts → tag → publish) is the driver:
[`.claude/skills/release-buttonbox/release.ps1`](release.ps1). The judgement half
is yours: the version number and the "what's new" notes.

**Windows + PowerShell only** — the companion is built with PyInstaller and the
firmware with the local PlatformIO. All paths below are relative to the repo root.

## Prerequisites

- **`gh` authenticated** to `github.com/Protoser/buttonbox` (`gh auth status`).
- **Python 3** with PyInstaller + the companion's deps. One-time:
  ```powershell
  py -m pip install --upgrade pyinstaller PySide6 pyserial psutil pythonnet
  ```
- **LibreHardwareMonitor `*.dll` files in `host/`** — already committed there.
  Without them the `.exe` still builds but loses CPU-temp/GPU telemetry.
- **PlatformIO** — not on PATH here; the embedded exe lives at
  `%USERPROFILE%\.platformio\penv\Scripts\pio.exe` (the script finds it automatically).

## Release first, then run this

The release is cut from a **committed, already-pushed `main`**. Do that first:

```powershell
git add -A
git commit -m "<your message>"
git push origin main          # see Gotchas: the harness blocks this for the agent
```

## Run (agent path) — the driver

**1. Dry-run to validate** — reuses existing artifacts, prints the tag/publish
commands, and touches nothing on the remote (a notes stub is generated since none
is passed yet):

```powershell
.\.claude\skills\release-buttonbox\release.ps1 -Version 1.3 -PrevTag v1.2-windows-only -SkipBuild -DryRun
```

**2. Write the release notes** to `notes.md` — a `## What's new since v1.2` bullet
list plus an `## Install` section. Mirror the previous release; `gh release view
v1.2-windows-only` prints its format.

**3. Publish** — builds fresh, tags `v1.3-windows-only`, pushes the tag, and
creates the release with both assets attached:

```powershell
.\.claude\skills\release-buttonbox\release.ps1 -Version 1.3 -PrevTag v1.2-windows-only -NotesFile notes.md
```

| flag | meaning |
|---|---|
| `-Version 1.3` | **required**, no leading `v`. Tag becomes `v1.3-windows-only`, title `Buttonbox v1.3`. |
| `-PrevTag v1.2-windows-only` | previous tag — only used for the generated notes header. |
| `-NotesFile notes.md` | markdown release notes. If omitted, a stub is written to `%TEMP%` and the run warns. |
| `-Title "..."` | override the release title (default `Buttonbox v<Version>`). |
| `-SkipBuild` | reuse existing `host/dist/` + `.pio/` artifacts instead of rebuilding. |
| `-DryRun` | build, but print the tag/push/publish commands instead of executing them. |

## Run (manual path) — what the driver automates

If you'd rather run it by hand, these are the exact steps (all verified for v1.2):

```powershell
# 1. build the companion .exe  ->  host/dist/ButtonboxCompanion.exe
py -m PyInstaller --noconfirm host/companion.spec   # (run from host/)

# 2. build the firmware  ->  .pio/build/esp32-s3-devkitc-1/firmware.bin
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -d (git rev-parse --show-toplevel)

# 3. tag + publish with both assets
git tag -a v1.3-windows-only -m "Buttonbox v1.3"
git push origin v1.3-windows-only
gh release create v1.3-windows-only --title "Buttonbox v1.3" --notes-file notes.md `
    host/dist/ButtonboxCompanion.exe .pio/build/esp32-s3-devkitc-1/firmware.bin
gh release view v1.3-windows-only        # confirm both assets attached
```

## Gotchas

- **The harness blocks `git push origin main`.** Claude Code's auto classifier
  denies a direct push to the default branch — a terse "make a release" is not
  enough authorization. Push `main` yourself (or have the user authorize it)
  **before** running this. Pushing the *tag* and creating the release are not
  blocked, so the script does only those; it warns if `HEAD != origin/main`.
- **A firmware "rebuild" is usually a no-op.** If `src/` hasn't changed since the
  last tag, `pio run` prints `[SUCCESS]` but reuses the existing `firmware.bin`
  (old timestamp). That's correct — same source, same binary. Ship it as-is.
- **Always two assets, with these exact basenames** — `ButtonboxCompanion.exe`
  and `firmware.bin`. The install notes reference them by name; don't rename.
- **Tag convention is `vX.Y-windows-only`, annotated**, title `Buttonbox vX.Y — <theme>`.
- **Write notes as UTF-8 without a BOM.** PowerShell's `Set-Content -Encoding utf8`
  adds a BOM (PS 5.1), which makes the first heading render with a stray char; the
  script uses `[IO.File]::WriteAllText` to avoid it.

## Troubleshooting

- **`git push origin main` says "Everything up-to-date"** when you expected to
  push: `main` was already pushed (e.g. from another terminal). Confirm with
  `git rev-parse HEAD origin/main` — equal SHAs means you're good to tag.
- **`Tag vX.Y-windows-only already exists`**: bump `-Version`, or delete the tag
  (`git tag -d <tag>; git push origin :refs/tags/<tag>`) and re-run.
- **Missing release asset: …**: the build failed, or `-SkipBuild` was passed with
  no prior build. Re-run without `-SkipBuild`.
- **`gh` errors with auth**: `gh auth login`, then retry just the publish step.
