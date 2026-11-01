/*
 * Render: startup/shutdown and everything that loads renderer data from the
 * .bar: sine table (5002), palettes (5005), texture mappings (5007),
 * bitshapes (5006, sprite masks compiled to column run lists), texels (flats
 * 7000+n, walls 7512+n, sprites 9000+n) and the .bsp map itself (nodes, lines,
 * sprites, tile events, commands, map strings, map flags, floor/ceiling
 * flats). Also the media "registration" that decides which textures and
 * sprite frames a map needs, the BSP leaf lookups / relinking of sprites and
 * moving lines, and the 3D view rectangle.
 *
 * Every resource is inflated whole into the InputStream buffer
 * (Resource_open(.., 1)) and parsed with Resource_shiftInt/Resource_shiftShort
 * (LE int32 / signed int16) or plain byte reads.
 */

#include "render_local.h"
#include "oe_hw.h"

#define RES_SINTABLE   5002
#define RES_PALETTES   5005
#define RES_BITSHAPES  5006
#define RES_MAPPINGS   5007
#define RES_FLAT_BASE  7000
#define RES_WALL_BASE  7512
#define RES_SPRITE_BASE 9000
#define MAP_HUB        6008   /* hub.bsp: one extra sprite (Render.hubSprite) */

#define BSP_VERSION    7
#define MAX_NODES      600
#define MAX_LINES      1500
#define MAX_SPRITES    1000
#define MAX_MOVING     160
#define MAX_EVENTS     250
#define MAX_COMMANDS   1000
#define MAX_FLATS      8
#define MAX_SHAPEDATA  0x20000
#define MAX_TEXELS     0x100000
#define WALL_TEXELS    0x3579  /* 117 x 117 8-bit texels */
#define WALL_TEXELS4   0x1abd  /* bytes of a 4-bit wall texture */
#define NUM_CUSTOM_SPRITES 24
#define NUM_DROP_SPRITES   8

/* App error codes used here */
#define ERR_GENERIC    1
#define ERR_NOMEM      2      /* also "limit exceeded" */
#define ERR_BADMAP     4
#define ERR_TRUNCATED  0x12
#define ERR_NORES      0x16
#define ERR_FATAL      (-666)

/* Developer error strings in the .mod's data area */
#define MOD_STR_PALETTE_MISMATCH 0x2fc40u
#define MOD_STR_FLAT_TRUNCATED   0x2fc6cu
#define MOD_STR_TILE_TRUNCATED   0x2fc94u
#define MOD_STR_MAX_TEXELS       0x2fcbcu
#define MOD_STR_SPRITE_TRUNCATED 0x2fcfcu
#define MOD_STR_MAX_SHAPEDATA    0x2fd24u
#define MOD_STR_NO_SUCH_TILE     0x2fd48u
#define MOD_STR_NO_SUCH_SPRITE   0x2fd58u
#define MOD_STR_MAX_MOVING_LINES 0x2fd6cu

