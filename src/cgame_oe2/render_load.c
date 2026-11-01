/*
 * Render: startup/shutdown and everything that loads renderer data from the
 * .bar: sine table, palettes (with the "green blood" recolouring), texture
 * mappings, bitshapes (sprite masks compiled to run lists), texels, and the
 * .bsp map itself (nodes, lines, sprites, tile events, bytecode, map flags,
 * floor/ceiling flats). Also the media "registration" that decides which
 * textures/sprite frames a map needs, and a few small lookups.
 */

#include "render_local.h"
#include "oe_hw.h"

#define RES_SINTABLE  1001
#define RES_PALETTES  1003
#define RES_MAPPINGS  1004
#define RES_BITSHAPES 1005
#define RES_STEXELS   1006
#define RES_WTEXELS   1007
#define RES_FTEXELS   1008
#define RES_MAP_BASE  1999  /* map n (1-based) is resource 1999 + n */

#define CHUNK         0x4000 /* size of the shared read buffer (Applet.dataBuffer) */
#define MAX_PALETTE_ENTRIES 0x22600
#define WALL_TEXELS   0x3579 /* bytes of one wall texture (8bpp) */
#define WALL_TEXELS4  0x1abd /* bytes of one 4bpp wall texture */

/* Render.memStats holds 13 int64 counters as lo/hi int pairs. */
enum {
	MEM_MAP = 0, MEM_BYTECODE = 2, MEM_BITSHAPES = 4, MEM_LINES = 5, MEM_SPRITES = 6,
	MEM_NODES = 7, MEM_TEXELS = 8, MEM_PALETTES = 9, MEM_MAPPINGS = 10, MEM_TEXELBYTES = 11,
	MEM_WALLTEXELS = 12
};

static int64_t memStat(const Render *r, int i)
{
	return (int64_t)(((uint64_t)(uint32_t)r->memStats[i * 2 + 1] << 32) | (uint32_t)r->memStats[i * 2]);
}

static void setMemStat(Render *r, int i, int64_t v)
{
	r->memStats[i * 2] = (int32_t)(uint32_t)v;
	r->memStats[i * 2 + 1] = (int32_t)(uint32_t)((uint64_t)v >> 32);
}

/* Read the next piece (at most CHUNK bytes) of a `total`-byte block into the
 * shared buffer; `done` bytes were consumed already. Returns the piece size. */
static int readChunk(InputStream *s, DataBuffer *buf, int done, int total)
{
	int n = (done + CHUNK > total) ? total - done : CHUNK;
	Stream_read(s, buf->data, 0, n);
	return n;
}

/* The 13 span-drawer slots Render+0x04..+0x34. */
static void **spanSlot(Render *r, int i)
{
	void **slots[13] = {
		&r->spanNormal, &r->spanBlend75, &r->spanBlend50, &r->spanBlend25, &r->spanAdditive,
		&r->span18, &r->span1c, &r->span20, &r->spanGray, &r->span28, &r->span2c,
		&r->spanMultiply, &r->spanNone
	};
	return slots[i];
}

static void *newSpanDrawer(SpanDrawFn fn)
{
	SpanDrawer *d = calloc(1, sizeof *d);
	if (d)
		d->draw = fn;
	return d;
}

/* 0x0003fdc8 */
int Render_startup(Render *r)
{
	Applet *app = g_app;
	InputStream s;
	int ok = 0;

	InputStream_ctor(&s);
	if (Resource_open(&s, RES_SINTABLE, 1)) {
		/* 1024 x int32 sine, 16.16 */
		Stream_read(&s, app->dataBuffer->data, 0, 0x1000);
		for (int ofs = 0, i = 0; ofs < 0x1000; ofs += 4, i++)
			r->sinTable[i] = Bytes_readIntAt8(app->dataBuffer, ofs);
		InputStream_close(&s);
		if (s.f00 == 0) {
			/* The original QueryInterface'd the device bitmap for its IDIB. */
			r->fbDib = NULL;
			Render_syncScreenDib(r);
			if (r->fbDib != NULL && (r->fbDib->nColorScheme == 15 || r->fbDib->nColorScheme == 16)
			    && r->fbDib->nDepth == 16) {
				r->spanNormal = newSpanDrawer(Span_drawNormal);
				r->spanNone = newSpanDrawer(Span_drawNone);
				r->spanBlend75 = newSpanDrawer(Span_drawBlend75);
				r->spanBlend50 = newSpanDrawer(Span_drawBlend50);
				r->spanBlend25 = newSpanDrawer(Span_drawBlend25);
				r->spanAdditive = newSpanDrawer(Span_drawAdditive);
				r->spanMultiply = newSpanDrawer(Span_drawMultiply);
				r->spanGray = newSpanDrawer(Span_drawGray);
				for (int i = 0; i < 13; i++) {
					SpanDrawer *d = *spanSlot(r, i);
					if (d)
						d->render = r;
				}
				r->dbgForceDrawer = 0;
				r->dbg18820 = 0;
				r->dbg18824 = 0;
				r->dbgSkipSprites = 0;
				r->dbgSkipWalls = 0;
				r->dbgSkipFlats = 0;
				r->dbgSkipSprites2 = 0;
				r->f19f6c = 0;
				r->numLines = 0;
				r->numNodes = 0;
				r->numMapSprites = 0;
				r->spawnTile = 0;
				r->floorColor = 0;
				r->ceilColor = 0;
				r->columnDepth = NULL;
				r->effCeil = 0x40;
				r->effFloor = 0;
				r->frameTime = 0;
				r->floorUniform = 0;
				r->ceilUniform = 0;
				r->bobEnable = 0;
				r->palettesLoaded = 0;
				ok = 1;
			} else {
				App_setErrorCode(g_app, 1);
			}
		}
	}
	InputStream_dtor(&s, 0);
	return ok;
}

/* 0x0003c5e4 */
void Render_shutdown(Render *r)
{
	Render_unloadMap(r);
	free(r->columnDepth);
	r->columnDepth = NULL;
	for (int i = 0; i < 13; i++) {
		void **slot = spanSlot(r, i);
		free(*slot);
		*slot = NULL;
	}
	if (r->fbDib != NULL) {
		free(r->fbDib); /* original: IDIB_Release */
		r->fbDib = NULL;
	}
	String_release(&r->mapName);
}

/* 0x00026e0c  mappings.bin: six u32 counts, then wall {texelOfs, palette},
 * flat {ofs, palette}, sprite {ofs, palette} records and three s16 remap tables. */
