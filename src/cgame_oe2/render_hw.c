/*
 * Render: the hardware rendering report (port; see cgame_common/oe_hw.h and
 * cgame_oe/render_hw.c, which this follows).
 *
 * Render_drawLine and Render_drawSpriteAt hand each wall, sprite wall and
 * billboard they draw to this file, in world space, before the view
 * transform. The geometry follows the rasterizers:
 *  - textured walls (Render_rasterizeWall): 117 x 117 texels per 64 x 64
 *    units in 64-unit slices from the top down, the top at projZ + top - 0x20;
 *    above a raised ceiling the top slice uses texture2;
 *  - bitshapes (Render_rasterizeWall2): an 88 x 88 texel box, 64 units at
 *    scale 1.0 (projScale 0x16000), its bottom at projZ - 0x20; billboards
 *    face the view, pulled 16 units towards it.
 * Texels live in four pages (Render.texelPages); flats are Render.flats.
 */

#include "render_local.h"
#include "oe_hw.h"

#define SHAPE_BOX        0x58   /* bitshape box: 88 texels square */

static int16_t wallTex[250][4];   /* texture numbers per media and colour effect */
static int16_t spriteTex[1500][4];
static int16_t flatTex[16];

/* Render.effectFlags colour effects -> texture variant (bit 2 grey, bit 3 bright) */
static int variant(uint32_t effectFlags)
{
	return (int)((effectFlags >> 2) & 3);
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

static int wallTexture(Render *render, int media, uint32_t effectFlags)
{
	const TexMap *tm;
	int v = variant(effectFlags);
	int id;

	if (media < 0 || media >= 250)
		return 0;
	if (wallTex[media][v])
		return wallTex[media][v];
	id = Hw_allocTexture();
	if (!id)
		return 0;
	tm = &render->wallMap[media];
	Hw_texture(id, OE_HWTEX_COLS8, 0x75, 0x75, OE_HWTEX_REPEAT | variantFlags(v),
	           Hw_sourceOf(OE_HWSRC_WALL, tm->page * TEXEL_PAGE_SIZE + tm->texelOfs),
	           render->texelPages + tm->page * TEXEL_PAGE_SIZE + tm->texelOfs, NULL,
	           render->palettes + tm->palette);
	wallTex[media][v] = (int16_t)id;
	return id;
}

/* A bitshape (see Render_loadBitShapes) unpacked into its 88 x 88 box. */
static int spriteTexture(Render *render, int media, uint32_t effectFlags)
{
	static uint8_t idx[SHAPE_BOX * SHAPE_BOX], mask[SHAPE_BOX * SHAPE_BOX];
	const int16_t *shape = (const int16_t *)render->shapeSpans;
	int v = variant(effectFlags);
	int ofs, col0, col1, c, id;
	const uint8_t *texels;
	int texBase;

	if (media < 0 || media >= 1500)
		return 0;
	if (spriteTex[media][v])
		return spriteTex[media][v];
	id = Hw_allocTexture();
	if (!id)
		return 0;

	memset(idx, 0, sizeof(idx));
	memset(mask, 0, sizeof(mask));
	ofs = render->spriteMap[media].ofs;
	texels = render->texelPages + ((render->shapeSpans[ofs + 1] >> 14) & 3) * TEXEL_PAGE_SIZE;
	texBase = (int)(render->shapeSpans[ofs] | ((uint32_t)(render->shapeSpans[ofs + 1] & ~0xc000u) << 16));
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
			src = texels + texBase + shape[run + 1];
			for (k = 0; k < len; k++) {
				int y = start + k;
				if (y >= 0 && y < SHAPE_BOX) {
					idx[y * SHAPE_BOX + x] = src[k];
					mask[y * SHAPE_BOX + x] = 1;
				}
			}
		}
	}
	Hw_texture(id, OE_HWTEX_ROWS8, SHAPE_BOX, SHAPE_BOX, variantFlags(v),
	           Hw_sourceOf(OE_HWSRC_SPRITE, (int)(texels - render->texelPages) + texBase), idx, mask,
	           render->palettes + render->spriteMap[media].palette);
	spriteTex[media][v] = (int16_t)id;
	return id;
}

