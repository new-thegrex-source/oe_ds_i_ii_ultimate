/*
 * oe_export, the game module's part (shared by both games): every wall,
 * sprite and floor texture of the game as a PNG with the name and number a
 * mod's WTEXELS.txt / STEXELS.txt / FTEXELS.txt use (see OE_HWSRC in
 * oe_public.h and the engine's oe_res.c), and those three files listing them.
 *
 * Read straight from the resources (the game's state is not touched), in the
 * formats the renderers load (render_load.c of each game):
 *   mappings.bin  six u32 counts (walls, flats, sprites, then three remap
 *                 tables), then {u32 texels number, u32 palette} per wall,
 *                 flat and sprite (a sprite's number is its bitshape offset)
 *   palettes.bin  u16 count, per palette u16 n and n RGB565 colours
 *   bitshapes.bin u32 size, shapes at 4 + offset: u32 sprite texels number,
 *                 bytes 8..11 first/last column and row in the 88 x 88 box,
 *                 from 12 a bit mask per column (LSB first row)
 *   texels        a format byte (4: two texels a byte, low nibble first),
 *                 then a wall's 117 x 117 column by column, a floor's 64 x 64
 *                 row by row, a sprite's masked texels column by column; in
 *                 O&E one resource each (base + number), in O&E II entries of
 *                 one file (u32 count, u32 offsets from byte 4)
 * A texture several palettes use is exported with the first one.
 */
#include "oe_vm.h"
#include "oe_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WALL_SIZE	117
#define FLAT_SIZE	64
#define SHAPE_BOX	88

static unsigned le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned le32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

static unsigned char *loadRes(int id, int *size)
{
	unsigned char *data;
	int n = trap_ResSize(id);
	*size = 0;
	if (n <= 0)
		return NULL;
	data = malloc((size_t)n);
	if (data && trap_ResRead(id, data, n) != n) {
		free(data);
		return NULL;
	}
	*size = n;
	return data;
}

/* the texels of (kind, number): the format byte and what follows it; *owned set if to be freed */
static const unsigned char *texelsOf(const oeExportGame_t *g, int kind, int number, int *len,
                                     unsigned char **owned, unsigned char *bins[4], int binSizes[4])
{
	*owned = NULL;
	if (g->texelBase[kind]) {
		*owned = loadRes(g->texelBase[kind] + number, len);
		return *owned;
	}
	if (bins[kind] && binSizes[kind] >= 4) {
		unsigned count = le32(bins[kind]), ofs;
		if ((unsigned)number >= count || 4 + 4 * (number + 1) > binSizes[kind])
			return NULL;
		ofs = 4 + le32(bins[kind] + 4 + 4 * number);
		if (ofs >= (unsigned)binSizes[kind])
			return NULL;
		*len = binSizes[kind] - (int)ofs;
		return bins[kind] + ofs;
	}
	return NULL;
}

/* texel i of a texels entry (format byte first) */
static int texel(const unsigned char *t, int len, int i)
{
	if (t[0] == 4) {
		int b = 1 + i / 2;
		return b < len ? (i & 1 ? t[b] >> 4 : t[b] & 15) : 0;
	}
	return 1 + i < len ? t[1 + i] : 0;
}

static void putColour(unsigned char *o, const unsigned short *pal, int idx)
{
	unsigned c = pal[idx & 255];
	o[0] = (unsigned char)(((c >> 11) & 31) * 255 / 31);
	o[1] = (unsigned char)(((c >> 5) & 63) * 255 / 63);
	o[2] = (unsigned char)((c & 31) * 255 / 31);
	o[3] = 255;
}

/* appends "KEYn=path" to a list file text */
static void listAdd(char **text, int *used, int *size, const char *line)
{
	int n = (int)strlen(line);
	if (*used + n + 3 > *size) {
		char *t = realloc(*text, (size_t)(*size * 2 + n + 3));
		if (!t)
			return;
		*text = t;
		*size = *size * 2 + n + 3;
	}
	memcpy(*text + *used, line, (size_t)n);
	*used += n;
	(*text)[(*used)++] = '\r';
	(*text)[(*used)++] = '\n';
}