int Render_loadMappings(Render *r)
{
	Applet *app = g_app;
	DataBuffer *buf = app->dataBuffer;
	InputStream s;
	int ok;

	InputStream_ctor(&s);
	if (!Resource_open(&s, RES_MAPPINGS, 1)) {
		ok = 0;
	} else {
		ByteBuf_fill(buf, &s, 0x18);
		int numWalls = Resource_shiftInt(buf);
		int numFlats = Resource_shiftInt(buf);
		int numSprites = Resource_shiftInt(buf);
		int numWallRemap = Resource_shiftInt(buf);
		int numFlatRemap = Resource_shiftInt(buf);
		int numSpriteRemap = Resource_shiftInt(buf);
		int total, k;

		setMemStat(r, MEM_MAPPINGS, 0);

		total = rmul(numWalls, 8);
		k = 0;
		for (int done = 0; done < total; done += CHUNK) {
			int n = readChunk(&s, buf, done, total);
			for (int ofs = 0; ofs < n; ofs += 8, k++) {
				r->wallMap[k].texelOfs = Bytes_readIntAt8(buf, ofs);
				r->wallMap[k].page = 0;
				r->wallMap[k].palette = rshl(Bytes_readIntAt8(buf, ofs + 4), 8);
			}
		}
		setMemStat(r, MEM_MAPPINGS, memStat(r, MEM_MAPPINGS) + total);

		total = rmul(numFlats, 8);
		k = 0;
		for (int done = 0; done < total; done += CHUNK) {
			int n = readChunk(&s, buf, done, total);
			for (int ofs = 0; ofs < n; ofs += 8, k++) {
				r->flatMap[k].ofs = Bytes_readIntAt8(buf, ofs);
				r->flatMap[k].palette = rshl(Bytes_readIntAt8(buf, ofs + 4), 8);
			}
		}
		setMemStat(r, MEM_MAPPINGS, memStat(r, MEM_MAPPINGS) + total);

		total = rmul(numSprites, 8);
		k = 0;
		for (int done = 0; done < total; done += CHUNK) {
			int n = readChunk(&s, buf, done, total);
			for (int ofs = 0; ofs < n; ofs += 8, k++) {
				r->spriteMap[k].ofs = Bytes_readIntAt8(buf, ofs);
				r->spriteMap[k].palette = rshl(Bytes_readIntAt8(buf, ofs + 4), 8);
			}
		}
		setMemStat(r, MEM_MAPPINGS, memStat(r, MEM_MAPPINGS) + total);

		total = rmul(numWallRemap, 2);
		k = 0;
		for (int done = 0; done < total; done += CHUNK) {
			int n = readChunk(&s, buf, done, total);
			for (int ofs = 0; ofs < n; ofs += 2)
				r->wallRemap[k++] = (int16_t)Resource_shortAt(buf, ofs);
		}
		setMemStat(r, MEM_MAPPINGS, memStat(r, MEM_MAPPINGS) + total);

		total = rmul(numFlatRemap, 2);
		k = 0;
		for (int done = 0; done < total; done += CHUNK) {
			int n = readChunk(&s, buf, done, total);
			for (int ofs = 0; ofs < n; ofs += 2)
				r->flatRemap[k++] = (int16_t)Resource_shortAt(buf, ofs);
		}
		setMemStat(r, MEM_MAPPINGS, memStat(r, MEM_MAPPINGS) + total);

		total = rmul(numSpriteRemap, 2);
		k = 0;
		for (int done = 0; done < total; done += CHUNK) {
			int n = readChunk(&s, buf, done, total);
			for (int ofs = 0; ofs < n; ofs += 2)
				r->spriteRemap[k++] = (int16_t)Resource_shortAt(buf, ofs);
		}
		setMemStat(r, MEM_MAPPINGS, memStat(r, MEM_MAPPINGS) + total);
		ok = 1;
	}
	InputStream_dtor(&s, 0);
	return ok;
}

/* ---- green blood ----------------------------------------------------------
 * With "Green-Blood: true" in the config every palette entry that the artists
 * used for blood is turned green by swapping its red and green channels.
 * Which entries count as blood is decided by hand-written rules per palette
 * and per colour index (exact colours, index ranges, or "clearly red"). The
 * channels are compared in 8-bit scale (R5<<3, G6<<2, B5<<3). Note the
 * original writes blue back as B5>>3, which the port keeps. */

/* Index/colour pairs tested first (before the per-palette rules). */
static int bloodColourA(int i, int r, int g, int b)
{
	switch (i) {
	case 0xc8: return r == 0x70 && g == 0 && b == 0;
	case 0xda: return r == 0xc8 && g == 0x68 && b == 0x68;
	case 0xc9: return r == 0x50 && g == 0 && b == 0;
	case 0xc2: return r == 0x28 && g == 0 && b == 0;
	case 0xc6: return r == 0x80 && g == 0 && b == 0;
	case 0xd4: return r == 0xb0 && g == 0x30 && b == 0x30;
	case 0xd6: return r == 0xc8 && g == 0x44 && b == 0x40;
	case 0xd8: return r == 0xb0 && g == 0x48 && b == 0x48;
	case 0xd9: return r == 0x98 && g == 0x40 && b == 0x40;
	case 0xdf: return r == 0xe8 && g == 0xac && b == 0xa8;
	case 0xde: return r == 0xe8 && g == 0x90 && b == 0x90;
	default: return 0;
	}
}

/* Index/colour pairs tested after the 0x1f1..0x1f4 palette rules. */
static int bloodColourB(int i, int r, int g, int b)
{
	switch (i) {
	case 0xfb: return r == 0x38 && g == 0x18 && b == 0x18;
	case 0xfc: return r == 0x48 ? (g == 0x20 && b == 0x20) : (r == 0x68 && g == 0 && b == 0);
	case 0xd9: return r == 0x28 && g == 0x0c && b == 0;
	case 0xfd: return r == 0xc8 && g == 4 && b == 0;
	case 0xfe: return r == 0xa0 && g == 4 && b == 0;
	case 0xff: return r == 0xc8 && g == 0x30 && b == 0x30;
	case 2: return r == 0xd8 && g == 0x64 && b == 0x68;
	case 3: return r == 0xf8 && g == 0x58 && b == 0x58;
	default: return 0;
	}
}

/* 0x00027c98.. per-palette rules; 1 = recolour this entry. */
static int bloodRule(int pal, int i, int r, int g, int b)
{
	unsigned ui = (unsigned)i;
	int redness = r - g;

	if (pal == 0x1f1) {
		if ((i == 1 || i == 4 || ui - 8 < 0x6e || ui - 0x87 < 6) && r != 0 && g == 0 && b == 0)
			return 1;
	} else if (pal == 0x1f2) {
		if (ui - 5 < 0x71 && r != 0 && g == 0 && b == 0)
			return 1;
	} else if (pal == 0x1f3) {
		if ((i == 1 || ui - 4 < 0x61) && r != 0 && g < 9 && b < 9)
			return 1;
	} else if (pal == 0x1f4) {
		if (i == 3 || i == 4 || ui - 4 < 0xf1) {
			if (r > 0x28 && g < 0x15 && b < 9)
				return 1;
			if (r != 0 && g < 5 && b < 9)
				return 1;
		}
	}
	if (bloodColourB(i, r, g, b))
		return 1;

	if (pal == 400 && ui - 2 < 0xd)
		return 1;
	if ((unsigned)(pal - 0xc6) < 0x15 && redness > 0x28)
		return 1;
	if (pal == 0x182) {
		if (i < 4 || ui - 0x6a < 0x8d)
			return 1;
	} else if (pal == 400) {
		if (ui - 2 < 0xd)
			return 1;
	} else if (pal == 0x1c2) {
		if (i != 0 && i < 0x81)
			return 1;
	}
	if ((unsigned)(pal - 0xe0) < 3 && ui - 0xcb < 0x30)
		return 1;
	if ((pal == 0x9b || pal == 0x9a || pal == 0xa3 || pal == 0xa2) && redness > 0x28)
		return 1;
	if ((pal == 0x43 || pal == 0x44) && redness > 0x28)
		return 1;
	if ((unsigned)(pal - 0xdb) < 4 && redness > 0x28)
		return 1;
	if ((unsigned)(pal - 0xf5) < 3 && redness > 0x28)
		return 1;
	if ((pal == 0xe7 || pal == 0xee || pal == 0xf5 || pal == 0x10e || pal == 0xe8 || pal == 0xef
	     || pal == 0xf6 || pal == 0x10f) && redness > 0x28)
		return 1;
	return 0;
}

