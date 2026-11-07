# Model and motion formats

What `hmd2glb.py` and `export_models.py` rely on. Everything here is from reading the files and the game's code. Marked **unverified** where it's a reading of the data that nothing has confirmed.

Run the export on your own copy of the game, and keep the output out of the repository:

```powershell
python analysis\scripts\cpk.py extract extracted\uroot\cpk\disc00.cpk extracted\cpk_out "root+/chara/VR*" "root+/motion/*" "root+/param/*"
python analysis\scripts\cpk.py extract extracted\uroot\cpk\disc01.cpk extracted\cpk_out "root+/chara/VR*" "root+/motion/*" "root+/param/*"
python analysis\scripts\export_models.py extracted\cpk_out\root+ extracted\models
```

## Files

Under `root+/`:

| Path | Holds |
|---|---|
| `chara/VRnnn/VRnnn_NN.hmd` | Body. `_00` is colours A and B, `_01` the VOOT colours, `_02` the special colour |
| `chara/VRnnn/VRnnn_NN_Cxx.htx` | Its textures for colour `xx`. Fei-Yen also has `Cxx` + `H`, her hyper mode |
| `chara/VRnnn/VRnnn_WPxx.hmd` | Weapons and other attachments, with `_Cxx.htx` or a plain `.htx` |
| `chara/VRnnn/*Z.hmd`, `*_Z.hmd` | Low detail versions, with one grey texture bank. **Unverified:** the game probably tints them |
| `motion/VRnnn/VRnnn.mbm` | Motion bank; also `_Upper`, `_menu_idling`, and banks for rigged weapons such as `_Weapon`, `_Wing` |
| `motion/VRnnn/vrnnn_wpxx_yyy.hmt` | A loose weapon motion |
| `param/mdl_param_vrnnn.prm` | For each colour, the body and its weapons as model and texture pairs. `_h` is Fei-Yen's hyper mode |
| `param/decken_model_vrnnn.prm` | The same pairs for the weapons it names, which is how a `WP07` with no textures gets `WP06`'s |

Robots: 000 Temjin, 001 Apharmd B, 002 Apharmd C, 003 Apharmd S, 004 Bal-Bados, 005 Cypher, 006 Raiden, 007 Grys-Vok, 008 Dordray, 009 Fei-Yen, 010 Angelan, 011 Specineff, 012 Bal-Rloon, 013 Blue Stalker, 014 PPRIMA. Names are from the gallery text; 000 to 012 follow its order.

All values are little-endian. Chunks start with a 4-character tag, then u32 body size, u32 header size (0x20), u32 flags. `EOFC` ends a level.

## HMD: model

`HMDL` > `KFMD` > `KFMS`, `KFMG`.

`KFMS` body, from its start:

| Offset | Holds |
|---|---|
| +0x04 | Node count |
| +0x08 | Skin bone count |
| +0x14 | Mesh count |
| +0x40 | Nine u64 table offsets: nodes, node flags, rest transforms, materials, meshes, submeshes, vertex declaration, texture slots, vertex format and skin tables |

- **Node** (0xA0): +0x04 u16 index, +0x06 u16 parent, +0x3E u16 is 1 on a skin bone.
- **Rest transform** (0x30): translation, quaternion xyzw, scale, as vec4s.
- **Mesh** (0x40): +0x08 material offset, +0x18 submesh count, +0x20 submesh offset.
- **Submesh** (0x20): +0x00 u16 palette size, +0x06 u16 vertex count, +0x08 u32 index count, +0x0C u32 first vertex, +0x10 u32 first index, +0x18 palette offset.
- **Palette**: pairs of u16 bone id and u16 local index. A vertex's bone bytes are local indices.
- **Skin table**: bone ids count up from the node count; id minus node count is the position in the table. Each entry is a u64 offset to an inverse bind matrix (row vectors, 16 floats). Skin bones are the flagged nodes, in order.

Details that caused bugs:

- Match skin bones by the flag, not by transform. Several bones share a transform, like the two at each ankle.
- Use the stored matrices rather than inverting the skeleton. Under a scaled bone they differ, and the stored ones place the vertices.
- With no skin bones, a palette id is a node index, and the vertices are relative to that node.
- A vertex range can hold vertices no triangle in that submesh uses, with bones its palette doesn't have.

