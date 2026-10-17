#!/usr/bin/env python3
"""The interface atlas for the OpenMW 3DS port's own GPU-drawn interface (PC side, run once per data set).

Hardware tests #34-#43 (PORTING_NOTES.md, docs/AZAHAR_INACCURACIES_2.md): the New 3DS draws nearby scene meshes
wrong for a frame when the interface binds many textures per frame; one texture bound for the whole interface
pass is clean (tests #38 and #43, the latter a 512x256 texture). So the port's interface draws everything from
ONE texture, built here:

  - the interface images the skins and layouts reference (Morrowind's menu frames, bars and buttons, OpenMW's
    scroll pieces, the port's own images under packaging/ui3ds/textures), each with a 1-texel repeated-edge
    gutter so bilinear filtering never bleeds a neighbour in;
  - the glyphs of the font OpenMW shows on the console (MysticCards, resources/vfs/fonts), pre-rendered at the
    pixel heights the port uses (16, 11 and 10 by default), monochrome as OpenMW's .omwfont asks (Antialias
    false), stored as white with the glyph in the alpha channel and a 1-texel transparent gutter;
  - a reserved region for item and effect icons (ICON_REGION slots of ICON_SIZE), filled by the console at run
    time from the texture pack, while the GPU is idle.

Output: <out> (format below), a PNG preview next to it (--preview) and a JSON sidecar of the index for tools.
The Morrowind data is only read.

    python tools/cook_lumiere_atlas.py --data "D:/.../Morrowind/Data Files" --resources build/openmw/resources/vfs \
        --out out/cooked/lumiere.o3ua

Format (little endian; the console loads the whole file into linear memory and binds the texels as they are):
  header  (64 bytes): magic 'O3UA', version, width, height, image count, font count, glyph count,
          icon region x, y, w, h (texels), icon slot size, image table offset, font table offset,
          glyph table offset, texel data offset (128-byte aligned), texel data size
  image   (24 bytes): u64 fnv1a64 of the normalized key (e.g. "textures/menu_thick_border_top"),
          u16 x, y, w, h (the image's texels, inside its gutter; y down from the top of the atlas as drawn),
          u16 flags (1 = has alpha, 2 = stored transposed: the image's (u, v) is at (x + v * h, y + u * w)), u16 reserved x3
  font    (16 bytes): u16 pixel height, u16 line height, i16 ascent (baseline from the line's top), u16 glyph
          count, u32 first glyph index, u32 reserved
  glyph   (16 bytes): u32 code point, u16 x, y, w, h (texels in the atlas), i8 bearing x, i8 bearing y (from
          the baseline up to the glyph's top), u8 advance, u8 reserved
  texels: RGBA8 in the PICA's tiled layout, flipped vertically like every cooked texture (tex3ds), so a point
          at atlas texel (x, y) is sampled at (u, v) = ((x + 0.5) / width, 1 - (y + 0.5) / height).
"""

import argparse
import io
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np
from PIL import Image, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cook_textures import (  # noqa: E402
    DEFAULT_TEX3DS,
    IMAGE_EXTENSIONS,
    bleed_transparent,
    collect_sources,
    fnv1a64,
    normalize,
    stem_key,
)

MAGIC = b"O3UA"
VERSION = 1
HEADER_SIZE = 64
IMAGE_SIZE = 24
FONT_SIZE = 16
GLYPH_SIZE = 16
DATA_ALIGN = 128

ATLAS_WIDTH = 512
ATLAS_HEIGHT = 256  # hardware test #43: one 512x256 texture bound for the interface is clean
GUTTER = 1
# Images: the short side at most this (the menu frames, bars and buttons; not the pictures).
IMAGE_MAX_SHORT_SIDE = 64
# Always in, whatever the skins say (drawn by the port's own code).
EXTRA_IMAGES = ("textures/compass", "textures/target", "textures/tx_cursor", "textures/menu_bar_gray")
# Never in (not on the M8 screens, or big).
SKIP_IMAGES = ("textures/omw_steam_", "textures/omw_mouse_emulation_cursor", "textures/tx_cursormove")
FLAG_ALPHA = 1
FLAG_TRANSPOSED = 2  # stored with its axes swapped (see below); w and h in the entry are the stored ones


