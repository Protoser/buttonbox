# -*- mode: python ; coding: utf-8 -*-
# Build:  py -m PyInstaller --noconfirm companion.spec   ->  dist\ButtonboxCompanion.exe
#
# Bundles every *.dll sitting next to this spec (the LibreHardwareMonitor DLLs)
# into the one-file exe; sensors.py finds them at runtime via sys._MEIPASS.
import glob
import os

here = os.path.abspath(os.getcwd())
dll_datas = [(f, ".") for f in glob.glob(os.path.join(here, "*.dll"))]

a = Analysis(
    ["app.py"],
    pathex=[here],
    binaries=[],
    datas=dll_datas,
    hiddenimports=["psutil", "serial.tools.list_ports"],
    hookspath=[],
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="ButtonboxCompanion",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    runtime_tmpdir=None,
    console=False,          # windowed: no console window
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
