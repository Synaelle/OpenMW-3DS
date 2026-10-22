"""Drives a real 3DS through Luma3DS's Rosalina input redirection (UDP port 4950).

Rosalina menu > "InputRedirection" must be enabled on the console (once per boot, with Wi-Fi on). Whatever the
client sends replaces the console's own buttons, circle pad, C-stick and touch screen until it sends a neutral
packet: every command here ends with neutral packets, so the console's own controls come back.

    python 3ds/tools/input_redirect.py press a                  # tap A (about 100 ms)
    python 3ds/tools/input_redirect.py press start --ms 300
    python 3ds/tools/input_redirect.py press zr+a               # several at once
    python 3ds/tools/input_redirect.py stick circle 0 1 --ms 1000   # circle pad up for a second (x, y in -1..1)
    python 3ds/tools/input_redirect.py stick c 1 0 --ms 500         # C-stick right
    python 3ds/tools/input_redirect.py touch 160 120            # tap the bottom screen (x 0-319, y 0-239)
    python 3ds/tools/input_redirect.py release                  # neutral: give the controls back
    python 3ds/tools/input_redirect.py home                     # HOME (asks for --yes: it leaves the game)

The console's address: --host, or Baguette's baguette.ini (host=) next to this repository's .tools.
The packet (20 bytes, little-endian), from Luma3DS's rosalina/source/input_redirection.c and its hooks:
  0  HID buttons, active low (0xFFF: none, and the console's own buttons pass through)
  4  touch screen: 1<<24 | y*0xFFF/240 << 12 | x*0xFFF/320 (0x2000000: none, the console's own touch)
  8  circle pad: y << 12 | x, 0x800 the center, +-0x5D0 the edges (0x7FF7FF: the console's own)
  12 C-stick and ZL/ZR: y << 24 | x << 16 | buttons << 8 | 0x81, the stick turned 45 degrees (0x80800081: none)
  16 HOME (bit 0), POWER (bit 1), POWER held (bit 2)
"""
import argparse
import configparser
import math
import os
import socket
import struct
import sys
import time

PORT = 4950
HID_BITS = {"a": 0, "b": 1, "select": 2, "start": 3, "right": 4, "left": 5, "up": 6, "down": 7, "r": 8, "l": 9,
            "x": 10, "y": 11}
IR_BITS = {"zr": 1, "zl": 2}
NEUTRAL = (0xFFF, 0x2000000, 0x7FF7FF, 0x80800081, 0)
FRAME = 1 / 60  # Rosalina keeps the last packet, resending at the frame rate covers lost ones (UDP)


def default_host():
    here = os.path.dirname(os.path.abspath(__file__))
    for ini in (os.path.join(here, "..", ".tools", "baguette", "baguette.ini"),
                os.path.join(here, "..", "..", "..", "openmw3ds", ".tools", "baguette", "baguette.ini")):
        if os.path.exists(ini):
            config = configparser.ConfigParser()
            config.read(ini)
            if config.has_option("server", "host"):
                return config.get("server", "host")
    return None


def packet(hid=0xFFF, touch=0x2000000, circle=0x7FF7FF, cpp=0x80800081, special=0):
    return struct.pack("<5I", hid, touch, circle, cpp, special)


def buttons(names):
    hid, ir = 0xFFF, 0
    for name in names.lower().split("+"):
        if name in HID_BITS:
            hid &= ~(1 << HID_BITS[name])
        elif name in IR_BITS:
            ir |= 1 << IR_BITS[name]
        else:
            sys.exit(f"unknown button: {name} (a b x y l r zl zr start select up down left right)")
    return hid, ir


def circle_value(x, y):
    def axis(v):
        return max(0, min(0xFFF, int(0x800 + v * 0x5D0)))
    return axis(y) << 12 | axis(x)


def cpp_value(x, y, ir=0):
    if x == 0 and y == 0 and ir == 0:
        return 0x80800081
    def axis(v):
        return max(0, min(0xFF, int(0x80 + v * 0x7F)))
    rx, ry = math.sqrt(0.5) * (x + y), math.sqrt(0.5) * (y - x)  # the C-stick reads 45 degrees off
    return axis(ry) << 24 | axis(rx) << 16 | ir << 8 | 0x81


def touch_value(x, y):
    x = max(0, min(319, x))
    y = max(0, min(239, y))
    return 1 << 24 | (0xFFF * y // 240) << 12 | (0xFFF * x // 320)


def send_for(sock, address, data, ms):
    end = time.monotonic() + ms / 1000
    while True:
        sock.sendto(data, address)
        if time.monotonic() >= end:
            break
        time.sleep(FRAME)


def release(sock, address):
    for _ in range(6):  # several: a lost neutral packet would leave the input held
        sock.sendto(packet(*NEUTRAL), address)
        time.sleep(FRAME)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=None, help="the console's IP (default: Baguette's)")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("press")
    p.add_argument("buttons")
    p.add_argument("--ms", type=int, default=100)
    s = sub.add_parser("stick")
    s.add_argument("which", choices=["circle", "c"])
    s.add_argument("x", type=float)
    s.add_argument("y", type=float)
    s.add_argument("--ms", type=int, default=300)
    t = sub.add_parser("touch")
    t.add_argument("x", type=int)
    t.add_argument("y", type=int)
    t.add_argument("--ms", type=int, default=100)
    sub.add_parser("release")
    h = sub.add_parser("home")
    h.add_argument("--yes", action="store_true", help="really press HOME (leaves the running game)")
    args = parser.parse_args()

    host = args.host or default_host()
    if not host:
        sys.exit("no console address: pass --host")
    address = (host, PORT)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        if args.command == "press":
            hid, ir = buttons(args.buttons)
            send_for(sock, address, packet(hid=hid, cpp=cpp_value(0, 0, ir)), args.ms)
        elif args.command == "stick":
            if args.which == "circle":
                data = packet(circle=circle_value(args.x, args.y))
            else:
                data = packet(cpp=cpp_value(args.x, args.y))
            send_for(sock, address, data, args.ms)
        elif args.command == "touch":
            send_for(sock, address, packet(touch=touch_value(args.x, args.y)), args.ms)
        elif args.command == "home":
            if not args.yes:
                sys.exit("HOME leaves the running game: add --yes")
            send_for(sock, address, packet(special=1), 150)
        # POWER is left out on purpose: it opens the power menu or puts the console to sleep.
    finally:
        release(sock, address)
        sock.close()
    print(f"{args.command}: sent to {host}:{PORT}")


if __name__ == "__main__":
    main()
