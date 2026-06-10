@echo off
REM Build the standalone companion .exe.
REM Put the LibreHardwareMonitor *.dll files in this folder first (for temps).
py -m pip install --upgrade pyinstaller PySide6 pyserial psutil pythonnet
py -m PyInstaller --noconfirm companion.spec
echo.
echo Done. Run: dist\ButtonboxCompanion.exe