static int flatTexture(Render *render, const uint16_t *flat)
{
	int k, id;

	if (flat == NULL)
		return 0;
	k = (int)((flat - render->flats) / 0x1000);
	if (k < 0 || k >= 16)
		return 0;
	if (flatTex[k])
		return flatTex[k];
	id = Hw_allocTexture();
	if (!id)
		return 0;
	Hw_texture(id, OE_HWTEX_RGB565, 64, 64, OE_HWTEX_REPEAT, Hw_sourceOf(OE_HWSRC_FLAT, k),
	           render->flats + k * 0x1000, NULL, NULL);
	flatTex[k] = (int16_t)id;
	return id;
}

/* Render.shade -> the GPU's blend (Render_setPalette); 0 when not drawn */
static int blendOf(int shade, int *blend, int *alpha)
{
	*alpha = 255;
	switch (shade) {
	case 0: case 9: case 10: *blend = OE_HWB_OPAQUE; return 1;
	case 1: *blend = OE_HWB_ALPHA; *alpha = 64; return 1;      /* 1/4 texture */
	case 2: *blend = OE_HWB_ALPHA; *alpha = 128; return 1;
	case 3: *blend = OE_HWB_ALPHA; *alpha = 192; return 1;
	case 4: *blend = OE_HWB_ADD; return 1;
	case 5: *blend = OE_HWB_ADD; *alpha = 192; return 1;
	case 6: *blend = OE_HWB_ADD; *alpha = 128; return 1;
	case 7: *blend = OE_HWB_ADD; *alpha = 64; return 1;
	case 11: *blend = OE_HWB_MULTIPLY; return 1;
	default: return 0;   /* 8 (grey what is behind), 12 (nothing) */
	}
}

/* A textured wall from (x1, y1) to (x2, y2) (16.16), texture u from u1 to u2
 * (world units, 64 per repeat): the slices of Render_rasterizeWall. */
static void wallSlices(Render *render, int tex, int tex2, int blend, int alpha, uint32_t flags,
                       int x1, int y1, int x2, int y2, int u1, int u2)
{
	int zOff = render->projZ;
	int top = 0x40, bottom = render->effFloor;
	int multiPiece = (flags & 0xc0010000u) != 0;
	int piece = 0;

	if (multiPiece) {
		top = render->ceilHeight;
		bottom = render->floorHeight;
		if (!(flags & LINE_FLOORCEIL))
			bottom = 0x40;
	}
	while (top > bottom) {
		int h = 0x40, topZ, skip = 0;
		if (flags & LINE_CLIP_CEIL) {
			int pieceTop = zOff + top - 0x20;
			if (pieceTop > render->effCeil) {
				h = 0x40 - (pieceTop - render->ceilHeight);
				skip = pieceTop - render->effCeil;
				top = render->ceilHeight - (zOff - 0x20);
			}
		}
		topZ = zOff + top - 0x20;
		if (topZ - h < bottom)
			h = topZ - bottom;
		if (h <= 0)
			break;
		Hw_wall(piece == 0 && tex2 ? tex2 : tex, blend, alpha, x1, y1, x2, y2, rshl(topZ - h, 16), rshl(topZ, 16),
		        rshl(u1, 10), rshl(u2, 10), rshl(skip, 10), rshl(skip + h, 10));
		piece++;
		top -= 0x40;
		if (!multiPiece)
			break;
	}
}

/* A bitshape across (x1, y1)..(x2, y2) (16.16), u from u1 to u2 (world units
 * from the shape's first column): the box of Render_rasterizeWall2. */
