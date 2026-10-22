"""Builds the HOME Menu's 3D banner model, the moon-and-star emblem turning, in Blender (run in the background):

    blender -b --factory-startup --python tools/make_banner_model.py -- <outline.json> <texture.png> <out dir>

<outline.json> and <texture.png> come from `python tools/trace_emblem.py outline.json --texture texture.png`.
Writes <out dir>/banner.gltf (+ .bin, texture) and preview renders through the HOME Menu's banner camera (pycgfx's
banner-camera.gltf: 44.786 units away, 30 degrees of vertical field of view). Then
`python .tools/pycgfx/main.py <out dir>/banner.gltf` writes banner.cgfx next to it: copy it to
packaging/banner3d.cgfx (the build uses it when it is there, else the flat banner).

Kept as plain as a banner can be (test #11: a model with a normal map, tangents, two meshes and mipmapped filtering
crashed the HOME Menu on the console): one mesh, one colour texture, no normal map, no skin, no mipmaps, one node
turning about the vertical axis.
"""

import math
import os
import sys
import json

import bmesh
import bpy

args = sys.argv[sys.argv.index("--") + 1:]
OUTLINE, TEXTURE, OUT = args[0], args[1], args[2]
HEIGHT = 14.25  # units: the banner shows ~40 x 24 at the emblem's depth (19 was too big, the tester: 25% smaller)
DEPTH = 1.2  # extrusion, units (scaled with the height)
SECONDS = 6.0  # one turn
FPS = 30

os.makedirs(OUT, exist_ok=True)
scene = bpy.context.scene
for o in list(scene.objects):
    bpy.data.objects.remove(o, do_unlink=True)
scene.render.fps = FPS
scene.frame_start = 0
scene.frame_end = int(SECONDS * FPS)

# The outline as a filled, extruded, slightly bevelled curve, then a mesh.
d = json.load(open(OUTLINE))
w, h = d["size"]
scale = HEIGHT / 2 * (256 / 234)  # the outline is in [-1, 1], the emblem fills ~234 of its 256 pixels
cu = bpy.data.curves.new("EmblemCurve", "CURVE")
cu.dimensions = "2D"
cu.fill_mode = "BOTH"
cu.extrude = DEPTH / 2 / scale  # the curve is in the outline's [-1, 1] units
cu.bevel_depth = DEPTH / 5 / scale
cu.bevel_resolution = 1
for loop in d["loops"]:
    sp = cu.splines.new("POLY")
    sp.points.add(len(loop) - 1)
    for i, (x, y) in enumerate(loop):
        sp.points[i].co = ((x - w / 2) / (w / 2), (h / 2 - y) / (h / 2), 0, 1)
    sp.use_cyclic_u = True
curve = bpy.data.objects.new("EmblemCurve", cu)
scene.collection.objects.link(curve)
mesh = bpy.data.meshes.new_from_object(curve.evaluated_get(bpy.context.evaluated_depsgraph_get()))
bpy.data.objects.remove(curve, do_unlink=True)
mesh.name = "Emblem"

# UVs: the texture projected from the front (the curve's local x, y in [-1, 1]), then the mesh scaled to the banner
# and stood up facing -Y (the camera, glTF +Z after export).
bm = bmesh.new()
bm.from_mesh(mesh)
uv = bm.loops.layers.uv.verify()
for f in bm.faces:
    for l in f.loops:
        l[uv].uv = ((l.vert.co.x + 1) / 2, (l.vert.co.y + 1) / 2)
bmesh.ops.triangulate(bm, faces=bm.faces[:])
for v in bm.verts:
    x, y, z = v.co.x * scale, v.co.y * scale, v.co.z * scale
    v.co = (x, -z, y)  # face -Y
bm.to_mesh(mesh)
bm.free()
for p in mesh.polygons:
    p.use_smooth = False

img = bpy.data.images.load(TEXTURE)
mat = bpy.data.materials.new("EmblemBronze")
mat.use_nodes = True
nt = mat.node_tree
bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
tex = nt.nodes.new("ShaderNodeTexImage")
tex.image = img
tex.interpolation = "Linear"
nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
# pycgfx turns roughness into the HOME Menu's specular: strength 1 - 0.9 r, Phong exponent 800 / (1 + 100 r).
# At 0.45 a bright highlight washed out the star's centre on the console (test #109): 0.9 leaves a faint sheen.
bsdf.inputs["Roughness"].default_value = 0.9
bsdf.inputs["Metallic"].default_value = 0.0
mat.use_backface_culling = True  # one-sided (glTF doubleSided false): the mesh is closed
mesh.materials.append(mat)

