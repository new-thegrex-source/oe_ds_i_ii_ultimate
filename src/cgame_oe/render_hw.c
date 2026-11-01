/*
 * Render: the hardware rendering report (port; see cgame_common/oe_hw.h).
 *
 * The renderer draws as before; while a frame is being rendered on the GPU
 * as well, Render_drawLine and Render_renderSprite also hand each wall,
 * sprite wall and billboard they draw to this file, in world space, before
 * the view transform. The geometry follows the rasterizers exactly:
 *  - textured walls (Render_rasterizeWall): 117 x 117 texels per 64 x 64
 *    units, from the top down in 64-unit slices that each restart the
 *    texture, the top at zOffset + top - 0x20;
 *  - bitshapes (Render_rasterizeWall2): an 88 x 88 texel box, 64 units at
 *    scale 1.0 (spriteScale 0x16000), its bottom at zOffset - 0x20;
 *    billboards face the view, pulled 16 units towards it.
 * The floor and ceiling are the 32 x 32 tile planes of Render_drawFloorCeiling.
 */

#include "render_local.h"
#include "oe_hw.h"

#define SHAPE_BOX        0x58   /* bitshape box: 88 texels square */

static int16_t wallTex[128][4];   /* texture numbers per media and colour effect */
static int16_t spriteTex[1024][4];
static int16_t flatTex[8];

/* Render.segFlags colour effects -> texture variant (bit 2 grey, bit 3 bright) */
static int variant(int segFlags)
{
	return (segFlags >> 2) & 3;
}

static int variantFlags(int v)
{
	return ((v & 1) ? OE_HWTEX_GREY : 0) | ((v & 2) ? OE_HWTEX_BRIGHT : 0);
}

/* The map's textures are (re)loaded or its palettes changed: forget them. */
void RenderHw_resetTextures(Render *render)
{
	(void)render;
	memset(wallTex, 0, sizeof(wallTex));
	memset(spriteTex, 0, sizeof(spriteTex));
	memset(flatTex, 0, sizeof(flatTex));
	Hw_resetTextures();
}

static int wallTexture(Render *render, int media, int segFlags)
{
	int v = variant(segFlags);
	int id;

	if (media < 0 || media >= 128)
		return 0;
	if (wallTex[media][v])
		return wallTex[media][v];
	id = Hw_allocTexture();
	if (!id)
		return 0;
	Hw_texture(id, OE_HWTEX_COLS8, 0x75, 0x75, OE_HWTEX_REPEAT | variantFlags(v),
	           Hw_sourceOf(OE_HWSRC_WALL, render->wallMap[media].ofs), render->texels + render->wallMap[media].ofs, NULL,
	           render->palettes + render->wallMap[media].palette);
	wallTex[media][v] = (int16_t)id;
	return id;
}

/* A bitshape (see Render_loadBitShapes) unpacked into its 88 x 88 box. */
static int spriteTexture(Render *render, int media, int segFlags)
{
	static uint8_t idx[SHAPE_BOX * SHAPE_BOX], mask[SHAPE_BOX * SHAPE_BOX];
	const int16_t *shape = render->shapeData;
	int v = variant(segFlags);
	int ofs, col0, col1, c, id;
	int32_t texBase;

	if (media < 0 || media >= 1024)
		return 0;
	if (spriteTex[media][v])
		return spriteTex[media][v];
	id = Hw_allocTexture();
	if (!id)
		return 0;

	memset(idx, 0, sizeof(idx));
	memset(mask, 0, sizeof(mask));
	ofs = render->spriteMap[media].ofs;
	texBase = (int32_t)((uint32_t)(uint16_t)shape[ofs] | ((uint32_t)(uint16_t)shape[ofs + 1] << 16));
	col0 = shape[ofs + 2];
	col1 = shape[ofs + 3];
	for (c = 0; c <= col1 - col0; c++) {
		int x = col0 + c;
		int run = shape[ofs + 4 + c] + ofs + 2;
		if (x < 0 || x >= SHAPE_BOX)
			continue;
		for (;; run += 2) {
			int word = shape[run];
			int start = word & 0xff, len = word >> 8, k;
			const uint8_t *src;
			if (start == SHAPE_END)
				break;
			src = render->texels + texBase + shape[run + 1];
			for (k = 0; k < len; k++) {
				int y = start + k;
				if (y >= 0 && y < SHAPE_BOX) {
					idx[y * SHAPE_BOX + x] = src[k];
					mask[y * SHAPE_BOX + x] = 1;
				}
			}
		}
	}
	Hw_texture(id, OE_HWTEX_ROWS8, SHAPE_BOX, SHAPE_BOX, variantFlags(v), Hw_sourceOf(OE_HWSRC_SPRITE, texBase), idx, mask,
	           render->palettes + render->spriteMap[media].palette);
	spriteTex[media][v] = (int16_t)id;
	return id;
}

