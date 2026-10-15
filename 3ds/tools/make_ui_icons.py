#!/usr/bin/env python3
"""Draws the 3DS interface's own icons (packaging/ui3ds/textures/), pixel by pixel at their display size.

    python tools/make_ui_icons.py

Outputs (committed; read at runtime as PNG, the 3DS build has OSG's PNG reader):
  packaging/ui3ds/textures/omw_3ds_home.png  house (12 high on 16x16): the tab bar's Home button, back to the HUD

The icons are white with a dark outline: the code tints them with the interface's font colours (sand, blue when
selected), as MW_Button tints its caption, so they follow the game's colours.
"""

import os

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "packaging", "ui3ds", "textures")

# W: fill (brightest at the top, like Morrowind's embossed lettering), K: outline, .: transparent. 12 pixels high,
# as the captions' letters inside a tab's frame (21 pixels, 13 inside); drawn on a 16x16 texture (a power of two).
HOME = [
    ".....KK.....",
    "....KWWK....",
    "...KWWWWK...",
    "..KWWWWWWK..",
    ".KWWWWWWWWK.",
    "KWWWWWWWWWWK",
    "KKKWWWWWWKKK",
    "..KWWKKWWK..",
    "..KWWKKWWK..",
    "..KWWKKWWK..",
    "..KKKKKKKK..",
]
TEXTURE = 16


def draw(rows):
    width, height = len(rows[0]), len(rows)
    image = Image.new("RGBA", (TEXTURE, TEXTURE), (0, 0, 0, 0))
    left, top = (TEXTURE - width) // 2, (TEXTURE - height) // 2
    for y, row in enumerate(rows):
        assert len(row) == width, (y, row)
        shade = round(255 - 50 * y / (height - 1))
        for x, cell in enumerate(row):
            if cell == "W":
                image.putpixel((left + x, top + y), (shade, shade, shade, 255))
            elif cell == "K":
                image.putpixel((left + x, top + y), (16, 12, 8, 230))
    return image


def main():
    os.makedirs(OUT, exist_ok=True)
    draw(HOME).save(os.path.join(OUT, "omw_3ds_home.png"))
    print("written:", os.path.join(OUT, "omw_3ds_home.png"))


if __name__ == "__main__":
    main()
