"""Converts a robot's .hmd model, .htx textures and .mbm motion banks to a rigged, animated glTF binary.

Usage: hmd2glb.py <model.hmd> <textures.htx or -> <out.glb> [motions.mbm or .hmt or +ids|weapon.hmd|textures.htx or @list ...]

A + entry mounts a weapon on the model by bone id, the way the game does, so it follows every motion. One id binds the
whole weapon to that bone. Several, separated by commas, bind each weapon part with one of those ids to the body bone
with the same id. A @list file holds one entry per line.

A model with no skin bones, like most weapons, comes out as a plain static mesh, or rigidly bound to its nodes if it
has motions. Its vertices are relative to the node its palette names, so that node's transform is applied. With - the
materials have no textures.

The output comes from the game: keep it out of the repository.

Model (HMDL > KFMD > KFMS, KFMG), offsets from the start of the KFMS body:
- +0x04 node count, +0x08 skin bone count, +0x14 mesh count, +0x40 nine u64 table offsets: nodes (0xA0 each),
  node flags, rest transforms (0x30 each: translation, quaternion xyzw, scale), materials, meshes (0x40 each),
  submeshes (0x20 each), vertex declaration, texture slots, vertex format and skin tables.
- A submesh names a palette of (bone id, local index). Bone ids start at the node count, and id - node count is
  the position in the skin table, which holds the inverse bind matrices. Nodes with the u16 at +0x3E set to 1 are
  the skin bones, in table order. Matching them by transform doesn't work: some bones share one, like the ankles.
- KFMG holds triangle strips (u16) and vertices (0x5C each): weights f3, bones u8x4, uv f2 x3, position f3,
  normal f3, colour f4, tangent f3.

Motion (HMOT > KFMO): one track per node. A track's top kind byte says which channels exist: 0xE0 translation,
0x0F rotation as xyzw, 0x0E rotation as Euler x, y, z in radians, composed Rz * Ry * Rx. Each channel has a
static value and, if it moves, a curve with one value per frame added to it: format 1 f32, 2 s16 or 3 s8, the
last two divided by 2^(curve byte 2). A .mbm is a bank: ids, names, then the HMOT files one after another.

Material rules are read from the data, not the shader. Slot 0 is the base colour. Type 5 materials are alpha
blended decals. Overlay layers, like the thin coat around Fei-Yen's torso and the copy of her chest text on it, are
left out; they are probably for hyper mode, though nothing here confirms it.
The game's rest pose has the arms straight down through the body, so the export moves them out to an A-pose. Tracks
hold absolute local values, so motions still apply as they are; only channels a motion leaves out need the old rest
value written. No vertex blends between bones, so this is exact. "APose" is the first animation because Blender
applies the first one on import.

A real texture in slot 3 glows, on the second UV set. A real texture in slot 4 is a decal layer on the third.
"""
import json
import os
import struct
import sys
import zlib

import numpy as np

STRIDE = 0x5C
DECAL_LIFT = 0.003
DECAL_TEXTURE = 1000  # marks a material's texture as the clamped copy until the texture list is written
# Each robot's left shoulder bones, the ones nearest the hand; the right ones are found by mirroring. Grys-Vok has two
# pairs of arms. They swing out by this much, about Z.
APOSE = {"VR000": (14,), "VR001": (14,), "VR002": (14,), "VR003": (14,), "VR004": (8,), "VR005": (11,), "VR006": (8,), "VR007": (8, 37), "VR008": (8,), "VR009": (7,), "VR010": (8,), "VR011": (13,), "VR012": (6,), "VR013": (11,), "VR014": (11,)}
APOSE_DEGREES = 45.0
VERTEX = np.dtype([("w", "<3f4"), ("b", "4u1"), ("uv0", "<2f4"), ("uv1", "<2f4"), ("uv2", "<2f4"), ("p", "<3f4"), ("n", "<3f4"), ("c", "<4f4"), ("t", "<3f4")])


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def u64(d, o):
    return struct.unpack_from("<Q", d, o)[0]