static int flatTexture(Render *render, const uint16_t *flat)
{
	int k, id;

	if (flat == NULL)
		return 0;
	k = (int)((flat - render->flatTexels) / 0x1000);
	if (k < 0 || k >= 8)
		return 0;
	if (flatTex[k])
		return flatTex[k];
	id = Hw_allocTexture();
	if (!id)
		return 0;
	Hw_texture(id, OE_HWTEX_RGB565, 64, 64, OE_HWTEX_REPEAT, Hw_sourceOf(OE_HWSRC_FLAT, k),
	           render->flatTexels + k * 0x1000, NULL, NULL);
	flatTex[k] = (int16_t)id;
	return id;
}

/* Render.blendMode -> the GPU's blend (Render_setPalette); 0 when not drawn */
static int blendOf(int mode, int *blend, int *alpha)
{
	*alpha = 255;
	switch (mode) {
	case 0: case 1: *blend = OE_HWB_OPAQUE; return 1;
	case 2: case 7: *blend = OE_HWB_ADD; return 1;
	case 3: case 0xb: case 0xf: *blend = OE_HWB_MULTIPLY; return 1;
	case 4: *blend = OE_HWB_ALPHA; *alpha = 64; return 1;      /* 1/4 texture */
	case 5: *blend = OE_HWB_ALPHA; *alpha = 128; return 1;
	case 6: *blend = OE_HWB_ALPHA; *alpha = 192; return 1;
	case 8: *blend = OE_HWB_ADD; *alpha = 192; return 1;
	case 9: *blend = OE_HWB_ADD; *alpha = 128; return 1;
	case 10: *blend = OE_HWB_ADD; *alpha = 64; return 1;
	default: return 0;   /* 0xc (grey what is behind), 0xff (nothing) */
	}
}

/* A textured wall from (x1, y1) to (x2, y2) (16.16), texture u from u1 to u2
 * (world units, 64 per repeat): the slices of Render_rasterizeWall. */
static void wallSlices(Render *render, int tex, int blend, int alpha, uint32_t flags,
                       int x1, int y1, int x2, int y2, int u1, int u2)
{
	int zOff = render->zOffset;
	int top = 0x40, bottom = render->floorZ;

	if (flags & 0xc0010000u) {
		top = render->ceilHeight;
		bottom = render->floorHeight;
		if (!(flags & LINE_FLOORCEIL))
			bottom = 0x40;
	}
	while (top > bottom) {
		int h = 0x40, topZ, skip = 0;
		if (flags & LINE_CLIP_CEIL) {
			int pieceTop = zOff + top - 0x20;
			if (pieceTop > render->ceilZ) {
				h = 0x40 - (pieceTop - render->ceilHeight);
				skip = pieceTop - render->ceilZ;
				top = render->ceilHeight - (zOff - 0x20);
			}
		}
		topZ = zOff + top - 0x20;
		if (topZ - h < bottom)
			h = topZ - bottom;
		if (h <= 0)
			break;
		Hw_wall(tex, blend, alpha, x1, y1, x2, y2, rshl(topZ - h, 16), rshl(topZ, 16),
		        rshl(u1, 10), rshl(u2, 10), rshl(skip, 10), rshl(skip + h, 10));
		top -= 0x40;
	}
}

/* A bitshape across (x1, y1)..(x2, y2) (16.16), u from u1 to u2 (world units
 * from the shape's first column): the box of Render_rasterizeWall2. */
