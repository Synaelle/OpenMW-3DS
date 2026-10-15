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
128 texels and ETC1/ETC1A4 for bigger pictures (book art, level-up art), up to 1024 (the PICA's largest).

Pack layout (little endian), read by components/render3ds/texturepack.cpp:
  header  (32 bytes): magic 'O3TP', version, entry count, index offset, data offset, 3 x reserved
  index   (entry count x 32 bytes, sorted by hash):
          u64 hash   FNV-1a 64 of the normalized VFS path without extension ("textures/tx_wood")
          u32 offset, u32 size        (texture data, from the start of the file)
          u16 width, u16 height       (cooked, power of two)
          u16 srcWidth, u16 srcHeight (original image size)
          u8 format (GPU_TEXCOLOR), u8 mip levels (including the base), u8 flags, u8 reserved
          u32 reserved
  data    texture data in PICA tiled layout, each texture 128-byte aligned, mip levels back to back.

Flags: 1 = has alpha (some texel < 255), 2 = alpha is only 0/255 (good for alpha testing), 4 = padded (UI: the
image fills srcWidth x srcHeight of the cooked width x height, from the top left; no mipmaps).

Usage:
  python tools/cook_textures.py --data "D:/.../Morrowind/Data Files" --resources build/openmw/resources/vfs \
      --out out/cooked/textures.o3tp
"""

import argparse
import io
import os
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor

import numpy as np
from PIL import Image

MAGIC = b"O3TP"
VERSION = 1
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

# UI images (see the docstring). Prefixes of normalized paths, then basename prefixes under textures/.
UI_PREFIXES = ("icons/", "bookart/", "textures/omw_", "textures/levelup/", "textures/birthsigns/")
UI_TEXTURE_NAMES = ("menu_", "tx_menu", "tx_cursor", "compass", "target", "scroll", "detect_", "menu")
UI_MAX_SIZE = 1024
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


def collect_sources(data_dir, archives, prefixes, resources=None):
    """name -> (file, offset, size). Later archives and loose files override earlier ones (VFS order); OpenMW's own
    resources (resources/vfs) come first, as OpenMW adds them before the game's data."""
    sources = {}
    if resources:
        for root, _, files in os.walk(resources):
            for file in files:
                full = os.path.join(root, file)
                name = normalize(os.path.relpath(full, resources))
                if name.startswith(prefixes) and name.endswith(IMAGE_EXTENSIONS):
                    sources[name] = (full, 0, os.path.getsize(full))
    for archive in archives:
        bsa = os.path.join(data_dir, archive)
        for name, src in read_bsa_index(bsa).items():
            if name.startswith(prefixes) and name.endswith(IMAGE_EXTENSIONS):
                sources[name] = src
    for root, _, files in os.walk(data_dir):
        for file in files:
            full = os.path.join(root, file)
            name = normalize(os.path.relpath(full, data_dir))
            if name.startswith(prefixes) and name.endswith(IMAGE_EXTENSIONS):
                sources[name] = (full, 0, os.path.getsize(full))
    # One texture per key; OpenMW prefers .dds when both exist.
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
    key, name, raw, max_size, tex3ds, quality = job
    image = Image.open(io.BytesIO(raw))
    image.load()
    if key.startswith(GLYPH_PREFIX) and max(image.size) > GLYPH_SIZE:
        image = image.resize((GLYPH_SIZE, GLYPH_SIZE), Image.Resampling.LANCZOS)
    src_w, src_h = image.size
    ui = is_ui(key) and max(src_w, src_h) <= UI_MAX_SIZE

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
        w, h = pow2_ceil(src_w, UI_MAX_SIZE), pow2_ceil(src_h, UI_MAX_SIZE)
        fmt = GPU_RGBA8 if max(src_w, src_h) <= 128 else (GPU_ETC1A4 if has_alpha else GPU_ETC1)
        if has_alpha:
            rgba = bleed_transparent(rgba, keep_alpha_zero=(fmt == GPU_RGBA8))
        if (w, h) != (src_w, src_h):
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

    return key, name, data, w, h, src_w, src_h, fmt, levels, flags


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
    args = parser.parse_args()

    started = time.time()
    archives = [a.strip() for a in args.archives.split(",") if a.strip()]
    prefixes = tuple(normalize(p.strip()) for p in args.prefixes.split(",") if p.strip())
    sources = collect_sources(args.data, archives, prefixes, args.resources)
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
            yield key, name, f.read(size), args.max_size, args.tex3ds, args.quality
        for f in handles.values():
            f.close()

    results = []
    failures = 0
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for i, result in enumerate(pool.map(cook_one, jobs(), chunksize=8)):
            results.append(result)
            if (i + 1) % 250 == 0:
                print(f"  {i + 1}/{len(keys)}", flush=True)

    entries = []
    for key, name, data, w, h, src_w, src_h, fmt, levels, flags in results:
        entries.append((fnv1a64(key), key, data, w, h, src_w, src_h, fmt, levels, flags))
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
        for hash_, key, data, w, h, src_w, src_h, fmt, levels, flags in entries:
            index += struct.pack("<QIIHHHHBBBBI", hash_, offset, len(data), w, h, src_w, src_h, fmt, levels, flags, 0, 0)
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
    names = {GPU_RGBA8: "RGBA8", GPU_ETC1: "ETC1", GPU_ETC1A4: "ETC1A4"}
    for fmt, (count, total) in sorted(by_format.items()):
        print(f"  {names[fmt]:7s} {count:5d} textures {total / 1048576:7.1f} MB")
    print(f"wrote {args.out}: {len(entries)} textures, {size / 1048576:.1f} MB in {time.time() - started:.0f} s")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