# One node turning about the vertical axis (Blender Z, glTF Y), the emblem under it.
turntable = bpy.data.objects.new("Turntable", None)
scene.collection.objects.link(turntable)
emblem = bpy.data.objects.new("Emblem", mesh)
scene.collection.objects.link(emblem)
emblem.parent = turntable
turntable.rotation_mode = "XYZ"
turntable.rotation_euler = (0, 0, 0)
turntable.keyframe_insert("rotation_euler", index=2, frame=scene.frame_start)
turntable.rotation_euler = (0, 0, 2 * math.pi)
turntable.keyframe_insert("rotation_euler", index=2, frame=scene.frame_end)
for fc in turntable.animation_data.action.layers[0].strips[0].channelbag(turntable.animation_data.action.slots[0]).fcurves \
        if hasattr(turntable.animation_data.action, "layers") and turntable.animation_data.action.layers else turntable.animation_data.action.fcurves:
    for k in fc.keyframe_points:
        k.interpolation = "LINEAR"

tris = len(mesh.polygons)
print(f"banner model: {tris} triangles, {len(mesh.vertices)} vertices, {SECONDS} s turn")

# Preview renders through the banner camera (glTF (0, 1, 44.786) looking down -Z = Blender (0, -44.786, 1) looking +Y).
cam_data = bpy.data.cameras.new("BannerCamera")
cam_data.sensor_fit = "VERTICAL"
cam_data.angle_y = math.radians(30)
cam_data.clip_start = 26.5
cam_data.clip_end = 1000
cam = bpy.data.objects.new("BannerCamera", cam_data)
cam.location = (0, -44.786, 1)
cam.rotation_euler = (math.radians(90), 0, 0)
scene.collection.objects.link(cam)
scene.camera = cam
sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
sun.rotation_euler = (math.radians(50), math.radians(-20), math.radians(-25))
sun.data.energy = 3.0
scene.collection.objects.link(sun)
scene.world = bpy.data.worlds.new("World")
scene.world.color = (0.05, 0.05, 0.05)
# Cycles on the CPU: EEVEE has no GPU context in a background Blender (it crashed there).
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
scene.cycles.samples = 24
scene.render.resolution_x, scene.render.resolution_y = 400, 240
for i, angle in enumerate((0, 45, 90, 135, 180)):
    scene.frame_set(int(scene.frame_end * angle / 360))
    scene.render.filepath = os.path.join(OUT, f"preview_{angle:03d}.png")
    bpy.ops.render.render(write_still=True)

# The model for pycgfx: no camera or light (the HOME Menu brings its own), the texture as its own PNG.
bpy.data.objects.remove(cam, do_unlink=True)
bpy.data.objects.remove(sun, do_unlink=True)
bpy.ops.export_scene.gltf(filepath=os.path.join(OUT, "banner.gltf"), export_format="GLTF_SEPARATE",
                          export_yup=True, export_apply=True, export_animations=True, export_tangents=False,
                          export_normals=True, export_texcoords=True, export_materials="EXPORT",
                          export_image_format="AUTO", export_cameras=False, export_lights=False)
# Plain linear filtering, clamped (the exporter writes mipmapped filtering, and the texture has no mipmaps).
gltf = os.path.join(OUT, "banner.gltf")
d = json.load(open(gltf))
d["samplers"] = [{"magFilter": 9729, "minFilter": 9729, "wrapS": 33071, "wrapT": 33071}]
for t in d["textures"]:
    t["sampler"] = 0
# The turn about the vertical axis (glTF Y) becomes a turn about the turntable's own Z under a "Stand" node
# turned -90 degrees about X (its Z is then the world's Y), the emblem turned back +90 degrees under it. pycgfx
# stores rotations as XYZ Euler angles, which flip past 90 degrees about Y: the HOME Menu then rocked the
# emblem back and forth instead of turning it, with a jump at the flip (test #110, Zeste's HOME Menu). About
# Z alone the angle only wraps, which pycgfx unwraps. Ry(t) = (0, s, 0, c) becomes Rz(t) = (0, 0, s, c).
import struct
half = math.sqrt(0.5)
nodes = d["nodes"]
emblem_i = next(i for i, n in enumerate(nodes) if n.get("mesh") is not None)
turn_i = next(i for i, n in enumerate(nodes) if emblem_i in n.get("children", []))
assert "rotation" not in nodes[emblem_i] and "rotation" not in nodes[turn_i]
nodes[emblem_i]["rotation"] = [half, 0, 0, half]
nodes.append({"name": "Stand", "children": [turn_i], "rotation": [-half, 0, 0, half]})
d["scenes"][0]["nodes"] = [len(nodes) - 1]
(sampler,) = d["animations"][0]["samplers"]
acc = d["accessors"][sampler["output"]]
view = d["bufferViews"][acc["bufferView"]]
binpath = os.path.join(OUT, d["buffers"][0]["uri"])
data = bytearray(open(binpath, "rb").read())
base = view["byteOffset"] + acc.get("byteOffset", 0)
for k in range(acc["count"]):
    x, y, z, w = struct.unpack_from("<4f", data, base + 16 * k)
    assert abs(x) < 1e-5 and abs(z) < 1e-5, "the turntable must turn about Y alone"
    struct.pack_into("<4f", data, base + 16 * k, 0.0, 0.0, y, w)
open(binpath, "wb").write(data)
json.dump(d, open(gltf, "w"), indent=1)
print("wrote", gltf)