static void greenBloodEntry(uint16_t *px, int pal, int i)
{
	uint16_t c = *px;
	int r = (c >> 11) << 3;
	int g = ((c >> 5) & 0x3f) << 2;
	int b = (c & 0x1f) << 3;
	int swapped = 0, t;

	if (i == 0xc5) {
		if (r == 0x98 && g == 0 && b == 0) {
			t = r; r = g; g = t;
			swapped = 1;
		}
	} else if (bloodColourA(i, r, g, b)) {
		goto recolour;
	}
	if (bloodRule(pal, i, r, g, b))
		goto recolour;
	if (!swapped)
		return;
	goto store;
recolour:
	t = r; r = g; g = t;
store:
	*px = (uint16_t)((((r >> 3) << 11) & 0xf800) | (((g >> 2) << 5) & 0x7e0) | ((b >> 3) >> 3));
}

/* 0x000278fc  palettes.bin: u16 count; per palette u16 n then n RGB565 words,
 * stored 256 entries apart. */
int Render_loadPalettes(Render *r)
{
	Applet *app;
	DataBuffer *buf;
	InputStream s;
	int ok = 0;

	if (r->palettesLoaded != 0)
		return 1;
	r->palettesLoaded = 1;
	app = g_app;
	buf = app->dataBuffer;
	InputStream_ctor(&s);
	if (!Resource_open(&s, RES_PALETTES, 1))
		goto done;

	r->timeLoadPalettes = App_getUpTimeMS();
	memset(r->palettes, 0, sizeof r->palettes);
	ByteBuf_fill(buf, &s, 2);
	r->numPalettes = Resource_shiftShort(buf);
	for (int pal = 0; pal < r->numPalettes; pal++) {
		int idx = pal << 8;
		ByteBuf_fill(buf, &s, 2);
		int total = Resource_shiftShort(buf) * 2;
		for (int done = 0; done < total; done += CHUNK) {
			int n = readChunk(&s, buf, done, total);
			for (int ofs = 0; ofs < n; ofs += 2) {
				if (idx == MAX_PALETTE_ENTRIES)
					goto done; /* too many entries: fail */
				r->palettes[idx++] = (uint16_t)Resource_shortAt(buf, ofs);
			}
		}
	}
	InputStream_close(&s);
	if (s.f00 != 0)
		goto done;

	r->timeLoadPalettes = App_getUpTimeMS() - r->timeLoadPalettes;
	setMemStat(r, MEM_PALETTES, rshl(r->numPalettes, 10));
	if (r->greenBlood != 0) {
		for (int pal = 0; pal < r->numPalettes; pal++)
			for (int i = 0; i < 256; i++)
				greenBloodEntry(&r->palettes[(pal << 8) + i], pal, i);
	}
	RenderHw_resetTextures(r);   /* port: the GPU's copies are stale */
	ok = 1;
done:
	InputStream_dtor(&s, 0);
	return ok;
}

/* Bubble-sort the registered sprite frames by their bitshape offset (shared
 * by Render_loadBitShapes and Render_loadTexels). */
static void sortSpriteFramesByShape(Render *r)
{
	int n = r->numMapSpriteFrames;
	for (int i = 0; i < n - 1; i++) {
		for (int j = 0; j < n - 1 - i; j++) {
			int a = r->mapSpriteFrames[j], b = r->mapSpriteFrames[j + 1];
			if (r->spriteMap[b].ofs < r->spriteMap[a].ofs) {
				r->mapSpriteFrames[j] = b;
				r->mapSpriteFrames[j + 1] = a;
			}
		}
	}
}

/* 0x000283c8  bitshapes.bin: u32 size, then shapes as bytes. Each shape:
 * [0..3] stexels index (u32), [8]/[9] first/last column, [10]/[11] first/last
 * row, then a 1-bit mask per column from byte 12 ((rows+7)/8 bytes per column).
 * Every registered shape is compiled into Render.shapeSpans:
 *   [0],[1] texel index (later texel offset|page<<14), [2],[3] first/last column,
 *   [4+c] offset of column c's runs minus 2 (relative to the shape),
 *   runs: {row | len<<8, texel offset} ..., 0x7f terminator.
 * spriteMap[frame].ofs is replaced by the shape's shapeSpans index. */
int Render_loadBitShapes(Render *r)
{
	Applet *app = g_app;
	DataBuffer *buf = app->dataBuffer;
	InputStream s;
	int ok;

	setMemStat(r, MEM_BITSHAPES, App_getFreeMemory(app));
	InputStream_ctor(&s);
	if (!Resource_open(&s, RES_BITSHAPES, 1)) {
		ok = 0;
	} else {
		ByteBuf_fill(buf, &s, 4);
		int size = Resource_shiftInt(buf);
		uint16_t *src = malloc((size_t)(uint32_t)size * 2);
		int k = 0;
		for (int done = 0; done < size; done += CHUNK) {
			int n = readChunk(&s, buf, done, size);
			for (int i = 0; i < n; i++)
				src[k++] = buf->data[i];
		}
		InputStream_close(&s);

		sortSpriteFramesByShape(r);

		int out = 0;        /* write cursor in shapeSpans */
		int prevSrc = -1;   /* source offset of the last compiled shape */
		int prevOut = 0;    /* its shapeSpans index */
		for (int f = 0; f < r->numMapSpriteFrames; f++) {
			ShapeMap *sm = &r->spriteMap[r->mapSpriteFrames[f]];
			int ofs = sm->ofs;
			if (ofs != prevSrc) {
				const uint16_t *sh = src + ofs;
				int cols = (int16_t)sh[9] - (int16_t)sh[8] + 1;
				int rows = (int16_t)sh[11] - (int16_t)sh[10] + 1;
				int colBytes = (rows & 7) ? rows / 8 + 1 : rows / 8;
				int texel = 0;

				prevSrc = ofs;
				prevOut = out;
				r->shapeSpans[out] = (uint16_t)((sh[0] & 0xff) | ((sh[1] << 8) & 0xff00));
				r->shapeSpans[out + 1] = (uint16_t)((sh[2] & 0xff) | ((sh[3] << 8) & 0xff00));
				r->shapeSpans[out + 2] = sh[8];
				r->shapeSpans[out + 3] = sh[9];
				out = out + 4 + cols;
				for (int c = 0; c < cols; c++) {
					int maskOfs = ofs + colBytes * c + 0xc;
					int y = 0;
					r->shapeSpans[prevOut + c + 4] = (uint16_t)(out - prevOut - 2);
					while (y < rows) {
						int start, runTexel;
						while (y < rows && ((int16_t)src[maskOfs + y / 8] & (1 << (y & 7))) == 0)
							y++;
						if (y == rows)
							break;
						start = y;
						runTexel = texel;
						while (y < rows && ((int16_t)src[maskOfs + y / 8] & (1 << (y & 7))) != 0) {
							texel++;
							y++;
						}
						r->shapeSpans[out] = (uint16_t)(((sh[10] + start) & 0xff) | ((y - start) << 8));
						r->shapeSpans[out + 1] = (uint16_t)runTexel;
						out += 2;
					}
					r->shapeSpans[out++] = SHAPE_END;
				}
			}
			sm->ofs = prevOut;
		}
		free(src);
		r->numShapeSpans = out;
		setMemStat(r, MEM_BITSHAPES, rshl(out, 1));
		ok = 1;
	}
	InputStream_dtor(&s, 0);
	return ok;
}

