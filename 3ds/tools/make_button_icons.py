#!/usr/bin/env python3
"""Draws the 3DS button glyphs of the interface's button hints (M17) into packaging/ui3ds/textures/ (committed; run
again only to change them):

    python tools/make_button_icons.py

White shapes on transparency, the letters cut out, like OpenMW's own controller glyphs: the hint layout tints them
(openmw_controllerbuttons.layout). Drawn here from plain shapes and the DejaVu font OpenMW ships (Bitstream Vera
licence: free to use, glyph images included), so nothing of anyone else's button art is in them.

Square glyphs are 16x16 and START and SELECT, wider, 32x16: the size they are shown at (the CPU interface draws
textures pixel for pixel, a scaled one would lose its anti-aliasing).
"""

import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "packaging", "ui3ds", "textures")
FONT = os.path.join(ROOT, "openmw", "files", "data", "fonts", "DejaVuLGCSansMono.ttf")
SS = 8  # supersampling
SCALE = 2  # drawn on a 32-pixel grid, saved at half: the interface shows them pixel for pixel, unfiltered


def canvas(w, h):
    img = Image.new("L", (w * SS, h * SS), 0)
    return img, ImageDraw.Draw(img)


def cut_text(d, box, text, size):
    """Cuts `text` out of what is drawn, centred in box (supersampled coordinates)."""
    font = ImageFont.truetype(FONT, size * SS)
    l, t, r, b = d.textbbox((0, 0), text, font=font)
    x = (box[0] + box[2]) / 2 - (l + r) / 2
    y = (box[1] + box[3]) / 2 - (t + b) / 2
    d.text((x, y), text, font=font, fill=0)


def save(img, name, w, h):
    w, h = w // SCALE, h // SCALE
    alpha = img.resize((w, h), Image.LANCZOS)
    out = Image.new("RGBA", (w, h), (255, 255, 255, 0))
    out.putalpha(alpha)
    out.save(os.path.join(OUT, f"omw_3ds_button_{name}.png"))


def face(letter):
    img, d = canvas(32, 32)
    d.ellipse((1 * SS, 1 * SS, 31 * SS, 31 * SS), fill=255)
    cut_text(d, (0, 0, 32 * SS, 32 * SS), letter, 21)
    save(img, letter.lower(), 32, 32)


def shoulder(name, text):
    img, d = canvas(32, 32)
    d.rounded_rectangle((1 * SS, 6 * SS, 31 * SS, 26 * SS), radius=7 * SS, fill=255)
    cut_text(d, (0, 6 * SS, 32 * SS, 26 * SS), text, 15 if len(text) > 1 else 17)
    save(img, name, 32, 32)


def pill(name, text):
    img, d = canvas(64, 32)
    d.rounded_rectangle((1 * SS, 5 * SS, 63 * SS, 27 * SS), radius=11 * SS, fill=255)
    cut_text(d, (0, 5 * SS, 64 * SS, 27 * SS), text, 14)
    save(img, name, 64, 32)


def dpad(name, lit):
    """The cross; `lit` arms (u, d, l, r) solid, the others outlined."""
    img, d = canvas(32, 32)
    arms = {"u": (11, 1, 21, 12), "d": (11, 20, 21, 31), "l": (1, 11, 12, 21), "r": (20, 11, 31, 21)}
    d.rectangle((11 * SS, 11 * SS, 21 * SS, 21 * SS), fill=255)
    for key, (x0, y0, x1, y1) in arms.items():
        box = (x0 * SS, y0 * SS, x1 * SS, y1 * SS)
        d.rectangle(box, fill=255)
        if key not in lit:
            d.rectangle((box[0] + 3 * SS, box[1] + 3 * SS, box[2] - 3 * SS, box[3] - 3 * SS), fill=0)
    save(img, name, 32, 32)


def circle_pad():
    img, d = canvas(32, 32)
    d.ellipse((1 * SS, 1 * SS, 31 * SS, 31 * SS), fill=255)
    d.ellipse((4 * SS, 4 * SS, 28 * SS, 28 * SS), fill=0)
    d.ellipse((8 * SS, 8 * SS, 24 * SS, 24 * SS), fill=255)
    save(img, "circlepad", 32, 32)


def c_stick():
    img, d = canvas(32, 32)
    d.ellipse((5 * SS, 5 * SS, 27 * SS, 27 * SS), fill=255)
    cut_text(d, (5 * SS, 5 * SS, 27 * SS, 27 * SS), "C", 16)
    save(img, "cstick", 32, 32)


def main():
    os.makedirs(OUT, exist_ok=True)
    for letter in "ABXY":
        face(letter)
    shoulder("l", "L")
    shoulder("r", "R")
    shoulder("zl", "ZL")
    shoulder("zr", "ZR")
    pill("start", "START")
    pill("select", "SELECT")
    dpad("dpad", "udlr")
    dpad("dpad_ud", "ud")
    dpad("dpad_lr", "lr")
    circle_pad()
    c_stick()
    print("wrote", OUT)


if __name__ == "__main__":
    main()
