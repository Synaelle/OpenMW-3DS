#!/usr/bin/env python3
"""Builds the HOME Menu assets of the 3DS builds (.cia and .3dsx): the icon, the 3D banner and its sound.

    python packaging/make_assets.py [--preview] [--dorsal 1|-1]

Icon: OpenMW's own icon (openmw/files/windows/openmw.ico, its hand-tuned 48 px frame) -> packaging/icon.png.
Banner: a Cliff Racer turning on a turntable, plus the swoosh sound.

Inputs (packaging/banner/source/):
  cliff_racer_old.zip  glTF model (Sketchfab export of Morrowind's old cliff racer)
  swoosh_1.wav         sound played when the title is selected
Outputs (committed, used by cmake/Ctr3dsPackaging.cmake through bannertool):
  packaging/banner/banner.cgfx  the 3D banner scene (CGFX, converted by pycgfx from build/banner/banner.gltf)
  packaging/banner/banner.wav   the sound as 16-bit stereo (banner sounds must be stereo, at most 3 s)
--preview renders what the HOME Menu camera sees at a few angles with Blender (build/banner/preview.png).

The model is rebuilt into this node chain (glTF, Y up; the banner camera sits at (0, 1, 44.8) looking at -Z):
  Banner     static: stands the turntable's Z axis up, tilted toward the camera so the wings are visible
  Turntable  animated: one turn about its own Z axis every 8 s. Z, not Y: pycgfx turns quaternions into XYZ
             Euler angles, and a rotation about Y flips representation at 90 degrees (a one-frame glitch);
             about Z the angle unwraps cleanly.
  Pose       static: centres the creature, orients it (body along X, wings along Y, back up), scales it to fit
  meshes     the original geometry, untouched
Textures are shrunk (pycgfx stores RGBA4 without mipmaps; the CGFX must stay under 512 KB).
"""

import argparse
import io
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import wave
import zipfile

from PIL import Image

PACKAGING = os.path.dirname(os.path.abspath(__file__))
HERE = os.path.join(PACKAGING, "banner")
ROOT = os.path.dirname(PACKAGING)
SOURCE_ZIP = os.path.join(HERE, "source", "cliff_racer_old.zip")
SOURCE_WAV = os.path.join(HERE, "source", "swoosh_1.wav")
SOURCE_ICON = os.path.join(ROOT, "openmw", "files", "windows", "openmw.ico")
WORK = os.path.join(ROOT, "build", "banner")
PYCGFX = os.path.join(ROOT, ".tools", "pycgfx")
BLENDER = r"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"

TURN_SECONDS = 8.0
SAMPLES_PER_SECOND = 30
TILT_DEGREES = 25.0  # turntable axis tilted toward the camera
RADIUS = 15.0  # horizontal radius of the creature on the turntable (the view is ~40 x 24 units at z = 0)
HEIGHT = 0.0  # centre height (the camera is at y = 1)
DIFFUSE_SIZE = 256
NORMAL_SIZE = 128
CGFX_LIMIT = 512 * 1024


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    )


def quat_axis(axis, degrees):
    half = math.radians(degrees) / 2
    s = math.sin(half)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half))


