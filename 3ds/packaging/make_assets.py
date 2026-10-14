#!/usr/bin/env python3
"""Builds the HOME Menu assets of the 3DS builds (.cia and .3dsx) from packaging/source/.

    python packaging/make_assets.py

Inputs (packaging/source/):
  morrowind_emblem.png  the moon-and-star emblem, 256x256 with transparency (supplied by the tester)
  swoosh_1.wav          sound played when the title is selected
Outputs (committed, used by cmake/Ctr3dsPackaging.cmake):
  packaging/icon.png    48x48 HOME Menu icon (SMDH); bannertool derives the 24x24 small icon from it
  packaging/banner.png  256x128 banner image (flat 2D banner made by bannertool)
  packaging/banner.wav  the sound as 16-bit stereo (banner sounds must be stereo, at most 3 s)

SMDH icons have no transparency, so the emblem sits on a dark warm background. The banner is a flat image on
purpose: a 3D CGFX banner (a turning Cliff Racer, converted with pycgfx) worked in the emulator but crashed the
HOME Menu on hardware (test #11).
"""

import os
import struct
import sys
import wave

from PIL import Image, ImageDraw

PACKAGING = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(PACKAGING, "source")
EMBLEM = os.path.join(SOURCE, "morrowind_emblem.png")
SWOOSH = os.path.join(SOURCE, "swoosh_1.wav")

BACKGROUND_TOP = (46, 37, 28)
BACKGROUND_BOTTOM = (16, 12, 9)


def background(width, height):
    """Vertical dark brown gradient with a soft vignette."""
    image = Image.new("RGB", (width, height))
    pixels = image.load()
    cx, cy = (width - 1) / 2, (height - 1) / 2
    radius = (cx * cx + cy * cy) ** 0.5
    for y in range(height):
        t = y / max(height - 1, 1)
        base = [BACKGROUND_TOP[k] * (1 - t) + BACKGROUND_BOTTOM[k] * t for k in range(3)]
        for x in range(width):
            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 / radius
            shade = 1.0 - 0.45 * d * d
            pixels[x, y] = tuple(int(c * shade) for c in base)
    return image


def emblem(size):
    """The emblem cropped to its visible pixels, scaled so its longer side is `size` (alpha-correct)."""
    image = Image.open(EMBLEM).convert("RGBA")
    image = image.crop(image.getchannel("A").getbbox())
    scale = size / max(image.size)
    target = (max(1, round(image.width * scale)), max(1, round(image.height * scale)))
    # Resize premultiplied so transparent pixels don't bleed dark fringes into the edges.
    premultiplied = image.convert("RGBa").resize(target, Image.Resampling.LANCZOS)
    return premultiplied.convert("RGBA")


def compose(width, height, emblem_size):
    image = background(width, height)
    mark = emblem(emblem_size)
    image.paste(mark, ((width - mark.width) // 2, (height - mark.height) // 2), mark)
    return image


def convert_audio():
    with wave.open(SWOOSH) as src:
        channels, width, rate, frames = src.getnchannels(), src.getsampwidth(), src.getframerate(), src.getnframes()
        data = src.readframes(frames)
    if width != 2:
        sys.exit("swoosh: expected 16-bit PCM")
    if frames / rate > 3.0:
        sys.exit("swoosh: banner sounds are limited to 3 s")
    if channels == 1:
        samples = struct.unpack(f"<{frames}h", data)
        data = b"".join(struct.pack("<hh", s, s) for s in samples)
    with wave.open(os.path.join(PACKAGING, "banner.wav"), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(data)
    print(f"banner.wav: {frames / rate:.2f} s, stereo, {rate} Hz")


def main():
    compose(48, 48, 44).save(os.path.join(PACKAGING, "icon.png"))
    print("icon.png: 48x48")
    compose(256, 128, 116).save(os.path.join(PACKAGING, "banner.png"))
    print("banner.png: 256x128")
    convert_audio()


if __name__ == "__main__":
    main()