/* Expand one 4bpp byte into two texels (low nibble first). */
static void putNibbles(Render *r, int *off, int v)
{
	uint8_t *page = r->texelPages + r->curPage * TEXEL_PAGE_SIZE;
	page[*off] = (uint8_t)(v & 0xf);
	page[*off + 1] = (uint8_t)(v >> 4);
	*off += 2;
}

/* 0x00029b38  Loads the texels of everything the map registered.
 * ftexels (1008): 64x64 flats converted to RGB565 through their palette.
 * wtexels (1007) / stexels (1006): palette indices copied into the four texel
 * pages. Each file: int array of offsets, then per texture a format byte
 * (4 = packed 4bpp) and the data. Textures are read in file order (the
 * registration lists are sorted first) so the stream only moves forward. */
int Render_loadTexels(Render *r)
{
	Applet *app = g_app;
	DataBuffer *buf = app->dataBuffer;
	InputStream s;
	IntArray offs = { 0, NULL };
	int16_t cache[4096];  /* raw bytes of the last flat (reused when two flats share data) */
	int flatSlot[16];     /* flatIds[k] -> slot in Render.flats (order before sorting) */
	int widths[1200];     /* texel bytes of each registered sprite frame */
	int off = 0;          /* write offset in the current texel page */
	int pos, ok = 0;

	InputStream_ctor(&s);
	Hw_clearSources();   /* port: for a mod's GPU textures (oe_hw.h) */
	setMemStat(r, MEM_TEXELS, App_getFreeMemory(app));
	r->texelBytes = rmul(r->numMapTextures, WALL_TEXELS);
	r->texelBytes2 = 0;

	sortSpriteFramesByShape(r);
	for (int i = 0; i < r->numMapTextures - 1; i++) {
		for (int j = 0; j < r->numMapTextures - 1 - i; j++) {
			int a = r->mapTextures[j], b = r->mapTextures[j + 1];
			if (r->wallMap[b].texelOfs < r->wallMap[a].texelOfs) {
				r->mapTextures[j] = b;
				r->mapTextures[j + 1] = a;
			}
		}
	}
	for (int k = 0; k < r->numFlats; k++)
		flatSlot[k] = k;
	for (int i = 0; i < r->numFlats - 1; i++) {
		for (int j = 0; j < r->numFlats - 1 - i; j++) {
			int a = r->flatIds[j], b = r->flatIds[j + 1];
			if (r->flatMap[r->flatRemap[b]].ofs < r->flatMap[r->flatRemap[a]].ofs) {
				int t;
				r->flatIds[j] = b;
				r->flatIds[j + 1] = a;
				t = flatSlot[j];
				flatSlot[j] = flatSlot[j + 1];
				flatSlot[j + 1] = t;
			}
		}
	}
	for (int k = 0; k < r->numMapSpriteFrames; k++) {
		widths[k] = Render_getShapeWidth(r, r->mapSpriteFrames[k]);
		r->texelBytes2 += widths[k];
	}

	/* ---- flats ---- */
	if (!Resource_open(&s, RES_FTEXELS, 1))
		goto out_noarray;
	Stream_readIntArray(buf, &s, &offs);
	{
		int prevOfs = -1, prevFmt = -1;
		pos = rshl(offs.count, 2);
		for (int k = 0; k < r->numFlats; k++) {
			ShapeMap *fm = &r->flatMap[r->flatRemap[r->flatIds[k]]];
			uint16_t *dst = r->flats + flatSlot[k] * 0x1000;
			int ofs = fm->ofs;

			r->curPalette = r->palettes + fm->palette;
			Hw_noteSource(OE_HWSRC_FLAT, flatSlot[k], ofs);
			if (ofs == prevOfs) {
				int n = prevFmt == 4 ? 0x800 : 0x1000;
				for (int i = 0; i < n; i++) {
					int v = cache[i];
					if (prevFmt == 4) {
						dst[i * 2] = r->curPalette[v & 0xf];
						dst[i * 2 + 1] = r->curPalette[(v >> 4) & 0xf];
					} else {
						dst[i] = r->curPalette[v];
					}
				}
				continue;
			}
			prevOfs = ofs;
			if (offs.data[ofs] < pos)
				goto out;
			if (offs.data[ofs] != pos)
				InputStream_skipChunked(buf, &s, offs.data[ofs] - pos, 0);
			ByteBuf_fill(buf, &s, 1);
			int fmt = InputStream_peekByte(buf, 0);
			int total = fmt == 4 ? 0x800 : 0x1000;
			prevFmt = fmt;
			for (int done = 0; done < total; done += CHUNK) {
				int n = readChunk(&s, buf, done, total);
				for (int i = 0; i < n; i++) {
					int v = Resource_bufferByte(buf, i);
					int t = done + i;
					cache[t] = (int16_t)v;
					if (fmt == 4) {
						dst[t * 2] = r->curPalette[v & 0xf];
						dst[t * 2 + 1] = r->curPalette[(v >> 4) & 0xf];
					} else {
						dst[t] = r->curPalette[v];
					}
				}
			}
			pos = offs.data[ofs] + total + 1;
		}
	}
	Array_free(&offs);
	InputStream_close(&s);

	/* ---- wall texels ---- */
	if (!Resource_open(&s, RES_WTEXELS, 1))
		goto out;
	Stream_readIntArray(buf, &s, &offs);
	{
		int prevIdx = -1, prevOff = -1, prevPage = -1;
		pos = rshl(offs.count, 2);
		for (int k = 0; k < r->numMapTextures; k++) {
			TexMap *wm = &r->wallMap[r->mapTextures[k]];
			int idx = wm->texelOfs; /* still the file index here */
			if (idx == prevIdx) {
				wm->texelOfs = prevOff;
				wm->page = prevPage;
				continue;
			}
			if (offs.data[idx] < pos)
				goto out;
			if (offs.data[idx] != pos)
				InputStream_skipChunked(buf, &s, offs.data[idx] - pos, 0);
			Stream_read(&s, buf->data, 0, 1);
			int fmt = InputStream_peekByte(buf, 0);
			int size = fmt == 4 ? WALL_TEXELS4 : WALL_TEXELS;
			if (off + WALL_TEXELS > TEXEL_PAGE_SIZE) {
				if (++r->curPage == 4)
					goto out;
				off = 0;
			}
			wm->texelOfs = off;
			wm->page = r->curPage;
			Hw_noteSource(OE_HWSRC_WALL, r->curPage * TEXEL_PAGE_SIZE + off, idx);
			prevOff = off;
			prevIdx = idx;
			prevPage = r->curPage;
			if (fmt == 4) {
				for (int done = 0; done < size; done += CHUNK) {
					int n = readChunk(&s, buf, done, size);
					for (int i = 0; i < n; i++)
						putNibbles(r, &off, InputStream_peekByte(buf, i));
				}
			} else {
				Stream_read(&s, r->texelPages + r->curPage * TEXEL_PAGE_SIZE, off, size);
				off += size;
			}
			pos = offs.data[idx] + size + 1;
		}
	}
	Array_free(&offs);
	InputStream_close(&s);
	setMemStat(r, MEM_WALLTEXELS, off);
	setMemStat(r, MEM_TEXELBYTES, off);

	/* ---- sprite texels ---- */
	if (!Resource_open(&s, RES_STEXELS, 1))
		goto out;
	Stream_readIntArray(buf, &s, &offs);
	{
		int prev = -1;
		pos = rshl(offs.count, 2);
		for (int k = 0; k < r->numMapSpriteFrames; k++) {
			int sp = r->spriteMap[r->mapSpriteFrames[k]].ofs; /* shapeSpans index */
			uint16_t *hdr = &r->shapeSpans[sp];
			if (sp == prev) {
				hdr[0] = r->shapeSpans[prev];
				hdr[1] = r->shapeSpans[prev + 1];
				continue;
			}
			int idx = (int32_t)((uint32_t)hdr[0] | ((uint32_t)hdr[1] << 16));
			if (idx < 0 || offs.count <= idx)
				goto out;
			prev = sp;
			if (offs.data[idx] < pos)
				goto out;
			if (offs.data[idx] != pos)
				InputStream_skipChunked(buf, &s, offs.data[idx] - pos, 0);
			Stream_read(&s, buf->data, 0, 1);
			int fmt = InputStream_peekByte(buf, 0);
			int size = fmt == 4 ? widths[k] >> 1 : widths[k];
			if (widths[k] + off > TEXEL_PAGE_SIZE) {
				if (++r->curPage == 4)
					goto out;
				off = 0;
			}
			hdr[0] = (uint16_t)off;
			hdr[1] = (uint16_t)((((uint32_t)off << 2) >> 18) | ((uint32_t)r->curPage << 14));
			Hw_noteSource(OE_HWSRC_SPRITE, r->curPage * TEXEL_PAGE_SIZE + off, idx);
			if (fmt == 4) {
				for (int done = 0; done < size; done += CHUNK) {
					int n = readChunk(&s, buf, done, size);
					for (int i = 0; i < n; i++)
						putNibbles(r, &off, InputStream_peekByte(buf, i));
				}
			} else {
				Stream_read(&s, r->texelPages + r->curPage * TEXEL_PAGE_SIZE, off, size);
				off += size;
			}
			pos = offs.data[idx] + size + 1;
		}
	}
	Array_free(&offs);
	InputStream_close(&s);
	setMemStat(r, MEM_TEXELS, memStat(r, MEM_TEXELS) - App_getFreeMemory(app));
	setMemStat(r, MEM_TEXELBYTES, (int64_t)off - memStat(r, MEM_TEXELBYTES));
	RenderHw_resetTextures(r);   /* port: the GPU's copies are stale */
	ok = 1;
out:
	Array_free(&offs);
out_noarray:
	InputStream_dtor(&s, 0);
	return ok;
}

