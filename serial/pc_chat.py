#!/usr/bin/env python3
"""
Tiny two-way chat with your phone through the Flipper's "USB BT Serial" app.

    pip install pyserial
    python pc_chat.py            # finds the Flipper automatically
    python pc_chat.py COM5       # or name the port yourself

Type a line and press Enter to send it to the phone. Whatever the phone
sends is printed as it arrives. Ctrl+C quits.

You can also pipe things in, e.g. send a file or a command's output:
    python pc_chat.py < notes.txt
    ping -t 8.8.8.8 | python pc_chat.py      (Windows; use `ping` on Mac/Linux)
"""
import sys
import threading

import serial
from serial.tools import list_ports

FLIPPER_VID, FLIPPER_PID = 0x0483, 0x5740


def find_port():
    for p in list_ports.comports():
        if p.vid == FLIPPER_VID and p.pid == FLIPPER_PID:
            return p.device
    sys.exit("No Flipper found. Is it plugged in with the USB BT Serial app open?")


def reader(ser):
    while True:
        try:
            data = ser.read(ser.in_waiting or 1)
        except serial.SerialException:
            print("\n[port closed]")
            return
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    # Baud rate is ignored (it's a virtual port). DTR on tells the Flipper
    # a program is listening, so it forwards what the phone sends.
    ser = serial.Serial(port, 115200, timeout=0.2)
    ser.dtr = True
    print(f"[connected to {port}; type and press Enter, Ctrl+C to quit]")
    threading.Thread(target=reader, args=(ser,), daemon=True).start()
    try:
        for line in sys.stdin:
            ser.write(line.replace("\r\n", "\n").encode("utf-8"))
        if not sys.stdin.isatty():
            # piped input finished: keep listening for replies
            threading.Event().wait()
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()


if __name__ == "__main__":
    main()