def quat_from_matrix(m):
    """m: 3x3 rotation, rows. Returns (x, y, z, w)."""
    trace = m[0][0] + m[1][1] + m[2][2]
    if trace > 0:
        s = math.sqrt(trace + 1.0) * 2
        return ((m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25 * s)
    if m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        return (0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s)
    if m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        return ((m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s)
    s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
    return ((m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s, (m[1][0] - m[0][1]) / s)


def read_positions(gltf, bin_data, accessor_index):
    acc = gltf["accessors"][accessor_index]
    view = gltf["bufferViews"][acc["bufferView"]]
    assert acc["componentType"] == 5126 and acc["type"] == "VEC3"
    stride = view.get("byteStride", 12)
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    return [struct.unpack_from("<3f", bin_data, base + i * stride) for i in range(acc["count"])]


def build_gltf(dorsal):
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    with zipfile.ZipFile(SOURCE_ZIP) as z:
        gltf = json.loads(z.read("Cliff_Racer_(old).gltf"))
        bin_name = gltf["buffers"][0]["uri"]
        bin_data = z.read(bin_name)
        images = {img["uri"]: z.read(img["uri"]) for img in gltf["images"]}

    mesh_nodes = [i for i, n in enumerate(gltf["nodes"]) if "mesh" in n]
    positions = []
    for i in mesh_nodes:
        for prim in gltf["meshes"][gltf["nodes"][i]["mesh"]]["primitives"]:
            positions += read_positions(gltf, bin_data, prim["attributes"]["POSITION"])

    # Mesh frame: wings along X, body along Y (head at -Y), back/belly along Z.
    lo = [min(p[k] for p in positions) for k in range(3)]
    hi = [max(p[k] for p in positions) for k in range(3)]
    centre = [(lo[k] + hi[k]) / 2 for k in range(3)]
    radius = max(math.hypot(p[0] - centre[0], p[1] - centre[1]) for p in positions)
    scale = RADIUS / radius
    # Rotation mesh -> turntable frame (rows = turntable axes): body (-Y, head) to +X, wings to Y, back to +Z.
    if dorsal > 0:
        rot = [[0, -1, 0], [1, 0, 0], [0, 0, 1]]  # X->Y, Y->-X, Z->Z
    else:
        rot = [[0, -1, 0], [-1, 0, 0], [0, 0, -1]]  # X->-Y, Y->-X, Z->-Z
    rc = [sum(rot[r][c] * centre[c] for c in range(3)) for r in range(3)]
    pose = {
        "name": "Pose",
        "translation": [-scale * v for v in rc],
        "rotation": list(quat_from_matrix(rot)),
        "scale": [scale, scale, scale],
        "children": [],
    }
    print(f"model: {len(positions)} vertices, bounds {[round(v) for v in lo]}..{[round(v) for v in hi]}, "
          f"turntable radius {radius:.0f} -> scale {scale:.5f}")

    # Node chain.
    nodes = []
    banner_rot = quat_mul(quat_axis((1, 0, 0), TILT_DEGREES), quat_axis((1, 0, 0), -90))
    nodes.append({"name": "Banner", "translation": [0, HEIGHT, 0], "rotation": list(banner_rot), "children": [1]})
    nodes.append({"name": "Turntable", "children": [2]})
    nodes.append(pose)
    for i in mesh_nodes:
        node = {"name": gltf["nodes"][i]["name"], "mesh": gltf["nodes"][i]["mesh"]}
        pose["children"].append(len(nodes))
        nodes.append(node)
    gltf["nodes"] = nodes
    gltf["scenes"] = [{"name": "Scene", "nodes": [0]}]
    gltf["scene"] = 0

    # Turntable animation, in a second buffer.
    count = int(TURN_SECONDS * SAMPLES_PER_SECOND) + 1
    times = [i / SAMPLES_PER_SECOND for i in range(count)]
    quats = [quat_axis((0, 0, 1), 360.0 * t / TURN_SECONDS) for t in times]
    anim = struct.pack(f"<{count}f", *times) + b"".join(struct.pack("<4f", *q) for q in quats)
    with open(os.path.join(WORK, "turntable.bin"), "wb") as f:
        f.write(anim)
    gltf["buffers"].append({"uri": "turntable.bin", "byteLength": len(anim)})
    buffer = len(gltf["buffers"]) - 1
    gltf["bufferViews"] += [
        {"buffer": buffer, "byteOffset": 0, "byteLength": count * 4},
        {"buffer": buffer, "byteOffset": count * 4, "byteLength": count * 16},
    ]
    views = len(gltf["bufferViews"])
    gltf["accessors"] += [
        {"bufferView": views - 2, "componentType": 5126, "count": count, "type": "SCALAR", "min": [0.0], "max": [times[-1]]},
        {"bufferView": views - 1, "componentType": 5126, "count": count, "type": "VEC4"},
    ]
    accessors = len(gltf["accessors"])
    gltf["animations"] = [{
        "name": "Turntable",
        "samplers": [{"input": accessors - 2, "output": accessors - 1, "interpolation": "LINEAR"}],
        "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}}],
    }]

    # Textures: diffuse and normal map only, shrunk; the specular texture/extension is dropped.
    material = gltf["materials"][0]
    material.pop("extensions", None)
    gltf.pop("extensionsUsed", None)
    gltf.pop("extensionsRequired", None)
    base_tex = material["pbrMetallicRoughness"]["baseColorTexture"]["index"]
    normal_tex = material["normalTexture"]["index"]
    new_images = []
    new_textures = []
    for tex_index, size, name in ((base_tex, DIFFUSE_SIZE, "cliffracer_diffuse.png"), (normal_tex, NORMAL_SIZE, "cliffracer_normal.png")):
        src = gltf["images"][gltf["textures"][tex_index]["source"]]["uri"]
        im = Image.open(io.BytesIO(images[src])).convert("RGB").resize((size, size), Image.Resampling.LANCZOS)
        im.save(os.path.join(WORK, name))
        new_images.append({"name": name, "uri": name, "mimeType": "image/png"})
        new_textures.append({"source": len(new_images) - 1, "sampler": 0})
    gltf["images"] = new_images
    gltf["textures"] = new_textures
    material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": 0}
    material["normalTexture"] = {"index": 1}
    material["name"] = "CliffRacer"

    with open(os.path.join(WORK, bin_name), "wb") as f:
        f.write(bin_data)
    path = os.path.join(WORK, "banner.gltf")
    with open(path, "w") as f:
        json.dump(gltf, f, indent=1)
    return path


def convert_cgfx(gltf_path):
    out = os.path.join(HERE, "banner.cgfx")
    subprocess.run([sys.executable, os.path.join(PYCGFX, "main.py"), gltf_path, out], check=True)
    size = os.path.getsize(out)
    print(f"banner.cgfx: {size / 1024:.0f} KB (limit {CGFX_LIMIT // 1024} KB)")
    if size > CGFX_LIMIT:
        sys.exit("banner.cgfx is too big for the HOME Menu")


def make_icon():
    icon = Image.open(SOURCE_ICON)
    icon.size = (48, 48)  # the .ico's own 48 px frame (SMDH large icon); bannertool derives the 24 px one
    icon.load()
    icon.convert("RGBA").save(os.path.join(PACKAGING, "icon.png"))
    print("icon.png: 48x48 from", os.path.relpath(SOURCE_ICON, ROOT))


def convert_audio():
    with wave.open(SOURCE_WAV) as src:
        channels, width, rate, frames = src.getnchannels(), src.getsampwidth(), src.getframerate(), src.getnframes()
        data = src.readframes(frames)
    if width != 2:
        sys.exit("swoosh: expected 16-bit PCM")
    if channels == 1:
        samples = struct.unpack(f"<{frames}h", data)
        data = b"".join(struct.pack("<hh", s, s) for s in samples)
    if frames / rate > 3.0:
        sys.exit("swoosh: banner sounds are limited to 3 s")
    with wave.open(os.path.join(HERE, "banner.wav"), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(data)
    print(f"banner.wav: {frames / rate:.2f} s, stereo, {rate} Hz")


def preview(gltf_path):
    script = os.path.join(WORK, "preview.py")
    with open(script, "w") as f:
        f.write(PREVIEW_SCRIPT)
    subprocess.run([BLENDER, "-b", "--python", script, "--", gltf_path, os.path.join(PYCGFX, "banner-camera.gltf"), WORK],
                   check=True, stdout=subprocess.DEVNULL)
    frames = sorted(p for p in os.listdir(WORK) if p.startswith("preview_") and p.endswith(".png"))
    tiles = [Image.open(os.path.join(WORK, p)).convert("RGB") for p in frames]
    w, h = tiles[0].size
    sheet = Image.new("RGB", (w * 4, h * ((len(tiles) + 3) // 4)))
    for i, tile in enumerate(tiles):
        sheet.paste(tile, ((i % 4) * w, (i // 4) * h))
    sheet.save(os.path.join(WORK, "preview.png"))
    print("preview:", os.path.join(WORK, "preview.png"))


PREVIEW_SCRIPT = r'''
import sys, os, bpy
gltf, camera_gltf, out = sys.argv[sys.argv.index("--") + 1:]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=gltf)
bpy.ops.import_scene.gltf(filepath=camera_gltf)
scene = bpy.context.scene
scene.camera = next(o for o in scene.objects if o.type == "CAMERA")
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.light = "STUDIO"
scene.display.shading.color_type = "TEXTURE"
scene.render.resolution_x, scene.render.resolution_y = 400, 240
scene.render.fps = 30
for i in range(8):
    scene.frame_set(int(i * 30) + 1)
    scene.render.filepath = os.path.join(out, "preview_%02d.png" % i)
    bpy.ops.render.render(write_still=True)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dorsal", type=int, default=1, choices=(1, -1), help="which side of the mesh is its back")
    parser.add_argument("--preview", action="store_true", help="render HOME Menu camera previews with Blender")
    args = parser.parse_args()
    make_icon()
    gltf_path = build_gltf(args.dorsal)
    if args.preview:
        preview(gltf_path)
    convert_cgfx(gltf_path)
    convert_audio()


if __name__ == "__main__":
    main()
