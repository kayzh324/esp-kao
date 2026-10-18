#!/usr/bin/env python3
"""Send CLI commands to the ESP-Claw device console over serial.

The device runs an esp_console REPL (prompt "app> "). This script opens the port,
drains the banner, sends each command, and prints the captured output.

Usage:
    serial_cmd.py [--port DEV] [--baud N] [--wait S] "cap list" "cap call get_system_info {}"

Env:
    CLAW_SERIAL_PORT   default: /dev/cu.usbserial-2120
    CLAW_SERIAL_BAUD   default: 115200

Note:
    * Requires pyserial. The ESP-IDF venv python has it:
        ~/.espressif/tools/python/v5.5.5/venv/bin/python serial_cmd.py "cap list"
    * Do NOT run `idf.py monitor` at the same time; it holds the port.
    * `cap call` output is capped at ~1024 B (CAP_OUTPUT_BUF_SIZE). For large
      content use the HTTP API instead (see dev_fetch.py).
"""
import argparse
import os
import sys
import time

import serial  # pyserial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=os.environ.get("CLAW_SERIAL_PORT", "/dev/cu.usbserial-2120"))
    ap.add_argument("--baud", type=int, default=int(os.environ.get("CLAW_SERIAL_BAUD", "115200")))
    ap.add_argument("--wait", type=float, default=3.0, help="seconds to wait after each command")
    ap.add_argument("cmds", nargs="+", help="CLI command(s), e.g. 'cap list'")
    args = ap.parse_args()

    try:
        s = serial.Serial(args.port, args.baud, timeout=1)
    except Exception as e:  # noqa: BLE001
        print(f"open {args.port} failed: {e}", file=sys.stderr)
        return 1

    time.sleep(0.6)
    s.reset_input_buffer()
    s.write(b"\r\n")
    time.sleep(0.6)
    s.read(65536)

    out = []
    for c in args.cmds:
        s.write(c.encode() + b"\r\n")
        time.sleep(args.wait)
        end = time.time() + 2.0
        buf = b""
        while time.time() < end:
            data = s.read(8192)
            if data:
                buf += data
                end = time.time() + 1.2
        out.append("$ " + c + "\n" + buf.decode("utf-8", "replace"))
    s.close()
    sys.stdout.write("\n".join(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