def gutters(texels):
    """The gutter on each axis: none along a side that spans the whole atlas (the edge clamps to it)."""
    h, w = texels.shape[:2]
    return (0 if w >= ATLAS_WIDTH else GUTTER), (0 if h >= ATLAS_HEIGHT else GUTTER)


FONT_FILE = "fonts/MysticCards.ttf"
FONT_HEIGHTS = (16, 11, 10)  # MyGUI's "real height" for the port's text, HUD values and small labels
# ASCII, Latin-1 (the Western European languages Morrowind ships in) and the punctuation OpenMW's text uses.
GLYPH_RANGES = ((32, 126), (160, 255))
GLYPH_EXTRA = "\u2013\u2014\u2018\u2019\u201c\u201d\u2026\u2022"

ICON_SIZE = 32
ICON_REGION = (5, 4)  # slots across, down: 20 icons (the Inventory tab's visible icons, the quick keys, the effects)


def referenced_textures(resources, layouts):
    """The texture names the MyGUI skins and layouts reference (normalized keys without extension)."""
    pattern = re.compile(r'texture\s*=\s*"([^"]+)"', re.IGNORECASE)
    names = set()
    for folder in [os.path.join(resources, "mygui"), layouts]:
        if not folder or not os.path.isdir(folder):
            continue
        for file in os.listdir(folder):
            if not file.endswith((".xml", ".layout", ".skin")):
                continue
            with open(os.path.join(folder, file), encoding="utf-8", errors="replace") as f:
                for match in pattern.finditer(f.read()):
                    name = normalize(match.group(1))
                    if name.endswith(IMAGE_EXTENSIONS):
                        names.add(stem_key(name))
    return names


def load_image(source):
    file, offset, size = source
    with open(file, "rb") as f:
        f.seek(offset)
        raw = f.read(size)
    image = Image.open(io.BytesIO(raw))
    image.load()
    return image.convert("RGBA")


def render_glyphs(font_path, height):
    """One font size: (line height, ascent, [(code, rgba array, bearing x, bearing y, advance)])."""
    # MyGUI's ResourceTrueTypeFont sizes the face so that its line height is the requested height; Pillow's
    # size is the em size, so search the em size whose ascent + descent gives that line height.
    def metrics(size):
        font = ImageFont.truetype(font_path, size)
        ascent, descent = font.getmetrics()
        return font, ascent, descent

    size = height
    font, ascent, descent = metrics(size)
    while ascent + descent > height and size > 4:
        size -= 1
        font, ascent, descent = metrics(size)
    glyphs = []
    codes = [c for lo, hi in GLYPH_RANGES for c in range(lo, hi + 1)] + [ord(c) for c in GLYPH_EXTRA]
    # The font's "missing glyph" box (what a code point it lacks renders as): left out, the console shows
    # OpenMW's substitute ('_', the .omwfont's SubstituteCode) for those.
    notdef = font.getmask(chr(0x0FFF), mode="1")
    notdef = (notdef.size, bytes(notdef)) if notdef.size[0] else None
    for code in codes:
        ch = chr(code)
        try:
            mask = font.getmask(ch, mode="1")  # monochrome, as the .omwfont's Antialias=false
        except OSError:
            continue
        if notdef and code != 0x0FFF and (mask.size, bytes(mask)) == notdef:
            continue
        advance = int(round(font.getlength(ch)))
        if mask.size[0] == 0 or mask.size[1] == 0:
            if advance > 0:
                glyphs.append((code, None, 0, 0, advance))
            continue
        left, top, right, bottom = font.getbbox(ch)
        w, h = right - left, bottom - top
        if w <= 0 or h <= 0:
            glyphs.append((code, None, 0, 0, advance))
            continue
        bitmap = np.array(mask, dtype=np.uint8).reshape(mask.size[1], mask.size[0])
        bitmap = bitmap[: h, : w] if bitmap.shape[0] >= h and bitmap.shape[1] >= w else bitmap
        h, w = bitmap.shape
        rgba = np.zeros((h, w, 4), dtype=np.uint8)
        rgba[..., :3] = 255
        rgba[..., 3] = np.where(bitmap > 0, 255, 0)
        # bearing y: from the baseline up to the glyph's top (Pillow's top is from the line's top: ascent - top)
        glyphs.append((code, rgba, left, ascent - top, advance))
    return ascent + descent, ascent, glyphs