/* 0x0002b674  Front-end background map: every palette and flat to gray. */
void Render_makeGrayscale(Render *r)
{
	for (int i = 0; i < r->numPalettes * 256; i++) {
		unsigned c = r->palettes[i];
		int g = (int)(((c & 0x7ff) >> 5) + ((c & 0xf800) >> 10) + (c & 0x1f) * 2) / 3;
		r->palettes[i] = (uint16_t)(((g >> 1) << 11) | (g << 5) | (g >> 1));
	}
	for (int f = 0; f < r->numFlats; f++) {
		for (int i = 0; i < 0x1000; i++) {
			unsigned c = r->flats[f * 0x1000 + i];
			int g = (int)(((c & 0x7ff) >> 5) + ((c & 0xf800) >> 10) + (c & 0x1f) * 2) / 3;
			r->flats[f * 0x1000 + i] = (uint16_t)(((g >> 1) << 11) | (g << 5) | (g >> 1));
		}
	}
	RenderHw_resetTextures(r);   /* port: the GPU's copies are stale */
}

/* 0x00007ca4  Opens map resource 1999+mapIdx and parses its 25-byte header.
 * Also loads the map's string group, the mappings and (once) the palettes. */
int Render_beginLoadMap(Render *r, int mapIdx)
{
	Applet *app = g_app;
	DataBuffer *buf = app->dataBuffer;
	int16_t group;

	setMemStat(r, MEM_MAP, App_getFreeMemory(app));
	group = (int16_t)(mapIdx + 3);
	app->canvas->mapStringGroup = group;
	if (!Text_loadStringGroup(app->canvas, group))
		return 0;
	if (!Render_loadMappings(r))
		return 0;
	if (!Render_loadPalettes(r))
		return 0;
	r->mapIdx = mapIdx;
	r->f19f6c = 0;
	if (!Resource_open(&r->mapFile, (int16_t)(mapIdx + RES_MAP_BASE), 1))
		return 0;
	Resource_bufferFill(buf, &r->mapFile);
	Resource_bufferEnsure(buf, &r->mapFile, 0x30);
	if (Resource_shiftUByte(buf) != 10) /* format version */
		return 0;

	r->mapHash = Resource_shiftInt(buf);
	int nameId = Resource_shiftShort(buf);
	String_setLength(&r->mapName, 0);
	Text_getString(app->canvas, app->canvas->mapStringGroup, nameId, &r->mapName);

	/* three RGB triplets: floor, ceiling, and a third colour */
	int p = buf->pos;
	r->floorColor = Color_rgb888to565(r, InputStream_peekByte(buf, p + 2) << 16
	                                         | InputStream_peekByte(buf, p + 1) << 8
	                                         | InputStream_peekByte(buf, p));
	r->ceilColor = Color_rgb888to565(r, InputStream_peekByte(buf, p + 5) << 16
	                                        | InputStream_peekByte(buf, p + 4) << 8
	                                        | InputStream_peekByte(buf, p + 3));
	r->color1762c = Color_rgb888to565(r, InputStream_peekByte(buf, p + 8) << 16
	                                         | InputStream_peekByte(buf, p + 7) << 8
	                                         | InputStream_peekByte(buf, p + 6));
	buf->pos += 9;
	r->hdr17628 = Resource_shiftByte(buf);
	r->playerTile = Resource_shiftUShort(buf);
	r->spawnDir = Resource_shiftUByte(buf);
	r->spawnTile = Resource_shiftUShort(buf);
	r->floorHeight = (int16_t)Resource_shiftUByte(buf);
	r->ceilHeight = (int16_t)Resource_shiftUByte(buf);
	r->mapFlagsByte = (uint8_t)Resource_shiftByte(buf);
	memset(r->mapFlags, 0, sizeof r->mapFlags);
	memset(r->flatLerps, 0, sizeof r->flatLerps);
	setMemStat(r, MEM_MAP, memStat(r, MEM_MAP) - App_getFreeMemory(app));
	return 1;
}

/* Sprite tiles that need extra media registered (0x1a8d4..0x1aa80). The tile
 * itself is always registered afterwards. */