static void shapeQuad(Render *render, int media, int tex, int blend, int alpha, uint32_t flags,
                      int x1, int y1, int x2, int y2, int u1, int u2)
{
	const int16_t *shape = render->shapeData;
	int col0 = shape[render->spriteMap[media].ofs + 2];
	int64_t scale = render->spriteScale > 0 ? ((int64_t)0x160000000LL / render->spriteScale) : 0x10000;
	int zb = rshl(render->zOffset - 0x20, 16);
	int zt = zb + (int)(scale * 64);
	/* texture s: (col0 + u * spriteScale / 65536) / 88 */
	int s1 = (int)(((int64_t)rshl(col0, 16) + (int64_t)u1 * render->spriteScale) / SHAPE_BOX);
	int s2 = (int)(((int64_t)rshl(col0, 16) + (int64_t)u2 * render->spriteScale) / SHAPE_BOX);
	int mirror = (flags & LINE_MIRROR) != 0;

	Hw_wall(tex, blend, alpha, x1, y1, x2, y2, zb, zt, s1, s2, mirror ? 0x10000 : 0, mirror ? 0 : 0x10000);
}

/* Render_drawLine: a wall or a wall-aligned sprite, world units, after the
 * back-face swap and the drunk wobble (render->seg holds its texture etc.). */
void RenderHw_line(Render *render, const Line *line)
{
	uint32_t flags = line->flags;
	int blend, alpha, tex;

	if (!Hw_inScene() || (flags & (LINE_OCCLUDER | LINE_HIDDEN)) || !blendOf(render->blendMode, &blend, &alpha))
		return;
	if (flags & LINE_SPRITE) {
		tex = spriteTexture(render, line->texture, render->segFlags);
		if (tex)
			shapeQuad(render, line->texture, tex, blend, alpha, flags, rshl(line->x1, 16), rshl(line->y1, 16),
			          rshl(line->x2, 16), rshl(line->y2, 16), line->z1, line->z2);
	} else {
		tex = wallTexture(render, line->texture, render->segFlags);
		if (tex)
			wallSlices(render, tex, blend, alpha, flags, rshl(line->x1, 16), rshl(line->y1, 16),
			           rshl(line->x2, 16), rshl(line->y2, 16), line->z1, line->z2);
	}
}

/* Render_renderSprite: a billboard at world (x, y) spanning left..right world
 * units across the view, texture u from u1 (left) to u2 (right); seg holds
 * its texture and flags (LINE_SPRITE: a bitshape, else a wall texture). */
void RenderHw_billboard(Render *render, int x, int y, int left, int right, int u1, int u2)
{
	const Line *seg = &render->seg;
	int fx = render->viewMatrix[0], fy = render->viewMatrix[1];   /* forward (16.16) */
	int rx = render->viewMatrix[3], ry = render->viewMatrix[4];   /* right */
	int px = rshl(x, 16) - fx * 16, py = rshl(y, 16) - fy * 16;   /* pulled 16 units towards the view */
	int x1 = px + left * rx, y1 = py + left * ry;
	int x2 = px + right * rx, y2 = py + right * ry;
	int blend, alpha, tex;

	if (!Hw_inScene() || !blendOf(render->blendMode, &blend, &alpha))
		return;
	if (seg->flags & LINE_SPRITE) {
		tex = spriteTexture(render, seg->texture, render->segFlags);
		if (tex)
			shapeQuad(render, seg->texture, tex, blend, alpha, seg->flags, x1, y1, x2, y2, u1, u2);
	} else {
		tex = wallTexture(render, seg->texture, render->segFlags);
		if (tex)
			wallSlices(render, tex, blend, alpha, seg->flags, x1, y1, x2, y2, u1, u2);
	}
}

/* Render_render, once the view is set up: the camera and the floor/ceiling planes. */
void RenderHw_beginFrame(Render *render)
{
	oeHwScene_t *s;
	short *tiles;
	int i;

	if (!Hw_enabled())
		return;
	Hw_beginScene();
	s = Hw_scene();
	s->viewX = rshl(render->viewX, 16);
	s->viewY = rshl(render->viewY, 16);
	s->viewZ = rshl(render->viewZ, 16);
	s->angle = render->viewAngle & 0x3ff;
	Hw_setRect(render->viewportX, render->viewportY, render->viewW, render->viewH);
	s->shiftX = render->canvas->shakeX * Brew_screenScale();   /* like the rect: screen (device) pixels */
	s->shiftY = render->canvas->shakeY * Brew_screenScale();
	s->floorZ = rshl(render->floorZ, 16);
	s->ceilZ = rshl(render->ceilZ, 16);

	tiles = Hw_flatTiles();
	for (i = 0; i < 1024; i++) {
		tiles[i] = (short)flatTexture(render, render->flatTile[render->floorFlat ? 0 : i]);
		tiles[1024 + i] = (short)flatTexture(render, render->flatTile[0x400 + (render->ceilFlat ? 0 : i)]);
	}
}
