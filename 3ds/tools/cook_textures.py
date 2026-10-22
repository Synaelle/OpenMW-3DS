#!/usr/bin/env python3
"""Texture cooker for the OpenMW 3DS port (PC side, run once per data set).

The PICA200 GPU has no S3TC/DXT support, and decoding DDS on the 3DS would cost load time and heap.
This tool reads the textures from the Morrowind BSA archives (and loose files, which override them,
as in OpenMW's VFS), converts them to native PICA formats and writes ONE pack file that the port
loads straight into GPU-readable (linear) memory. The source data is only read, never modified.

Per texture:
  - decode (Pillow: DXT1/DXT3/DXT5/uncompressed DDS, TGA, BMP);
  - resize to a power of two in [8, --max-size] (the PICA only supports power-of-two sizes);
  - textures with alpha: give the fully transparent texels the colour of the visible ones around them
    (tex3ds turns them black, which bilinear filtering and mipmaps then blend into the visible edges:
    dark fringes, and Morrowind's faint white raindrops drawn black), see bleed_transparent;
  - flip vertically (PICA texture memory starts at t = 0, which OpenMW's images keep as the top row);
  - encode with devkitPro's tex3ds: ETC1 (4 bpp) for opaque textures, ETC1A4 (8 bpp) with alpha,
    RGBA8 for small uncompressed sources (UI elements, where ETC1 artifacts show); full mip chain.

UI images (menu frames, icons, book art, OpenMW's own menu textures; see is_ui) are cooked differently, so menus
look pixel-exact (M8): never resampled but padded (edge texels repeated) to the next power of two (the runtime
scales the texture coordinates by source / cooked size: flag 4), no mipmaps (MyGUI never uses them), RGBA8 up to
128 texels and ETC1/ETC1A4 for bigger pictures (book art, level-up art). Hardware test #40 (the flashing meshes
kept flashing with the 1024x512 atlas, while every clean run bound only textures of at most 256 per side, the cap
of every scene texture): no cooked UI texture has a side above 256. A UI image longer than that on a side is
resampled to a power of two of at most 256 on each side (it then fills its texture, no padding; the runtime still
tells MyGUI the original size); only the UI atlas is wider.

UI atlas (hardware tests #34-#38: the interface's ~220 texture changes per frame, between tiny textures, make the
New 3DS draw nearby scene meshes wrong for a frame): the small interface images (see is_atlas_candidate: menu
frames, bars, buttons, cursors, the 512-long border strips; not the item icons, book art or level-up art) are also
packed into ONE RGBA8 atlas, "ui-atlas/0" (512x256: hardware tests #44-#46 drew a whole interface from a texture
of that size without a flash; 1024x512 flashed), each with a 1-texel repeated-edge gutter so bilinear filtering
never samples a neighbour (none along a side that spans the whole page: the page's clamped edge is the image's
own edge). Strips taller than they are wide are stored transposed (flag 16), so every row of the atlas stays
short. The image's entry keeps its own standalone texture (the runtime uses it for the rare skins whose texture
coordinates leave [0, 1]) and records its atlas and position (flag 8, see the index), so the renderer maps the
batch's texture coordinates into the atlas and binds one texture for the whole interface.

Pack layout (little endian), read by components/render3ds/texturepack.cpp:
  header  (32 bytes): magic 'O3TP', version, entry count, index offset, data offset, 3 x reserved (0; packs
          cooked before M20 kept the atlas fonts and the GPU interface's reserved region there, read no more)
  index   (entry count x 32 bytes, sorted by hash):
          u64 hash   FNV-1a 64 of the normalized VFS path without extension ("textures/tx_wood")
          u32 offset, u32 size        (texture data, from the start of the file)
          u16 width, u16 height       (cooked, power of two)
          u16 srcWidth, u16 srcHeight (original image size)
          u8 format (GPU_TEXCOLOR), u8 mip levels (including the base), u8 flags, u8 atlas (flag 8: the index
          of the "ui-atlas/<n>" entry holding this image too)
          u32 atlasPos (flag 8: x | y << 16, the image's top-left texel in the atlas, inside its gutter)
  data    texture data in PICA tiled layout, each texture 128-byte aligned, mip levels back to back.

Flags: 1 = has alpha (some texel < 255), 2 = alpha is only 0/255 (good for alpha testing), 4 = padded (UI: the
image fills srcWidth x srcHeight of the cooked width x height, from the top left; no mipmaps), 8 = also in a UI
atlas (atlas, atlasPos), 16 = stored transposed in the atlas (atlas x runs along the image's rows: the image's
(u, v) is at atlas texel (x + v * srcHeight, y + u * srcWidth)), 32 = the atlas copy is half the size on each
side (the item frames, ATLAS_HALVED_PREFIX).

Version 4 (one 512x256 UI atlas); older packs are refused by the runtime. Needs Pillow and numpy.

Usage:
  python tools/cook_textures.py --data "D:/.../Morrowind/Data Files" --resources build/openmw/resources/vfs \
      --out out/cooked/textures.o3tp
"""