class Model:
    def __init__(self, path):
        d = self.d = open(path, "rb").read()
        if d[:4] != b"HMDL" or d[0x20:0x24] != b"KFMD" or d[0x40:0x44] != b"KFMS":
            raise ValueError("not a model: " + path)
        b = self.base = 0x60
        node_count, self.skin_count, mesh_count = u32(d, b + 4), u32(d, b + 8), u32(d, b + 0x14)
        self.tables = [u64(d, b + 0x40 + i * 8) for i in range(9)]
        self.nodes = []
        for i in range(node_count):
            _, _, parent = struct.unpack_from("<IHH", d, b + self.tables[0] + i * 0xA0)
            t = struct.unpack_from("<12f", d, b + self.tables[2] + i * 0x30)
            self.nodes.append({"parent": parent, "t": t[:3], "q": t[4:8], "s": t[8:11]})
        kfmg = 0x60 + u32(d, 0x44)
        if d[kfmg : kfmg + 4] != b"KFMG":
            raise ValueError("no KFMG in " + path)
        g = kfmg + 0x20
        index_at, index_size, vertex_at, _ = struct.unpack_from("<4I", d, g + 0x10)
        self.indices = np.frombuffer(d, "<u2", index_size // 2, g + index_at)
        self.vertices_at = g + vertex_at
        self.meshes = []
        for k in range(mesh_count):
            o = b + self.tables[4] + k * 0x40
            subs = []
            for s in range(u32(d, o + 0x18)):
                q = b + u64(d, o + 0x20) + s * 0x20
                palette_size = struct.unpack_from("<H", d, q)[0]
                vertex_count = struct.unpack_from("<H", d, q + 6)[0]
                index_count, vertex_base, index_start = struct.unpack_from("<3I", d, q + 8)
                palette = [struct.unpack_from("<HH", d, b + u64(d, q + 0x18) + j * 4) for j in range(palette_size)]
                subs.append((vertex_count, index_count, vertex_base, index_start, palette))
            self.meshes.append((u64(d, o + 8), subs))

    def skin(self):
        """Skin table: the node of each entry, and its inverse bind matrix in column-vector form."""
        d, b = self.d, self.base
        joints = [i for i in range(len(self.nodes)) if struct.unpack_from("<H", d, b + self.tables[0] + i * 0xA0 + 0x3E)[0] == 1]
        if len(joints) != self.skin_count:
            raise ValueError("%d skin bones, %d flagged nodes" % (self.skin_count, len(joints)))
        if not joints:
            return [], []
        world = self.rest_world()
        ids = struct.pack("<%dH" % self.skin_count, *range(len(self.nodes), len(self.nodes) + self.skin_count))
        at = d.find(ids, b + self.tables[8], kfms_end(d))
        # A short list can match by chance, so the matrices decide. Under a scaled bone they needn't invert it exactly.
        best, best_fit = None, 0
        while at >= 0:
            entries = (at + self.skin_count * 2 + 15) & ~15
            offsets = [u64(d, entries + k * 0x10) for k in range(self.skin_count)]
            if all(o + 0x40 + b <= len(d) for o in offsets):
                matrices = [np.array(struct.unpack_from("<16f", d, b + o)).reshape(4, 4).T for o in offsets]
                fit = sum(np.abs(m @ world[n] - np.eye(4)).max() < 1e-3 for n, m in zip(joints, matrices))
                if fit > best_fit:
                    best, best_fit = matrices, fit
            at = d.find(ids, at + 2, kfms_end(d))
        if best is None:
            raise ValueError("no skin table fits")
        return joints, best

    def node_with_id(self, wanted):
        """The first node with this bone id (u32 at +0x7C), as FUN_0137b950 finds it."""
        for i in range(len(self.nodes)):
            if u32(self.d, self.base + self.tables[0] + i * 0xA0 + 0x7C) == wanted:
                return i
        raise ValueError("no bone with id %d" % wanted)

    def rest_world(self):
        world = []
        for i, n in enumerate(self.nodes):
            m = np.eye(4)
            m[:3, :3] = quaternion_matrix(n["q"]) * np.array(n["s"])[None, :]
            m[:3, 3] = n["t"]
            world.append(world[n["parent"]] @ m if i else m)
        return world

    def material(self, at):
        """Material type and the texture index in each of its six slots."""
        d, b = self.d, self.base
        slots = [u32(d, b + p + 4) & 0xFFFF for p in struct.unpack_from("<6Q", d, b + at + 0x80)]
        return u32(d, b + at), slots


def kfms_end(d):
    return 0x60 + u32(d, 0x44)


def quaternion_matrix(q):
    x, y, z, w = q
    return np.array(
        [
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
        ]
    )


def load_textures(path):
    d = open(path, "rb").read()
    if d[:4] != b"HTEX":
        raise ValueError("not a texture bank: " + path)
    textures = []
    o = 0x20
    while d[o : o + 4] == b"HTSF":
        size = u32(d, o + 4)
        dds = o + 0x40
        if d[dds : dds + 4] != b"DDS ":
            raise ValueError("no DDS in " + path)
        height, width = struct.unpack_from("<II", d, dds + 12)
        textures.append((d, dds + 128, width, height, d[dds + 84 : dds + 88]))
        o += 0x20 + size
    return textures


def decode_dxt(texture):
    d, at, width, height, fourcc = texture
    if fourcc not in (b"DXT1", b"DXT3", b"DXT5"):
        raise ValueError("texture format %r" % fourcc)
    size = 8 if fourcc == b"DXT1" else 16
    bw, bh = (width + 3) // 4, (height + 3) // 4
    a = np.frombuffer(d, "u1", bw * bh * size, at).reshape(-1, size)
    colour = a[:, -8:]
    c0 = colour[:, 0].astype(int) | colour[:, 1].astype(int) << 8
    c1 = colour[:, 2].astype(int) | colour[:, 3].astype(int) << 8
    bits = colour[:, 4].astype(np.int64) | colour[:, 5].astype(np.int64) << 8 | colour[:, 6].astype(np.int64) << 16 | colour[:, 7].astype(np.int64) << 24

    def rgb(c):
        return np.stack([(c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31], -1)

    p0, p1 = rgb(c0), rgb(c1)
    four = c0 > c1 if fourcc == b"DXT1" else np.ones(len(a), bool)
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    palette = np.stack([p0, p1, p2, p3], 1)
    index = np.stack([(bits >> (2 * i)) & 3 for i in range(16)], 1)
    row = np.arange(len(a))[:, None]
    pixels = palette[row, index]
    if fourcc == b"DXT5":
        a0, a1 = a[:, 0].astype(int), a[:, 1].astype(int)
        steps = [np.where(a0 > a1, ((7 - i) * a0 + i * a1) // 7, ((5 - i) * a0 + i * a1) // 5 if i < 5 else (0 if i == 5 else 255)) for i in range(1, 7)]
        table = np.stack([a0, a1] + steps, 1)
        alpha_bits = sum(a[:, 2 + i].astype(np.int64) << (8 * i) for i in range(6))
        alpha = table[row, np.stack([(alpha_bits >> (3 * i)) & 7 for i in range(16)], 1)]
    elif fourcc == b"DXT3":
        alpha_bits = sum(a[:, i].astype(np.uint64) << np.uint64(8 * i) for i in range(8))
        alpha = np.stack([((alpha_bits >> np.uint64(4 * i)) & np.uint64(15)).astype(int) * 17 for i in range(16)], 1)
    else:
        alpha = np.where((~four)[:, None] & (index == 3), 0, 255)
    rgba = np.concatenate([pixels, alpha[..., None]], -1).reshape(bh, bw, 4, 4, 4).astype("u1")
    return rgba.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)[:height, :width]


def png(rgba):
    height, width = rgba.shape[:2]
    raw = b"".join(b"\0" + rgba[y].tobytes() for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")


def strip_triangles(indices):
    triangles = []
    for i in range(len(indices) - 2):
        a, b, c = int(indices[i]), int(indices[i + 1]), int(indices[i + 2])
        if a != b and b != c and a != c:
            triangles.append((a, b, c) if i % 2 == 0 else (b, a, c))
    return np.array(triangles, "<u4").reshape(-1, 3)


def read_bank(path):
    d = open(path, "rb").read()
    b = 0x40
    count = u32(d, b + 4)
    ids = struct.unpack_from("<%dI" % count, d, b + u32(d, b + 8))
    names = []
    for i in range(count):
        o = b + u32(d, b + u32(d, b + 16) + i * 4)
        names.append(d[o : d.index(b"\0", o)].decode())
    at = []
    o = 0
    while True:
        o = d.find(b"HMOT", o)
        if o < 0:
            break
        if d[o + 0x20 : o + 0x24] == b"KFMO":
            at.append(o)
        o += 4
    if len(at) != count:
        raise ValueError("%s: %d names, %d motions" % (path, count, len(at)))
    return d, list(zip(ids, names, at))


def curve(d, p, frames, static, at):
    if not at:
        return np.full(frames + 1, static)
    kind, _, shift = d[p + at], d[p + at + 1], d[p + at + 2]
    data = p + u64(d, p + at + 0x18)
    if kind == 1:
        return static + np.frombuffer(d, "<f4", frames + 1, data).astype(float)
    if kind == 2:
        return static + np.frombuffer(d, "<i2", frames + 1, data) / float(1 << shift)
    if kind == 3:
        return static + np.frombuffer(d, "i1", frames + 1, data) / float(1 << shift)
    raise ValueError("curve format %d" % kind)


def euler_quaternions(r):
    """Rz * Ry * Rx, as quaternions."""
    half = r / 2
    sx, sy, sz = np.sin(half[:, 0]), np.sin(half[:, 1]), np.sin(half[:, 2])
    cx, cy, cz = np.cos(half[:, 0]), np.cos(half[:, 1]), np.cos(half[:, 2])
    return np.stack([sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz], 1)


def read_motion(d, o, node_count):
    """Frame count, rate and {node: (translations or None, rotations or None)}. Tracks past the skeleton are dropped."""
    p = o + 0x40
    track_count = u32(d, p + 4)
    frames = int(round(struct.unpack_from("<f", d, p + 0x10)[0]))
    rate = struct.unpack_from("<f", d, p + 0x14)[0]
    table = u64(d, p + 0x20)
    tracks = {}
    for i in range(min(track_count, node_count)):
        r = p + table + i * 0x20
        kind = d[r + 7]
        if not kind:
            continue
        n = bin(kind).count("1")
        curves_at, statics_at = u64(d, r + 8), u64(d, r + 0x10)
        curves = struct.unpack_from("<%dQ" % n, d, p + curves_at) if curves_at else (0,) * n
        statics = struct.unpack_from("<%df" % n, d, p + statics_at)
        channels = [curve(d, p, frames, s, c) for s, c in zip(statics, curves)]
        translation = rotation = None
        at = 0
        if kind & 0xE0:
            if kind & 0xE0 != 0xE0:
                raise ValueError("track kind %#x" % kind)
            translation = np.stack(channels[:3], 1)
            at = 3
        if kind & 0x0F == 0x0F:
            rotation = np.stack(channels[at:], 1)
            rotation /= np.linalg.norm(rotation, axis=1, keepdims=True)
        elif kind & 0x0F == 0x0E:
            rotation = euler_quaternions(np.stack(channels[at:], 1))
        elif kind & 0x0F:
            raise ValueError("track kind %#x" % kind)
        tracks[i] = (translation, rotation)
    return frames, rate, tracks


class Writer:
    def __init__(self):
        self.blob = bytearray()
        self.views = []
        self.accessors = []

    def view(self, data, target=None):
        while len(self.blob) % 4:
            self.blob.append(0)
        v = {"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data)}
        if target:
            v["target"] = target
        self.blob += data
        self.views.append(v)
        return len(self.views) - 1

    def accessor(self, array, kind, component, target=None, bounds=False):
        a = {"bufferView": self.view(array.tobytes(), target), "componentType": component, "count": len(array), "type": kind}
        if bounds:
            a["min"] = [float(x) for x in np.atleast_1d(array.min(0))]
            a["max"] = [float(x) for x in np.atleast_1d(array.max(0))]
        self.accessors.append(a)
        return len(self.accessors) - 1


def is_overlay(model, textures, at):
    """An effect layer over the body: a type 5 material with a blank base colour, or the copy of a decal that sits on it.

    The copy to drop is the one with the body's slot 5. It rests on the coat; its twin lies on the surface below."""
    kind, slots = model.material(at)
    if kind != 5 or slots[0] >= len(textures):
        return False
    if textures[slots[0]][2] <= 8:
        return True
    body = model.material(model.meshes[0][0])[1][5]
    others = [model.material(m[0]) for m in model.meshes if m[0] != at]
    return slots[5] == body and any(k == 5 and o[0] == slots[0] and o[5] != body for k, o in others)


def choose_material(model, textures, at):
    """Texture index for the base colour, and whether it is an alpha-blended decal."""
    kind, slots = model.material(at)
    return slots[0], kind == 5


def multiply_quaternions(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def mirrored_shoulders(model, shoulders):
    """{node: degrees} for the given left shoulders and the bones mirroring them: same place and depth. Where two bones
    share the place, as on Fei-Yen, the arm is the one with the same bones below; arms can differ otherwise."""
    world = model.rest_world()

    def depth(node):
        return 1 + depth(model.nodes[node]["parent"]) if node else 0

    def below(node):
        return sum(1 + below(i) for i in range(1, len(world)) if model.nodes[i]["parent"] == node)

    swings = {}
    for node in shoulders:
        target = world[node][:3, 3] * (-1, 1, 1)
        twins = [i for i in range(len(world)) if depth(i) == depth(node) and np.abs(world[i][:3, 3] - target).max() < 0.05]
        if len(twins) > 1:
            twins = [i for i in twins if below(i) == below(node)]
        if len(twins) != 1:
            raise ValueError("no single mirror for bone %d: %s" % (node, twins))
        swings[node], swings[twins[0]] = APOSE_DEGREES, -APOSE_DEGREES
    return swings


def motion_fits(model, d, at):
    """Whether a motion was made for this skeleton: it covers the bones, and those that hold still are the same length."""
    if u32(d, at + 0x44) < len(model.nodes) - 1:
        return False
    errors = []
    for node, (translation, _) in read_motion(d, at, len(model.nodes))[2].items():
        if translation is not None and np.ptp(translation, axis=0).max() < 1e-6:
            errors.append(np.abs(translation[0] - np.array(model.nodes[node]["t"])).max())
    return not errors or float(np.median(errors)) < 0.05


def placement(model):
    """For each node, what puts a vertex bound to it where the game draws it in the rest pose."""
    world = np.array(model.rest_world())
    for node, matrix in zip(*model.skin()):
        world[node] = world[node] @ matrix
    return world


def build(model, textures, banks, apose, overlays=False, attachments=()):
    w = Writer()
    joints, _ = model.skin()
    rigged = bool(joints or banks)
    old_rotation = {}
    for node, degrees in apose.items():
        old_rotation[node] = model.nodes[node]["q"]
        half = np.radians(degrees) / 2
        parent = (0.0, 0.0, 0.0, 1.0)
        at = node
        while at:
            at = model.nodes[at]["parent"]
            parent = multiply_quaternions(model.nodes[at]["q"], parent)
        inverse = (-parent[0], -parent[1], -parent[2], parent[3])
        swing = multiply_quaternions(multiply_quaternions(inverse, (0.0, 0.0, np.sin(half), np.cos(half))), parent)
        model.nodes[node]["q"] = multiply_quaternions(swing, old_rotation[node])
    new_world = model.rest_world()
    gltf = {"asset": {"version": "2.0", "generator": "hmd2glb.py"}, "scene": 0, "scenes": [{"nodes": []}]}
    nodes = [{"name": "node%03d" % i, "translation": list(map(float, n["t"])), "rotation": list(map(float, n["q"])), "scale": list(map(float, n["s"]))} for i, n in enumerate(model.nodes)]
    for i, n in enumerate(model.nodes):
        if i == 0:
            continue
        nodes[n["parent"]].setdefault("children", []).append(i)
    gltf["scenes"][0]["nodes"].append(0)

    images = {}
    image_banks = {}
    materials = []
    material_of = {}
    decal_of = {}
    primitives = []
    def image(textures, index):
        key = id(textures), index
        image_banks[key] = textures[index]
        return images.setdefault(key, len(images))

    body = model

    def add_model(model, textures, mount, keep_overlays):
        own = model.skin()[0]
        moved = placement(model)
        if mount is not None and len(mount) > 1:
            # Each part follows the body bone with its id, and the bones under it follow the part.
            weapon_world = model.rest_world()
            parts = {}
            for wanted in mount:
                parts[model.node_with_id(wanted)] = body.node_with_id(wanted)
            part_bone = np.zeros(len(model.nodes), "<u2")
            part_move = np.array([new_world[0]] * len(model.nodes))
            for i in range(len(model.nodes)):
                at = i
                while at not in parts and at:
                    at = model.nodes[at]["parent"]
                if at in parts:
                    part_bone[i] = parts[at]
                    part_move[i] = new_world[parts[at]] @ np.linalg.inv(weapon_world[at])
        for mesh_index, (material_at, subs) in enumerate(model.meshes):
            if not keep_overlays and is_overlay(model, textures, material_at):
                continue
            if (id(model), material_at) not in material_of:
                texture, decal = choose_material(model, textures, material_at)
                m = {"name": "material%02d_tex%02d" % (len(materials), texture), "doubleSided": True, "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 1.0}}
                if texture < len(textures):
                    m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": image(textures, texture)}
                if decal:
                    m["alphaMode"] = "BLEND"
                glow = model.material(material_at)[1][3]
                if glow < len(textures) and textures[glow][2] > 8:
                    m["emissiveTexture"] = {"index": image(textures, glow), "texCoord": 1}
                    m["emissiveFactor"] = [1.0, 1.0, 1.0]
                material_of[id(model), material_at] = len(materials)
                materials.append(m)
            for vertex_count, index_count, vertex_base, index_start, palette in subs:
                v = np.frombuffer(model.d, VERTEX, vertex_count, model.vertices_at + vertex_base * STRIDE)
                triangles = strip_triangles(model.indices[index_start : index_start + index_count])
                if len(triangles) == 0:
                    continue
                if triangles.max() >= vertex_count:
                    print("warning: %d triangles past the vertices of mesh %d dropped" % ((triangles.max(1) >= vertex_count).sum(), mesh_index))
                    triangles = triangles[triangles.max(1) < vertex_count]
                    if len(triangles) == 0:
                        continue
                lookup = np.zeros(256, "<u2")
                for bone, local in palette:
                    lookup[local] = own[bone - len(model.nodes)] if own else bone
                bones = lookup[v["b"]]
                weights = np.concatenate([v["w"], np.maximum(0, 1 - v["w"].sum(1, keepdims=True))], 1).astype("<f4")
                weights /= weights.sum(1, keepdims=True)
                weights[weights < 1e-6] = 0
                bones = np.where(weights > 0, bones, 0)
                used = np.unique(triangles)
                if mount is None and apose and (np.abs(moved[bones[used]] - moved[bones[used][:, :1]]).max((2, 3)) * (weights[used] > 0)).max() > 1e-4:
                    print("warning: vertices blend across a swung shoulder, so motions distort there")
                transform = (moved[bones] * weights[..., None, None]).sum(1)
                if mount is not None:
                    if len(mount) > 1:
                        transform = part_move[bones[:, 0]] @ transform
                        bones = np.tile(part_bone[bones[:, :1]], (1, 4))
                    else:
                        at = body.node_with_id(mount[0])
                        transform = new_world[at] @ transform
                        bones = np.full_like(bones, at)
                    weights = np.tile(np.array([1, 0, 0, 0], "<f4"), (vertex_count, 1))
                positions = (transform[:, :3, :3] @ v["p"][..., None])[..., 0] + transform[:, :3, 3]
                normals = (transform[:, :3, :3] @ v["n"][..., None])[..., 0]
                normals = normals / np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-9)
                a, b, c = positions[triangles[:, 0]], positions[triangles[:, 1]], positions[triangles[:, 2]]
                if (np.cross(b - a, c - a) * normals[triangles[:, 0]]).sum(1).mean() < 0:
                    triangles = triangles[:, [0, 2, 1]]
                attributes = {
                    "POSITION": w.accessor(positions.astype("<f4"), "VEC3", 5126, 34962, True),
                    "NORMAL": w.accessor(normals.astype("<f4"), "VEC3", 5126, 34962),
                    "TEXCOORD_0": w.accessor(np.nan_to_num(np.array(v["uv0"], "<f4")), "VEC2", 5126, 34962),
                    "JOINTS_0": w.accessor(bones.astype("<u2"), "VEC4", 5123, 34962),
                    "WEIGHTS_0": w.accessor(weights, "VEC4", 5126, 34962),
                }
                if not rigged:
                    del attributes["JOINTS_0"], attributes["WEIGHTS_0"]
                if "emissiveTexture" in materials[material_of[id(model), material_at]]:
                    attributes["TEXCOORD_1"] = w.accessor(np.array(np.nan_to_num(v["uv1"]), "<f4"), "VEC2", 5126, 34962)
                primitives.append({"attributes": attributes, "indices": w.accessor(triangles.astype("<u4").reshape(-1), "SCALAR", 5125, 34963), "material": material_of[id(model), material_at]})
                decal = model.material(material_at)[1][4]
                if decal < len(textures) and textures[decal][2] > 8:
                    # The decal layer: slot 4 on the third UV set, clamped. glTF has no second base layer, so it is a copy
                    # of the triangles that reach the decal sheet, lifted off the surface so it can't z-fight.
                    uv = np.nan_to_num(v["uv2"])[triangles]
                    hit = triangles[(uv.max((1, 2)) >= 0) & (uv.min((1, 2)) <= 1) & (uv[..., 0].max(1) >= 0) & (uv[..., 0].min(1) <= 1) & (uv[..., 1].max(1) >= 0) & (uv[..., 1].min(1) <= 1)]
                    if len(hit):
                        if (id(textures), decal) not in decal_of:
                            decal_of[id(textures), decal] = len(materials)
                            materials.append({"name": "decal_tex%02d" % decal, "doubleSided": True, "alphaMode": "BLEND", "pbrMetallicRoughness": {"baseColorTexture": {"index": image(textures, decal) + DECAL_TEXTURE}, "metallicFactor": 0.0, "roughnessFactor": 1.0}})
                        lifted = dict(attributes)
                        lifted["POSITION"] = w.accessor((positions + normals * DECAL_LIFT).astype("<f4"), "VEC3", 5126, 34962, True)
                        lifted["TEXCOORD_0"] = w.accessor(np.array(np.nan_to_num(v["uv2"]), "<f4"), "VEC2", 5126, 34962)
                        primitives.append({"attributes": lifted, "indices": w.accessor(hit.astype("<u4").reshape(-1), "SCALAR", 5125, 34963), "material": decal_of[id(textures), decal]})

    add_model(model, textures, None, overlays)
    for mount, weapon, weapon_textures in attachments:
        add_model(weapon, weapon_textures, mount, True)
    gltf["meshes"] = [{"name": "body", "primitives": primitives}]
    # Every node is a joint, so that Blender makes one armature and one action per motion.
    matrices = np.array([np.linalg.inv(m).T for m in new_world], "<f4")
    if rigged:
        gltf["skins"] = [{"joints": list(range(len(model.nodes))), "inverseBindMatrices": w.accessor(matrices, "MAT4", 5126), "skeleton": 0}]
    nodes.append({"name": "body", "mesh": 0, "skin": 0} if rigged else {"name": "body", "mesh": 0})
    gltf["scenes"][0]["nodes"].append(len(nodes) - 1)
    gltf["nodes"] = nodes
    gltf["materials"] = materials

    if images:
        gltf["images"] = []
        for key in sorted(images, key=images.get):
            gltf["images"].append({"name": "tex%02d" % key[1], "mimeType": "image/png", "bufferView": w.view(png(decode_dxt(image_banks[key])))})
        gltf["textures"] = [{"source": i, "sampler": 0} for i in range(len(images))] + [{"source": i, "sampler": 1} for i in range(len(images))]
        for m in materials:
            texture = m["pbrMetallicRoughness"].get("baseColorTexture")
            if texture and texture["index"] >= DECAL_TEXTURE:
                texture["index"] += len(images) - DECAL_TEXTURE
        gltf["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}, {"magFilter": 9729, "minFilter": 9987, "wrapS": 33071, "wrapT": 33071}]

    first = w.accessor(np.zeros(1, "<f4"), "SCALAR", 5126, None, True)
    samplers, channels = [], []
    for i, n in enumerate(model.nodes):
        for path, value, kind in (("translation", n["t"], "VEC3"), ("rotation", n["q"], "VEC4")):
            samplers.append({"input": first, "output": w.accessor(np.array([value], "<f4"), kind, 5126), "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1, "target": {"node": i, "path": path}})
    animations = [{"name": "APose" if apose else "Rest", "samplers": samplers, "channels": channels}] if rigged else []
    for prefix, d, entries in banks:
        for motion_id, name, at in entries:
            if at and not motion_fits(model, d, at):
                print("skipped motion for another skeleton:", prefix + name)
                continue
            frames, rate, tracks = read_motion(d, at, len(model.nodes))
            times = w.accessor((np.arange(frames + 1) / rate).astype("<f4"), "SCALAR", 5126, None, True)
            samplers, channels = [], []

            def add(node, path, values):
                moving = np.ptp(values, axis=0).max() > 1e-7
                data = values if moving else values[:1]
                samplers.append({"input": times if moving else first, "output": w.accessor(data.astype("<f4"), "VEC3" if path == "translation" else "VEC4", 5126), "interpolation": "LINEAR"})
                channels.append({"sampler": len(samplers) - 1, "target": {"node": node, "path": path}})

            for node, (translation, rotation) in sorted(tracks.items()):
                if translation is not None:
                    add(node, "translation", translation)
                if rotation is not None:
                    add(node, "rotation", rotation)
            for node, rotation in old_rotation.items():
                if node not in tracks or tracks[node][1] is None:
                    add(node, "rotation", np.array([rotation]))
            title = os.path.splitext(name)[0]
            animations.append({"name": prefix + title, "samplers": samplers, "channels": channels, "extras": {"motionId": motion_id, "frames": frames}})
    if animations:
        gltf["animations"] = animations
    gltf["bufferViews"] = w.views
    gltf["accessors"] = w.accessors
    gltf["buffers"] = [{"byteLength": len(w.blob)}]
    return gltf, w.blob


def write_glb(path, gltf, blob):
    text = json.dumps(gltf, separators=(",", ":")).encode()
    text += b" " * (-len(text) % 4)
    blob = bytes(blob) + b"\0" * (-len(blob) % 4)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(blob)))
        f.write(struct.pack("<II", len(text), 0x4E4F534A) + text)
        f.write(struct.pack("<II", len(blob), 0x004E4942) + blob)


def main():
    model_path, texture_path, out = sys.argv[1:4]
    model = Model(model_path)
    textures = load_textures(texture_path) if texture_path != "-" else []
    banks = []
    paths = []
    for arg in sys.argv[4:]:
        paths += open(arg[1:], encoding="utf8").read().split("\n") if arg.startswith("@") else [arg]
    attachments = []
    for path in filter(None, paths):
        if path.startswith("+"):
            ids, weapon, bank = path[1:].split("|")
            attachments.append((tuple(int(i) for i in ids.split(",")), Model(weapon), load_textures(bank) if bank != "-" else []))
            continue
        stem = os.path.splitext(os.path.basename(path))[0]
        if path.lower().endswith(".hmt"):
            banks.append(("", open(path, "rb").read(), [(0, stem, 0)]))
            continue
        prefix = stem.split("_", 1)[1] + "/" if "_" in stem else ""
        d, entries = read_bank(path)
        banks.append((prefix, d, entries))
    name = os.path.splitext(os.path.basename(model_path))[0]
    body = name[5:] == "_Z" or name[5:].lstrip("_").isdigit()
    swings = mirrored_shoulders(model, APOSE[name[:5]]) if body and name[:5] in APOSE else {}
    gltf, blob = build(model, textures, banks, swings, not body, attachments)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    write_glb(out, gltf, blob)
    print("%s: %d nodes, %d primitives, %d images, %d animations, %.1f MB" % (out, len(gltf["nodes"]), len(gltf["meshes"][0]["primitives"]), len(gltf.get("images", [])), len(gltf.get("animations", [])), len(blob) / 1048576))


if __name__ == "__main__":
    main()
