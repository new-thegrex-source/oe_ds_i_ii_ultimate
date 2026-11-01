# Orcs & Elves DS: map format

Reverse engineered from the USA ROM (`YOEE`, arm9 code at `0x02000000`; the European ROM has the
same data files). Loader functions in the Ghidra project `C:\Experements\OE_NDS_RE\ghidra\OE_DS.gpr`:
`Render_beginLoadMap` (`0x02042364`), `Render_finishLoadMap` (`0x02042694`),
`Render_LoadMapNodes` (`0x020468f4`), the `.light` loader `0x02046bbc` and its lookup `0x02046c38`,
`TextureMappings_LoadShaders` (`0x02062b04`).

Checked: every `.bsp` of the 14 maps parses to the last byte; every `.lz` node list runs without
an unknown GX command; `map_m01` rendered from these files shows the level with correct
geometry, texture mapping and baked lighting (`C:\Experements\OE_NDS_RE\export\map_m01\view*.png`).

## Files

All under NitroFS `data/` (unpacked in `C:\Experements\OE_NDS_RE\usa\data`). `*.lz` files are
Nintendo LZ10 (`0x10`, u24 size, flag byte MSB first, 2-byte back references of 3..18 bytes).

| File | Contents |
|---|---|
| `maps/<map>.bsp` | level data, a variant of the BREW `.bsp` (version 7): BSP nodes, wall lines, sprites, emitters, tile events, script commands, tile flags |
| `maps/<map>.lz` | 3D geometry: one NDS GX display list per BSP leaf |
| `maps/<map>.light` | 32×32 RGB light grid for sprites and entities |
| `shaders.bin` | texture, palette and "shader" tables (shader id → texture + palette) |
| `textures/texels.bin` | texture pixels, each texture LZ10-compressed, native NDS formats |
| `textures/palettes.bin` | palettes, RGB555 |
| `textures/models.bin` | 61 object models (GX lists; not covered yet) |
| `strings/<language>/map_<map>.bin` | the map's strings (the BREW `.bsp` held them inline) |

Maps: `map_m01`..`map_m08`, `map_hub`, `map_bug`, `map_forge`, `map_lava`, `map_menu`, `map_test`
(path built from `rom:/maps/%s.bsp`, `%s.lz`, `%s.light`).

## Coordinates

A map is 32×32 tiles of 64 units. `.bsp` stores coordinates as bytes shifted left by 3 (0..2040).
Z points up, the floor is at 0, and the ceiling is at the header's ceiling height (64 or 128). In the
`.lz` lists each leaf scales by 64 and then translates by tile coordinates, so vertices are in tile
units. A tile number is `y * 32 + x`.

## `.bsp`

Little-endian, read sequentially (`u8`/`u16`/`u32`). Differences from BREW are marked **DS**.

### Header (25 bytes; BREW: 39)

| Field | Type | Notes |
|---|---|---|
| version | u8 | 7 |
| save key | u32 | |
| name | u16 | **DS:** string id instead of BREW's `char[16]` |
| colours | 9 × u8 | 3 RGB triples: floor, ceiling, intro; the DS converts the first two to RGB555 |
| unknown | u8 | BREW `f1e024` |
| spawn tile | u16 | |
| spawn direction | u8 | |
| menu spawn tile | u16 | |
| floor height | u8 | 0, or 18 in `map_m02` |
| ceiling height | u8 | 64 or 128 |
| map flag bits | u8 | |

### Sections, in order

