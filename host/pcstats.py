#!/usr/bin/env python3
"""Headless CLI fallback: stream PC stats to the buttonbox, no GUI.

The full companion is `app.py` (system tray, device settings, chords). This is a
minimal sender that shares the same sensor code and also auto-reconnects.

    python pcstats.py                 # auto-detect the box's COM port
    python pcstats.py --port COM15 --interval 0.5
"""
import argparse
import sys
import time

import serial
import serial.tools.list_ports as list_ports

import sensors

VID_ESP32S3 = 0x303A


def find_port():
    return next((p.device for p in list_ports.comports() if (p.vid or 0) == VID_ESP32S3), None)


def main():
    ap = argparse.ArgumentParser(description="Stream PC stats to the buttonbox (CLI).")
    ap.add_argument("--port", help="serial port (default: auto-detect)")
    ap.add_argument("--interval", type=float, default=0.5)
    args = ap.parse_args()

    ok, err = sensors.init()
    if not ok:
        print(f"[info] LibreHardwareMonitor not active ({err}); CPU temp/GPU limited.")
    print("[ok] streaming; Ctrl-C to stop.")

    ser = None
    while True:
        try:
            if ser is None:
                port = args.port or find_port()
                if not port:
                    time.sleep(1.0)
                    continue
                ser = serial.Serial(port, 115200, timeout=0)
                print(f"[ok] connected {port}")
            ser.write((sensors.encode(sensors.read_stats()) + "\n").encode())
            time.sleep(args.interval)
        except (serial.SerialException, OSError):
            try:
                ser.close()
            except Exception:        # noqa: BLE001
                pass
            ser = None
            print("[info] disconnected; searching…")
            time.sleep(1.0)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