import argparse
import io
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

MAGIC = b"O3TP"
VERSION = 4
HEADER_SIZE = 32
ENTRY_SIZE = 32
DATA_ALIGN = 128

# GPU_TEXCOLOR values (libctru gpu enums)
GPU_RGBA8 = 0x0
GPU_ETC1 = 0xC
GPU_ETC1A4 = 0xD

FLAG_ALPHA = 1
FLAG_ALPHA_BINARY = 2
FLAG_PADDED = 4
FLAG_ATLAS = 8
FLAG_ATLAS_TRANSPOSED = 16
FLAG_ATLAS_HALVED = 32
# ItemWidget's frames over the icons (named by concatenation in the code, so not found by the reference scan),
# 64x64 drawn at about 40 px: their atlas copy is halved (flag 32, their own texture keeps the full size).
ATLAS_HALVED_PREFIX = "textures/menu_icon_"

# UI images (see the docstring). Prefixes of normalized paths, then basename prefixes under textures/.
UI_PREFIXES = ("icons/", "bookart/", "textures/omw_", "textures/levelup/", "textures/birthsigns/")
UI_TEXTURE_NAMES = ("menu_", "tx_menu", "tx_cursor", "compass", "target", "scroll", "detect_", "menu")
UI_MAX_SIZE = 256  # the longest side of any cooked UI texture (hardware test #40)
# Exceptions: UI pictures the CPU interface shows large (the title screen's picture on the whole top screen).
UI_BIG = {"textures/menu_morrowind": 512}
# UI atlas (see the docstring): the images that go in, and its shape.
ATLAS_KEY = "ui-atlas/"
ATLAS_WIDTH = 512
ATLAS_HEIGHT = 256
ATLAS_GUTTER = 1
# Not atlased: the item icons (1,200 of them: two more atlases, 8 MB of linear memory for the Inventory tab alone),
# book and level-up art, birthsigns (big pictures).
ATLAS_EXCLUDED = ("icons/", "bookart/", "textures/levelup/", "textures/birthsigns/")
ATLAS_MAX_SIDE = 512  # the border strips
ATLAS_MAX_AREA = 128 * 64  # leaves out the 13 128x128 PlayStation button glyphs (220 K texels, never in the HUD)
# Read by the CPU (the global map's colours) or not used on the 3DS: left out of the pack.
EXCLUDED = ("textures/omw_map_color_palette", "textures/omw/water_nm")
# The controller button glyphs are drawn small (button hints): cooked down to this size.
GLYPH_PREFIX = "textures/omw_steam_button_"
GLYPH_SIZE = 32

TEX3DS_FORMAT = {GPU_RGBA8: "rgba8", GPU_ETC1: "etc1", GPU_ETC1A4: "etc1a4"}
BITS_PER_PIXEL = {GPU_RGBA8: 32, GPU_ETC1: 4, GPU_ETC1A4: 8}

DEFAULT_TEX3DS = r"C:\msys64\opt\devkitpro\tools\bin\tex3ds.exe"
IMAGE_EXTENSIONS = (".dds", ".tga", ".bmp", ".png", ".jpg", ".jpeg")