static void shapeQuad(Render *render, int media, int tex, int blend, int alpha, uint32_t flags,
                      int x1, int y1, int x2, int y2, int u1, int u2)
{
	const int16_t *shape = (const int16_t *)render->shapeSpans;
	int col0 = shape[render->spriteMap[media].ofs + 2];
	int64_t scale = render->projScale > 0 ? ((int64_t)0x160000000LL / render->projScale) : 0x10000;
	int zb = rshl(render->projZ - 0x20, 16);
	int zt = zb + (int)(scale * 64);
	/* texture s: (col0 + u * projScale / 65536) / 88 */
	int s1 = (int)(((int64_t)rshl(col0, 16) + (int64_t)u1 * render->projScale) / SHAPE_BOX);
	int s2 = (int)(((int64_t)rshl(col0, 16) + (int64_t)u2 * render->projScale) / SHAPE_BOX);
	int mirror = (flags & LINE_MIRROR) != 0;

	Hw_wall(tex, blend, alpha, x1, y1, x2, y2, zb, zt, s1, s2, mirror ? 0x10000 : 0, mirror ? 0 : 0x10000);
}

/* Render_drawLine: a wall or a wall-aligned sprite, world units, after the
 * back-face swap and the drunk wobble. */
void RenderHw_line(Render *render, const Line *line)
{
	uint32_t flags = line->flags;
	int blend, alpha, tex, tex2 = 0;

	if (!Hw_inScene() || (flags & (LINE_OCCLUDER | LINE_HIDDEN)) || !blendOf(render->shade, &blend, &alpha))
		return;
	if (flags & LINE_SPRITE) {
		tex = spriteTexture(render, line->texture, render->effectFlags);
		if (tex)
			shapeQuad(render, line->texture, tex, blend, alpha, flags, rshl(line->x1, 16), rshl(line->y1, 16),
			          rshl(line->x2, 16), rshl(line->y2, 16), line->z1, line->z2);
		return;
	}
	tex = wallTexture(render, line->texture, render->effectFlags);
	if (render->ceilHeight > 0x40 && (flags & 0xc0010000u) && (uint16_t)line->texture != (uint16_t)line->texture2)
		tex2 = wallTexture(render, line->texture2, render->effectFlags);
	if (tex)
		wallSlices(render, tex, tex2, blend, alpha, flags, rshl(line->x1, 16), rshl(line->y1, 16),
		           rshl(line->x2, 16), rshl(line->y2, 16), line->z1, line->z2);
}

/* Render_drawSpriteAt: a billboard at world (x, y) spanning left..right world
 * units across the view, texture u from u1 (left) to u2 (right); tmpLine
 * holds its texture and flags (LINE_SPRITE: a bitshape, else a wall texture). */
void RenderHw_billboard(Render *render, int x, int y, int left, int right, int u1, int u2)
{
	const Line *seg = &render->tmpLine;
	int fx = render->viewPlane[0], fy = render->viewPlane[1];   /* forward (16.16) */
	int rx = render->viewPlane[3], ry = render->viewPlane[4];   /* right */
	int px = rshl(x, 16) - fx * 16, py = rshl(y, 16) - fy * 16;  /* pulled 16 units towards the view */
	int x1 = px + left * rx, y1 = py + left * ry;
	int x2 = px + right * rx, y2 = py + right * ry;
	int blend, alpha, tex;

	if (!Hw_inScene() || !blendOf(render->shade, &blend, &alpha))
		return;
	if (seg->flags & LINE_SPRITE) {
		tex = spriteTexture(render, seg->texture, render->effectFlags);
		if (tex)
			shapeQuad(render, seg->texture, tex, blend, alpha, seg->flags, x1, y1, x2, y2, u1, u2);
	} else {
		tex = wallTexture(render, seg->texture, render->effectFlags);
		if (tex)
			wallSlices(render, tex, 0, blend, alpha, seg->flags, x1, y1, x2, y2, u1, u2);
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
	Hw_setRect(render->fbOffsetX, render->fbOffsetY, render->width, render->height);
	s->floorZ = rshl(render->effFloor, 16);
	s->ceilZ = rshl(render->effCeil, 16);

	tiles = Hw_flatTiles();
	for (i = 0; i < 1024; i++) {
		tiles[i] = (short)flatTexture(render, render->floorTiles[render->floorUniform ? 0 : i]);
		tiles[1024 + i] = (short)flatTexture(render, render->ceilTiles[render->ceilUniform ? 0 : i]);
	}
}