static void registerSpriteExtras(Render *r, int tile)
{
	switch (tile) {
	case 0x74: Render_registerTile(r, 0x76); break;
	case 0x99: Render_registerTile(r, 0x98); break;
	case 0x8e: Render_registerTile(r, 0x8f); break;
	case 0x90: Render_registerTile(r, 0x91); break;
	case 0xae: Render_registerTile(r, 0xad); break;
	case 0xad: Render_registerTile(r, 0xae); break;
	case 0x23: case 0x24: case 0x25:
		Render_registerTile(r, 0xc6);
		Render_registerTile(r, 0xe9);
		break;
	case 0x29: case 0x2a: case 0x2b:
		Render_registerTile(r, 0xdf);
		Render_registerTile(r, 0x69);
		Render_registerTile(r, 0xe0);
		break;
	case 0x1a: case 0x1b: Render_registerTile(r, 0xe0); break;
	case 0x1d: case 0x1e: case 0x1f: Render_registerTile(r, 0xc0); break;
	case 0x94: case 0x79: Render_registerTile(r, 0x8d); break;
	case 0x2c: case 0x2d: case 0x2e:
		Render_registerTile(r, 0x63);
		Render_registerTile(r, 0x62);
		break;
	case 0x17: case 0x18: case 0x19:
		Render_registerTile(r, 0x62);
		Render_registerTile(r, 0x63);
		Render_registerTile(r, 0x72);
		break;
	case 0x14: case 0x15: case 0x16: Render_registerTile(r, 0x72); break;
	case 0x3a: case 0x22: Render_registerTile(r, 0xf); break;
	case 0x3b: case 0x3c:
		Render_registerTile(r, 0xeb);
		Render_registerTile(r, 0xee);
		Render_registerTile(r, 0xfc);
		if (tile == 0x3c)
			Render_registerTile(r, 0xce);
		break;
	case 0x88: Render_registerTile(r, 0xc1); break;
	case 0x38: case 0x39: Render_registerTile(r, 0x71); break;
	default: break;
	}
}

static void resetSpareSprite(Sprite *sp)
{
	sp->node = NULL;
	sp->info = 0;
	sp->sortNext = NULL;
	sp->nodeNext = NULL;
	sp->ent = NULL;
	sp->scale = 0x40;
	sp->renderMode = 0;
	sp->y = 0;
	sp->z = 0x20;
	sp->x = 0;
}

/* 0x0001a2a0  Parses the .bsp body (see re/notes/r4 analysis.md), registers
 * the media the map needs, then loads bitshapes and texels. */
