"""The test build's HOME Menu icon and banner (packaging/icon_test.png, banner_test.png).

Test builds (OPENMW3DS_DEBUG) install as their own title, "OpenMW Test", next to the player's: its icon and banner
are the release ones marked TEST in orange, so the two can't be mixed up on the HOME Menu.

    python 3ds/tools/make_test_assets.py
"""
import os

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PACKAGING = os.path.join(HERE, "..", "packaging")
FONT = os.path.join(HERE, "..", "..", "files", "data", "fonts", "DejaVuLGCSansMono.ttf")
ORANGE = (235, 120, 20, 255)
WHITE = (255, 255, 255, 255)


def centred_text(draw, box, text, font, fill):
    left, top, right, bottom = box
    x0, y0, x1, y1 = draw.textbbox((0, 0), text, font=font)
    draw.text((left + (right - left - (x1 - x0)) / 2 - x0, top + (bottom - top - (y1 - y0)) / 2 - y0), text,
              font=font, fill=fill)


def icon():
    im = Image.open(os.path.join(PACKAGING, "icon.png")).convert("RGBA")
    draw = ImageDraw.Draw(im)
    band = (0, 34, 48, 48)
    draw.rectangle(band, fill=ORANGE)
    centred_text(draw, band, "TEST", ImageFont.truetype(FONT, 12), WHITE)
    im.convert("RGB").save(os.path.join(PACKAGING, "icon_test.png"))


def banner():
    im = Image.open(os.path.join(PACKAGING, "banner.png")).convert("RGBA")
    # The emblem moved left, the label on the right.
    out = Image.new("RGBA", im.size, (0, 0, 0, 0))
    out.alpha_composite(im, (-60, 0))
    draw = ImageDraw.Draw(out)
    label = (150, 38, 250, 90)
    draw.rounded_rectangle(label, radius=8, fill=ORANGE)
    font = ImageFont.truetype(FONT, 20)
    centred_text(draw, (label[0], label[1], label[2], (label[1] + label[3]) // 2 + 2), "TEST", font, WHITE)
    centred_text(draw, (label[0], (label[1] + label[3]) // 2 - 2, label[2], label[3]), "BUILD", font, WHITE)
    out.save(os.path.join(PACKAGING, "banner_test.png"))


if __name__ == "__main__":
    icon()
    banner()
    print("packaging/icon_test.png, packaging/banner_test.png")
