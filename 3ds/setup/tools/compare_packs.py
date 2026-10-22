#!/usr/bin/env python3
"""Compares two cooked texture packs (.o3tp) entry by entry: the setup app's against the port's Python cooker's.

    python tools/compare_packs.py reference.o3tp candidate.o3tp [--show N]

Prints how many entries match byte for byte, and for the others which fields differ (size, format, flags, atlas
position, data) with the first few examples.
"""

import argparse
import struct
from collections import Counter

FORMATS = {0x0: "RGBA8", 0xC: "ETC1", 0xD: "ETC1A4"}


def read_pack(path):
    with open(path, "rb") as f:
        blob = f.read()
    magic, version, count, index_offset, data_offset = struct.unpack_from("<4sIIII", blob, 0)
    if magic != b"O3TP":
        raise SystemExit(f"{path}: not a texture pack")
    entries = {}
    for i in range(count):
        (h, offset, size, w, hgt, sw, sh, fmt, levels, flags, atlas, pos) = struct.unpack_from(
            "<QIIHHHHBBBBI", blob, index_offset + 32 * i)
        entries[h] = dict(w=w, h=hgt, sw=sw, sh=sh, fmt=fmt, levels=levels, flags=flags, atlas=atlas, pos=pos,
                          data=blob[offset:offset + size])
    return version, entries


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("reference")
    parser.add_argument("candidate")
    parser.add_argument("--show", type=int, default=8)
    parser.add_argument("--keys", default="", help="the setup app's key list (cook --keys): names in the examples")
    args = parser.parse_args()
    names = {}
    if args.keys:
        with open(args.keys, encoding="utf-8") as f:
            for line in f:
                h, key = line.rstrip("\n").split(" ", 1)
                names[int(h, 16)] = key
    _, ref = read_pack(args.reference)
    _, cand = read_pack(args.candidate)
    missing = set(ref) - set(cand)
    extra = set(cand) - set(ref)
    print(f"reference {len(ref)} entries, candidate {len(cand)}; missing {len(missing)}, extra {len(extra)}")
    same = 0
    reasons = Counter()
    by_format = Counter()
    examples = []
    for h in sorted(set(ref) & set(cand)):
        a, b = ref[h], cand[h]
        diffs = [k for k in ("w", "h", "sw", "sh", "fmt", "levels", "flags", "atlas", "pos") if a[k] != b[k]]
        if a["data"] != b["data"]:
            diffs.append("data")
        if not diffs:
            same += 1
            continue
        reasons[",".join(diffs)] += 1
        by_format[FORMATS.get(a["fmt"], a["fmt"])] += 1
        if len(examples) < args.show:
            first = next((i for i, (x, y) in enumerate(zip(a["data"], b["data"])) if x != y), None)
            examples.append((h, diffs, {k: (a[k], b[k]) for k in diffs if k != "data"}, first, len(a["data"])))
    print(f"identical: {same} of {len(set(ref) & set(cand))}")
    for reason, count in reasons.most_common():
        print(f"  {count:5d} differ in {reason}")
    for fmt, count in by_format.most_common():
        print(f"  {count:5d} of the differing are {fmt}")
    for h, diffs, fields, first, size in examples:
        print(f"  {names.get(h, f'{h:016x}')}: {diffs} {fields} first data byte differing at {first} of {size}")


if __name__ == "__main__":
    main()
