#!/usr/bin/env python3
"""OpenMW for New 3DS: the setup app without its window, in Python (a backup for when the app doesn't run).

Prepares the folder to copy to the root of the 3DS's SD card from a Morrowind installation, as the setup app does:
the port's files, openmw/config/openmw.cfg with Morrowind.ini's values, the sound archive, the game's files and the
textures cooked for the 3DS's GPU. The game's files are only read.

    python openmw3ds_setup.py --morrowind "<Morrowind folder>" --output <folder> [--port <port files>]

Needs Python 3.9+ with Pillow and numpy (pip install pillow numpy). --port defaults to the "port" folder next to this
script (the release's port files). The textures are the same as the app's in every way but the ETC1 blocks, which
this script's own encoder (numpy) packs a little differently from the app's (rg_etc1, as devkitPro's tex3ds).
"""

import argparse
import io
import math
import os
import re
import shutil
import struct
import sys
import time
from concurrent.futures import ProcessPoolExecutor

try:
    import numpy as np
    from PIL import Image
except ImportError:
    sys.exit("This script needs Pillow and numpy: pip install pillow numpy")

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data") if os.path.isdir(os.path.join(HERE, "data")) else os.path.join(HERE, "..", "data")

CONTENT = ["Morrowind.esm"]
ARCHIVES = ["Morrowind.bsa"]
EMPTY_FOLDERS = ["cache", "saves", "screenshots", "logs"]


# ------------------------------------------------------------------------------------------------ small helpers

def read_list(name):
    with open(os.path.join(DATA, name), encoding="utf-8") as f:
        return [line.rstrip("\n") for line in f if line.strip()]


def step(text):
    print(f"  {text}", flush=True)


def check_morrowind(morrowind):
    problems = []
    data_files = os.path.join(morrowind, "Data Files")
    if not os.path.isdir(data_files):
        return [f'No "Data Files" folder in {morrowind}.']
    for name in CONTENT + ARCHIVES:
        if not os.path.isfile(os.path.join(data_files, name)):
            problems.append(f"{name} is missing from Data Files.")
    for folder in ("Sound", "Music"):
        if not os.path.isdir(os.path.join(data_files, folder)):
            problems.append(f"The {folder} folder is missing from Data Files.")
    if not os.path.isfile(os.path.join(morrowind, "Morrowind.ini")):
        problems.append("Morrowind.ini is missing.")
    if not problems:
        language = esm_language(os.path.join(data_files, "Morrowind.esm"))
        if not language:
            problems.append("This Morrowind's language isn't recognised: this version needs the English game.")
        elif language != "English":
            problems.append(f"This Morrowind is in {language}: this version needs the English game. Other "
                            "languages come in a later version.")
    return problems


def esm_language(esm):
    """The language of a Morrowind.esm, from its own interface words (the game settings sYes, sNo, sCancel)."""
    words = {}
    with open(esm, "rb") as f:
        for _ in range(20000):
            header = f.read(16)
            if len(header) < 16 or len(words) == 3:
                break
            size = struct.unpack_from("<I", header, 4)[0]
            if header[:4] != b"GMST":
                f.seek(size, 1)
                continue
            data = f.read(size)
            gid = value = b""
            p = 0
            while p + 8 <= len(data):
                kind, length = data[p:p + 4], struct.unpack_from("<I", data, p + 4)[0]
                if kind == b"NAME":
                    gid = data[p + 8:p + 8 + length].rstrip(b"\0")
                elif kind == b"STRV":
                    value = data[p + 8:p + 8 + length].rstrip(b"\0")
                p += 8 + length
            if gid in (b"sYes", b"sNo", b"sCancel"):
                words[gid] = value
    if words.get(b"sYes") == b"Yes" and words.get(b"sNo") == b"No" and words.get(b"sCancel") == b"Cancel":
        return "English"
    return {b"Ja": "German", b"Oui": "French", b"S\xec": "Italian", b"S\xed": "Spanish", b"Tak": "Polish",
            b"\xc4\xe0": "Russian"}.get(words.get(b"sYes"), "")


# ------------------------------------------------------------------------------------------------ openmw.cfg

