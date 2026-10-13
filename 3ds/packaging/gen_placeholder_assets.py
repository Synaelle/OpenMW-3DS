"""Generates placeholder icon/banner PNGs and a silent banner WAV (no third-party deps).

Run once; outputs are committed. Replace with real artwork whenever.
    python packaging/gen_placeholder_assets.py
"""
import os
import struct
import wave
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def write_png(path, w, h, pixel):
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter: none
        for x in range(w):
            raw.extend(pixel(x, y))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def emblem(x, y, w, h):
    # Dark stone background with a golden diamond: vaguely Morrowind-ish, clearly a placeholder.
    cx, cy = w / 2, h / 2
    d = abs(x - cx) / (w * 0.32) + abs(y - cy) / (h * 0.42)
    if d < 0.85:
        return (205, 160, 70, 255)
    if d < 1.0:
        return (120, 85, 35, 255)
    shade = 40 + ((x // 4 + y // 4) % 2) * 6
    return (shade, shade - 6, shade - 12, 255)


def main():
    write_png(os.path.join(HERE, "icon.png"), 48, 48, lambda x, y: emblem(x, y, 48, 48))
    write_png(os.path.join(HERE, "banner.png"), 256, 128, lambda x, y: emblem(x, y, 256, 128))
    with wave.open(os.path.join(HERE, "banner.wav"), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(22050)
        wav.writeframes(b"\x00\x00" * 22050)  # 1 s of silence


if __name__ == "__main__":
    main()