| Section | Layout | Notes |
|---|---|---|
| nodes | `u16 n` (≤ 0x200), `n × 10`: 4 bbox bytes (`<<3`), u8 a, u8 b, u16 c, u16 d | same as BREW; `args1 = a<<16 \| b<<3 \| 0xff000000`, `args2 = d<<16 \| c`; a node with `args1 & 0x30000 == 0` is a leaf and owns a model in `.lz` |
| lines | `u16 n` (≤ 0x400), `n × 10`: x1, y1, x2, y2 (`<<3`), u8 texture, **u8 3D texture**, u32 flags | **DS:** the extra byte is the wall's 3D texture; the loader registers shader `byte + 0x101`. Flags as in BREW: 0x8/0x10 side, 0x100/0x200 door axis, 0x800..0x4000 facing, 0x80000 moving line (≤ 0xa0) |
| sprites | `u16 a`, `u16 b`; `a × 6`: x, y (`<<3`), u32 info (z = 0x20); `b × 7`: x, y (`<<3`), u8 z (stored + 0x20), u32 info | **DS:** info is one u32 (BREW: byte + short, or 2 bytes + short); low byte = tile/sprite id |
| emitters | **DS only:** `u32 n`, `n × 8`: u32 type, u32 packed | packed: tile x = bits 15..19, tile y = bits 10..14, two 5-bit params at bits 5 and 0; position = `tile * 64 + 32`; passed to the particle system (`emitters.bin`) |
| tile events | `u16 n` (≤ 250), `n × u16` tile, then `n × u32` event | **DS:** tiles moved to their own array; `event & 0x1ff` indexes it (BREW: `event & 0x3ff` was the tile itself). `event & 0x7f00000` = number of commands; command index = `(event & mask) >> 9` |
| commands | `u16 n`, `n × 9`: u8 op, u32 arg1, u32 arg2 | same as BREW; op 0x22 changes a tile's sprite (`arg1 >> 13`) |
| tile flags | 0x200 bytes: 2 tiles per byte, 4 bits each, low nibble first | **DS:** BREW had 0x100 bytes, 2 bits per tile; bits 0x20/0x200 are added at load for tiles with events |
| flats | 2 × 0x400 bytes: floor id, ceiling id per tile | BREW leftover; **the DS does not read it** (the loader closes the file before) |

There are no inline map strings; they live in `strings/<language>/map_<map>.bin`.

## `.lz`: 3D geometry

After LZ10 there is one block per BSP leaf, in node order (the block count equals the leaf count, e.g. 199
nodes → 100 blocks). The loader stops at `MAX_NODE_MODELS` (0x100) blocks.

| Offset | Type | Contents |
|---|---|---|
| 0 | u32 | header size H (12 + 12·n) |
| 4 | u32 | display list size S |
| 8 | u32 | n patch entries |
| 12 | n × {u32, u32, u32} | offset of a `TEXIMAGE_PARAM` parameter, offset of the following `PLTT_BASE` parameter (both relative to the list), shader id |
| H | S bytes | packed NDS GX commands (GBATEK "3D Geometry Commands") |

The next block starts at H + S. At load the game patches the VRAM address of each patch entry's texture into the
`TEXIMAGE_PARAM` / `PLTT_BASE` words. A list typically starts with `MTX_SCALE` 64, `MTX_TRANS` (leaf
tile position), then `TEXIMAGE_PARAM` / `PLTT_BASE` / `POLYGON_ATTR` / `BEGIN_VTXS` and per vertex
`COLOR` (baked light), `TEXCOORD`, `VTX_16`/`VTX_10`/`VTX_XY`/`VTX_XZ`/`VTX_YZ`/`VTX_DIFF`. The list's
own `TEXIMAGE_PARAM` value gives the texture size, repeat/flip and "colour 0 transparent" (bit 29);
only the address is patched.

## `.light`

0xC00 bytes = 32×32 tiles × RGB (u8 each), row by row (`(y * 32 + x) * 3`). For a position the game
bilinearly interpolates the four nearest tiles with 6-bit fractions and returns RGB555
(`0x02046c38`); it lights sprites and entities. The walls have their light in the vertex colours.

## Textures: `shaders.bin`, `texels.bin`, `palettes.bin`

`shaders.bin` (fully consumed by this layout):