def fnv1a64(text):
    h = 0xCBF29CE484222325
    for b in text.encode("utf-8"):
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def normalize(path):
    return path.replace("\\", "/").lower()


def stem_key(path):
    """Pack key: normalized path without extension, so .tga/.dds requests find the same texture."""
    base, _ = os.path.splitext(path)
    return base


def read_bsa_index(path):
    """TES3 BSA: header, (size, offset) records, name offsets, names, hashes, data."""
    with open(path, "rb") as f:
        version, hash_offset, count = struct.unpack("<III", f.read(12))
        if version != 0x100:
            raise ValueError(f"{path}: not a TES3 BSA (version {version:#x})")
        records = [struct.unpack("<II", f.read(8)) for _ in range(count)]
        name_offsets = struct.unpack(f"<{count}I", f.read(4 * count))
        names = f.read(hash_offset - 12 * count)
    data_start = 12 + hash_offset + 8 * count
    files = {}
    for (size, offset), name_offset in zip(records, name_offsets):
        end = names.index(b"\0", name_offset)
        name = normalize(names[name_offset:end].decode("latin-1"))
        files[name] = (path, data_start + offset, size)
    return files


def is_ui(name):
    if name.startswith(UI_PREFIXES):
        return True
    if name.startswith("textures/") and "/" not in name[len("textures/"):]:
        return name[len("textures/"):].startswith(UI_TEXTURE_NAMES)
    return False


def is_atlas_candidate(key, width, height):
    """A small interface image (see the docstring): packed into the UI atlas as well as cooked on its own."""
    if key.startswith(ATLAS_EXCLUDED):
        return False
    return max(width, height) <= ATLAS_MAX_SIDE and width * height <= ATLAS_MAX_AREA


def referenced_textures(folders):
    """The texture names the MyGUI skins and layouts in `folders` reference (texture="..." in .xml/.layout/.skin)
    and the ones OpenMW's interface code names ("textures/..." literals in .cpp), as normalized keys without
    extension. Only these go in the UI atlas: the whole menu set (167 images) needs three pages."""
    xml_pattern = re.compile(r'texture\s*=\s*"([^"]+)"', re.IGNORECASE)
    code_pattern = re.compile(r'"(textures[/\\][A-Za-z0-9_/\\]+?)(?:\.(?:dds|tga|png))?"')
    names = set()
    for folder in folders:
        if not folder or not os.path.isdir(folder):
            continue
        for file in os.listdir(folder):
            path = os.path.join(folder, file)
            if file.endswith((".xml", ".layout", ".skin")):
                pattern = xml_pattern
            elif file.endswith((".cpp", ".hpp")):
                pattern = code_pattern
            else:
                continue
            with open(path, encoding="utf-8", errors="replace") as f:
                for match in pattern.finditer(f.read()):
                    names.add(stem_key(normalize(match.group(1))))
    return names


def atlas_gutters(width, height):
    """The gutter on each axis: none along a side that spans the whole page (the page edge clamps to it)."""
    return (0 if width >= ATLAS_WIDTH else ATLAS_GUTTER), (0 if height >= ATLAS_HEIGHT else ATLAS_GUTTER)


def cell_gutters(key, width, height):
    """(left, top, right, bottom, numpy pad mode) of a cell: a skin image repeats its edge all around (bilinear
    filtering at its edge sees the edge itself)."""
    gx, gy = atlas_gutters(width, height)
    return gx, gy, gx, gy, "edge"