void Export_textures(const oeExportGame_t *g)
{
	static const char *dirs[4] = { NULL, "WTEXELS", "STEXELS", "FTEXELS" };
	static const char *keys[4] = { NULL, "WALL", "SPRITE", "FLAT" };
	unsigned char *maps, *pals, *shapes, *bins[4] = { NULL, NULL, NULL, NULL };
	int mapsLen, palsLen, shapesLen, binSizes[4] = { 0, 0, 0, 0 };
	unsigned short *palette;
	unsigned char *done[4], *rgba;
	char *lists[4] = { NULL, NULL, NULL, NULL };
	int listUsed[4] = { 0, 0, 0, 0 }, listSize[4] = { 0, 0, 0, 0 }, count[4] = { 0, 0, 0, 0 };
	int numPal, k, i, p, kind;
	unsigned numRec[3];

	maps = loadRes(g->mappings, &mapsLen);
	pals = loadRes(g->palettes, &palsLen);
	shapes = loadRes(g->bitshapes, &shapesLen);
	for (k = 1; k < 4; k++)
		if (g->texelBin[k])
			bins[k] = loadRes(g->texelBin[k], &binSizes[k]);
	if (!maps || !pals || !shapes || mapsLen < 24 || palsLen < 2) {
		Oe_print("oe_export: the game's mappings, palettes or bitshapes cannot be read\n");
		goto out;
	}

	/* palettes, 256 entries apart */
	numPal = (int)le16(pals);
	palette = calloc((size_t)(numPal > 0 ? numPal : 1) * 256, sizeof(unsigned short));
	for (p = 0, k = 2; p < numPal && k + 2 <= palsLen; p++) {
		int n = (int)le16(pals + k), j;
		k += 2;
		for (j = 0; j < n && k + 2 <= palsLen; j++, k += 2)
			if (j < 256)
				palette[p * 256 + j] = (unsigned short)le16(pals + k);
	}

	for (k = 0; k < 4; k++) {
		done[k] = calloc(65536, 1);
		listSize[k] = 4096;
		lists[k] = malloc((size_t)listSize[k]);
	}
	rgba = malloc(SHAPE_BOX * SHAPE_BOX * 4 > WALL_SIZE * WALL_SIZE * 4 ? SHAPE_BOX * SHAPE_BOX * 4 : WALL_SIZE * WALL_SIZE * 4);

	numRec[0] = le32(maps);      /* walls */
	numRec[1] = le32(maps + 4);  /* flats */
	numRec[2] = le32(maps + 8);  /* sprites */
	for (i = 0, p = 24; i < 3; i++) {
		unsigned r;
		kind = i == 0 ? OE_HWSRC_WALL : i == 1 ? OE_HWSRC_FLAT : OE_HWSRC_SPRITE;
		for (r = 0; r < numRec[i] && p + 8 <= mapsLen; r++, p += 8) {
			int number = (int)le32(maps + p), pal = (int)le32(maps + p + 4), len, x, y;
			const unsigned short *pl = pal >= 0 && pal < numPal ? palette + pal * 256 : palette;
			const unsigned char *t, *sh = NULL;
			unsigned char *owned;
			char path[96], line[160];
			int w, h;

			if (kind == OE_HWSRC_SPRITE) {
				/* the record names a bitshape; its texels number is in the shape */
				if (number < 0 || 4 + number + 12 > shapesLen)
					continue;
				sh = shapes + 4 + number;
				number = (int)le32(sh);
			}
			if (number < 0 || number >= 65536 || done[kind][number])
				continue;
			t = texelsOf(g, kind, number, &len, &owned, bins, binSizes);
			if (!t || len < 2)
				continue;
			done[kind][number] = 1;

			if (kind == OE_HWSRC_WALL) {
				w = h = WALL_SIZE;
				for (x = 0; x < w; x++)
					for (y = 0; y < h; y++)
						putColour(rgba + (y * w + x) * 4, pl, texel(t, len, x * h + y));
			} else if (kind == OE_HWSRC_FLAT) {
				w = h = FLAT_SIZE;
				for (y = 0; y < h; y++)
					for (x = 0; x < w; x++)
						putColour(rgba + (y * w + x) * 4, pl, texel(t, len, y * w + x));
			} else {
				/* the bitshape's columns, their set rows in order */
				int col0 = sh[8], col1 = sh[9], row0 = sh[10], row1 = sh[11];
				int rows = row1 - row0 + 1, colBytes = (rows + 7) / 8, c, n = 0;
				w = h = SHAPE_BOX;
				memset(rgba, 0, SHAPE_BOX * SHAPE_BOX * 4);
				for (c = 0; c <= col1 - col0; c++) {
					const unsigned char *mask = sh + 12 + colBytes * c;
					if (mask + colBytes > shapes + shapesLen)
						break;
					for (y = 0; y < rows; y++) {
						if (!(mask[y >> 3] & (1 << (y & 7))))
							continue;
						if (col0 + c < SHAPE_BOX && row0 + y < SHAPE_BOX)
							putColour(rgba + ((row0 + y) * SHAPE_BOX + col0 + c) * 4, pl, texel(t, len, n));
						n++;
					}
				}
			}
			free(owned);

			snprintf(path, sizeof(path), "Textures/%s/%s%d.png", dirs[kind], keys[kind], number);
			trap_ExportFile(path, rgba, 0, w, h);
			snprintf(line, sizeof(line), "%s%d=%s", keys[kind], number, path);
			listAdd(&lists[kind], &listUsed[kind], &listSize[kind], line);
			count[kind]++;
		}
	}
	for (kind = OE_HWSRC_WALL; kind <= OE_HWSRC_FLAT; kind++) {
		char name[32];
		snprintf(name, sizeof(name), "%s.txt", dirs[kind]);
		trap_ExportFile(name, lists[kind], listUsed[kind], 0, 0);
	}
	Oe_print("oe_export: %d walls, %d sprites, %d floors (WTEXELS.txt, STEXELS.txt, FTEXELS.txt)\n",
	         count[OE_HWSRC_WALL], count[OE_HWSRC_SPRITE], count[OE_HWSRC_FLAT]);
	free(rgba);
	for (k = 0; k < 4; k++) {
		free(done[k]);
		free(lists[k]);
	}
	free(palette);
out:
	free(maps);
	free(pals);
	free(shapes);
	for (k = 1; k < 4; k++)
		free(bins[k]);
}
