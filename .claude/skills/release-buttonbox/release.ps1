<#
.SYNOPSIS
  Cut a Buttonbox GitHub release: build the companion .exe + firmware.bin,
  tag vX.Y-windows-only, and publish a gh release with both assets attached.

.DESCRIPTION
  Automates the mechanical half of a release. The judgement half -- version
  number and the "what's new" notes -- are yours (pass -Version and -NotesFile).

  Windows only: the companion .exe is built with PyInstaller and needs the
  LibreHardwareMonitor *.dll files in host/. Run from anywhere in the repo.

  IMPORTANT: this script does NOT push main. The release must be cut from a
  committed, already-pushed main. The Claude Code harness blocks direct pushes
  to main, so push/authorize that step yourself first (see SKILL.md).

.EXAMPLE
  # Dry run first -- prints the tag/publish commands without touching the remote:
  .\release.ps1 -Version 1.3 -PrevTag v1.2-windows-only -NotesFile notes.md -DryRun

.EXAMPLE
  # For real:
  .\release.ps1 -Version 1.3 -PrevTag v1.2-windows-only -NotesFile notes.md
#>
param(
    [Parameter(Mandatory = $true)][string]$Version,   # e.g. 1.3  (no leading v)
    [string]$PrevTag,                                  # e.g. v1.2-windows-only (for the notes header)
    [string]$Title,                                    # default: "Buttonbox v<Version>"
    [string]$NotesFile,                                # markdown release notes; a stub is generated if omitted
    [switch]$SkipBuild,                                # reuse existing dist/ and .pio/ artifacts
    [switch]$DryRun                                    # build, but print tag/publish commands instead of running them
)
$ErrorActionPreference = "Stop"
function Assert-LastExit($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)." } }

# --- locations -----------------------------------------------------------------
$root = (git rev-parse --show-toplevel); Assert-LastExit "git rev-parse"
$root = $root.Trim()
$hostDir = Join-Path $root "host"
$exe     = Join-Path $hostDir "dist\ButtonboxCompanion.exe"
$fw      = Join-Path $root ".pio\build\esp32-s3-devkitc-1\firmware.bin"
$tag     = "v$Version-windows-only"
if (-not $Title) { $Title = "Buttonbox v$Version" }

# pio is not on PATH on this machine; fall back to the per-user embedded exe.
$pio = (Get-Command pio -ErrorAction SilentlyContinue).Source
if (-not $pio) { $pio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe" }
if (-not (Test-Path $pio)) { throw "pio not found (not on PATH, and no $pio). Install PlatformIO." }

Write-Host "Release $tag  --  $Title" -ForegroundColor Green

# --- preflight -----------------------------------------------------------------
if (git status --porcelain) {
    Write-Warning "Working tree is not clean. A release should be cut from a committed, pushed state."
}
$head = (git rev-parse HEAD).Trim()
$originMain = (git rev-parse origin/main); if ($LASTEXITCODE -eq 0) { $originMain = $originMain.Trim() } else { $originMain = "" }
if ($head -ne $originMain) {
    Write-Warning "HEAD ($($head.Substring(0,7))) does not match origin/main. Push main FIRST -- the harness blocks direct pushes, so authorize/run that yourself. Building and tagging anyway."
}

# --- build artifacts -----------------------------------------------------------
if (-not $SkipBuild) {
    Write-Host "==> Building companion .exe (PyInstaller)..." -ForegroundColor Cyan
    Push-Location $hostDir
    try { py -m PyInstaller --noconfirm companion.spec; Assert-LastExit "PyInstaller" } finally { Pop-Location }

    Write-Host "==> Building firmware.bin (PlatformIO)..." -ForegroundColor Cyan
    & $pio run -d $root; Assert-LastExit "pio run"
}
foreach ($a in @($exe, $fw)) {
    if (-not (Test-Path $a)) { throw "Missing release asset: $a  (build failed, or -SkipBuild with no prior build)." }
}
Write-Host "Assets ready:`n  $exe`n  $fw" -ForegroundColor Green

# --- notes ---------------------------------------------------------------------
$tmpNotes = $false
if (-not $NotesFile) {
    $NotesFile = Join-Path ([System.IO.Path]::GetTempPath()) "bb-$tag-notes.md"
    $prevHeader = if ($PrevTag) { "## What's new since $PrevTag" } else { "## What's new" }
    $stub = @"
$prevHeader

- <fill in the highlights of this release>

## Install

- **Firmware:** flash **firmware.bin** to the ESP32-S3 (PlatformIO, or the on-device Flash Mode + esptool).
- **Companion (Windows only):** run **ButtonboxCompanion.exe**.
"@
    [System.IO.File]::WriteAllText($NotesFile, $stub)   # UTF-8 no BOM
    $tmpNotes = $true
    Write-Warning "No -NotesFile given. Wrote a stub to $NotesFile -- edit it before a real publish."
}
elseif (-not (Test-Path $NotesFile)) { throw "NotesFile not found: $NotesFile" }

# --- tag + publish -------------------------------------------------------------
if ($DryRun) {
    Write-Host "`n--- DRY RUN: would run ---" -ForegroundColor Yellow
    Write-Host "git tag -a $tag -m `"$Title`""
    Write-Host "git push origin $tag"
    Write-Host "gh release create $tag --title `"$Title`" --notes-file `"$NotesFile`" `"$exe`" `"$fw`""
}
else {
    if (git tag --list $tag) { throw "Tag $tag already exists. Bump -Version or delete the tag." }
    git tag -a $tag -m $Title;             Assert-LastExit "git tag"
    git push origin $tag;                  Assert-LastExit "git push tag"
    gh release create $tag --title $Title --notes-file $NotesFile $exe $fw; Assert-LastExit "gh release create"
    Write-Host "Published: " -NoNewline -ForegroundColor Green
    gh release view $tag --json url -q .url
}

if ($tmpNotes -and -not $DryRun) { Remove-Item $NotesFile -ErrorAction SilentlyContinue }