`KFMG` body: +0x10 u32 index offset, index size, vertex offset, vertex size. Indices are u16 triangle strips, with repeats for degenerate triangles.

Vertex (0x5C, `SampleVertexFormatWT3PNCT`):

| Offset | Holds |
|---|---|
| 0x00 | Weights, 3 floats; the fourth is what's left of 1 |
| 0x0C | Bones, 4 bytes |
| 0x10 | UV0, base colour |
| 0x18 | UV1, glow |
| 0x20 | UV2, decals |
| 0x28 | Position |
| 0x34 | Normal |
| 0x40 | Colour, 4 floats |
| 0x50 | Tangent |

Y is up and models face +Z, her left being +X, which needs no conversion for glTF.

## Materials

+0x00 is the type; +0x80 holds six u64 offsets to texture slots, each with its texture index in the low 16 bits of +0x04.

| Slot | Use |
|---|---|
| 0 | Base colour |
| 1 | **Unverified:** normal map, going by its colour |
| 2 | **Unverified:** a surface control map; unused by the export |
| 3 | Glow, on UV1, when it's more than a blank 8x8. Fei-Yen's blue visor sheet |
| 4 | Decal sheet, on UV2, clamped, when it's more than a blank 8x8 |
| 5 | **Unverified:** reflection map |

Type 5 is alpha blended. A type 5 material with a blank 8x8 base is a thin coat around the body, with a constant alpha of 35 on Fei-Yen. **Unverified:** it's probably for hyper mode. The export leaves it and the copy of a decal lying on it out of bodies, and keeps the copy on the surface beneath.

## HTX: textures

`HTEX` > a run of `HTSF`, each 0x20 of header then a DDS file. DXT1, DXT3 or DXT5. Material slots index this run.

## Motion

`HMOT` > `KFMO`. Its body:

| Offset | Holds |
|---|---|
| +0x04 | Track count |
| +0x10 | Frame count, float |
| +0x14 | Frames per second, 59.94 |
| +0x20 | Track table offset |

One track per node, in node order, 0x20 each: +0x07 kind, +0x08 curve list offset, +0x10 static value offset. Offsets are from the body's start.

The kind byte says which channels exist, in this order:

| Bits | Channels |
|---|---|
| 0xE0 | Translation x, y, z |
| 0x0F | Rotation as quaternion x, y, z, w |
| 0x0E | Rotation as Euler x, y, z in radians, composed Rz * Ry * Rx |

The static list has one float per channel. The curve list has one u64 per channel, 0 for a channel that holds still, or the whole list's offset is 0. A curve is +0x00 format, +0x02 shift, +0x18 data offset, with one value per frame, added to the static value:

| Format | Value |
|---|---|
| 1 | f32 |
| 2 | s16 / 2^shift |
| 3 | s8 / 2^shift |

Values are absolute local transforms, so a motion doesn't depend on the rest pose except for channels it leaves out.

Some banks hold motions for another skeleton. The export tells by comparing bones that hold still against the model's, and skips them.

`.mbm`: `MBMP` > `MBMD`; from its body, +0x04 count, +0x08 id list offset, +0x10 name offset list, followed by the `HMOT` files. `.mbv` is `MBHV` > `MBHD`, not decoded.

## The export

- **A-pose:** the game's rest pose has the arms straight down, through the body on some robots. The export swings them out 45 degrees from the shoulder bones in `APOSE`, and writes the old rotation into any motion that leaves it out. It warns if a vertex blends across a shoulder.
- **Armed models:** each body's mount bones are the unskinned leaf bones below its shoulders. **Unverified:** which weapon goes where. The game sets a mount value on each weapon in per-robot code (`FUN_013395a0`'s callers, the int at +0x128), which isn't decoded. The export puts a colour's first weapon on the right, and uses the left only for the robots in `ARMED_LEFT`. Fei-Yen's was checked against the game.
- **glTF limits:** a decal is a lifted copy of the triangles, as glTF has one base layer. Reflections, animated glow and tinting aren't reproduced.