def read_fallbacks(ini_path):
    """OpenMW's fallback values from Morrowind.ini, as its mwiniimporter writes them."""
    values = {}
    section = ""
    with open(ini_path, encoding="cp1252", errors="replace") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith(";"):
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                continue
            if "=" not in line:
                continue
            key, value = line.split("=", 1)
            key, value = key.strip(), value.strip()
            if key:
                values.setdefault(f"{section}:{key}", []).append(value)
    lines = []
    for key in read_list("fallback_keys.txt"):
        for value in values.get(key, []):
            lines.append(f"fallback={key.replace(' ', '_').replace(':', '_')},{value}")
    return lines


def write_config(morrowind, openmw):
    cfg = ['# OpenMW 3DS user configuration (written by the setup script)',
           'data="/openmw/data"', 'data="/openmw/ui3ds"']
    cfg += [f"content={c}" for c in CONTENT]
    cfg += [f"fallback-archive={a}" for a in ARCHIVES] + ["fallback-archive=sound3ds.bsa", "encoding=win1252",
                                                          "# fallback values imported from Morrowind.ini"]
    cfg += read_fallbacks(os.path.join(morrowind, "Morrowind.ini"))
    with open(os.path.join(openmw, "config", "openmw.cfg"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(cfg) + "\n")


# ------------------------------------------------------------------------------------------------ sound3ds.bsa

def tes3_hash(name):
    data = name.encode("latin-1")
    half = len(data) >> 1
    low = 0
    off = 0
    for i in range(half):
        low ^= (data[i] << (off & 0x1F)) & 0xFFFFFFFF
        off += 8
    high = 0
    off = 0
    for i in range(half, len(data)):
        temp = (data[i] << (off & 0x1F)) & 0xFFFFFFFF
        high ^= temp
        n = temp & 0x1F
        high = ((high >> n) | (high << (32 - n))) & 0xFFFFFFFF if n else high
        off += 8
    return low, high


def pack_sounds(data_files, out_path):
    """Sound and Music in one TES3 archive: indexing ~7,200 loose files took 12.8 s on the console."""
    files = []
    for folder in ("Sound", "Music"):
        root = os.path.join(data_files, folder)
        for dirpath, _, names in os.walk(root):
            for f in names:
                if f.lower().endswith((".wav", ".mp3")):
                    path = os.path.join(dirpath, f)
                    files.append((os.path.relpath(path, data_files).replace("/", "\\").lower(), path))
    files.sort(key=lambda f: tes3_hash(f[0]))
    names = b""
    name_offsets = []
    for rel, _ in files:
        name_offsets.append(len(names))
        names += rel.encode("latin-1") + b"\0"
    sizes = [os.path.getsize(p) for _, p in files]
    tmp = out_path + ".part"
    with open(tmp, "wb") as out:
        out.write(struct.pack("<III", 0x100, 12 * len(files) + len(names), len(files)))
        offset = 0
        for size in sizes:
            out.write(struct.pack("<II", size, offset))
            offset += size
        for o in name_offsets:
            out.write(struct.pack("<I", o))
        out.write(names)
        for rel, _ in files:
            out.write(struct.pack("<II", *tes3_hash(rel)))
        for _, path in files:
            with open(path, "rb") as src:
                shutil.copyfileobj(src, out, 4 << 20)
    os.replace(tmp, out_path)


# ------------------------------------------------------------------------------------------------ the GPU's formats

GPU_RGBA8, GPU_ETC1, GPU_ETC1A4 = 0x0, 0xC, 0xD
BITS_PER_PIXEL = {GPU_RGBA8: 32, GPU_ETC1: 4, GPU_ETC1A4: 8}
# ETC1's modifier tables (a, b): a texel takes +a, +b, -a or -b (index 0..3) on its subblock's base colour.
ETC1_TABLES = np.array([(2, 8), (5, 17), (9, 29), (13, 42), (18, 60), (24, 80), (33, 106), (47, 183)], np.int32)
ETC1_MODIFIERS = np.stack([ETC1_TABLES[:, 0], ETC1_TABLES[:, 1], -ETC1_TABLES[:, 0], -ETC1_TABLES[:, 1]], axis=1)


def morton_order():
    """tex3ds's 8x8 tile order: slot i holds the texel order[i] (y * 8 + x)."""
    p = list(range(64))
    for e in ((2, 8, 16, 4), (3, 9, 17, 5), (6, 10, 24, 20), (7, 11, 25, 21), (14, 26, 28, 22), (15, 27, 29, 23),
              (34, 40, 48, 36), (35, 41, 49, 37), (38, 42, 56, 52), (39, 43, 57, 53), (46, 58, 60, 54),
              (47, 59, 61, 55)):
        p[e[0]], p[e[1]], p[e[2]], p[e[3]] = p[e[1]], p[e[2]], p[e[3]], p[e[0]]
    for a, b in ((12, 18), (13, 19), (44, 50), (45, 51)):
        p[a], p[b] = p[b], p[a]
    return np.array(p)


MORTON = morton_order()


def pica_levels(rgba, mipmaps):
    """The levels tex3ds encodes, as 8-bit RGBA: the image over transparent black (alpha-0 texels black), then
    each mip level the alpha-weighted average of the base level's blocks, truncated to 8 bits as tex3ds does."""
    q = rgba.astype(np.float64)
    alpha = q[..., 3]
    q[..., :3] = np.where(alpha[..., None] == 0, 0.0, q[..., :3] * 257.0)
    q[..., 3] = alpha * 257.0
    levels = [np.where(rgba[..., 3:4] == 0, np.concatenate([np.zeros_like(rgba[..., :3]), rgba[..., 3:]], -1), rgba)]
    h, w = rgba.shape[:2]
    while mipmaps and w > 8 and h > 8:
        w //= 2
        h //= 2
        f = rgba.shape[1] // w
        blocks = q.reshape(h, f, w, f, 4)
        weight = blocks[..., 3] / 65535.0
        weight_sum = weight.sum(axis=(1, 3))
        colour = (blocks[..., :3] * weight[..., None]).sum(axis=(1, 3))
        colour = np.where(weight_sum[..., None] > 0, colour / np.maximum(weight_sum, 1e-300)[..., None], 0.0)
        a = blocks[..., 3].mean(axis=(1, 3))
        level = np.concatenate([colour, a[..., None]], axis=-1)
        levels.append(np.clip(np.floor(level * 256.0 / 65536.0), 0, 255).astype(np.uint8))
    return levels


def encode_rgba8(level):
    h, w = level.shape[:2]
    tiles = level.reshape(h // 8, 8, w // 8, 8, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 64, 4)[:, MORTON]
    return tiles[..., ::-1].tobytes()  # A B G R


def etc1_blocks(level):
    """The 4x4 blocks in the GPU's order: 8x8 tiles row by row, in each the blocks top left, top right, bottom
    left, bottom right. (N, 4, 4, 4): y, x, RGBA."""
    h, w = level.shape[:2]
    blocks = level.reshape(h // 8, 2, 4, w // 8, 2, 4, 4).transpose(0, 3, 1, 4, 2, 5, 6)
    return blocks.reshape(-1, 4, 4, 4)


def best_table(texels, base):
    """For each block: the modifier table and the texels' indices closest to `texels` (N, 8, 3) around `base`
    (N, 3). Returns (error, table, indices (N, 8))."""
    best_err = None
    for t in range(8):
        candidates = np.clip(base[:, None, :] + ETC1_MODIFIERS[t][None, :, None], 0, 255)  # (N, 4, 3)
        diff = texels[:, :, None, :] - candidates[:, None, :, :]  # (N, 8, 4, 3)
        err = (diff * diff).sum(axis=-1)  # (N, 8, 4)
        index = err.argmin(axis=-1)
        total = np.take_along_axis(err, index[..., None], -1)[..., 0].sum(axis=-1)
        if best_err is None:
            best_err, best_t, best_index = total, np.zeros_like(total), index
        else:
            better = total < best_err
            best_err = np.where(better, total, best_err)
            best_t = np.where(better, t, best_t)
            best_index = np.where(better[:, None], index, best_index)
    return best_err, best_t, best_index


def encode_etc1(level, with_alpha):
    """ETC1 (ETC1A4: each block's 4-bit alpha first, the axes swapped), each block a little-endian 64-bit word."""
    blocks = etc1_blocks(level)
    n = blocks.shape[0]
    rgb = blocks[..., :3].astype(np.int32)  # (N, y, x, 3)
    # Each block takes the best of four encodings: individual or differential colours, split down or across.
    options = []
    best_err = None
    choice = np.zeros(n, np.int32)
    for flip in (0, 1):
        if flip == 0:  # left and right halves, 2x4 each; the texels in (x, y) order
            halves = [rgb[:, :, 0:2].transpose(0, 2, 1, 3), rgb[:, :, 2:4].transpose(0, 2, 1, 3)]
            coords = [[(x, y) for x in (0, 1) for y in range(4)], [(x, y) for x in (2, 3) for y in range(4)]]
        else:  # top and bottom halves, 4x2 each
            halves = [rgb[:, 0:2].transpose(0, 2, 1, 3), rgb[:, 2:4].transpose(0, 2, 1, 3)]
            coords = [[(x, y) for x in range(4) for y in (0, 1)], [(x, y) for x in range(4) for y in (2, 3)]]
        texels = [h.reshape(n, 8, 3) for h in halves]
        averages = [t.mean(axis=1) for t in texels]
        # Individual mode: two 4-bit colours.
        q4 = [np.clip(np.rint(a * 15.0 / 255.0), 0, 15).astype(np.int32) for a in averages]
        bases4 = [q * 17 for q in q4]
        # Differential mode: a 5-bit colour and a 3-bit difference.
        q5a = np.clip(np.rint(averages[0] * 31.0 / 255.0), 0, 31).astype(np.int32)
        q5b = np.clip(np.rint(averages[1] * 31.0 / 255.0), 0, 31).astype(np.int32)
        delta = np.clip(q5b - q5a, -4, 3)
        q5b = q5a + delta
        bases5 = [(q << 3) | (q >> 2) for q in (q5a, q5b)]
        for diff, bases in ((0, bases4), (1, bases5)):
            e1, t1, i1 = best_table(texels[0], bases[0])
            e2, t2, i2 = best_table(texels[1], bases[1])
            err = e1 + e2
            options.append(dict(flip=flip, diff=diff, t1=t1, t2=t2, i1=i1, i2=i2, coords=coords,
                                q=(q4 if diff == 0 else (q5a, delta))))
            if best_err is None:
                best_err = err
            else:
                better = err < best_err
                best_err = np.where(better, err, best_err)
                choice = np.where(better, len(options) - 1, choice)
    words = np.zeros(n, np.uint64)
    for k, o in enumerate(options):
        sel = choice == k
        if not sel.any():
            continue
        w = np.zeros(int(sel.sum()), np.uint64)
        if o["diff"] == 0:
            c1, c2 = o["q"][0][sel].astype(np.uint64), o["q"][1][sel].astype(np.uint64)
            w |= (c1[:, 0] << np.uint64(60)) | (c2[:, 0] << np.uint64(56)) | (c1[:, 1] << np.uint64(52)) \
                | (c2[:, 1] << np.uint64(48)) | (c1[:, 2] << np.uint64(44)) | (c2[:, 2] << np.uint64(40))
        else:
            c1 = o["q"][0][sel].astype(np.uint64)
            d = (o["q"][1][sel] & 7).astype(np.uint64)
            w |= (c1[:, 0] << np.uint64(59)) | (d[:, 0] << np.uint64(56)) | (c1[:, 1] << np.uint64(51)) \
                | (d[:, 1] << np.uint64(48)) | (c1[:, 2] << np.uint64(43)) | (d[:, 2] << np.uint64(40)) \
                | np.uint64(1 << 33)
        w |= (o["t1"][sel].astype(np.uint64) << np.uint64(37)) | (o["t2"][sel].astype(np.uint64) << np.uint64(34))
        w |= np.uint64(o["flip"] << 32)
        for half, indices in ((0, o["i1"][sel]), (1, o["i2"][sel])):
            for p, (x, y) in enumerate(o["coords"][half]):
                bit = np.uint64(x * 4 + y)
                index = indices[:, p].astype(np.uint64)
                w |= ((index & np.uint64(1)) << bit) | ((index >> np.uint64(1)) << (bit + np.uint64(16)))
        words[sel] = w
    etc = words.astype("<u8").view(np.uint8).reshape(n, 8)
    if not with_alpha:
        return etc.tobytes()
    a4 = np.floor(blocks[..., 3].astype(np.float64) * 257.0 * 16.0 / 65536.0).astype(np.uint8)  # (N, y, x)
    alpha = np.zeros((n, 8), np.uint8)
    for y in range(4):
        for x in range(4):
            alpha[:, 2 * x + y // 2] |= a4[:, y, x] << (4 if y & 1 else 0)
    return np.concatenate([alpha, etc], axis=1).tobytes()


def encode_pica(rgba, fmt, mipmaps):
    levels = pica_levels(rgba, mipmaps)
    data = b"".join(encode_rgba8(l) if fmt == GPU_RGBA8 else encode_etc1(l, fmt == GPU_ETC1A4) for l in levels)
    return data, len(levels)


# ------------------------------------------------------------------------------------------------ the textures
# The port's tools/cook_textures.py, step for step (its docstring explains each choice), with the encoder above.

FLAG_ALPHA, FLAG_ALPHA_BINARY, FLAG_PADDED, FLAG_ATLAS, FLAG_ATLAS_TRANSPOSED, FLAG_ATLAS_HALVED = 1, 2, 4, 8, 16, 32
ATLAS_HALVED_PREFIX = "textures/menu_icon_"
UI_PREFIXES = ("icons/", "bookart/", "textures/omw_", "textures/levelup/", "textures/birthsigns/")
UI_TEXTURE_NAMES = ("menu_", "tx_menu", "tx_cursor", "compass", "target", "scroll", "detect_", "menu")
UI_MAX_SIZE = 256
UI_BIG = {"textures/menu_morrowind": 512}
ATLAS_KEY = "ui-atlas/"
ATLAS_WIDTH, ATLAS_HEIGHT, ATLAS_GUTTER = 512, 256, 1
ATLAS_EXCLUDED = ("icons/", "bookart/", "textures/levelup/", "textures/birthsigns/")
ATLAS_MAX_SIDE = 512
ATLAS_MAX_AREA = 128 * 64
EXCLUDED = ("textures/omw_map_color_palette", "textures/omw/water_nm")
GLYPH_PREFIX = "textures/omw_steam_button_"
GLYPH_SIZE = 32
PREFIXES = ("textures/", "icons/", "bookart/")
IMAGE_EXTENSIONS = (".dds", ".tga", ".bmp", ".png", ".jpg", ".jpeg")
MAX_SIZE = 256


def fnv1a64(text):
    h = 0xCBF29CE484222325
    for b in text.encode("utf-8"):
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def normalize(path):
    return path.replace("\\", "/").lower()


def stem_key(path):
    return os.path.splitext(path)[0]


def read_bsa_index(path):
    with open(path, "rb") as f:
        version, hash_offset, count = struct.unpack("<III", f.read(12))
        if version != 0x100:
            raise ValueError(f"{path}: not a Morrowind archive")
        records = [struct.unpack("<II", f.read(8)) for _ in range(count)]
        name_offsets = struct.unpack(f"<{count}I", f.read(4 * count))
        names = f.read(hash_offset - 12 * count)
    data_start = 12 + hash_offset + 8 * count
    files = {}
    for (size, offset), name_offset in zip(records, name_offsets):
        end = names.index(b"\0", name_offset)
        files[normalize(names[name_offset:end].decode("latin-1"))] = (path, data_start + offset, size)
    return files


def collect_sources(data_files, engine_vfs, port_interface):
    """name -> (file, offset, size) in the VFS order (OpenMW's resources, the game's archives, its loose files, the
    port's interface), then one texture per key, .dds preferred."""
    sources = {}

    def walk(folder):
        if not os.path.isdir(folder):
            return
        for root, _, files in os.walk(folder):
            for file in files:
                full = os.path.join(root, file)
                name = normalize(os.path.relpath(full, folder))
                if name.startswith(PREFIXES) and name.endswith(IMAGE_EXTENSIONS):
                    sources[name] = (full, 0, os.path.getsize(full))

    walk(engine_vfs)
    for archive in ARCHIVES:
        for name, src in read_bsa_index(os.path.join(data_files, archive)).items():
            if name.startswith(PREFIXES) and name.endswith(IMAGE_EXTENSIONS):
                sources[name] = src
    walk(data_files)
    walk(port_interface)
    by_key = {}
    for name in sorted(sources):
        key = stem_key(name)
        if key.startswith(EXCLUDED):
            continue
        if key not in by_key or name.endswith(".dds"):
            by_key[key] = (name, sources[name])
    return by_key


def referenced_textures(folders):
    names = set(read_list("ui_code_textures.txt"))
    pattern = re.compile(r'texture\s*=\s*"([^"]+)"', re.IGNORECASE)
    for folder in folders:
        if not os.path.isdir(folder):
            continue
        for file in os.listdir(folder):
            if file.endswith((".xml", ".layout", ".skin")):
                with open(os.path.join(folder, file), encoding="utf-8", errors="replace") as f:
                    for match in pattern.finditer(f.read()):
                        names.add(stem_key(normalize(match.group(1))))
    return names


def is_ui(name):
    if name.startswith(UI_PREFIXES):
        return True
    if name.startswith("textures/") and "/" not in name[len("textures/"):]:
        return name[len("textures/"):].startswith(UI_TEXTURE_NAMES)
    return False


def is_atlas_candidate(key, width, height):
    if key.startswith(ATLAS_EXCLUDED):
        return False
    return max(width, height) <= ATLAS_MAX_SIDE and width * height <= ATLAS_MAX_AREA


def pow2_size(n, max_size):
    p = 8
    while p < n and p < max_size:
        p *= 2
    if p > 8 and p > n and (p - n) > (n - p // 2):
        p //= 2
    return max(8, min(p, max_size))


def pow2_ceil(n, max_size):
    p = 8
    while p < n and p < max_size:
        p *= 2
    return p


def bleed_transparent(rgba, keep_alpha_zero):
    a = np.asarray(rgba).astype(np.float32)
    clear = a[..., 3] == 0
    if not clear.any() or clear.all():
        return rgba
    weight = a[..., 3] / 255.0
    color = a[..., :3] * weight[..., None]
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
    image_type = raw[2]
    depth = raw[7] if image_type in (1, 9) else raw[16]
    return depth == 16 and (raw[17] & 0x0F) == 1


def cook_one(job):
    key, name, raw, referenced = job
    image = Image.open(io.BytesIO(raw))
    image.load()
    if key.startswith(GLYPH_PREFIX) and max(image.size) > GLYPH_SIZE:
        image = image.resize((GLYPH_SIZE, GLYPH_SIZE), Image.Resampling.LANCZOS)
    src_w, src_h = image.size
    ui = is_ui(key) or referenced
    if name.endswith(".tga") and tga_kill_alpha(raw):
        image = image.convert("RGB")
    rgba = image.convert("RGBA")
    alpha = rgba.getchannel("A")
    lo, _ = alpha.getextrema()
    has_alpha = lo < 255
    flags = 0
    if has_alpha:
        flags |= FLAG_ALPHA
        if sum(alpha.histogram()[1:255]) == 0:
            flags |= FLAG_ALPHA_BINARY
    resampled = False
    if ui:
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
            texels = np.pad(np.asarray(rgba), ((0, h - src_h), (0, w - src_w), (0, 0)), mode="edge")
            rgba = Image.fromarray(texels, "RGBA")
            flags |= FLAG_PADDED
    else:
        w, h = pow2_size(src_w, MAX_SIZE), pow2_size(src_h, MAX_SIZE)
        if (w, h) != (src_w, src_h):
            rgba = rgba.resize(
                (w, h), Image.Resampling.LANCZOS if w < src_w or h < src_h else Image.Resampling.BICUBIC)
        small_raw = name.endswith(".dds") and raw[:4] == b"DDS " and raw[84:88] == b"\0\0\0\0" and max(w, h) <= 128
        fmt = GPU_RGBA8 if small_raw else (GPU_ETC1A4 if has_alpha else GPU_ETC1)
        if has_alpha:
            rgba = bleed_transparent(rgba, keep_alpha_zero=(fmt == GPU_RGBA8))
    atlas_texels = None
    if ui and referenced and is_atlas_candidate(key, src_w, src_h):
        atlas_texels = np.array(rgba) if resampled else np.array(rgba)[:src_h, :src_w].copy()
        if key.startswith(ATLAS_HALVED_PREFIX) and max(src_w, src_h) > 32:
            half = Image.fromarray(atlas_texels, "RGBA").resize((src_w // 2, src_h // 2), Image.Resampling.LANCZOS)
            atlas_texels = np.array(half)
            flags |= FLAG_ATLAS_HALVED
        if atlas_texels.shape[0] > atlas_texels.shape[1] and atlas_texels.shape[0] > 64:
            atlas_texels = np.ascontiguousarray(atlas_texels.transpose(1, 0, 2))
            flags |= FLAG_ATLAS_TRANSPOSED
        flags |= FLAG_ATLAS
    flipped = np.asarray(rgba.transpose(Image.Transpose.FLIP_TOP_BOTTOM).convert("RGBA"))
    data, levels = encode_pica(flipped, fmt, not ui)
    return key, data, w, h, src_w, src_h, fmt, levels, flags, atlas_texels


def atlas_gutters(width, height):
    return (0 if width >= ATLAS_WIDTH else ATLAS_GUTTER), (0 if height >= ATLAS_HEIGHT else ATLAS_GUTTER)


def pack_atlas(cells):
    free = [(0, 0, ATLAS_WIDTH, ATLAS_HEIGHT)]
    sized = []
    for key, texels in cells:
        h, w = texels.shape[:2]
        gx, gy = atlas_gutters(w, h)
        sized.append((key, w + 2 * gx, h + 2 * gy, gx, gy))
    placements = {}
    transposed = set()
    for key, w, h, gx, gy in sorted(sized, key=lambda c: (0 if c[1] > ATLAS_WIDTH // 4 else 1, -c[2], -c[1], c[0])):
        best = None
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
            raise RuntimeError(f"the UI atlas is full at {key}")
        if best[2]:
            transposed.add(key)
            w, h, gx, gy = h, w, gy, gx
        fx, fy, fw, fh = free.pop(best[1])
        placements[key] = (fx + gx, fy + gy)
        if fw - w > fh - h:
            free.append((fx + w, fy, fw - w, fh))
            free.append((fx, fy + h, w, fh - h))
        else:
            free.append((fx, fy + h, fw, fh - h))
            free.append((fx + w, fy, fw - w, h))
        free = [r for r in free if r[2] > 0 and r[3] > 0]
    return placements, transposed


def cook_textures(data_files, port, out_path, jobs):
    engine_vfs = os.path.join(port, "openmw", "resources", "vfs")
    port_interface = os.path.join(port, "openmw", "ui3ds")
    sources = collect_sources(data_files, engine_vfs, port_interface)
    referenced = referenced_textures([os.path.join(engine_vfs, "mygui"), os.path.join(port_interface, "mygui")])
    referenced.update(key for key in sources if key.startswith(ATLAS_HALVED_PREFIX))
    keys = sorted(sources)

    def work():
        handles = {}
        for key in keys:
            name, (path, offset, size) = sources[key]
            f = handles.get(path) or handles.setdefault(path, open(path, "rb"))
            f.seek(offset)
            yield key, name, f.read(size), key in referenced
        for f in handles.values():
            f.close()

    results = []
    with ProcessPoolExecutor(max_workers=jobs) as pool:
        for i, result in enumerate(pool.map(cook_one, work(), chunksize=8)):
            results.append(result)
            if (i + 1) % 250 == 0 or i + 1 == len(keys):
                print(f"\r  Preparing the textures: {i + 1} / {len(keys)}", end="", flush=True)
    print()

    cells = [(r[0], r[9]) for r in results if r[9] is not None]
    placements, turned = pack_atlas(cells)
    canvas = np.zeros((ATLAS_HEIGHT, ATLAS_WIDTH, 4), dtype=np.uint8)
    for key, texels in cells:
        if key in turned:
            texels = np.ascontiguousarray(texels.transpose(1, 0, 2))
        x, y = placements[key]
        h, w = texels.shape[:2]
        gx, gy = atlas_gutters(w, h)
        canvas[y - gy:y + h + gy, x - gx:x + w + gx] = np.pad(texels, ((gy, gy), (gx, gx), (0, 0)), mode="edge")
    entries = []
    for key, data, w, h, src_w, src_h, fmt, levels, flags, _ in results:
        x, y = placements.get(key, (0, 0))
        if key in turned:
            flags |= FLAG_ATLAS_TRANSPOSED
        entries.append((fnv1a64(key), key, data, w, h, src_w, src_h, fmt, levels, flags, 0, x | (y << 16)))
    atlas_data, _ = encode_pica(canvas[::-1].copy(), GPU_RGBA8, False)
    key = f"{ATLAS_KEY}0"
    entries.append((fnv1a64(key), key, atlas_data, ATLAS_WIDTH, ATLAS_HEIGHT, ATLAS_WIDTH, ATLAS_HEIGHT, GPU_RGBA8, 1,
                    FLAG_ALPHA, 0, 0))
    entries.sort(key=lambda e: e[0])
    for a, b in zip(entries, entries[1:]):
        if a[0] == b[0]:
            raise RuntimeError(f"texture name hash collision: {a[1]} / {b[1]}")

    align = 128
    index_offset = 32
    data_offset = (index_offset + 32 * len(entries) + align - 1) // align * align
    tmp = out_path + ".part"
    with open(tmp, "wb") as f:
        f.write(struct.pack("<4sIIII3I", b"O3TP", 4, len(entries), index_offset, data_offset, 0, 0, 0))
        offset = data_offset
        index = bytearray()
        for hash_, key, data, w, h, src_w, src_h, fmt, levels, flags, atlas, pos in entries:
            index += struct.pack("<QIIHHHHBBBBI", hash_, offset, len(data), w, h, src_w, src_h, fmt, levels, flags,
                                 atlas, pos)
            offset = (offset + len(data) + align - 1) // align * align
        f.write(index)
        for entry in entries:
            f.seek((f.tell() + align - 1) // align * align)
            f.write(entry[2])
    os.replace(tmp, out_path)
    return len(entries)


# ------------------------------------------------------------------------------------------------ main

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--morrowind", required=True, help="the Morrowind folder (Morrowind.ini and Data Files)")
    parser.add_argument("--output", default="", help="where to put the folder to copy to the SD card (default: the "
                        "port folder itself, the game's files added to it)")
    default_port = os.path.join(HERE, "port")
    if not os.path.isdir(default_port):
        default_port = os.path.join(HERE, "..", "port")  # python/ next to port/
    parser.add_argument("--port", default=default_port, help="the release's port files")
    parser.add_argument("--version", default="0.1.1")
    parser.add_argument("--jobs", type=int, default=os.cpu_count(), help="processes for the textures")
    args = parser.parse_args()

    problems = check_morrowind(args.morrowind)
    if problems:
        sys.exit(problems[0])
    if not os.path.isdir(args.port):
        sys.exit(f"The port's files are missing: {args.port}")
    # As the app: the port folder by default, else a folder of its own inside the one given.
    if not args.output:
        args.output = args.port
    elif not (os.path.isdir(args.output) and os.path.samefile(args.output, args.port)):
        name = f"OpenMW-3DS-{args.version}"
        if os.path.basename(os.path.normpath(args.output)) != name:
            args.output = os.path.join(args.output, name)
    same_as_port = os.path.isdir(args.output) and os.path.samefile(args.output, args.port)
    started = time.time()
    data_files = os.path.join(args.morrowind, "Data Files")
    openmw = os.path.join(args.output, "openmw")
    for folder in EMPTY_FOLDERS + ["config", "data", "cooked"]:
        os.makedirs(os.path.join(openmw, folder), exist_ok=True)

    if not same_as_port:
        step("Copying the port's files")
        shutil.copytree(args.port, args.output, dirs_exist_ok=True)
    step("Reading Morrowind.ini")
    write_config(args.morrowind, openmw)
    step("Packing the sounds")
    pack_sounds(data_files, os.path.join(openmw, "data", "sound3ds.bsa"))
    for name in CONTENT + ARCHIVES:
        step(f"Copying {name}")
        shutil.copyfile(os.path.join(data_files, name), os.path.join(openmw, "data", name))
    count = cook_textures(data_files, args.port, os.path.join(openmw, "cooked", "textures.o3tp"), args.jobs)
    with open(os.path.join(openmw, "VERSION"), "w", encoding="utf-8", newline="\n") as f:
        f.write(args.version + "\n")
    print(f"\nReady in {args.output} ({time.time() - started:.0f} s)")
    print("1. Copy its contents to your SD card's root (slow over Wi-Fi for now).")
    print("2. Install cias/openmw3ds.cia with FBI (https://github.com/Steveice10/FBI/releases/latest) or Cement "
          "(https://github.com/Synaelle/cement/releases/latest).")
    print("3. Start OpenMW. The first launch takes a while.")


if __name__ == "__main__":
    main()
