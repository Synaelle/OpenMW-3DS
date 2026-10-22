#!/usr/bin/env python3
"""Traces the HOME Menu emblem's outline (packaging/source/morrowind_emblem.png, its alpha) into closed polygons for
the 3D banner model (tools/make_banner_model.py builds it in Blender from them):

    python tools/trace_emblem.py out.json [--texture texture.png]

Marching squares on the alpha at half coverage, with the crossing points interpolated along the cell edges (smooth
outlines, no pixel steps), then Douglas-Peucker simplification to TOLERANCE pixels. The polygons are in image pixels
(x right, y down), each a list of [x, y], outer outlines and holes alike (Blender's curve fill tells them apart).
--texture also writes the model's colour texture: the emblem at TEXTURE_SIZE over dark bronze (the sides and the
bevel, projected from the front, sample the bronze).
"""

import argparse
import json
import os

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMBLEM = os.path.join(ROOT, "packaging", "source", "morrowind_emblem.png")
LEVEL = 0.5
TOLERANCE = 0.6  # pixels of the 256x256 image
MIN_POINTS = 6  # smaller loops are specks
TEXTURE_SIZE = 128
BRONZE = (92, 72, 40)


def segments(alpha):
    """Marching squares: the contour's segments, as pairs of points, cell by cell."""
    h, w = alpha.shape
    a = np.pad(alpha, 1)  # closed outlines at the image's edges
    out = []

    def cross(p, q, vp, vq):
        t = (LEVEL - vp) / (vq - vp)
        return (p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t)

    for y in range(h + 1):
        for x in range(w + 1):
            v = [a[y, x], a[y, x + 1], a[y + 1, x + 1], a[y + 1, x]]  # tl, tr, br, bl
            corners = [(x, y), (x + 1, y), (x + 1, y + 1), (x, y + 1)]
            idx = sum(1 << i for i in range(4) if v[i] >= LEVEL)
            if idx in (0, 15):
                continue
            edges = []
            for i in range(4):
                j = (i + 1) % 4
                if (v[i] >= LEVEL) != (v[j] >= LEVEL):
                    edges.append(cross(corners[i], corners[j], v[i], v[j]))
            if len(edges) == 2:
                out.append((edges[0], edges[1]))
            elif len(edges) == 4:  # saddle: pair by the centre's value
                centre = sum(v) / 4
                if (centre >= LEVEL) == (v[0] >= LEVEL):
                    out += [(edges[0], edges[1]), (edges[2], edges[3])]
                else:
                    out += [(edges[0], edges[3]), (edges[1], edges[2])]
    return out


def chain(segs):
    """Joins segments into closed loops."""
    key = lambda p: (round(p[0], 4), round(p[1], 4))
    neighbours = {}
    for a, b in segs:
        neighbours.setdefault(key(a), []).append(key(b))
        neighbours.setdefault(key(b), []).append(key(a))
    seen = set()
    loops = []
    for start in neighbours:
        if start in seen:
            continue
        loop = [start]
        seen.add(start)
        prev, cur = None, start
        while True:
            nxt = [n for n in neighbours[cur] if n != prev and n not in seen]
            if not nxt:
                break
            prev, cur = cur, nxt[0]
            seen.add(cur)
            loop.append(cur)
        if len(loop) >= MIN_POINTS:
            loops.append(loop)
    return loops


def simplify(points, tol):
    """Douglas-Peucker on a closed loop (split at its two farthest points)."""
    def dp(pts):
        if len(pts) < 3:
            return pts
        a, b = np.array(pts[0]), np.array(pts[-1])
        ab = b - a
        n = np.linalg.norm(ab)
        best, index = -1.0, 0
        for i in range(1, len(pts) - 1):
            p = np.array(pts[i])
            d = abs(ab[0] * (p - a)[1] - ab[1] * (p - a)[0]) / n if n > 0 else np.linalg.norm(p - a)
            if d > best:
                best, index = d, i
        if best <= tol:
            return [pts[0], pts[-1]]
        return dp(pts[: index + 1])[:-1] + dp(pts[index:])

    far = max(range(len(points)), key=lambda i: (points[i][0] - points[0][0]) ** 2 + (points[i][1] - points[0][1]) ** 2)
    first = dp(points[: far + 1])
    second = dp(points[far:] + [points[0]])
    return first[:-1] + second[:-1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("out")
    parser.add_argument("--texture", default="", help="also write the model's colour texture as this PNG")
    args = parser.parse_args()
    if args.texture:
        emblem = Image.open(EMBLEM).convert("RGBA").resize((TEXTURE_SIZE, TEXTURE_SIZE), Image.LANCZOS)
        texture = Image.new("RGBA", (TEXTURE_SIZE, TEXTURE_SIZE), BRONZE + (255,))
        texture.alpha_composite(emblem)
        texture.convert("RGB").save(args.texture)
    alpha = np.asarray(Image.open(EMBLEM).convert("RGBA"), dtype=np.float32)[:, :, 3] / 255.0
    loops = [simplify(loop, TOLERANCE) for loop in chain(segments(alpha))]
    loops = [l for l in loops if len(l) >= 3]
    # Back to pixel coordinates (the padding shifted everything by one).
    loops = [[[round(float(x) - 1, 3), round(float(y) - 1, 3)] for x, y in loop] for loop in loops]
    json.dump({"size": list(alpha.shape[::-1]), "loops": loops}, open(args.out, "w"))
    print(f"{len(loops)} loops, {sum(len(l) for l in loops)} points")


if __name__ == "__main__":
    main()
