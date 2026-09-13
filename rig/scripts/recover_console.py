#!/usr/bin/env python3
"""Recover the M5Stick console after a transport wedge.

The Hades2001 USB-serial bridge drops TX/RX chunks under dense console
traffic; the serialport crate's RTS toggle does not reach the bridge, and
open/close hammering makes it worse. This script is the proven path in
one place: pulse the boot lines (hardware reset), wait for the banner,
then require a healthy `status` (wifi connected) before exiting 0.

    python3 rig/scripts/recover_console.py [timeout_secs=120]
"""

import sys
import time

import serial

PORT = "/dev/serial/by-id/usb-Hades2001_M5stack_49D6163EBE-if00-port0"


def main() -> int:
    deadline = time.monotonic() + float(sys.argv[1] if len(sys.argv) > 1 else 120)
    while time.monotonic() < deadline:
        try:
            s = serial.Serial(PORT, 115200, timeout=1)
        except serial.SerialException:
            time.sleep(2)
            continue
        try:
            # Hardware reset via the boot lines (AGENTS gotcha).
            s.dtr = False
            s.rts = True
            time.sleep(0.15)
            s.rts = False

            # One open handle for the whole wait; banner + status probe.
            boot_deadline = time.monotonic() + 45
            saw_banner = False
            while time.monotonic() < boot_deadline:
                data = s.read(2048)
                if b"nucula>" in data:
                    saw_banner = True
                    break
            if not saw_banner:
                continue

            for _ in range(15):  # wifi join window
                s.write(b"\r\nstatus\r\n")
                time.sleep(1.5)
                out = s.read(8192).decode(errors="replace")
                if "connected" in out and "nucula>" in out:
                    print("recovered: console up, wifi connected")
                    return 0
        finally:
            s.close()
    print("recovery failed", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