| Part | Layout |
|---|---|
| header | u32 palettes (384), u32 textures (431), u32 models (61), u32 shaders (939) |
| textures | × 11 bytes: u8 format (low 4 bits = NDS format; 0x80 flag), u8 S size (`8 << s`), u8 T size, u8 width (0 = 256), u8 height (0 = 256), u32 **end** offset in `texels.bin`, u16 decompressed size |
| palettes | × u32 **end** offset in `palettes.bin` (colour count = length / 2) |
| models | × u32 end offset in `models.bin` (model i runs from the previous end, 0 for the first) |
| shaders | × { u8 flag, u16 key, u16 palette index, u16 texture index, u8 n, n bytes } |

- A texture starts where the previous one ended; its bytes in `texels.bin` are an LZ10 stream of native NDS texels.
  Of the 431 textures: 286 are format 4 (256 colours, 8bpp), 63 are format 6 (A5I3, translucent), 56 are format 3
  (16 colours, 4bpp), 26 are format 1 (A3I5). Formats 2, 5 and 7 are not used. A row is `8 << S` texels long;
  width × height are the used part.
- Shader lookup (`0x02062ffc`): the key is `id * 16 + frame`. The first shader with `key & 0xfff0 == id * 16` is frame 0;
  the following ones are animation frames. The trailing n bytes are not decoded yet (likely animation data).
- Ids: sprites/tiles below 0x100; wall textures `0x101 + line 3D texture byte`.

## Sprites

`Render_drawSprite` (`0x020491d4`) takes a sprite's tile (`info & 0xff`, + 0x101 with `info & 0x4000`)
and frame (`(info >> 8) & 15`) and looks up that shader. Notes:

- **Texture storage.** Sprite textures are stored bottom row first: the top of a quad is t = height − 1.
  Rows are packed `w` texels long; the game lays them out at the VRAM width itself.
- **Big textures.** Texture format bit 0x80 means the sprite is drawn at 2/3 of its size: 96 texels
  become 64 units.
- **Place and turn.** The base matrix is a translation to `(x, y, z − 32)`, then turns about X, Y and Z.
  With `0x2000000` it is `z + 32` and turned upside down. The wall flags `0x8000`/`0x10000`/`0x40000`/`0x20000`
  give a Z turn of 0x300/0x100/0x200/0 (1024 a turn).
- **Shader with a program.** The program is the shader's trailing bytes in `shaders.bin`, run by
  `0x0204c73c` (drawn via `0x02048110`).
- **Shader without one.** It is a billboard (`0x020484fc`): `(w − 1) × (h − 1)`, facing the camera, or
  flat on the wall with the wall flags. The U mirror is `0x100000`, which a wall facing X turns around.
- **Light.** The colour comes from `.light` (always for billboards, with shader flag 1 for programs).
- **Size.** The sprite's byte `+0x15` scales billboards and programs (64 = 1.0). `Entity_initspawn`
  (`0x020241d4`) sets it per monster subtype: 0x2c for subtypes 1, 3 and 6 with parm 0, 0x58 for 0x12,
  0x48 for 0x13.
- **Doors.** They are not in the leaves' geometry. `0x0204b6f4` draws each line of a leaf with texture
  0x10..0x1f (flags 2, 0x40000 and 0x1080000 clear) as the program of tile `texture + 0x101` at the
  line's middle: z 32, turned 0x300 with line flag 0x100 or 0x200 with 0x200, nudged 3 units toward the
  camera. The door programs draw model 15 (a 64 × 64 door) with the door's texture. Moving lines
  (`0x80000`, pushable blocks) are upright quads instead (`0x0204b3c4`).

A program is a byte code. Values are 4 bytes `{ type | phase << 4, low, high, period / 32 }`
(`0x0204bce0`):