int Render_beginLoadMapData(Render *r)
{
	Applet *app = g_app;
	DataBuffer *buf = app->dataBuffer;
	InputStream *stream = &r->mapFile;
	int64_t freeAtStart = App_getFreeMemory(app);
	int i;

	memset(r->mapTextures, 0xff, sizeof r->mapTextures);
	memset(r->mapSpriteFrames, 0xff, sizeof r->mapSpriteFrames);

	/* ---- BSP nodes ---- */
	setMemStat(r, MEM_NODES, App_getFreeMemory(app));
	Resource_bufferEnsure(buf, stream, 2);
	r->numNodes = Resource_shiftShort(buf);
	if (r->numNodes > 400)
		return 0;
	for (i = 0; i < r->numNodes; i++) {
		Node *n = &r->nodes[i];
		if ((i & 7) == 0)
			Resource_bufferEnsure(buf, stream, 0x50);
		n->x1 = Resource_shiftCoord(buf);
		n->y1 = Resource_shiftCoord(buf);
		n->x2 = Resource_shiftCoord(buf);
		n->y2 = Resource_shiftCoord(buf);
		int axis = Resource_shiftByte(buf);
		n->args1 = (uint32_t)Resource_shiftCoord(buf) | ((uint32_t)axis << 16);
		int lo = Resource_shiftShort(buf);
		n->args2 = (uint32_t)lo | ((uint32_t)Resource_shiftShort(buf) << 16);
		n->sprites = NULL;
	}
	setMemStat(r, MEM_NODES, (int64_t)App_getFreeMemory(app) - memStat(r, MEM_NODES));

	/* ---- lines ---- */
	setMemStat(r, MEM_LINES, App_getFreeMemory(app));
	Resource_bufferEnsure(buf, stream, 2);
	r->numLines = Resource_shiftShort(buf);
	if (r->numLines > 750)
		return 0;
	r->numMapTextures = 0;
	for (i = 0; i < r->numLines; i++) {
		Line *ln = &r->lines[i];
		if ((i & 7) == 0)
			Resource_bufferEnsure(buf, stream, 0x48);
		ln->x1 = Resource_shiftCoord(buf);
		ln->y1 = Resource_shiftCoord(buf);
		ln->x2 = Resource_shiftCoord(buf);
		ln->y2 = Resource_shiftCoord(buf);
		ln->texture = (int16_t)Resource_shiftUByte(buf);
		ln->texture2 = (int16_t)Resource_shiftUByte(buf);
		ln->flags = (uint32_t)Resource_shiftInt(buf);
		/* z holds the texture u at each end: the line's length (max axis) */
		int adx = Math_abs(ln->x2 - ln->x1);
		int ady = Math_abs(ln->y2 - ln->y1);
		int len = adx > ady ? adx : ady;
		if ((ln->flags & 0x8000) == 0) {
			ln->z1 = 0;
			ln->z2 = len;
		} else {
			ln->z1 = len;
			ln->z2 = 0;
		}
		/* doors: nudge the line 2 units off its tile edge */
		if (ln->flags & 8) {
			if (ln->flags & 0x200) {
				if (ln->flags & 0x4000) {
					ln->x1 += 2;
					ln->x2 += 2;
				} else if (ln->flags & 0x2000) {
					ln->x1 -= 2;
					ln->x2 -= 2;
				}
			} else if (ln->flags & 0x100) {
				if (ln->flags & 0x800) {
					ln->y1 += 2;
					ln->y2 += 2;
				} else if (ln->flags & 0x1000) {
					ln->y1 -= 2;
					ln->y2 -= 2;
				}
			}
		}
		Render_registerTex4(r, ln->texture);
		Render_registerTex4(r, ln->texture2);
		if ((uint16_t)ln->texture == 0x13)
			Render_registerTex4(r, 0x12);
		else if ((uint16_t)ln->texture == 0x12)
			Render_registerTex4(r, 0x13);
	}
	setMemStat(r, MEM_LINES, memStat(r, MEM_LINES) - App_getFreeMemory(app));

	/* ---- sprites: first the floor-standing ones, then the placed ones ---- */
	r->numMapSpriteFrames = 0;
	Resource_bufferEnsure(buf, stream, 4);
	int numStanding = Resource_shiftShort(buf);
	r->numMapSprites = numStanding + Resource_shiftShort(buf);
	r->numSprites = r->numMapSprites + 0x38;
	setMemStat(r, MEM_SPRITES, App_getFreeMemory(app));
	if (r->numSprites > 800)
		return 0;
	for (i = 0; i < r->numMapSprites; i++) {
		Sprite *sp = &r->sprites[i];
		if ((i & 7) == 0)
			Resource_bufferEnsure(buf, stream, 0x38);
		sp->node = NULL;
		sp->sortNext = NULL;
		sp->nodeNext = NULL;
		sp->ent = NULL;
		sp->renderMode = 0;
		sp->scale = 0x40;
		if (i < numStanding) {
			sp->x = Resource_shiftCoord(buf);
			sp->y = Resource_shiftCoord(buf);
			sp->z = 0x20;
			sp->info = (uint32_t)Resource_shiftUByte(buf);
		} else {
			sp->x = Resource_shiftCoord(buf);
			sp->y = Resource_shiftCoord(buf);
			sp->z = Resource_shiftUByte(buf) - 0x20;
			sp->info = (uint32_t)Resource_shiftUByte(buf);
			sp->info |= (uint32_t)Resource_shiftUByte(buf) << 8;
		}
		sp->info |= (uint32_t)Resource_shiftShort(buf) << 16;
		/* wall decorations: nudge 1 unit off the wall they hang on */
		if (sp->info & 0x800000) {
			if (sp->info & 0x100000)
				sp->y += 1;
			else if (sp->info & 0x80000)
				sp->y -= 1;
			else if (sp->info & 0x200000)
				sp->x += 1;
			else if (sp->info & 0x400000)
				sp->x -= 1;
		}
		Render_relinkSpriteAt(r, sp, -1, -1);

		int tile = sp->info & 0xff;
		switch (tile) {
		case 0xb0: case 0xd0: case 0xce: case 0xea: case 0xf2: case 0xf3: case 0xd4:
			sp->renderMode = 4;
			break;
		case 0xa1: case 0xd2: case 0xd5: case 0xd7:
			sp->renderMode = 0xb;
			sp->info |= 0x80000000u;
			break;
		case 0xf7: case 0xf6: case 0xf9: case 0xf5:
			sp->renderMode = 2;
			break;
		case 0x7b:
			sp->info |= 0x80000000u;
			break;
		default:
			sp->renderMode = 0;
			if (tile == 0xc8)
				sp->info |= 0x20000000u;
			break;
		}
		if (sp->info & 0x40000) {
			Render_registerTex4(r, tile); /* wall-textured sprite */
		} else {
			registerSpriteExtras(r, tile);
			Render_registerTile(r, tile);
		}
	}
	/* media that can appear on any map (effects, drops, gibs, ...) */
	Render_registerTile(r, 0x17);
	Render_registerTile(r, 0xff);
	Render_registerTile(r, 0xe0);
	Render_registerTile(r, 0xea);
	Render_registerTile(r, 0xfe);
	Render_registerTile(r, 0xd);
	Render_registerTile(r, 0x46);
	Render_registerTile(r, 0xa1);
	/* 48 script-controlled sprites and 8 drop sprites follow the map's */
	for (int k = 0; k < 48; k++, i++) {
		r->customSprites[k] = &r->sprites[i];
		resetSpareSprite(&r->sprites[i]);
	}
	r->firstDropSprite = i;
	for (int k = 0; k < 8; k++, i++) {
		r->dropSprites[k] = &r->sprites[i];
		resetSpareSprite(&r->sprites[i]);
	}
	setMemStat(r, MEM_SPRITES, memStat(r, MEM_SPRITES) - App_getFreeMemory(app));
	for (int t = 0xf2; t <= 0xfd; t++)
		Render_registerTile(r, t);
	for (int t = 0xe1; t <= 0xe7; t++)
		Render_registerTile(r, t);
	for (int t = 0xdc; t <= 0xde; t++)
		Render_registerTile(r, t);
	for (int t = 1; t <= 0xb; t++)
		Render_registerTile(r, t);

	/* ---- tile events ---- */
	Resource_bufferEnsure(buf, stream, 2);
	r->numTileEvents = Resource_shiftShort(buf);
	if (r->numTileEvents > 0) {
		if (r->numTileEvents > 400)
			return 0;
		Resource_bufferEnsure(buf, stream, r->numTileEvents << 1);
		for (i = 0; i < r->numTileEvents; i++)
			r->eventTiles[i] = (int16_t)Resource_shiftShort(buf);
		Resource_bufferEnsure(buf, stream, r->numTileEvents << 2);
		for (i = 0; i < r->numTileEvents; i++) {
			uint32_t ev = (uint32_t)Resource_shiftInt(buf);
			if (ev & 0x7f00000)
				r->mapFlags[r->eventTiles[ev & 0x1ff]] |= 0x10;
			r->tileEvents[i] = ev;
		}
	}

	/* ---- event bytecode: {op, arg, flags} ---- */
	Resource_bufferEnsure(buf, stream, 2);
	r->numByteCodes = Resource_shiftShort(buf);
	if (r->numByteCodes > 0) {
		if (r->numByteCodes > 1200)
			return 0;
		Resource_bufferEnsure(buf, stream, r->numByteCodes * 9);
		for (i = 0; i < r->numByteCodes; i++) {
			int32_t *bc = &r->byteCodes[i * 3];
			bc[0] = Resource_shiftByte(buf);
			bc[1] = Resource_shiftInt(buf);
			bc[2] = Resource_shiftInt(buf);
			if (bc[0] == 0x22) { /* CHANGESPRITE: preload the new sprite / wall texture */
				int v = bc[1] >> 13;
				if (v >= 0x101)
					Render_registerTex4(r, v - 0x101);
				else
					Render_registerTile(r, v);
			} else if (bc[0] == 0x19) { /* ANIM */
				Render_registerTile(r, (int)(((uint32_t)bc[1] << 14) >> 24));
			}
		}
	}
	setMemStat(r, MEM_BYTECODE, rmul(r->numByteCodes, 12));

	/* compact the registration tables (-1 = unused) */
	for (int src = 0, dst = 0; src < 1200; src++) {
		int v = r->mapSpriteFrames[src];
		if (v != -1) {
			r->mapSpriteFrames[dst++] = v;
			if (src >= dst)
				r->mapSpriteFrames[src] = -1;
		}
	}
	for (int src = 0, dst = 0; src < 256; src++) {
		int v = r->mapTextures[src];
		if (v != -1) {
			r->mapTextures[dst++] = v;
			if (src >= dst)
				r->mapTextures[src] = -1;
		}
	}

	/* ---- map flags: a nibble per tile ---- */
	Resource_bufferEnsure(buf, stream, 0x200);
	for (i = 0; i < 0x200; i++) {
		int b = Resource_shiftByte(buf);
		r->mapFlags[i * 2] |= (uint16_t)(b & 0xf);
		r->mapFlags[i * 2 + 1] |= (uint16_t)(((uint32_t)b << 24) >> 28);
	}

	/* ---- floor and ceiling flat per tile ---- */
	r->numFlats = 0;
	Resource_bufferEnsure(buf, stream, 0x800);
	for (int pass = 0; pass < 2; pass++) {
		uint16_t **tiles = pass == 0 ? r->floorTiles : r->ceilTiles;
		int changes = 0, prev = 0;
		for (i = 0; i < 0x400; i++) {
			int id = Resource_shiftUByte(buf);
			int slot;
			for (slot = 0; slot < r->numFlats; slot++)
				if (r->flatIds[slot] == id)
					break;
			if (slot == r->numFlats) {
				if (r->numFlats == 16)
					return 0;
				r->flatIds[r->numFlats++] = id;
			}
			if (id != prev) {
				prev = id;
				changes++;
			}
			tiles[i] = r->flats + slot * 0x1000;
		}
		if (changes == 1) {
			if (pass != 0)
				r->ceilUniform = 1;
			else
				r->floorUniform = 1;
		}
	}
	InputStream_close(stream);
	setMemStat(r, MEM_MAP, memStat(r, MEM_MAP) + App_getFreeMemory(app) - freeAtStart);

	r->timeLoadA = App_getUpTimeMS();
	if (!Render_loadBitShapes(r))
		return 0;
	r->timeLoadA = App_getUpTimeMS() - r->timeLoadA;
	r->timeLoadB = App_getUpTimeMS();
	if (!Render_loadTexels(r))
		return 0;
	r->timeLoadB = App_getUpTimeMS() - r->timeLoadB;
	return 1;
}