def pack_atlas(cells):
    """cells: [(key, rgba numpy array)] -> {key: (x, y)} of each cell's top-left texel inside its gutter. Guillotine
    packing of one ATLAS_WIDTH x ATLAS_HEIGHT page: each cell (tallest first, then widest) takes the free rectangle whose shorter
    leftover is the smallest and splits its remainder along the longer leftover; the long strips go first."""
    free = [(0, 0, ATLAS_WIDTH, ATLAS_HEIGHT)]
    sized = []
    for key, texels in cells:
        h, w = texels.shape[:2]
        gl, gt, gr, gb, _ = cell_gutters(key, w, h)
        sized.append((key, w + gl + gr, h + gt + gb, gl, gt))
    placements = {}
    transposed = set()
    area = sum(w * h for _, w, h, _, _ in sized)
    # The long strips first: they take whole rows at the top before the page fragments.
    for key, w, h, gx, gy in sorted(sized, key=lambda c: (0 if c[1] > ATLAS_WIDTH // 4 else 1, -c[2], -c[1], c[0])):
        best = None
        # A skin image may also go in turned on its side (flag 16) when that is the only way it still fits.
        for turned in ((False, True) if w != h else (False,)):
            cw, ch = (h, w) if turned else (w, h)
            for index, (fx, fy, fw, fh) in enumerate(free):
                if cw <= fw and ch <= fh:
                    score = (min(fw - cw, fh - ch), max(fw - cw, fh - ch))
                    if best is None or score < best[0]:
                        best = (score, index, turned)
            if best is not None:
                break
        if best is None:
            raise RuntimeError(f"the UI atlas is full at {key} ({w}x{h}): {len(placements)} of {len(cells)} cells "
                               f"placed, {100 * area // (ATLAS_WIDTH * ATLAS_HEIGHT)}% of the page needed by the "
                               f"cells alone")
        if best[2]:
            transposed.add(key)
            w, h, gx, gy = h, w, gy, gx
        fx, fy, fw, fh = free.pop(best[1])
        placements[key] = (fx + gx, fy + gy)
        if fw - w > fh - h:  # split vertically: the whole strip to the right, the rest under the cell
            free.append((fx + w, fy, fw - w, fh))
            free.append((fx, fy + h, w, fh - h))
        else:
            free.append((fx, fy + h, fw, fh - h))
            free.append((fx + w, fy, fw - w, h))
        free = [r for r in free if r[2] > 0 and r[3] > 0]
    return placements, transposed


def build_atlas(cells, placements, tex3ds):
    """Composes the atlas (each cell with its repeated-edge gutter), flips it like every cooked texture and encodes
    it as RGBA8. Returns the pack entry tuple."""
    width, height = ATLAS_WIDTH, ATLAS_HEIGHT
    canvas = np.zeros((height, width, 4), dtype=np.uint8)
    for key, texels in cells:
        x, y = placements[key]
        h, w = texels.shape[:2]
        gl, gt, gr, gb, mode = cell_gutters(key, w, h)
        canvas[y - gt : y + h + gb, x - gl : x + w + gr] = np.pad(texels, ((gt, gb), (gl, gr), (0, 0)), mode=mode)
    rgba = Image.fromarray(canvas, "RGBA").transpose(Image.Transpose.FLIP_TOP_BOTTOM)
    with tempfile.TemporaryDirectory(prefix="cook3ds_") as tmp:
        png = os.path.join(tmp, "atlas.png")
        out = os.path.join(tmp, "atlas.bin")
        rgba.save(png)
        cmd = [tex3ds, "-f", "rgba8", "-z", "none", "-r", "-o", out, png]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"tex3ds failed for the UI atlas: {result.stderr.strip()}")
        with open(out, "rb") as f:
            blob = f.read()
    data = blob[4:]
    if len(data) != width * height * 4:
        raise RuntimeError(f"UI atlas: {len(data)} bytes, expected {width * height * 4}")
    key = f"{ATLAS_KEY}0"
    return (fnv1a64(key), key, data, width, height, width, height, GPU_RGBA8, 1, FLAG_ALPHA, 0, 0), canvas


def collect_sources(data_dir, archives, prefixes, resources=None, after=(), loose=True):
    """name -> (file, offset, size). Later archives and loose files override earlier ones (VFS order); OpenMW's own
    resources (resources/vfs) come first, as OpenMW adds them before the game's data, and the folders in `after`
    (the port's own interface, packaging/ui3ds) last, as the port's openmw.cfg adds them after it."""
    sources = {}

    def walk(folder):
        for root, _, files in os.walk(folder):
            for file in files:
                full = os.path.join(root, file)
                name = normalize(os.path.relpath(full, folder))
                if name.startswith(prefixes) and name.endswith(IMAGE_EXTENSIONS):
                    sources[name] = (full, 0, os.path.getsize(full))

    if resources:
        walk(resources)
    for archive in archives:
        bsa = os.path.join(data_dir, archive)
        for name, src in read_bsa_index(bsa).items():
            if name.startswith(prefixes) and name.endswith(IMAGE_EXTENSIONS):
                sources[name] = src
    if loose:
        walk(data_dir)
    for folder in after:
        walk(folder)
    # One texture per key. OpenMW prefers .dds when both exist.
    by_key = {}
    for name in sorted(sources):
        key = stem_key(name)
        if key.startswith(EXCLUDED):
            continue
        if key not in by_key or name.endswith(".dds"):
            by_key[key] = (name, sources[name])
    return by_key


def pow2_size(n, max_size):
    p = 8
    while p < n and p < max_size:
        p *= 2
    # Round to the nearest power of two (48 -> 64, 40 -> 32) rather than always up.
    if p > 8 and p > n and (p - n) > (n - p // 2):
        p //= 2
    return max(8, min(p, max_size))


def dds_is_uncompressed(raw):
    return raw[:4] == b"DDS " and raw[84:88] == b"\0\0\0\0"


def bleed_transparent(rgba, keep_alpha_zero):
    """Colours the fully transparent texels with the alpha-weighted colour of the visible texels nearby (up to 8
    texels away, wrapping like a repeating texture), then the texture's average colour. tex3ds replaces the colour
    of alpha-0 texels with black, so for ETC1A4 they get alpha 1 instead, which keeps the colour and still
    encodes as 0 in its 4-bit alpha. (RGBA8 would keep alpha 1: keep_alpha_zero, so those stay black.)"""
    a = np.asarray(rgba).astype(np.float32)
    clear = a[..., 3] == 0
    if not clear.any() or clear.all():
        return rgba
    weight = a[..., 3] / 255.0
    color = a[..., :3] * weight[..., None]  # premultiplied
    filled = ~clear
    for _ in range(8):
        if filled.all():
            break
        color_sum = np.zeros_like(color)
        weight_sum = np.zeros_like(weight)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                color_sum += np.roll(color, (dy, dx), axis=(0, 1))
                weight_sum += np.roll(weight, (dy, dx), axis=(0, 1))
        grow = ~filled & (weight_sum > 0)
        if not grow.any():
            break
        # New texels carry their neighbours' average colour on, at a fraction of their weight.
        new_weight = weight_sum[grow] / 18.0
        color[grow] = color_sum[grow] / weight_sum[grow][:, None] * new_weight[:, None]
        weight[grow] = new_weight
        filled |= grow
    visible = ~clear
    mean = (a[..., :3][visible] * (a[..., 3][visible] / 255.0)[:, None]).sum(axis=0) / max(
        1e-6, (a[..., 3][visible] / 255.0).sum())
    rgb = np.where(filled[..., None], color / np.maximum(weight, 1e-6)[..., None], mean)
    out = a.copy()
    out[..., :3] = np.where(clear[..., None], rgb, a[..., :3])
    if not keep_alpha_zero:
        out[..., 3] = np.where(clear, 1.0, a[..., 3])
    return Image.fromarray(np.clip(np.rint(out), 0, 255).astype(np.uint8), "RGBA")


def tga_kill_alpha(raw):
    """Morrowind ignores the alpha of 16 bpp TGAs (as OpenMW's ImageManager does)."""
    image_type = raw[2]
    depth = raw[7] if image_type in (1, 9) else raw[16]
    return depth == 16 and (raw[17] & 0x0F) == 1


def pow2_ceil(n, max_size):
    p = 8
    while p < n and p < max_size:
        p *= 2
    return p


def cook_one(job):
    key, name, raw, max_size, tex3ds, quality, referenced = job
    image = Image.open(io.BytesIO(raw))
    image.load()
    if key.startswith(GLYPH_PREFIX) and max(image.size) > GLYPH_SIZE:
        image = image.resize((GLYPH_SIZE, GLYPH_SIZE), Image.Resampling.LANCZOS)
    src_w, src_h = image.size
    ui = is_ui(key) or referenced  # a skin's texture is an interface image whatever its name (door_icon)

    if name.endswith(".tga") and tga_kill_alpha(raw):
        image = image.convert("RGB")
    rgba = image.convert("RGBA")

    alpha = rgba.getchannel("A")
    lo, hi = alpha.getextrema()
    has_alpha = lo < 255
    flags = 0
    if has_alpha:
        flags |= FLAG_ALPHA
        histogram = alpha.histogram()
        if sum(histogram[1:255]) == 0:
            flags |= FLAG_ALPHA_BINARY

    if ui:
        # A side above UI_MAX_SIZE: resampled to a power of two of at most UI_MAX_SIZE on each side, filling the
        # texture (no padding, no flag: the texture coordinates MyGUI computes from the original size cover it).
        ui_max = UI_BIG.get(key, UI_MAX_SIZE)
        resampled = max(src_w, src_h) > ui_max
        w = pow2_ceil(min(src_w, ui_max), ui_max)
        h = pow2_ceil(min(src_h, ui_max), ui_max)
        fmt = GPU_RGBA8 if max(w, h) <= 128 else (GPU_ETC1A4 if has_alpha else GPU_ETC1)
        if has_alpha:
            rgba = bleed_transparent(rgba, keep_alpha_zero=(fmt == GPU_RGBA8))
        if resampled:
            rgba = rgba.resize((w, h), Image.Resampling.LANCZOS)
        elif (w, h) != (src_w, src_h):
            # Pad by repeating the edge texels: filtering at the image's edge then sees the edge itself.
            texels = np.asarray(rgba)
            texels = np.pad(texels, ((0, h - src_h), (0, w - src_w), (0, 0)), mode="edge")
            rgba = Image.fromarray(texels, "RGBA")
            flags |= FLAG_PADDED
    else:
        w, h = pow2_size(src_w, max_size), pow2_size(src_h, max_size)
        if (w, h) != (src_w, src_h):
            rgba = rgba.resize(
                (w, h), Image.Resampling.LANCZOS if w < src_w or h < src_h else Image.Resampling.BICUBIC)

        small_raw = name.endswith(".dds") and dds_is_uncompressed(raw) and max(w, h) <= 128
        if small_raw:
            fmt = GPU_RGBA8
        else:
            fmt = GPU_ETC1A4 if has_alpha else GPU_ETC1

        if has_alpha:
            rgba = bleed_transparent(rgba, keep_alpha_zero=(fmt == GPU_RGBA8))

    atlas_texels = None
    if ui and referenced and is_atlas_candidate(key, src_w, src_h):
        # The image itself (no padding, top row first). Strips taller than wide are stored transposed.
        atlas_texels = np.array(rgba) if resampled else np.array(rgba)[:src_h, :src_w].copy()
        if key.startswith(ATLAS_HALVED_PREFIX) and max(src_w, src_h) > 32:
            half = Image.fromarray(atlas_texels, "RGBA").resize((src_w // 2, src_h // 2), Image.Resampling.LANCZOS)
            atlas_texels = np.array(half)
            flags |= FLAG_ATLAS_HALVED
        if atlas_texels.shape[0] > atlas_texels.shape[1] and atlas_texels.shape[0] > 64:
            atlas_texels = np.ascontiguousarray(atlas_texels.transpose(1, 0, 2))
            flags |= FLAG_ATLAS_TRANSPOSED
        flags |= FLAG_ATLAS

    rgba = rgba.transpose(Image.Transpose.FLIP_TOP_BOTTOM)

    with tempfile.TemporaryDirectory(prefix="cook3ds_") as tmp:
        png = os.path.join(tmp, "in.png")
        out = os.path.join(tmp, "out.bin")
        (rgba if has_alpha else rgba.convert("RGB")).save(png)
        cmd = [tex3ds, "-f", TEX3DS_FORMAT[fmt], "-m", "box", "-z", "none", "-r", "-o", out, png]
        if ui:
            cmd[3:5] = []  # no mipmaps
        if fmt != GPU_RGBA8:
            cmd[1:1] = ["-q", quality]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"tex3ds failed for {name}: {result.stderr.strip()}")
        with open(out, "rb") as f:
            blob = f.read()

    # -r still writes a 4-byte compression header (type 0 = none, 24-bit size).
    if blob[0] != 0 or int.from_bytes(blob[1:4], "little") != len(blob) - 4:
        raise RuntimeError(f"{name}: unexpected tex3ds output header {blob[:4].hex()}")
    data = blob[4:]

    levels = 0
    expected = 0
    lw, lh = w, h
    while True:
        expected += lw * lh * BITS_PER_PIXEL[fmt] // 8
        levels += 1
        if ui or lw <= 8 or lh <= 8:
            break
        lw //= 2
        lh //= 2
    if expected != len(data):
        raise RuntimeError(f"{name}: {len(data)} bytes, expected {expected} for {w}x{h} with {levels} levels")

    return key, name, data, w, h, src_w, src_h, fmt, levels, flags, atlas_texels


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", required=True, help='Morrowind "Data Files" directory (read only)')
    parser.add_argument("--out", required=True, help="output pack file")
    parser.add_argument("--archives", default="Morrowind.bsa", help="comma-separated BSAs, in load order")
    parser.add_argument("--prefixes", default="textures/,icons/,bookart/",
                        help="comma-separated VFS path prefixes to cook")
    parser.add_argument("--resources", default="", help="OpenMW's resources/vfs (its own UI textures), read first")
    parser.add_argument("--max-size", type=int, default=256, help="largest cooked dimension (power of two)")
    parser.add_argument("--quality", default="medium", choices=("low", "medium", "high"), help="ETC1 quality")
    parser.add_argument("--tex3ds", default=DEFAULT_TEX3DS)
    parser.add_argument("--jobs", type=int, default=os.cpu_count())
    parser.add_argument("--limit", type=int, default=0, help="cook only the first N textures (testing)")
    parser.add_argument("--atlas-preview", default="", help="also write the UI atlas as this PNG (top row first)")
    parser.add_argument("--layouts", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                          "packaging", "ui3ds", "mygui"),
                        help="comma-separated folders of the port's own MyGUI layouts and skins (their parent "
                             "folders are VFS roots read last: the port's own textures)")
    args = parser.parse_args()

    started = time.time()
    archives = [a.strip() for a in args.archives.split(",") if a.strip()]
    prefixes = tuple(normalize(p.strip()) for p in args.prefixes.split(",") if p.strip())
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    layouts = [folder for folder in args.layouts.split(",") if folder]
    sources = collect_sources(args.data, archives, prefixes, args.resources,
                              after=[os.path.dirname(folder) for folder in layouts])
    referenced = referenced_textures([os.path.join(args.resources, "mygui")] + layouts
                                     + [os.path.join(repo, "openmw", "apps", "openmw", "mwgui")])
    referenced.update(key for key in sources if key.startswith(ATLAS_HALVED_PREFIX))
    keys = sorted(sources)
    if args.limit:
        keys = keys[: args.limit]
    print(f"{len(keys)} textures from {', '.join(archives)} + loose files", flush=True)

    def jobs():
        handles = {}
        for key in keys:
            name, (path, offset, size) = sources[key]
            f = handles.get(path) or handles.setdefault(path, open(path, "rb"))
            f.seek(offset)
            yield key, name, f.read(size), args.max_size, args.tex3ds, args.quality, key in referenced
        for f in handles.values():
            f.close()

    results = []
    failures = 0
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for i, result in enumerate(pool.map(cook_one, jobs(), chunksize=8)):
            results.append(result)
            if (i + 1) % 250 == 0:
                print(f"  {i + 1}/{len(keys)}", flush=True)

    # The UI atlas: the candidate images packed into one page.
    cells = [(key, texels) for key, name, data, w, h, src_w, src_h, fmt, levels, flags, texels in results
             if texels is not None]
    placements, turned = pack_atlas(cells)
    cells = [(key, np.ascontiguousarray(texels.transpose(1, 0, 2)) if key in turned else texels) for key, texels in cells]
    entries = []
    for key, name, data, w, h, src_w, src_h, fmt, levels, flags, texels in results:
        x, y = placements.get(key, (0, 0))
        if key in turned:
            flags |= FLAG_ATLAS_TRANSPOSED
        entries.append((fnv1a64(key), key, data, w, h, src_w, src_h, fmt, levels, flags, 0, x | (y << 16)))
    atlas_entry, atlas_canvas = build_atlas(cells, placements, args.tex3ds)
    entries.append(atlas_entry)
    if args.atlas_preview:
        Image.fromarray(atlas_canvas, "RGBA").save(args.atlas_preview)
    used = sum((t.shape[1] + cell_gutters(k, t.shape[1], t.shape[0])[0] + cell_gutters(k, t.shape[1], t.shape[0])[2])
               * (t.shape[0] + cell_gutters(k, t.shape[1], t.shape[0])[1] + cell_gutters(k, t.shape[1], t.shape[0])[3])
               for k, t in cells)
    print(f"  UI atlas: {ATLAS_WIDTH}x{ATLAS_HEIGHT} RGBA8, {len(cells)} images, "
          f"{100 * used // (ATLAS_WIDTH * ATLAS_HEIGHT)}% used")
    entries.sort(key=lambda e: e[0])
    for a, b in zip(entries, entries[1:]):
        if a[0] == b[0]:
            raise RuntimeError(f"hash collision: {a[1]} / {b[1]}")

    index_offset = HEADER_SIZE
    data_offset = (index_offset + ENTRY_SIZE * len(entries) + DATA_ALIGN - 1) // DATA_ALIGN * DATA_ALIGN
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    tmp_out = args.out + ".tmp"
    by_format = {}
    with open(tmp_out, "wb") as f:
        f.write(struct.pack("<4sIIII3I", MAGIC, VERSION, len(entries), index_offset, data_offset, 0, 0, 0))
        offset = data_offset
        index = bytearray()
        for hash_, key, data, w, h, src_w, src_h, fmt, levels, flags, atlas, atlas_pos in entries:
            index += struct.pack("<QIIHHHHBBBBI", hash_, offset, len(data), w, h, src_w, src_h, fmt, levels, flags,
                                 atlas, atlas_pos)
            offset = (offset + len(data) + DATA_ALIGN - 1) // DATA_ALIGN * DATA_ALIGN
            count, total = by_format.get(fmt, (0, 0))
            by_format[fmt] = (count + 1, total + len(data))
        f.write(index)
        for hash_, key, data, *_ in entries:
            f.seek((f.tell() + DATA_ALIGN - 1) // DATA_ALIGN * DATA_ALIGN)
            f.write(data)
        size = f.tell()
    os.replace(tmp_out, args.out)

    ui_count = sum(1 for e in entries if e[9] & FLAG_PADDED)
    print(f"  UI images padded to a power of two (no mipmaps): {ui_count}")
    ui_max = max((max(e[3], e[4]) for e in entries if is_ui(e[1]) and e[1] not in UI_BIG), default=0)
    all_max = max(max(e[3], e[4]) for e in entries if not e[1].startswith(ATLAS_KEY))
    print(f"  largest cooked side: UI textures {ui_max}, every texture {all_max} (hardware test #40: UI <= 256), "
          f"the UI atlas {ATLAS_WIDTH}x{ATLAS_HEIGHT}")
    if ui_max > UI_MAX_SIZE:
        raise RuntimeError(f"a UI texture has a side of {ui_max} > {UI_MAX_SIZE}")
    names = {GPU_RGBA8: "RGBA8", GPU_ETC1: "ETC1", GPU_ETC1A4: "ETC1A4"}
    for fmt, (count, total) in sorted(by_format.items()):
        print(f"  {names[fmt]:7s} {count:5d} textures {total / 1048576:7.1f} MB")
    print(f"wrote {args.out}: {len(entries)} textures, {size / 1048576:.1f} MB in {time.time() - started:.0f} s")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