def shelf_pack(cells, width, height, reserved):
    """cells: [(key, w, h)] with gutters included -> {key: (x, y)}; the reserved rectangle (x, y, w, h) is kept
    free. Rows of cells sorted by height, tallest first."""
    rx, ry, rw, rh = reserved
    # Cells too wide to sit beside the reserved region (the full-width border strips) take the top rows first.
    order = sorted(range(len(cells)),
                   key=lambda i: (0 if cells[i][1] > width - rw else 1, -cells[i][2], -cells[i][1], cells[i][0]))
    placements = {}
    x = y = row_height = 0
    for i in order:
        key, w, h = cells[i]
        if w > width or h > height:
            raise RuntimeError(f"{key}: {w}x{h} does not fit the {width}x{height} atlas")
        if x + w > width:
            x, y, row_height = 0, y + row_height, 0
        # The reserved region sits at the right edge: a cell whose rows cross it must end left of it, or start a
        # new row (a cell wider than the space left of the region can only live in the rows above it).
        if y < ry + rh and y + h > ry and x + w > rx:
            if w > rx:
                raise RuntimeError(f"{key}: {w}x{h} can't be placed beside the icon region (rows above it full)")
            x, y, row_height = 0, y + max(row_height, 1), 0
        if y + h > height:
            raise RuntimeError(f"the atlas is full at {key} ({w}x{h}); {len(placements)} of {len(cells)} placed")
        placements[key] = (x, y)
        x += w
        row_height = max(row_height, h)
    return placements, y + row_height