/* 0x000439a4 */
void Render_unloadMap(Render *r)
{
	Canvas *cv;

	r->numMapSprites = 0;
	r->numNodes = 0;
	r->numLines = 0;
	r->numTileEvents = 0;
	r->numByteCodes = 0;
	r->curPage = 0;
	cv = g_app->canvas;
	if (cv->mapStringGroup != -1)
		Canvas_unloadStringGroup(cv, cv->mapStringGroup);
	g_app->canvas->mapStringGroup = -1;
	r->floorUniform = 0;
	r->ceilUniform = 0;
	r->bobEnable = 0;
	r->frameCount = 0;
}

/* 0x000043c8  Mark one sprite frame as needed by the map. */
void Render_registerSpriteFrame(Render *r, int tile, int frame)
{
	int i = r->spriteRemap[tile] + frame;
	if (r->mapSpriteFrames[i] == -1) {
		r->mapSpriteFrames[i] = i;
		r->numMapSpriteFrames++;
	}
}

/* 0x00004404  Mark one wall texture frame as needed by the map. */
void Render_registerTexFrame(Render *r, int tile, int frame)
{
	int i = r->wallRemap[tile] + frame;
	if (r->mapTextures[i] == -1) {
		r->mapTextures[i] = i;
		r->numMapTextures++;
	}
}

/* 0x000052a8  Register every frame a sprite tile can show. */
void Render_registerTile(Render *r, int tile)
{
	int frames, f;

	if ((unsigned)(tile - 0x11) < 0x30) { /* monsters: 9 animation frames */
		for (f = 0; f < 9; f++)
			Render_registerSpriteFrame(r, tile, f);
		if ((unsigned)(tile - 0x26) <= 2) {
			Render_registerSpriteFrame(r, tile, 9);
			Render_registerTile(r, 0x95);
		}
		if ((unsigned)(tile - 0x11) <= 2)
			Render_registerSpriteFrame(r, tile, 9);
		if ((unsigned)(tile - 0x14) <= 2)
			Render_registerSpriteFrame(r, tile, 9);
		if ((unsigned)(tile - 0x2c) <= 2)
			Render_registerSpriteFrame(r, tile, 9);
		if (tile == 0x39) {
			Render_registerSpriteFrame(r, 0x39, 9);
			return;
		}
		if (tile == 0x3b || tile == 0x3c)
			for (; f < 13; f++)
				Render_registerSpriteFrame(r, tile, f);
		return;
	}
	switch (tile) {
	case 8: case 9: case 10: case 0xe: frames = 7; break;
	case 5: frames = 8; break;
	case 1: case 0xf: case 0x10: case 0x96: case 0x97: case 0xb5: frames = 5; break;
	case 7: case 0x72: frames = 6; break;
	case 0xb: frames = 9; break;
	default: frames = 4; break;
	}
	for (f = 0; f < frames; f++)
		Render_registerSpriteFrame(r, tile, f);
}

/* 0x0000566c */
void Render_registerTex4(Render *r, int tile)
{
	Render_registerTexFrame(r, tile, 0);
	Render_registerTexFrame(r, tile, 1);
	Render_registerTexFrame(r, tile, 2);
	Render_registerTexFrame(r, tile, 3);
}

/* 0x00019c80  Binary search of the sorted event tile list; -1 if absent. */
int Render_findTileEvent(Render *r, int tileIndex)
{
	int lo = 0, hi = r->numTileEvents, mid;

	do {
		mid = lo + ((hi - lo) >> 1);
		int t = r->eventTiles[mid];
		if (t == tileIndex)
			return mid;
		if (tileIndex >= t)
			lo = mid;
		else
			hi = mid;
	} while (mid != 0 && hi - 1 != mid);
	return -1;
}

/* 0x0003811c  out[1] = AWARDSECRET (op 0x53) count, out[0] = those not yet awarded. */
void Render_countSecrets(void *unused, int *out)
{
	Render *r = g_app->render;
	int total = 0, open = 0;

	for (int i = 0; i < r->numByteCodes; i++) {
		if (r->byteCodes[i * 3] == 0x53) {
			total++;
			if (r->byteCodes[i * 3 + 2] == 0)
				open++;
		}
	}
	out[1] = total;
	out[0] = open;
}

/* 0x0001bc28 */
int Render_getLineIndex(Line *line)
{
	return (int)(line - g_app->render->lines);
}

/* 0x00031968 */
void Render_relinkSprite(Render *r, Sprite *sprite)
{
	Render_relinkSpriteAt(r, sprite, -1, -1);
}

/* 0x00031974  Move a sprite to the sprite list of the BSP leaf containing
 * (x, y) (x == -1: the sprite's own position). */
void Render_relinkSpriteAt(Render *r, Sprite *sprite, int x, int y)
{
	Node *node;

	if (x == -1) {
		x = sprite->x;
		y = sprite->y;
	}
	node = sprite->node;
	if (node != NULL) {
		if (node->sprites == sprite) {
			node->sprites = sprite->nodeNext;
		} else {
			for (Sprite *p = node->sprites; p != NULL; p = p->nodeNext) {
				if (p->nodeNext == sprite) {
					p->nodeNext = sprite->nodeNext;
					break;
				}
			}
		}
	}
	node = &r->nodes[0];
	while (node->args1 & 0x30000) {
		int split = node->args1 & 0xffff;
		int v = (node->args1 & 0x10000) ? x : y;
		node = &r->nodes[v < split ? node->args2 >> 16 : node->args2 & 0xffff];
	}
	sprite->node = node;
	sprite->nodeNext = node->sprites;
	node->sprites = sprite;
}

/* 0x0003afa0  View rectangle {x, y, w, h}; the height is made even. The
 * rectangle is in game pixels; the renderer keeps it in device pixels (times
 * Brew_screenScale(), the HD mode), so everything it draws comes out at the
 * device resolution. */
void Render_setScreenRect(Render *r, short *rect)
{
	int s = Brew_screenScale();
	int w = rect[2] * s;

	r->width = w;
	r->height = rect[3] * s;
	r->fbOffsetX = rect[0] * s;
	r->fbOffsetY = rect[1] * s;
	if (r->height & 1)
		r->height = r->height - 1;
	r->centerY = r->height / 2;
	r->centerX = w / 2;
	r->projX = rshl(w / 2, 16) - 0x8000;
	r->projH = rshl(w, 16);
	r->projW = rshl(w, 16) - 0x8000;
	free(r->columnDepth);
	r->columnDepth = malloc((size_t)r->width * sizeof *r->columnDepth);
}