- The type is 0 constant, 1 sawtooth, 2 sine, 3 square, 4 random, or 5 table.
- The value runs from `low` to `high`; ops marked "−128" below take `low − 128` and `high − 128`.
- The result is `<< 12`, over time `t`: `u = (phase << 8) + (t << 12) / period`.

| Op | Arguments | Meaning |
|---|---|---|
| 0..7 | value | material colours (lighting) |
| 8, 9, 10 / 0x0b / 0x14 | value | scale X, Y, Z / all / X and Y (64 = 1.0) |
| 0x0c, 0x0d, 0x0e / 0x0f / 0x15 | value −128 | translate X, Y, Z / all / X and Y (units) |
| 0x10, 0x11, 0x12 / 0x13 | value −128 | turn about X, Y, Z / all (`>> 9`: 8/1024 of a turn per unit) |
| 0x16 | u8 flags, value | polygon attributes: alpha (0..31); flags bit 4 lighting, bits 0..3 mode and culling |
| 0x17 | u16 id, value frame, u8 flags | bind texture: shader `id`, frame; flags bit 0 colour 0 clear, bits 3..4 repeat, bits 5..6 mirror |
| 0x18 / 0x19 | | the following transforms go to the texture matrix (scrolling) / back to the position matrix |
| 0x1a / 0x1b | | identity / back to the sprite's matrix |
| 0x1c | u8 kind | draw the bound texture. Kind < 0xfd: model `kind` of `models.bin` (a GX list without a texture). 0xff: a quad facing the camera (w/2 each side, h − 1 up). 0xfe: a quad in the object's Y-Z plane. 0xfd: a quad on its X-Y plane |

Examples:

- **Torch (0x88).** Two camera-facing quads: the pole, then the flame (frames 1..5 sawtooth) 31 units up.
- **Crystals (0x62..0x68).** A model with a scrolling texture, plus a translucent pulsing glow model.
- **Ghost (0x42).** A translucent model bobbing on three sines.

## Script cameras

`Game_loadCameras` (`0x020345e8`) builds the cameras from the event commands. A camera is the commands
between ops 0x3e and 0x3f; when run, these two open and close a skippable cinematic. Its keys are:

- **0x36, a tile.** x is bits 0-4, y bits 5-9, z is (s8) bits 10-17, the direction bits 18-22. Bits 31..28
  take x, y, z and the angle from the player when the camera starts (key 1).
- **0x37 + 0x38, a world point.** 0x37 gives x (bits 0-10), y (bits 11-21) and z (bits 22-29, minus 0x80).
  The following 0x38 gives pitch (bits 0-9), yaw (bits 10-19) and roll (bits 20-29), and then becomes
  the key's 0x36.

0x35 sets the duration of the following keys and 0x39 the loop flag. Each key command's argument is
rewritten to `camera | key << 8`, which runs that key (0x36 with key 0 stops the camera). At most 0x60
cameras and 0x180 keys.

## In the engine

`src/client/cl_dsmap.c` (fs_game `oeds`, `oe_dsmap <map>`) reads these formats straight from
`oeds/rom.nds` and draws the maps with the Quake 3 renderer; see README-OE.md.

## Tools

In `C:\Experements\OE_NDS_RE\scripts`:

- `NdsMap.cs`: LZ10; the `shaders.bin` tables and texture decode; the GX interpreter for `.lz` lists
  (matrix stack, all primitive types); OBJ/MTL/PNG export; a software-rendered preview.
- `dsmap.ps1`: runs the export (`dsmap.ps1 -maps map_m01,map_hub`); output goes to `C:\Experements\OE_NDS_RE\export\<map>\`.

## Not done yet

- The texture matrix of programs (scrolling) and the material colours / lighting of program ops 0..7.
- The header's save key and unknown byte, node args a/b/c/d in detail, and the meaning of the emitter types.
- What decides a monster's facing and animation (the entity state of `Render_drawSprite`).
- The translucent formats (1, 6) are decoded, but their alpha has not yet been checked against the game's output.