def main():
    started = time.time()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", required=True, help="Morrowind's Data Files folder (read only)")
    parser.add_argument("--resources", required=True, help="OpenMW's resources/vfs folder (fonts, mygui)")
    parser.add_argument("--layouts", default=os.path.join(os.path.dirname(__file__), "..", "packaging", "ui3ds", "mygui"))
    parser.add_argument("--port-textures", default=os.path.join(os.path.dirname(__file__), "..", "packaging", "ui3ds", "textures"))
    parser.add_argument("--out", required=True)
    parser.add_argument("--preview", default=None, help="PNG preview (default: next to --out)")
    parser.add_argument("--archives", default="Morrowind.bsa,Tribunal.bsa,Bloodmoon.bsa")
    parser.add_argument("--font-heights", default=",".join(str(h) for h in FONT_HEIGHTS))
    parser.add_argument("--tex3ds", default=DEFAULT_TEX3DS)
    args = parser.parse_args()
    archives = [a for a in args.archives.split(",") if os.path.exists(os.path.join(args.data, a))]
    heights = [int(h) for h in args.font_heights.split(",")]

    # ---- images
    wanted = referenced_textures(args.resources, args.layouts) | set(EXTRA_IMAGES)
    sources = collect_sources(args.data, archives, ("textures/",), resources=args.resources)
    if os.path.isdir(args.port_textures):
        for file in os.listdir(args.port_textures):
            name = normalize(os.path.join("textures", file))
            if name.endswith(IMAGE_EXTENSIONS):
                full = os.path.join(args.port_textures, file)
                key = stem_key(name)
                sources[key] = (name, (full, 0, os.path.getsize(full)))
                wanted.add(key)
    images = []  # (key, rgba array, flags)
    # A solid white square for untextured rectangles (bars' backgrounds, dimming), drawn from the same texture.
    images.append(("lumiere/white", np.full((4, 4, 4), 255, dtype=np.uint8), 0))
    missing, skipped = [], []
    for key in sorted(wanted):
        if key.startswith(SKIP_IMAGES):
            continue
        if key not in sources:
            missing.append(key)
            continue
        name, source = sources[key]
        rgba = load_image(source)
        w, h = rgba.size
        if min(w, h) > IMAGE_MAX_SHORT_SIDE:
            skipped.append(f"{key} {w}x{h}")
            continue
        alpha = rgba.getchannel("A")
        has_alpha = alpha.getextrema()[0] < 255
        if has_alpha:
            rgba = bleed_transparent(rgba, keep_alpha_zero=True)
        texels = np.array(rgba, dtype=np.uint8)
        flags = FLAG_ALPHA if has_alpha else 0
        if h > ATLAS_HEIGHT:
            # Taller than the atlas (the 512-long vertical border strips): stored transposed, so the image's
            # (u, v) is at atlas texel (x + v * srcHeight, y + u * srcWidth).
            texels = np.ascontiguousarray(np.transpose(texels, (1, 0, 2)))
            flags |= FLAG_TRANSPOSED
        images.append((key, texels, flags))

    # ---- fonts
    font_path = os.path.join(args.resources, FONT_FILE)
    if not os.path.exists(font_path):
        raise SystemExit(f"font not found: {font_path}")
    fonts = []
    for height in heights:
        line_height, ascent, glyphs = render_glyphs(font_path, height)
        fonts.append((height, line_height, ascent, glyphs))

    # ---- packing: images with a repeated-edge gutter, glyphs with a transparent one
    icon_w, icon_h = ICON_REGION[0] * ICON_SIZE, ICON_REGION[1] * ICON_SIZE
    reserved = (ATLAS_WIDTH - icon_w, ATLAS_HEIGHT - icon_h, icon_w, icon_h)  # bottom-right corner
    cells = [(key, texels.shape[1] + 2 * gutters(texels)[0], texels.shape[0] + 2 * gutters(texels)[1])
             for key, texels, _ in images]
    for height, _, _, glyphs in fonts:
        for code, texels, *_ in glyphs:
            if texels is not None:
                cells.append((f"glyph/{height}/{code}", texels.shape[1] + 2 * GUTTER, texels.shape[0] + 2 * GUTTER))
    placements, used_height = shelf_pack(cells, ATLAS_WIDTH, ATLAS_HEIGHT, reserved)

    canvas = np.zeros((ATLAS_HEIGHT, ATLAS_WIDTH, 4), dtype=np.uint8)
    image_entries = []
    for key, texels, flags in images:
        x, y = placements[key]
        h, w = texels.shape[:2]
        gx, gy = gutters(texels)
        canvas[y : y + h + 2 * gy, x : x + w + 2 * gx] = np.pad(texels, ((gy, gy), (gx, gx), (0, 0)), mode="edge")
        image_entries.append((fnv1a64(key), key, x + gx, y + gy, w, h, flags))
    glyph_entries = []  # per font: list
    for height, line_height, ascent, glyphs in fonts:
        entries = []
        for code, texels, bearing_x, bearing_y, advance in glyphs:
            if texels is None:
                entries.append((code, 0, 0, 0, 0, bearing_x, bearing_y, advance))
                continue
            x, y = placements[f"glyph/{height}/{code}"]
            h, w = texels.shape[:2]
            canvas[y + GUTTER : y + GUTTER + h, x + GUTTER : x + GUTTER + w] = texels
            entries.append((code, x + GUTTER, y + GUTTER, w, h, bearing_x, bearing_y, advance))
        glyph_entries.append(entries)
    # the icon region: transparent, with a marker frame in the preview only
    preview = Image.fromarray(canvas, "RGBA")

    # ---- texels through tex3ds (PICA tiling), flipped like the texture pack
    flipped = preview.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    with tempfile.TemporaryDirectory(prefix="cook3ds_") as tmp:
        png = os.path.join(tmp, "atlas.png")
        out = os.path.join(tmp, "atlas.bin")
        flipped.save(png)
        result = subprocess.run([args.tex3ds, "-f", "rgba8", "-z", "none", "-r", "-o", out, png],
                                capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"tex3ds failed: {result.stderr.strip()}")
        with open(out, "rb") as f:
            texels = f.read()[4:]
    if len(texels) != ATLAS_WIDTH * ATLAS_HEIGHT * 4:
        raise RuntimeError(f"tex3ds output {len(texels)} bytes, expected {ATLAS_WIDTH * ATLAS_HEIGHT * 4}")

    # ---- the file
    glyph_count = sum(len(e) for e in glyph_entries)
    image_table = HEADER_SIZE
    font_table = image_table + IMAGE_SIZE * len(image_entries)
    glyph_table = font_table + FONT_SIZE * len(fonts)
    data_offset = (glyph_table + GLYPH_SIZE * glyph_count + DATA_ALIGN - 1) // DATA_ALIGN * DATA_ALIGN
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out + ".tmp", "wb") as f:
        f.write(struct.pack("<4sIHHHHIHHHHHHIIIII", MAGIC, VERSION, ATLAS_WIDTH, ATLAS_HEIGHT, len(image_entries),
                            len(fonts), glyph_count, reserved[0], reserved[1], reserved[2], reserved[3], ICON_SIZE,
                            0, image_table, font_table, glyph_table, data_offset, len(texels)))
        assert f.tell() <= HEADER_SIZE
        f.write(b"\0" * (HEADER_SIZE - f.tell()))
        for hash_, key, x, y, w, h, flags in image_entries:
            f.write(struct.pack("<QHHHHHHHH", hash_, x, y, w, h, flags, 0, 0, 0))
        first = 0
        for (height, line_height, ascent, _), entries in zip(fonts, glyph_entries):
            f.write(struct.pack("<HHhHII", height, line_height, ascent, len(entries), first, 0))
            first += len(entries)
        for entries in glyph_entries:
            for code, x, y, w, h, bx, by, advance in entries:
                f.write(struct.pack("<IHHHHbbBB", code, x, y, w, h, max(-128, min(127, bx)), max(-128, min(127, by)),
                                    min(255, advance), 0))
        f.write(b"\0" * (data_offset - f.tell()))
        f.write(texels)
        size = f.tell()
    os.replace(args.out + ".tmp", args.out)

    # ---- preview and sidecar
    preview_path = args.preview or os.path.splitext(args.out)[0] + "-preview.png"
    shown = preview.copy()
    frame = np.array(shown)
    rx, ry, rw, rh = reserved
    frame[ry, rx : rx + rw] = frame[ry + rh - 1, rx : rx + rw] = (255, 0, 255, 255)
    frame[ry : ry + rh, rx] = frame[ry : ry + rh, rx + rw - 1] = (255, 0, 255, 255)
    Image.fromarray(frame, "RGBA").save(preview_path)
    with open(os.path.splitext(args.out)[0] + "-index.json", "w", encoding="utf-8") as f:
        json.dump({
            "atlas": [ATLAS_WIDTH, ATLAS_HEIGHT], "icon_region": reserved, "icon_size": ICON_SIZE,
            "images": {key: {"x": x, "y": y, "w": w, "h": h, "alpha": bool(flags & FLAG_ALPHA)}
                       for _, key, x, y, w, h, flags in image_entries},
            "fonts": [{"height": h, "line_height": lh, "ascent": a, "glyphs": len(e)}
                      for (h, lh, a, _), e in zip(fonts, glyph_entries)],
            "missing": missing, "skipped": skipped,
        }, f, indent=1)

    used = sum((w + 2) * (h + 2) for _, _, _, _, w, h, _ in image_entries) \
        + sum((w + 2) * (h + 2) for e in glyph_entries for _, _, _, w, h, *_ in e if w) + rw * rh
    print(f"interface atlas {ATLAS_WIDTH}x{ATLAS_HEIGHT}: {len(image_entries)} images, "
          f"{len(fonts)} font sizes ({', '.join(str(h) for h in heights)} px) with {glyph_count} glyphs, "
          f"icon region {ICON_REGION[0]}x{ICON_REGION[1]} slots of {ICON_SIZE}; rows reach y = {used_height}, "
          f"{100 * used // (ATLAS_WIDTH * ATLAS_HEIGHT)}% of the texels used")
    for height, line_height, ascent, glyphs in fonts:
        print(f"  font {height} px: line height {line_height}, ascent {ascent}, {len(glyphs)} glyphs")
    if missing:
        print(f"  not found in the data ({len(missing)}): " + ", ".join(missing))
    if skipped:
        print(f"  too big for the atlas, left out ({len(skipped)}): " + ", ".join(skipped))
    print(f"wrote {args.out} ({size / 1024:.0f} KB), {preview_path}, in {time.time() - started:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