static char *modStr(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

/* Next byte of the map stream (Render.mapPos is the read cursor). */
static int mapByte(Render *r, const uint8_t *buf)
{
	return buf[r->mapPos++];
}

/* The original had numMovingLines as a byte read with LDRB (unsigned). */
static int movingCount(const Render *r)
{
	return (uint8_t)r->numMovingLines;
}

/* Child to descend into from BSP node `n` for point (x, y). Split on x when
 * args1 bit 16, else on y; the split coordinate is args1's low 16 bits. Points
 * below the split go to the high-16 child of args2, others to the low one.
 * `inclusive` makes a point exactly on the split count as below (only
 * Render_linkMovable does that). */
static int bspChild(const Node *n, int x, int y, int inclusive)
{
	int split = (int)(n->args1 & 0xffff);
	int v = (n->args1 & 0x10000) ? x : y;
	int below = inclusive ? (v <= split) : (v < split);
	return below ? (int)(n->args2 >> 16) : (int)(n->args2 & 0xffff);
}

static int isLeaf(const Node *n)
{
	return ((n->args1 & 0x3ffff) >> 16) == 0;
}

/* ====================================================================== */

/* 0x00029608 */
int Render_startup(Render *r)
{
	Applet *app;
	InputStream s;
	int ok = 0;

	if (r->app == NULL) /* port: back-pointer normally set by App_initSubsystems */
		r->app = g_app;
	app = r->app;
	r->canvas = app->canvas;
	r->game = app->game;
	r->player = app->player;
	InputStream_init(&r->mapStream, app, 1);
	r->rNoTexture = 0;
	r->r1f171 = 0;
	r->rSkipCull = 0;
	r->rSkipBSP = 0;
	r->rSkipLines = 0;
	r->r1f176 = 0;
	r->rSkipSprites = 0;
	r->noViewOffset = 0;
	r->mapPos = 0;
	r->numLines = 0;
	r->numNodes = 0;
	r->numMapSprites = 0;
	r->menuSpawnTile = 0;
	r->floorColor = 0;
	r->ceilColor = 0;
	r->ceilZ = 0x40;
	r->floorZ = 0;
	r->frameTime = 0;
	r->floorFlat = 0;
	r->ceilFlat = 0;
	r->wobble = 0;
	r->mapStrings = NULL;
	r->numMapStrings = 0;

	InputStream_ctor(&s, r->app, 1);
	if (!Resource_open(&s, RES_SINTABLE, 1)) {
		App_setErrorCode(g_app, ERR_NORES);
	} else {
		/* 1024 x int32 16.16 sine (the original memmove'd the whole resource) */
		const uint8_t *src = InputStream_getBuffer(&s);
		int size = InputStream_getSize(&s);
		int n = size / 4;
		if (n > 1024)
			n = 1024;
		for (int i = 0; i < n; i++)
			r->sinTable[i] = (int32_t)((uint32_t)src[i * 4] | ((uint32_t)src[i * 4 + 1] << 8) |
			                           ((uint32_t)src[i * 4 + 2] << 16) | ((uint32_t)src[i * 4 + 3] << 24));
		InputStream_close(&s);

		/* IDISPLAY_GetClipRect: the whole screen */
		{
			IBitmap *screen = Brew_screen();
			r->clipRect[0] = 0;
			r->clipRect[1] = 0;
			r->clipRect[2] = (int16_t)(screen ? screen->width : 0);
			r->clipRect[3] = (int16_t)(screen ? screen->height : 0);
		}
		/* The original QueryInterface'd the device bitmap (app+0x78) for its
		 * IDIB (AEECLSID_DIB 0x1001045, after clearing Render.dib); the port
		 * describes Brew_screen() and reuses an IDIB it already allocated. */
		Render_syncScreenDib(r);
		if (r->dib != NULL && (r->dib->nColorScheme == 15 || r->dib->nColorScheme == 16) &&
		    r->dib->nDepth == 16) {
			Render_loadPalettes(r);
			ok = 1;
		} else {
			App_setErrorCode(g_app, ERR_GENERIC);
		}
	}
	InputStream_dtor(&s, 0);
	return ok;
}

/* 0x00026de8 */
void Render_shutdown(Render *r)
{
	Render_unloadMap(r);
	if (r->columnZ != NULL) {
		free(r->columnZ);
		r->columnZ = NULL;
	}
	/* IDIB_Release (allocated by Render_syncScreenDib in the port) */
	free(r->dib);
	r->dib = NULL;
}

/* 0x00019608 mappings.bin: six int counts (walls, flats, sprites, then the
 * three remap tables), {int ofs, int palette} records for walls/flats/sprites
 * (palette stored <<8 = index of its first entry in Render.palettes), then the
 * short tables wallBase, flatRemap, spriteBase. */
int Render_loadMappings(Render *render)
{
	InputStream s;
	int ok;

	InputStream_ctor(&s, render->app, 1);
	if (!Resource_open(&s, RES_MAPPINGS, 1)) {
		App_setErrorCode(g_app, ERR_NORES);
		ok = 0;
	} else {
		void *buf = InputStream_getBuffer(&s);
		int pos = 0;
		int numWalls = Resource_shiftInt(buf, &pos);
		int numFlats = Resource_shiftInt(buf, &pos);
		int numSprites = Resource_shiftInt(buf, &pos);
		int numWallBase = Resource_shiftInt(buf, &pos);
		int numFlatRemap = Resource_shiftInt(buf, &pos);
		int numSpriteBase = Resource_shiftInt(buf, &pos);
		int i;

		/* The original wrote without bounds checks; the port drops entries
		 * beyond the tables (the 1049 data fits: 120/13/982/187/210/256). */
		render->memMappings = 0;
		for (i = 0; i < numWalls; i++) {
			int ofs = Resource_shiftInt(buf, &pos);
			int pal = Resource_shiftInt(buf, &pos);
			if (i < 128) {
				render->wallMap[i].ofs = ofs;
				render->wallMap[i].palette = rshl(pal, 8);
			}
		}
		render->memMappings += 0x400;
		for (i = 0; i < numFlats; i++) {
			int ofs = Resource_shiftInt(buf, &pos);
			int pal = Resource_shiftInt(buf, &pos);
			if (i < 16) {
				render->flatMap[i].ofs = ofs;
				render->flatMap[i].palette = rshl(pal, 8);
			}
		}
		render->memMappings += 0x80;
		for (i = 0; i < numSprites; i++) {
			int ofs = Resource_shiftInt(buf, &pos);
			int pal = Resource_shiftInt(buf, &pos);
			if (i < 1024) {
				render->spriteMap[i].ofs = ofs;
				render->spriteMap[i].palette = rshl(pal, 8);
			}
		}
		render->memMappings += 0x2000;
		for (i = 0; i < numWallBase; i++) {
			int v = Resource_shiftShort(buf, &pos);
			if (i < 256)
				render->wallBase[i] = (int16_t)v;
		}
		render->memMappings += 0x200;
		for (i = 0; i < numFlatRemap; i++) {
			int v = Resource_shiftShort(buf, &pos);
			if (i < 256)
				render->flatRemap[i] = (int16_t)v;
		}
		render->memMappings += 0x200;
		for (i = 0; i < numSpriteBase; i++) {
			int v = Resource_shiftShort(buf, &pos);
			if (i < 300)
				render->spriteBase[i] = (int16_t)v;
		}
		render->memMappings += 600;
		ok = 1;
	}
	InputStream_dtor(&s, 0);
	return ok;
}

/* 0x00019ea0 palettes.bin: short count, then per palette a short n and n
 * RGB565 entries (palette i lives at Render.palettes[i * 256]). The whole
 * resource must be consumed exactly. */
void Render_loadPalettes(Render *render)
{
	InputStream s;

	render->memPalettes = App_getMemUsed(render->app);
	InputStream_ctor(&s, render->app, 1);
	if (!Resource_open(&s, RES_PALETTES, 1)) {
		App_setErrorCode(g_app, ERR_NORES);
	} else {
		const uint8_t *src = InputStream_getBuffer(&s);
		int size = InputStream_getSize(&s);
		int numShorts = size / 2;
		int k = 1; /* read cursor in shorts */

		render->numPalettes = (int16_t)le16(src);
		memset(render->palettes, 0, sizeof render->palettes);
		for (int p = 0; p < render->numPalettes; p++) {
			int n = k < numShorts ? le16(src + k * 2) : 0;
			k++;
			for (int j = 0; j < n; j++, k++) {
				int dst = p * 256 + j;
				if (k < numShorts && dst < (int)(sizeof render->palettes / sizeof render->palettes[0]))
					render->palettes[dst] = le16(src + k * 2);
			}
		}
		if (k * 2 != size) {
			App_setErrorStr(render->app, modStr(MOD_STR_PALETTE_MISMATCH), k * 2, size);
			App_setErrorCode(g_app, ERR_GENERIC);
		}
		render->memPalettes = rshl(render->numPalettes, 9);
		RenderHw_resetTextures(render);   /* port: the GPU's copies are stale */
	}
	InputStream_dtor(&s, 0);
}

/* Bubble-sort a registered media list by a TexMap table's ofs (ascending), so
 * media sharing data are adjacent and loaded once. */
static void sortMediaByOfs(int32_t *media, int n, const TexMap *map)
{
	for (int i = 0; i < n - 1; i++) {
		for (int j = 0; j < n - 1 - i; j++) {
			int a = media[j], b = media[j + 1];
			if (map[b].ofs < map[a].ofs) {
				media[j] = b;
				media[j + 1] = a;
			}
		}
	}
}

/* 0x0001a088 bitshapes.bin: int total size, then shapes at 4 + ofs:
 *   +0 int sprite-texel resource (9000 + n), +6 u16 compiled size (shorts*2?),
 *   +8/+9 first/last column, +10/+11 first/last row, +12 column bit masks
 *   (ceil(rows/8) bytes per column, LSB = first row).
 * Each registered shape is compiled once into Render.shapeData:
 *   [0],[1] texel resource (later the texel offset, Render_loadTexels),
 *   [2],[3] first/last column, [4+c] offset of column c's runs minus 2,
 *   runs {len<<8 | row, texel index}, terminated by 0x7f.
 * spriteMap[frame].ofs becomes the shape's index in shapeData. */
int Render_loadBitShapes(Render *render)
{
	InputStream s;
	int ok = 0;

	render->memShapes = App_getMemUsed(render->app);
	InputStream_ctor(&s, render->app, 1);
	if (!Resource_open(&s, RES_BITSHAPES, 1)) {
		App_setErrorCode(g_app, ERR_NORES);
	} else {
		const uint8_t *buf = InputStream_getBuffer(&s);
		int n = render->numSpriteMedia;
		int total = 0, prev = -1;
		int i;

		sortMediaByOfs(render->spriteMedia, n, render->spriteMap);

		for (i = 0; i < n; i++) {
			int ofs = render->spriteMap[render->spriteMedia[i]].ofs;
			if (ofs != prev) {
				total += le16(buf + ofs + 10);
				prev = ofs;
			}
		}

		if (total <= MAX_SHAPEDATA) {
			int16_t *out = render->shapeData;
			int cursor = 0;      /* write cursor in shapeData */
			int prevSrc = -1;    /* source offset of the last compiled shape */
			int prevOut = 0;     /* its shapeData index */

			for (i = 0; i < n; i++) {
				int frame = render->spriteMedia[i];
				int ofs = render->spriteMap[frame].ofs;
				if (ofs != prevSrc) {
					const uint8_t *sh = buf + ofs + 4;
					int cols = sh[9] - sh[8] + 1;
					int rows = sh[11] - sh[10] + 1;
					int colBytes = (rows & 7) == 0 ? rows / 8 : rows / 8 + 1;
					int16_t texel = 0;
					int run;

					prevSrc = ofs;
					prevOut = cursor;
					out[cursor] = (int16_t)le16(sh);
					out[cursor + 1] = (int16_t)le16(sh + 2);
					out[cursor + 2] = sh[8];
					out[cursor + 3] = sh[9];
					run = cursor + 4 + cols;
					for (int c = 0; c < cols; c++) {
						const uint8_t *mask = buf + ofs + 0xc + colBytes * c + 4;
						int y = 0;
						out[prevOut + 4 + c] = (int16_t)(run - prevOut - 2);
						while (y < rows) {
							int start;
							int16_t runTexel = texel;
							while (y < rows && (mask[y >> 3] & (1 << (y & 7))) == 0)
								y++;
							if (y == rows)
								break;
							start = y;
							while (y < rows && (mask[y >> 3] & (1 << (y & 7))) != 0) {
								texel++;
								y++;
							}
							out[run] = (int16_t)(((y - start) << 8) | ((sh[10] + start) & 0xff));
							out[run + 1] = runTexel;
							run += 2;
						}
						out[run++] = SHAPE_END;
					}
					cursor = run;
				}
				render->spriteMap[frame].ofs = prevOut;
			}
			render->memShapes = rshl(cursor, 1);
			ok = 1;
		} else {
			App_setErrorCode(g_app, ERR_NOMEM);
			App_setErrorStr(render->app, modStr(MOD_STR_MAX_SHAPEDATA), MAX_SHAPEDATA, total / 2);
		}
	}
	InputStream_dtor(&s, 0);
	return ok;
}

/* Report the texel buffer overflowing (shared by walls and sprites). */
static void texelOverflow(Render *render)
{
	App_setErrorCode(g_app, ERR_NOMEM);
	App_setErrorStr(render->app, modStr(MOD_STR_MAX_TEXELS), MAX_TEXELS, render->tileTexelsSize,
	                render->spriteTexelsSize, render->tileTexelsSize + render->spriteTexelsSize);
}

/* Copy `n` bytes of texel data (format byte 4 = two 4-bit texels per byte,
 * low nibble first) to Render.texels at *pos. 0 if it would overflow. */
static int copyTexels(Render *render, int *pos, const uint8_t *src, int n, int format)
{
	int p = *pos;
	if (format == 4) {
		if (n * 2 + p > MAX_TEXELS) {
			texelOverflow(render);
			return 0;
		}
		for (int i = 0; i < n; i++) {
			render->texels[p] = src[i] & 0xf;
			render->texels[p + 1] = src[i] >> 4;
			p += 2;
		}
	} else {
		if (p + n > MAX_TEXELS) {
			texelOverflow(render);
			return 0;
		}
		for (int i = 0; i < n; i++)
			render->texels[p++] = src[i];
	}
	*pos = p;
	return 1;
}

/* 0x0001afa8 Load the texels the map registered: flats (7000+ofs, converted
 * to RGB565 64x64 through their palette), walls (7512+ofs, 117x117 8-bit) and
 * sprites (9000+res, one byte per bitshape texel). Each resource starts with a
 * format byte (4 = 4-bit packed) and must have exactly the expected size.
 * wallMap[].ofs and the shapes' [0],[1] words become texel offsets. */
int Render_loadTexels(Render *render)
{
	InputStream s;
	int32_t *widths;
	int pos, prevSrc, prevOut;
	int i;

	render->memTexels = App_getMemUsed(render->app);
	Hw_clearSources();   /* port: for a mod's GPU textures (oe_hw.h) */
	render->tileTexelsSize = render->numWallMedia * WALL_TEXELS;
	render->spriteTexelsSize = 0;
	widths = malloc((size_t)(render->numSpriteMedia > 0 ? render->numSpriteMedia : 1) * sizeof(int32_t));
	if (widths == NULL) {
		App_setErrorCode(g_app, ERR_NOMEM);
		return 0;
	}
	sortMediaByOfs(render->spriteMedia, render->numSpriteMedia, render->spriteMap);
	sortMediaByOfs(render->wallMedia, render->numWallMedia, render->wallMap);
	for (i = 0; i < render->numSpriteMedia; i++) {
		widths[i] = Render_getShapeWidth(render, render->spriteMedia[i]);
		render->spriteTexelsSize += widths[i];
	}

	/* flats */
	for (i = 0; i < render->numFlats; i++) {
		const TexMap *fm = &render->flatMap[render->flatRemap[render->flatIds[i]]];
		const uint16_t *pal = render->palettes + fm->palette;
		int resId = (int16_t)(fm->ofs + RES_FLAT_BASE);
		const uint8_t *src;
		int format, n;

		InputStream_ctor(&s, render->app, 1);
		if (!Resource_open(&s, resId, 1)) {
			App_setErrorCode(g_app, ERR_NORES);
			free(widths);
			InputStream_dtor(&s, 0);
			return 0;
		}
		src = InputStream_getBuffer(&s);
		format = *src++;
		n = format == 4 ? 0x800 : 0x1000;
		if (InputStream_getSize(&s) != n + 1) {
			App_setErrorCode(g_app, ERR_TRUNCATED);
			App_setErrorStr(render->app, modStr(MOD_STR_FLAT_TRUNCATED), fm->ofs + RES_FLAT_BASE,
			                InputStream_getSize(&s), n + 1);
			free(widths);
			InputStream_dtor(&s, 0);
			return 0;
		}
		Hw_noteSource(OE_HWSRC_FLAT, i, fm->ofs);
		{
			uint16_t *dst = render->flatTexels + i * 0x1000;
			for (int k = 0; k < n; k++) {
				if (format == 4) {
					dst[k * 2] = pal[src[k] & 0xf];
					dst[k * 2 + 1] = pal[src[k] >> 4];
				} else {
					dst[k] = pal[src[k]];
				}
			}
		}
		InputStream_close(&s);
		InputStream_dtor(&s, 0);
	}

	/* walls: textures sharing a source offset share texels */
	prevSrc = -1;
	prevOut = -1;
	pos = 0;
	for (i = 0; i < render->numWallMedia; i++) {
		TexMap *wm = &render->wallMap[render->wallMedia[i]];
		int ofs = wm->ofs;
		const uint8_t *src;
		int format, n;

		if (ofs == prevSrc) {
			wm->ofs = prevOut;
			continue;
		}
		InputStream_ctor(&s, render->app, 1);
		if (!Resource_open(&s, (int16_t)(ofs + RES_WALL_BASE), 1)) {
			App_setErrorCode(g_app, ERR_NORES);
			goto fail;
		}
		src = InputStream_getBuffer(&s);
		format = *src++;
		n = format == 4 ? WALL_TEXELS4 : WALL_TEXELS;
		if (InputStream_getSize(&s) != n + 1) {
			App_setErrorCode(g_app, ERR_TRUNCATED);
			App_setErrorStr(render->app, modStr(MOD_STR_TILE_TRUNCATED), ofs + RES_WALL_BASE,
			                InputStream_getSize(&s), n + 1);
			goto fail;
		}
		wm->ofs = pos;
		Hw_noteSource(OE_HWSRC_WALL, pos, ofs);
		prevSrc = ofs;
		prevOut = pos;
		if (!copyTexels(render, &pos, src, n, format))
			goto fail;
		InputStream_close(&s);
		InputStream_dtor(&s, 0);
	}
	render->memTTexels = pos;
	render->memSTexels = pos;

	/* sprites: one resource per distinct shape */
	prevSrc = -1;
	for (i = 0; i < render->numSpriteMedia; i++) {
		int shape = render->spriteMap[render->spriteMedia[i]].ofs;
		const uint8_t *src;
		int res, format, n;

		if (shape == prevSrc)
			continue; /* (the original copied the shape's words onto themselves) */
		res = (int32_t)((uint16_t)render->shapeData[shape] | ((uint32_t)(uint16_t)render->shapeData[shape + 1] << 16));
		prevSrc = shape;
		InputStream_ctor(&s, render->app, 1);
		if (!Resource_open(&s, (int16_t)(res + RES_SPRITE_BASE), 1)) {
			App_setErrorCode(g_app, ERR_NORES);
			goto fail;
		}
		src = InputStream_getBuffer(&s);
		format = *src++;
		n = format == 4 ? (int)((uint32_t)widths[i] >> 1) : widths[i];
		if (InputStream_getSize(&s) != n + 1) {
			App_setErrorCode(g_app, ERR_TRUNCATED);
			App_setErrorStr(render->app, modStr(MOD_STR_SPRITE_TRUNCATED), res + RES_SPRITE_BASE,
			                InputStream_getSize(&s), n + 1);
			goto fail;
		}
		render->shapeData[shape] = (int16_t)pos;
		render->shapeData[shape + 1] = (int16_t)((uint32_t)pos >> 16);
		Hw_noteSource(OE_HWSRC_SPRITE, pos, res);
		if (!copyTexels(render, &pos, src, n, format))
			goto fail;
		InputStream_dtor(&s, 0);
	}
	free(widths);
	render->memTexels = App_getMemUsed(render->app) - render->memTexels;
	RenderHw_resetTextures(render);   /* port: the GPU's copies are stale */
	render->memSTexels = pos - render->memSTexels;
	return 1;

fail:
	free(widths);
	InputStream_dtor(&s, 0);
	return 0;
}

/* 0x0001bdfc Turn every palette entry and flat texel gray:
 * g = (2R + G + 2B) / 3 on the 5/6/5 channel values, out = 565(g/2, g, g/2). */
void Render_makeGrayscale(Render *render)
{
	int count = render->numPalettes * 256;
	int i;

	for (i = 0; count - i != 0 && i <= count; i++) {
		uint32_t c = render->palettes[i];
		int g = (int)((c >> 11) * 2 + ((c & 0x7ff) >> 5) + (c & 0x1f) * 2) / 3;
		render->palettes[i] = (uint16_t)((g << 5) | ((g >> 1) << 11) | (g >> 1));
	}
	for (i = 0; i < render->numFlats; i++) {
		for (int k = 0; k < 0x1000; k++) {
			uint32_t c = render->flatTexels[i * 0x1000 + k];
			int g = (int)((c >> 11) * 2 + ((c & 0x7ff) >> 5) + (c & 0x1f) * 2) / 3;
			render->flatTexels[i * 0x1000 + k] = (uint16_t)((g << 5) | ((g >> 1) << 11) | (g >> 1));
		}
	}
	RenderHw_resetTextures(render);   /* port: the GPU's copies are stale */
}

/* 0x000059b0 Open map resource `mapId` (whole file kept in Render.mapStream)
 * and parse its header: u8 version (7), int save key, char[16] name, floor and
 * ceiling colours (r, g, b), intro colour (r, g, b), u8, short spawn tile,
 * u8 spawn dir, short menu spawn tile, u8 floor height, u8 ceiling height,
 * u8 map flag bits. Render_beginLoadMapData continues at Render.mapPos. */
int Render_beginLoadMap(Render *render, short mapId)
{
	const uint8_t *buf;
	int err;

	render->memMap = App_getMemUsed(render->app);
	if (!Render_loadMappings(render))
		return 0;
	render->mapId = mapId;
	render->mapPos = 0;
	if (!Resource_open(&render->mapStream, mapId, 1)) {
		err = ERR_NORES;
	} else {
		buf = InputStream_getBuffer(&render->mapStream);
		if (mapByte(render, buf) == BSP_VERSION) {
			int rr, gg, bb;
			size_t len;

			render->mapSaveKey = Resource_shiftInt((void *)buf, &render->mapPos);
			/* strcpy of the 16-byte name field */
			for (len = 0; len < 16 && buf[render->mapPos + len] != 0; len++)
				;
			memcpy(render->mapName, buf + render->mapPos, len);
			render->mapName[len] = '\0';
			render->mapPos += 0x10;
			rr = mapByte(render, buf);
			gg = mapByte(render, buf);
			bb = mapByte(render, buf);
			render->floorColor = (uint16_t)Render_rgbToPixel(render, bb, gg, rr);
			rr = mapByte(render, buf);
			gg = mapByte(render, buf);
			bb = mapByte(render, buf);
			render->ceilColor = (uint16_t)Render_rgbToPixel(render, bb, gg, rr);
			rr = mapByte(render, buf);
			gg = mapByte(render, buf);
			bb = mapByte(render, buf);
			render->introColor = 0xff000000u | ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
			render->f1e024 = mapByte(render, buf);
			render->spawnTile = Resource_shiftShort((void *)buf, &render->mapPos);
			render->spawnDir = mapByte(render, buf);
			render->menuSpawnTile = Resource_shiftShort((void *)buf, &render->mapPos);
			render->floorHeight = (uint8_t)mapByte(render, buf);
			render->ceilHeight = (uint8_t)mapByte(render, buf);
			render->mapFlagBits = (uint8_t)mapByte(render, buf);
			memset(render->mapFlags, 0, sizeof render->mapFlags);
			memset(render->flatLerps, 0, sizeof render->flatLerps);
			render->memMap = App_getMemUsed(render->app) - render->memMap;
			return 1;
		}
		err = ERR_BADMAP;
	}
	App_setErrorCode(g_app, err);
	return 0;
}

/* Reset one of the spare (custom/drop) sprites appended after the map's. */
static void resetSpareSprite(Sprite *sp)
{
	sp->node = NULL;
	sp->info = 0;
	sp->sortNext = NULL;
	sp->nodeNext = NULL;
	sp->ent = NULL;
	sp->renderMode = 0;
	sp->scale = 0x40;
	sp->y = 0;
	sp->x = 0;
	sp->z = 0x20;
}

/* Register the companion tiles a map sprite needs besides its own (death
 * frames, projectiles, effects), then the tile itself. */
static void registerSpriteTiles(Render *render, int tile)
{
	switch (tile) {
	case 0x74: Render_registerTile(render, 0x76); break;
	case 0xb9:
		Render_registerTile(render, 0xb8);
		Render_registerTile(render, 0xbc);
		Render_registerTile(render, 0xbb);
		Render_registerTile(render, 0xba);
		Render_registerTile(render, 0xe8);
		Render_registerTile(render, 0xbd);
		break;
	case 0x91: Render_registerTile(render, 0x92); break;
	case 0xae: Render_registerTile(render, 0xad); break;
	case 0xad: Render_registerTile(render, 0xae); break;
	case 0xcd: Render_registerTile(render, 0xcc); break;
	case 0xcc: Render_registerTile(render, 0xcd); break;
	case 0x3a:
		Render_registerTile(render, 0x7d);
		Render_registerTile(render, 0x79);
		break;
	case 0x38:
		Render_registerTile(render, 0x7e);
		Render_registerTile(render, 0x79);
		break;
	case 0x39:
		Render_registerTile(render, 0xff);
		Render_registerTile(render, 0x7f);
		Render_registerTile(render, 0x79);
		break;
	case 0x23: case 0x24: case 0x25:
		Render_registerTile(render, 0xc6);
		Render_registerTile(render, 0xe9);
		break;
	case 0x32: case 0x33: case 0x34: Render_registerTile(render, 0xf6); break;
	case 0x29: case 0x2a: case 0x2b:
		Render_registerTile(render, 0xdf);
		Render_registerTile(render, 0x69);
		Render_registerTile(render, 0xe0);
		break;
	case 0x1d: case 0x1e: case 0x1f: Render_registerTile(render, 0xc0); break;
	case 0x94: case 0x8e: case 0x93: Render_registerTile(render, 0x8d); break;
	case 0x2c: case 0x2d: case 0x2e:
		Render_registerTile(render, 0x63);
		Render_registerTile(render, 0x62);
		break;
	case 0x17: case 0x18: case 0x19:
		Render_registerTile(render, 0x62);
		Render_registerTile(render, 0x63);
		Render_registerTile(render, 0x72);
		break;
	case 0x14: case 0x15: case 0x16: Render_registerTile(render, 0x72); break;
	case 0x20: case 0x21: case 0x22: Render_registerTile(render, 0xc6); break;
	case 0x3b:
		Render_registerTile(render, 0xfd);
		Render_registerTile(render, 0xe9);
		break;
	case 0x88: Render_registerTile(render, 0xc1); break;
	default: break;
	}
	Render_registerTile(render, tile);
}

/* 0x00011898 Parse the .bsp body (see re/notes/r4_0000f348/analysis.md):
 * nodes, lines, sprites, tile events, commands, map strings, map flags and
 * flats; then compile bitshapes and load texels for the registered media. */
int Render_beginLoadMapData(Render *render)
{
	int memStart = App_getMemUsed(render->app);
	const uint8_t *buf;
	int *pos = &render->mapPos;
	int err = ERR_NOMEM;
	int i, n, numA, numB;

	render->wallMedia = malloc(0x400);
	if (render->wallMedia == NULL)
		goto fail;
	render->spriteMedia = malloc(0x1000);
	if (render->spriteMedia == NULL)
		goto fail;

	/* ---- nodes: 4 bbox bytes, split bytes a/b, short c, short d ---- */
	render->memNodes = App_getMemUsed(render->app);
	buf = InputStream_getBuffer(&render->mapStream);
	render->numNodes = Resource_shiftShort((void *)buf, pos);
	if (render->numNodes > MAX_NODES)
		goto fail;
	memset(render->nodes, 0, sizeof(Node) * (size_t)(render->numNodes > 0 ? render->numNodes : 0));
	for (i = 0; i < render->numNodes; i++) {
		Node *nd = &render->nodes[i];
		int a, b;
		uint32_t c, d;
		nd->bbox[0] = (uint16_t)(mapByte(render, buf) << 3);
		nd->bbox[1] = (uint16_t)(mapByte(render, buf) << 3);
		nd->bbox[2] = (uint16_t)(mapByte(render, buf) << 3);
		nd->bbox[3] = (uint16_t)(mapByte(render, buf) << 3);
		a = mapByte(render, buf);
		b = mapByte(render, buf);
		nd->args1 = ((uint32_t)b << 3) | ((uint32_t)a << 16) | 0xff000000u; /* no moving lines yet */
		c = (uint32_t)Resource_shiftShort((void *)buf, pos);
		d = (uint32_t)Resource_shiftShort((void *)buf, pos);
		nd->args2 = (d << 16) | c;
		nd->sprites = NULL;
	}
	render->memNodes = App_getMemUsed(render->app) - render->memNodes;

	/* ---- lines: 4 coord bytes, u8 texture, u32 flags ---- */
	render->memLines = App_getMemUsed(render->app);
	render->numLines = Resource_shiftShort((void *)buf, pos);
	if (render->numLines > MAX_LINES)
		goto fail;
	memset(render->lines, 0, sizeof(Line) * (size_t)(render->numLines > 0 ? render->numLines : 0));
	render->numWallMedia = 0;
	render->numMovingLines = 0;
	for (i = 0; i < render->numLines; i++) {
		Line *ln = &render->lines[i];
		int adx, ady, len;
		uint32_t flags;

		ln->x1 = mapByte(render, buf) << 3;
		ln->y1 = mapByte(render, buf) << 3;
		ln->x2 = mapByte(render, buf) << 3;
		ln->y2 = mapByte(render, buf) << 3;
		ln->texture = (int16_t)mapByte(render, buf);
		ln->flags = (uint32_t)Resource_shiftInt((void *)buf, pos);

		/* texture u runs 0..len along the longer axis, reversed with 0x8000 */
		adx = ln->x2 - ln->x1;
		if (adx < 1)
			adx = -adx;
		ady = ln->y2 - ln->y1;
		if (ady < 1)
			ady = -ady;
		len = ady < adx ? adx : ady;
		if (ln->flags & 0x8000) {
			ln->z1 = len;
			ln->z2 = 0;
		} else {
			ln->z1 = 0;
			ln->z2 = len;
		}

		/* doors sit 3 units off their tile edge */
		flags = ln->flags;
		if (flags & 0x200) {
			if (flags & 0x8) {
				ln->x1 += 3;
				ln->x2 += 3;
			} else if (flags & 0x10) {
				ln->x1 -= 3;
				ln->x2 -= 3;
			}
		} else if (flags & 0x100) {
			if (flags & 0x8) {
				ln->y1 += 3;
				ln->y2 += 3;
			} else if (flags & 0x10) {
				ln->y1 -= 3;
				ln->y2 -= 3;
			}
		}

		if (ln->texture == 0x18) {
			int mx = (ln->x2 - ln->x1) / 2 + ln->x1;
			int my = (ln->y2 - ln->y1) / 2 + ln->y1;
			render->mapFlags[(my >> 6) * 32 + (mx >> 6)] |= 4;
		}

		if (ln->flags & LINE_MOVING) {
			int idx = movingCount(render);
			int mx, my;
			if (idx > MAX_MOVING - 1) {
				App_setErrorStr(render->app, modStr(MOD_STR_MAX_MOVING_LINES));
				err = ERR_FATAL;
				goto fail;
			}
			render->movingLines[idx].line = ln;
			render->movingLines[idx].next = 0xff;
			render->movingLines[idx].prev = 0xff;
			/* link it at the tile it faces */
			mx = (ln->x2 + ln->x1) >> 1;
			my = (ln->y2 + ln->y1) >> 1;
			flags = ln->flags;
			if (flags & 0x800)
				my -= 0x20;
			else if (flags & 0x1000)
				my += 0x20;
			else if (flags & 0x2000)
				mx += 0x20;
			else if (flags & 0x4000)
				mx -= 0x20;
			Render_linkMovable(render, idx, mx, my);
			render->numMovingLines = (int8_t)(idx + 1);
		}

		Render_registerTex4(render, ln->texture);
		if (ln->texture == 0x13)
			Render_registerTex4(render, 0x12);
		else if (ln->texture == 0x12)
			Render_registerTex4(render, 0x13);
	}
	Render_markDynamicLineTiles(render);
	render->memLines = App_getMemUsed(render->app) - render->memLines;

	/* ---- sprites: numA {x, y, tile, short hi}, numB {x, y, z, tile, frame, short hi} ---- */
	render->numSpriteMedia = 0;
	numA = Resource_shiftShort((void *)buf, pos);
	numB = Resource_shiftShort((void *)buf, pos);
	render->numMapSprites = numA + numB;
	render->numSprites = numA + numB + NUM_CUSTOM_SPRITES + NUM_DROP_SPRITES;
	if (render->mapId == MAP_HUB) {
		Render_registerTile(render, 0x14);
		render->numSprites++;
	}
	render->memSprites = App_getMemUsed(render->app);
	if (render->numSprites > MAX_SPRITES)
		goto fail;
	memset(render->sprites, 0, sizeof(Sprite) * (size_t)(render->numSprites > 0 ? render->numSprites : 0));
	for (i = 0; i < render->numMapSprites; i++) {
		Sprite *sp = &render->sprites[i];
		uint32_t info;
		int tile;

		sp->node = NULL;
		sp->sortNext = NULL;
		sp->nodeNext = NULL;
		sp->ent = NULL;
		sp->renderMode = 0;
		sp->scale = 0x40;
		if (i < numA) {
			sp->x = mapByte(render, buf) << 3;
			sp->y = mapByte(render, buf) << 3;
			sp->z = 0x20;
			info = (uint32_t)mapByte(render, buf);
		} else {
			sp->x = mapByte(render, buf) << 3;
			sp->y = mapByte(render, buf) << 3;
			sp->z = mapByte(render, buf);
			info = (uint32_t)mapByte(render, buf);
			info |= (uint32_t)mapByte(render, buf) << 8;
		}
		sp->info = info;
		sp->info = ((uint32_t)Resource_shiftShort((void *)buf, pos) << 16) | sp->info;

		/* nudge off the wall it hangs on */
		info = sp->info;
		if (info & 0x800000) {
			if (info & 0x100000)
				sp->y += 1;
			else if (info & 0x80000)
				sp->y -= 1;
			else if (info & 0x200000)
				sp->x += 1;
			else if (info & 0x400000)
				sp->x -= 1;
		}
		Render_relinkSpriteAt(render, sp, -1, -1);

		tile = (int)(sp->info & 0xff);
		if (tile == 0xb0 || tile == 0xa5 || tile == 0x89 || tile == 0x82 || tile == 0xce)
			sp->renderMode = 7;
		else if (tile == 0xa1)
			sp->renderMode = 0xf;
		else
			sp->renderMode = 0;

		if (sp->info & 0x40000)
			Render_registerTex4(render, tile); /* wall-texture sprite */
		else
			registerSpriteTiles(render, tile);
	}
	/* media every map needs (player effects, items, HUD sprites) */
	Render_registerTile(render, 0x17);
	Render_registerTile(render, 0xff);
	Render_registerTile(render, 0xe0);
	Render_registerTile(render, 0xea);
	Render_registerTile(render, 0xfe);
	Render_registerTile(render, 0xeb);
	Render_registerTile(render, 0xec);

	n = render->numMapSprites;
	for (i = 0; i < NUM_CUSTOM_SPRITES; i++, n++) {
		render->customSprites[i] = &render->sprites[n];
		resetSpareSprite(&render->sprites[n]);
	}
	render->firstDropSprite = n;
	for (i = 0; i < NUM_DROP_SPRITES; i++, n++) {
		render->dropSprites[i] = &render->sprites[n];
		resetSpareSprite(&render->sprites[n]);
	}
	if (render->mapId == MAP_HUB) {
		/* only position/info are set; the rest stays as cleared above */
		Sprite *hub = &render->sprites[n];
		render->hubSprite = hub;
		hub->info = 0x10014;
		hub->y = 0;
		hub->x = 0;
		hub->z = 0x20;
		Render_relinkSpriteAt(render, hub, -1, -1);
	} else {
		render->hubSprite = NULL;
	}
	render->memSprites = App_getMemUsed(render->app) - render->memSprites;
	for (i = 0xf2; i < 0xfc; i++)
		Render_registerTile(render, i);
	for (i = 0xe1; i < 0xe7; i++)
		Render_registerTile(render, i);
	for (i = 0xd1; i < 0xdc; i++)
		Render_registerTile(render, i);

	/* ---- tile events ---- */
	render->numTileEvents = Resource_shiftShort((void *)buf, pos);
	if (render->numTileEvents > 0) {
		if (render->numTileEvents > MAX_EVENTS)
			goto fail;
		for (i = 0; i < render->numTileEvents; i++) {
			uint32_t ev = (uint32_t)Resource_shiftInt((void *)buf, pos);
			if ((ev & 0x7ffffff) >> 20 != 0) /* has commands */
				render->mapFlags[ev & 0x3ff] |= 8;
			render->tileEvents[i] = ev;
		}
	}

	/* ---- commands: u8 op, int arg1, int arg2 ---- */
	render->numCommands = Resource_shiftShort((void *)buf, pos);
	if (render->numCommands > 0) {
		if (render->numCommands > MAX_COMMANDS)
			goto fail;
		for (i = 0; i < render->numCommands; i++) {
			Command *cmd = &render->commands[i];
			cmd->op = mapByte(render, buf);
			cmd->arg1 = Resource_shiftInt((void *)buf, pos);
			cmd->arg2 = Resource_shiftInt((void *)buf, pos);
			if (cmd->op == 0x22) /* change tile sprite */
				Render_registerTile(render, cmd->arg1 >> 13);
		}
	}
	render->memBytecode = render->numCommands * 0xc;

	/* ---- map strings: short n, n x {short len, chars} ---- */
	n = Resource_shiftShort((void *)buf, pos);
	render->memStrings = App_getMemUsed(render->app);
	free(render->mapStrings);
	render->mapStrings = NULL;
	if (n > 0) {
		render->mapStrings = malloc((size_t)n * sizeof(char *));
		if (render->mapStrings == NULL)
			goto fail;
		render->numMapStrings = 0;
		while (render->numMapStrings < n) {
			int len = Resource_shiftShort((void *)buf, pos);
			char *str = malloc((size_t)(len > 0 ? len : 0) + 1);
			render->mapStrings[render->numMapStrings] = str;
			if (str == NULL)
				goto fail;
			if (len > 0)
				strncpy(str, (const char *)buf + *pos, (size_t)len);
			str[len > 0 ? len : 0] = '\0';
			*pos += len;
			render->numMapStrings++;
		}
	}
	render->memStrings = App_getMemUsed(render->app) - render->memStrings;

	/* ---- map flags: 2 bits per tile, 4 tiles per byte, LSB first ---- */
	for (i = 0; i < 0x100; i++) {
		int b = buf[*pos + i];
		render->mapFlags[i * 4] |= (uint8_t)(b & 3);
		render->mapFlags[i * 4 + 1] |= (uint8_t)((b & 0xf) >> 2);
		render->mapFlags[i * 4 + 2] |= (uint8_t)((b & 0x3f) >> 4);
		render->mapFlags[i * 4 + 3] |= (uint8_t)(b >> 6);
	}
	*pos += 0x100;

	/* ---- flats: floor then ceiling, one flat id per tile, <= 8 distinct ---- */
	render->numFlats = 0;
	for (int layer = 0; layer < 2; layer++) {
		for (int t = 0; t < 0x400; t++) {
			int id = mapByte(render, buf);
			int k;
			for (k = 0; k < render->numFlats && render->flatIds[k] != id; k++)
				;
			if (k == render->numFlats) {
				if (render->numFlats == MAX_FLATS)
					goto fail;
				render->flatIds[render->numFlats++] = id;
			}
			render->flatTile[layer * 0x400 + t] = render->flatTexels + k * 0x1000;
		}
	}
	InputStream_close(&render->mapStream);
	render->memMap = App_getMemUsed(render->app) + render->memMap - memStart;

	if (!Render_loadBitShapes(render))
		return 0;
	if (!Render_loadTexels(render))
		return 0;
	free(render->wallMedia);
	render->wallMedia = NULL;
	free(render->spriteMedia);
	render->spriteMedia = NULL;
	return 1;

fail:
	App_setErrorCode(g_app, err);
	return 0;
}

/* 0x0002b0a4 */
void Render_unloadMap(Render *r)
{
	r->numMapSprites = 0;
	r->numNodes = 0;
	r->numLines = 0;
	r->numTileEvents = 0;
	r->numCommands = 0;
	for (int i = 0; i < r->numMapStrings; i++)
		free(r->mapStrings[i]);
	free(r->mapStrings);
	r->numMapStrings = 0;
	r->mapStrings = NULL;
	free(r->wallMedia);
	r->wallMedia = NULL;
	free(r->spriteMedia);
	r->spriteMedia = NULL;
	r->floorFlat = 0;
	r->ceilFlat = 0;
	r->wobble = 0;
}

/* 0x00002f70 Add sprite media spriteBase[tile] + frame to the map's list. */
void Render_registerSpriteFrame(Render *r, int tile, int frame)
{
	int media;

	if (r->spriteBase[tile] == MEDIA_NONE) {
		App_setErrorStr(r->app, modStr(MOD_STR_NO_SUCH_SPRITE), tile);
		App_setErrorCode(g_app, ERR_GENERIC);
		return;
	}
	media = r->spriteBase[tile] + frame;
	for (int i = 0; i < r->numSpriteMedia; i++)
		if (r->spriteMedia[i] == media)
			return;
	r->spriteMedia[r->numSpriteMedia++] = media;
}

/* 0x00002ff0 Add wall texture wallBase[tile] + frame to the map's list. */
void Render_registerTexFrame(Render *r, int tile, int frame)
{
	int media;

	if (r->wallBase[tile] == MEDIA_NONE) {
		App_setErrorStr(r->app, modStr(MOD_STR_NO_SUCH_TILE), tile);
		App_setErrorCode(g_app, ERR_GENERIC);
		return;
	}
	media = r->wallBase[tile] + frame;
	for (int i = 0; i < r->numWallMedia; i++)
		if (r->wallMedia[i] == media)
			return;
	r->wallMedia[r->numWallMedia++] = media;
}

static void registerFrames(Render *r, int tile, int first, int end)
{
	for (int f = first; f < end; f++)
		Render_registerSpriteFrame(r, tile, f);
}

/* 0x000036d4 Register every frame a sprite tile can show. Monsters
 * (0x11..0x40) have 9 frames; 0x26..0x28 also frame 9 and tile 0x95, 0x39
 * frames 9..15, 0x3b frames 9..14. */
void Render_registerTile(Render *r, int tile)
{
	for (;;) {
		if ((unsigned)(tile - 0x11) > 0x2f) {
			if (tile == 0xd3 || tile == 0xd4)
				registerFrames(r, tile, 0, 7);
			else if (tile == 0xd8)
				registerFrames(r, 0xd8, 0, 7);
			else if (tile == 0xd1)
				registerFrames(r, 0xd1, 0, 5);
			else if (tile == 0xcb)
				registerFrames(r, 0xcb, 0, 16);
			else if ((unsigned)(tile - 0x7d) < 3)
				registerFrames(r, tile, 0, 5);
			else
				registerFrames(r, tile, 0, 4);
			return;
		}
		registerFrames(r, tile, 0, 9);
		if ((unsigned)(tile - 0x26) > 2)
			break;
		Render_registerSpriteFrame(r, tile, 9);
		tile = 0x95;
	}
	if (tile == 0x39)
		registerFrames(r, 0x39, 9, 16);
	else if (tile == 0x3b)
		registerFrames(r, 0x3b, 9, 15);
}

/* 0x00003894 Wall textures are animated in 4 frames. */
void Render_registerTex4(Render *r, int tile)
{
	Render_registerTexFrame(r, tile, 0);
	Render_registerTexFrame(r, tile, 1);
	Render_registerTexFrame(r, tile, 2);
	Render_registerTexFrame(r, tile, 3);
}

/* 0x000115a4 Binary search of the (sorted) tile events by tile index
 * (ev & 0x3ff). Returns the event index or -1. Like the original it probes
 * entry 0 even when there are no events. */
int Render_findTileEvent(Render *render, int tileIndex)
{
	int lo = 0, hi = render->numTileEvents;

	for (;;) {
		int mid = ((hi - lo) >> 1) + lo;
		int t = (int)(render->tileEvents[mid] & 0x3ff);
		int nextLo = mid;
		if (t == tileIndex)
			return mid;
		if (tileIndex < t) {
			nextLo = lo;
			hi = mid;
		}
		if (mid == 0)
			return -1;
		lo = nextLo;
		if (hi - 1 == mid)
			return -1;
	}
}

/* 0x00012b7c BSP leaf containing (x, y). */
void *Render_findLeaf(Render *render, int x, int y)
{
	Node *n = &render->nodes[0];
	while (!isLeaf(n))
		n = &render->nodes[bspChild(n, x, y, 0)];
	return n;
}

/* 0x00014b44 */
int Render_inSameLeaf(Render *render, int x1, int y1, int x2, int y2)
{
	return Render_findLeaf(render, x1, y1) == Render_findLeaf(render, x2, y2);
}

/* 0x000187cc Push moving line `idx` on the dynamic-line list of the leaf
 * containing (x, y): the head is args1 bits 24..31 (0xff = empty), links
 * are bytes in Render.movingLines. Points on a split go to the lower side. */
void Render_linkMovable(Render *render, int idx, int x, int y)
{
	Node *n = &render->nodes[0];
	int head;

	while (!isLeaf(n))
		n = &render->nodes[bspChild(n, x, y, 1)];
	head = (int)(n->args1 >> 24) & 0xff;
	render->movingLines[idx].prev = 0xff;
	render->movingLines[idx].next = (uint8_t)head;
	n->args1 = (n->args1 & 0xffffff) | ((uint32_t)idx << 24);
	if (head != 0xff)
		render->movingLines[head].prev = (uint8_t)idx;
}

/* 0x000200d8 Move a moving line to the leaf containing (x, y). */
void Render_relinkLine(Render *render, int lineIdx, int x, int y)
{
	Render_unlinkDynamicLine(render, lineIdx);
	Render_linkMovable(render, lineIdx, x, y);
}

/* 0x000200f8 Move a sprite to the sprite list of the leaf containing (x, y)
 * (x == -1: the sprite's own position). */
void Render_relinkSpriteAt(Render *render, void *spritePtr, int x, int y)
{
	Sprite *sprite = spritePtr;
	Node *n;

	if (x == -1) {
		x = sprite->x;
		y = sprite->y;
	}
	if (sprite->node != NULL) {
		Node *old = sprite->node;
		if (old->sprites == sprite) {
			old->sprites = sprite->nodeNext;
		} else {
			Sprite *p = old->sprites;
			while (p != NULL && p->nodeNext != sprite)
				p = p->nodeNext;
			if (p != NULL)
				p->nodeNext = sprite->nodeNext;
		}
	}
	n = &render->nodes[0];
	while (!isLeaf(n))
		n = &render->nodes[bspChild(n, x, y, 0)];
	sprite->node = n;
	sprite->nodeNext = n->sprites;
	n->sprites = sprite;
}

/* 0x0002aed8 Remove moving line `idx` from its leaf list; when it was the
 * head, every node whose head is idx gets the next entry. */
void Render_unlinkDynamicLine(Render *r, int idx)
{
	int next = r->movingLines[idx].next;
	int prev;

	if (next != 0xff)
		r->movingLines[next].prev = r->movingLines[idx].prev;
	prev = r->movingLines[idx].prev;
	if (prev == 0xff) {
		for (int i = 0; i < r->numNodes; i++) {
			uint32_t a = r->nodes[i].args1;
			if ((a & 0xff000000u) == ((uint32_t)idx << 24))
				r->nodes[i].args1 = (a & 0xffffff) | ((uint32_t)r->movingLines[idx].next << 24);
		}
	} else {
		r->movingLines[prev].next = r->movingLines[idx].next;
	}
}

/* 0x0002b454 Mark the tile each moving line faces (0x800 north/-y, 0x1000
 * south/+y, 0x4000 west/-x, 0x2000 east/+x) as blocked (map flag bit 0). */
void Render_markDynamicLineTiles(Render *r)
{
	for (int i = 0; i < movingCount(r); i++) {
		const Line *ln = r->movingLines[i].line;
		uint32_t flags = ln->flags;
		int sx = ln->x1 + ln->x2;
		int mx = sx >> 1;
		int my = (ln->y1 + ln->y2) >> 1;
		int tx = sx >> 7;

		if (flags & 0x800) {
			my -= 0x20;
		} else if (flags & 0x1000) {
			my += 0x20;
		} else if (flags & 0x4000) {
			tx = (mx - 0x20) >> 6;
		} else if (flags & 0x2000) {
			tx = (mx + 0x20) >> 6;
		} else {
			continue;
		}
		r->mapFlags[(my >> 6) * 32 + tx] |= 1;
	}
}

/* 0x0002604c Set the 3D view rectangle {x, y, w, h} (h forced even) and
 * reallocate the per-column depth buffer. The rectangle is in game pixels;
 * the renderer keeps it in device pixels (times Brew_screenScale(), the HD
 * mode), so everything it draws comes out at the device resolution. */
void Render_setScreenRect(Render *render, short *rect)
{
	int s = Brew_screenScale();
	int w = rect[2] * s;
	int h = rect[3] * s;

	render->viewW = w;
	render->viewH = h;
	render->viewportX = rect[0] * s;
	render->viewportY = rect[1] * s;
	if (h & 1)
		render->viewH = h - 1;
	render->halfH = render->viewH / 2;
	render->halfW = w / 2;
	render->centerX16 = rshl(w / 2, 16) - 0x8000;
	render->width16 = rshl(w, 16);
	render->maxX16 = rshl(w, 16) - 0x8000;
	if (render->columnZ != NULL) {
		free(render->columnZ);
		render->columnZ = NULL;
	}
	render->columnZ = malloc((size_t)(render->viewW > 0 ? render->viewW : 0) * sizeof(int32_t));
	if (render->columnZ == NULL)
		App_setErrorCode(g_app, ERR_NOMEM);
}
