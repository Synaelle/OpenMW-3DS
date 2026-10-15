#!/usr/bin/env python3
"""Packs Morrowind's loose Sound and Music folders into one TES3 BSA (sound3ds.bsa) for the 3DS.

OpenMW indexes every loose file of its data folders at startup: Morrowind's ~7,200 sound files took 12.8 s and
3.3 MB of heap to index in Azahar (the 3DS SD card lists directories slowly). A BSA's index is read in one go, and
the tester copies one file instead of 7,200. OpenMW reads TES3 BSAs natively (fallback-archive=sound3ds.bsa).

    python tools/pack_sound_bsa.py "<Morrowind>/Data Files" out/cooked/sound3ds.bsa

The Morrowind data is only read. Format (components/bsa/bsafile.cpp): a 12-byte header (0x100, directory size,
file count), per file (size, offset), per file a name offset, the names, per file a hash, then the file data.
"""

import os
import struct
import sys

FOLDERS = ('Sound', 'Music')


def tes3_hash(name):
    """The TES3 BSA name hash (getHash in components/bsa/bsafile.cpp), on the lowercase backslash path."""
    data = name.encode('latin-1')
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


def collect(data_files):
    files = []
    for folder in FOLDERS:
        root = os.path.join(data_files, folder)
        if not os.path.isdir(root):
            raise SystemExit(f'no {folder} folder in {data_files}')
        for dirpath, _, names in os.walk(root):
            for f in names:
                if not f.lower().endswith(('.wav', '.mp3')):
                    continue
                path = os.path.join(dirpath, f)
                rel = os.path.relpath(path, data_files).replace('/', '\\').lower()
                files.append((rel, path))
    return files


def pack(data_files, out_path):
    files = collect(data_files)
    files.sort(key=lambda f: tes3_hash(f[0]))
    names = b''
    name_offsets = []
    for rel, _ in files:
        name_offsets.append(len(names))
        names += rel.encode('latin-1') + b'\0'
    count = len(files)
    dir_size = 12 * count + len(names)

    sizes = [os.path.getsize(path) for _, path in files]
    offsets = []
    total = 0
    for size in sizes:
        offsets.append(total)
        total += size
    if 12 + dir_size + 8 * count + total >= 1 << 32:
        raise SystemExit('archive larger than 4 GB')

    tmp = out_path + '.part'
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(tmp, 'wb') as out:
        out.write(struct.pack('<III', 0x100, dir_size, count))
        for size, offset in zip(sizes, offsets):
            out.write(struct.pack('<II', size, offset))
        for offset in name_offsets:
            out.write(struct.pack('<I', offset))
        out.write(names)
        for rel, _ in files:
            out.write(struct.pack('<II', *tes3_hash(rel)))
        for (rel, path), size in zip(files, sizes):
            with open(path, 'rb') as src:
                data = src.read()
            if len(data) != size:
                raise SystemExit(f'{path} changed while packing')
            out.write(data)
    os.replace(tmp, out_path)
    print(f'{out_path}: {count} files, {total / 1e6:.1f} MB of data, index {(dir_size + 8 * count) / 1024:.0f} KB')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    pack(sys.argv[1], sys.argv[2])
