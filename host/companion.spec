# -*- mode: python ; coding: utf-8 -*-
# Build:  py -m PyInstaller --noconfirm companion.spec   ->  dist\ButtonboxCompanion.exe
#
# Bundles every *.dll sitting next to this spec (the LibreHardwareMonitor DLLs)
# into the one-file exe; sensors.py finds them at runtime via sys._MEIPASS.
import glob
import os

here = os.path.abspath(os.getcwd())
dll_datas = [(f, ".") for f in glob.glob(os.path.join(here, "*.dll"))]

# WinRT (Music page) ships compiled .pyd submodules — pull in whichever backend
# is installed at build time (the modern winrt-* packages or the legacy winsdk);
# skip cleanly if neither is (Music degrades to "Nothing playing").
w_datas, w_bins, w_hidden = [], [], []
try:
    from PyInstaller.utils.hooks import collect_all
    for _pkg in ("winrt", "winsdk"):
        try:
            _d, _b, _h = collect_all(_pkg)
            w_datas += _d; w_bins += _b; w_hidden += _h
        except Exception:
            pass
except Exception:
    pass

a = Analysis(
    ["app.py"],
    pathex=[here],
    binaries=w_bins,
    datas=dll_datas + w_datas,
    hiddenimports=["psutil", "serial.tools.list_ports"] + w_hidden,
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
