/*
 * BREW runtime subset used by Orcs & Elves and Orcs & Elves II (see brew_shim.h),
 * running inside the game module. Bitmaps and 2D drawing are plain C here;
 * files, resources, time and the screen go to the engine through system calls.
 *
 * Based on the brew_shim.c of oe-re / oe2-re (one shim for both games: O&E II
 * uses a subset). Differences from those:
 *   - files live in the engine (save folder of the game), see trap_File*;
 *   - the .bar and .mod are opened by the engine, resources are copied in;
 *   - IDISPLAY_Update is deferred for both games, as BREW did (oe2-re presented
 *     at once; only the last frame of a tick could ever be seen either way).
 */
#include "brew_shim.h"
#include "oe_vm.h"

#include <stdlib.h>
#include <string.h>

#include "moddata.h"
#include "oe_hw.h"

static IBitmap *screen;
static const char *brewGameName = "Orcs & Elves";

/* ---------------------------------------------------------------- colour */

static NativeColor rgb_to_565(unsigned r, unsigned g, unsigned b)
{
	return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}

NativeColor IBITMAP_RGBToNative(IBitmap *b, RGBVAL rgb)
{
	(void)b;
	return rgb_to_565((rgb >> 8) & 0xff, (rgb >> 16) & 0xff, (rgb >> 24) & 0xff);
}

RGBVAL IBITMAP_NativeToRGB(IBitmap *b, NativeColor c)
{
	unsigned r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, bl = c & 0x1f;
	(void)b;
	return MAKE_RGB((r << 3) | (r >> 2), (g << 2) | (g >> 4), (bl << 3) | (bl >> 2));
}

/* ---------------------------------------------------------------- bitmaps */

IBitmap *IBITMAP_Create(int w, int h)
{
	IBitmap *b = calloc(1, sizeof(*b));
	if (!b)
		return NULL;
	b->px = calloc((size_t)(w > 0 ? w : 1) * (size_t)(h > 0 ? h : 1), sizeof(uint16_t));
	if (!b->px) {
		free(b);
		return NULL;
	}
	b->width = w;
	b->height = h;
	b->pitch = w;
	b->ownsPixels = 1;
	b->transparent = 0xf81f; /* magenta, the colour key used by the game's art */
	b->scale = 1;
	return b;
}

/* Fills game pixels [x, x + w) of game row y with c, as scale x scale device
 * blocks (the caller has clipped). */
static void put_span(IBitmap *b, int x, int y, int w, uint16_t c)
{
	int s = b->scale, i, r;
	for (r = 0; r < s; r++) {
		uint16_t *p = b->px + (y * s + r) * b->pitch + x * s;
		for (i = 0; i < w * s; i++)
			p[i] = c;
	}
}

void IBITMAP_Release(IBitmap *b)
{
	if (!b || b == screen)
		return;
	if (b->ownsPixels)
		free(b->px);
	free(b);
}

static int clip_rect(int *x, int *y, int *w, int *h, int bw, int bh)
{
	if (*x < 0) { *w += *x; *x = 0; }
	if (*y < 0) { *h += *y; *y = 0; }
	if (*x + *w > bw) *w = bw - *x;
	if (*y + *h > bh) *h = bh - *y;
	return *w > 0 && *h > 0;
}

int IBITMAP_FillRect(IBitmap *b, const AEERect *rc, NativeColor c, int rop)
{
	int x = rc->x, y = rc->y, w = rc->dx, h = rc->dy;
	/* FillRect with AEE_RO_TRANSPARENT is BREW's IBitmap_Invalidate: mark dirty, draw nothing. */
	if (rop == AEE_RO_TRANSPARENT)
		return SUCCESS;
	if (!b || !clip_rect(&x, &y, &w, &h, b->width, b->height))
		return SUCCESS;
	for (int j = 0; j < h; j++)
		put_span(b, x, y + j, w, (uint16_t)c);
	return SUCCESS;
}

int IBITMAP_BltIn(IBitmap *dst, int xDst, int yDst, int dx, int dy, IBitmap *src, int xSrc, int ySrc, int rop)
{
	int i, j;
	if (!dst || !src)
		return EFAILED;
	/* clip against the source */
	if (xSrc < 0) { xDst -= xSrc; dx += xSrc; xSrc = 0; }
	if (ySrc < 0) { yDst -= ySrc; dy += ySrc; ySrc = 0; }
	if (xSrc + dx > src->width) dx = src->width - xSrc;
	if (ySrc + dy > src->height) dy = src->height - ySrc;
	/* clip against the destination */
	if (xDst < 0) { xSrc -= xDst; dx += xDst; xDst = 0; }
	if (yDst < 0) { ySrc -= yDst; dy += yDst; yDst = 0; }
	if (xDst + dx > dst->width) dx = dst->width - xDst;
	if (yDst + dy > dst->height) dy = dst->height - yDst;
	if (dx <= 0 || dy <= 0)
		return SUCCESS;
	if (src->scale != 1) {
		/* an HD picture (Brew_hdBitmap): its device pixels sampled at the destination's */
		int s = dst->scale, t = src->scale, r, c;
		uint16_t key = (uint16_t)src->transparent;
		for (r = 0; r < dy * s; r++) {
			const uint16_t *sp = src->px + (ySrc * t + r * t / s) * src->pitch + xSrc * t;
			uint16_t *d = dst->px + (yDst * s + r) * dst->pitch + xDst * s;
			for (c = 0; c < dx * s; c++) {
				uint16_t v = sp[c * t / s];
				if (rop != AEE_RO_TRANSPARENT || v != key)
					d[c] = v;
			}
		}
		return SUCCESS;
	}
	if (dst->scale > 1) {
		/* into the HD screen: every source pixel becomes a scale x scale block */
		int s = dst->scale, r, k;
		for (j = 0; j < dy; j++) {
			const uint16_t *sp = src->px + (ySrc + j) * src->pitch + xSrc;
			uint16_t key = (uint16_t)src->transparent;
			for (r = 0; r < s; r++) {
				uint16_t *d = dst->px + ((yDst + j) * s + r) * dst->pitch + xDst * s;
				for (i = 0; i < dx; i++, d += s) {
					if (rop == AEE_RO_TRANSPARENT && sp[i] == key)
						continue;
					for (k = 0; k < s; k++)
						d[k] = sp[i];
				}
			}
		}
		return SUCCESS;
	}
	for (j = 0; j < dy; j++) {
		const uint16_t *s = src->px + (ySrc + j) * src->pitch + xSrc;
		uint16_t *d = dst->px + (yDst + j) * dst->pitch + xDst;
		if (rop == AEE_RO_TRANSPARENT) {
			uint16_t key = (uint16_t)src->transparent;
			for (i = 0; i < dx; i++)
				if (s[i] != key)
					d[i] = s[i];
		} else {
			memmove(d, s, (size_t)dx * sizeof(uint16_t));
		}
	}
	return SUCCESS;
}

/* A picture shown from another one (OE_TRAP_RES_IMAGE: a mod's HD picture, the id Tech 3
 * font): b keeps its size for the game, its pixels become that picture's (scale x larger).
 * The 2D code draws such a bitmap at the screen's resolution. */
void Brew_hdBitmap(int resId, IBitmap *b)
{
	int info[3];
	uint16_t *px;
	int s;

	if (!b || b->scale != 1 || b->width <= 0 || b->height <= 0)
		return;
	info[2] = Brew_screenScale();
	if (!trap_ResImage(resId, NULL, 0, info) || info[0] % b->width || info[1] % b->height ||
	    info[0] / b->width != info[1] / b->height)
		return;
	s = info[0] / b->width;
	px = malloc((size_t)info[0] * (size_t)info[1] * sizeof(uint16_t));
	if (!px)
		return;
	if (!trap_ResImage(resId, px, info[0] * info[1], info)) {
		free(px);
		return;
	}
	if (s == 1) {
		/* the same size: the pixels only */
		int y;
		for (y = 0; y < b->height; y++)
			memcpy(b->px + y * b->pitch, px + y * b->width, (size_t)b->width * sizeof(uint16_t));
		free(px);
		return;
	}
	if (b->ownsPixels)
		free(b->px);
	b->px = px;
	b->ownsPixels = 1;
	b->pitch = info[0];
	b->scale = s;
}

/* Brew_hdBitmap's pixel at game pixel (x, y) of the picture, sub-pixel (i, j) of the screen's s x s block */
uint16_t Brew_hdPixel(const IBitmap *b, int x, int y, int i, int j, int s)
{
	int t = b->scale;
	return b->px[(y * t + j * t / s) * b->pitch + x * t + i * t / s];
}
#define hd_pixel Brew_hdPixel

void Brew_blendHd(IBitmap *dst, int dx, int dy, int w, int h, const IBitmap *src, int sx, int sy, int keyed,
                  int (*op)(int d, int s, void *arg), void *arg)
{
	int s = dst->scale, x, y, i, j;
	for (y = 0; y < h; y++) {
		for (j = 0; j < s; j++) {
			uint16_t *d = dst->px + ((dy + y) * s + j) * dst->pitch + dx * s;
			for (x = 0; x < w; x++) {
				for (i = 0; i < s; i++, d++) {
					uint16_t v = hd_pixel(src, sx + x, sy + y, i, j, s);
					if (keyed && v == 0xf81f)
						continue;
					*d = (uint16_t)op(*d, v, arg);
				}
			}
		}
	}
}

int IBITMAP_SetTransparencyColor(IBitmap *b, NativeColor c)
{
	if (b)
		b->transparent = c;
	return SUCCESS;
}

int IBITMAP_GetInfo(IBitmap *b, AEEBitmapInfo *info)
{
	if (!b || !info)
		return EFAILED;
	info->cx = (uint32_t)b->width;
	info->cy = (uint32_t)b->height;
	info->nDepth = 16;
	return SUCCESS;
}

int IBITMAP_CreateCompatibleBitmap(IBitmap *b, IBitmap **out, int w, int h)
{
	(void)b;
	*out = IBITMAP_Create(w, h);
	return *out ? SUCCESS : EFAILED;
}

/* ---------------------------------------------------------------- screen */

static int screenScale = 1;

void Brew_setScreenScale(int scale) { screenScale = scale > 1 ? scale : 1; }
int Brew_screenScale(void) { return screen ? screen->scale : screenScale; }

void Brew_initScreen(int w, int h)
{
	if (screen) {
		screen->ownsPixels = 1;
		free(screen->px);
		free(screen);
	}
	screen = IBITMAP_Create(w * screenScale, h * screenScale);
	if (screen) {
		screen->width = w;
		screen->height = h;
		screen->scale = screenScale;
	}
}

IBitmap *Brew_screen(void) { return screen; }

/* BREW's IDISPLAY_Update is IDISPLAY_UpdateEx(FALSE): deferred until the applet returns to the event loop.
 * vmMain calls Brew_flushDisplay after every event and tick. */
static int displayDirty;

void IDISPLAY_Update(void)
{
	displayDirty = 1;
}

void Brew_flushDisplay(void)
{
	if (displayDirty && screen)
		Hw_flush(); /* the 3D view goes with the frame it is in */
	if (displayDirty && screen)
		trap_ScreenUpdate(screen->px, screen->width * screen->scale, screen->height * screen->scale, screen->pitch);
	displayDirty = 0;
}

void IDISPLAY_Backlight(int on) { (void)on; }

void IDISPLAY_ClearScreen(RGBVAL rgb)
{
	AEERect rc = { 0, 0, 0, 0 };
	if (!screen)
		return;
	rc.dx = (int16_t)screen->width;
	rc.dy = (int16_t)screen->height;
	IBITMAP_FillRect(screen, &rc, IBITMAP_RGBToNative(screen, rgb), AEE_RO_COPY);
}

/* ---------------------------------------------------------------- IGraphics */

IGraphics *IGRAPHICS_Create(IBitmap *dst)
{
	IGraphics *g = calloc(1, sizeof(*g));
	if (!g)
		return NULL;
	g->dst = dst;
	g->alpha = g->fillAlpha = 255;
	g->clip.dx = (int16_t)(dst ? dst->width : 0);
	g->clip.dy = (int16_t)(dst ? dst->height : 0);
	return g;
}

void IGRAPHICS_Release(IGraphics *g) { free(g); }

RGBVAL IGRAPHICS_SetBackground(IGraphics *g, uint8_t r, uint8_t gr, uint8_t b)
{
	g->background = rgb_to_565(r, gr, b);
	return MAKE_RGB(r, gr, b);
}

RGBVAL IGRAPHICS_SetColor(IGraphics *g, uint8_t r, uint8_t gr, uint8_t b, uint8_t a)
{
	g->color = rgb_to_565(r, gr, b);
	g->alpha = a;
	return MAKE_RGB(r, gr, b);
}

RGBVAL IGRAPHICS_SetFillColor(IGraphics *g, uint8_t r, uint8_t gr, uint8_t b, uint8_t a)
{
	g->fillColor = rgb_to_565(r, gr, b);
	g->fillAlpha = a;
	return MAKE_RGB(r, gr, b);
}

boolean IGRAPHICS_SetFillMode(IGraphics *g, boolean fill)
{
	boolean old = g->fill;
	g->fill = fill ? 1 : 0;
	return old;
}

boolean IGRAPHICS_SetClip(IGraphics *g, const AEEClip *clip, uint8_t flags)
{
	(void)flags;
	if (!clip || clip->type == CLIPPING_NONE || !g->dst) {
		g->clip.x = g->clip.y = 0;
		g->clip.dx = (int16_t)(g->dst ? g->dst->width : 0);
		g->clip.dy = (int16_t)(g->dst ? g->dst->height : 0);
	} else {
		g->clip = clip->rect;
	}
	return TRUE;
}

void IGRAPHICS_Translate(IGraphics *g, int16_t x, int16_t y)
{
	g->tx = (int16_t)(g->tx + x);
	g->ty = (int16_t)(g->ty + y);
}

static void g_pixel(IGraphics *g, int x, int y, NativeColor c)
{
	if (x < g->clip.x || y < g->clip.y || x >= g->clip.x + g->clip.dx || y >= g->clip.y + g->clip.dy)
		return;
	if (x < 0 || y < 0 || x >= g->dst->width || y >= g->dst->height)
		return;
	put_span(g->dst, x, y, 1, (uint16_t)c);
}

static void g_hspan(IGraphics *g, int x0, int x1, int y, NativeColor c)
{
	int cx0 = g->clip.x, cx1 = g->clip.x + g->clip.dx - 1;
	if (y < g->clip.y || y >= g->clip.y + g->clip.dy || y < 0 || y >= g->dst->height)
		return;
	if (cx0 < 0) cx0 = 0;
	if (cx1 > g->dst->width - 1) cx1 = g->dst->width - 1;
	if (x0 < cx0) x0 = cx0;
	if (x1 > cx1) x1 = cx1;
	if (x0 <= x1)
		put_span(g->dst, x0, y, x1 - x0 + 1, (uint16_t)c);
}

int IGRAPHICS_DrawLine(IGraphics *g, const AEELine *ln)
{
	int x0 = ln->sx + g->tx, y0 = ln->sy + g->ty, x1 = ln->ex + g->tx, y1 = ln->ey + g->ty;
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	if (!g->dst)
		return EFAILED;
	for (;;) {
		g_pixel(g, x0, y0, g->color);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
	return SUCCESS;
}

int IGRAPHICS_DrawRect(IGraphics *g, const AEERect *rc)
{
	int x = rc->x + g->tx, y = rc->y + g->ty, w = rc->dx, h = rc->dy;
	if (!g->dst || w <= 0 || h <= 0)
		return SUCCESS;
	if (g->fill)
		for (int j = y + 1; j < y + h - 1; j++)
			g_hspan(g, x + 1, x + w - 2, j, g->fillColor);
	g_hspan(g, x, x + w - 1, y, g->color);
	g_hspan(g, x, x + w - 1, y + h - 1, g->color);
	for (int j = y; j < y + h; j++) {
		g_pixel(g, x, j, g->color);
		g_pixel(g, x + w - 1, j, g->color);
	}
	return SUCCESS;
}

int IGRAPHICS_DrawCircle(IGraphics *g, const AEECircle *c)
{
	int cx = c->cx + g->tx, cy = c->cy + g->ty, r = c->r;
	int x = r, y = 0, err = 1 - r;
	if (!g->dst || r < 0)
		return SUCCESS;
	while (x >= y) {
		if (g->fill) {
			g_hspan(g, cx - x, cx + x, cy + y, g->fillColor);
			g_hspan(g, cx - x, cx + x, cy - y, g->fillColor);
			g_hspan(g, cx - y, cx + y, cy + x, g->fillColor);
			g_hspan(g, cx - y, cx + y, cy - x, g->fillColor);
		}
		g_pixel(g, cx + x, cy + y, g->color); g_pixel(g, cx - x, cy + y, g->color);
		g_pixel(g, cx + x, cy - y, g->color); g_pixel(g, cx - x, cy - y, g->color);
		g_pixel(g, cx + y, cy + x, g->color); g_pixel(g, cx - y, cy + x, g->color);
		g_pixel(g, cx + y, cy - x, g->color); g_pixel(g, cx - y, cy - x, g->color);
		y++;
		if (err < 0) {
			err += 2 * y + 1;
		} else {
			x--;
			err += 2 * (y - x) + 1;
		}
	}
	return SUCCESS;
}

int IGRAPHICS_ClearRect(IGraphics *g, const AEERect *rc)
{
	int x = rc->x + g->tx, y = rc->y + g->ty;
	if (!g->dst)
		return EFAILED;
	for (int j = y; j < y + rc->dy; j++)
		g_hspan(g, x, x + rc->dx - 1, j, g->background);
	return SUCCESS;
}

void IGRAPHICS_Update(IGraphics *g) { (void)g; IDISPLAY_Update(); }

/* ---------------------------------------------------------------- files
 * The engine keeps the files (the game's save folder) and their BREW
 * semantics: create fails over an existing file, a write either completes
 * or returns 0, a seek past the end of a writable file extends it. */

struct IFile {
	int handle;
};

static IFileMgr fileMgr;

IFileMgr *IFILEMGR_Get(void) { return &fileMgr; }

IFile *IFILEMGR_OpenFile(IFileMgr *m, const char *name, int mode)
{
	IFile *file;
	int h = trap_FileOpen(name, mode);
	m->lastError = trap_FileLastError();
	if (h <= 0)
		return NULL;
	file = calloc(1, sizeof(*file));
	if (!file) {
		trap_FileClose(h);
		m->lastError = EFAILED;
		return NULL;
	}
	file->handle = h;
	return file;
}

int IFILEMGR_Remove(IFileMgr *m, const char *name)
{
	m->lastError = trap_FileRemove(name);
	return m->lastError;
}

int IFILEMGR_Test(IFileMgr *m, const char *name)
{
	(void)m;
	return trap_FileExists(name) ? SUCCESS : EFILENOEXISTS;
}

int IFILEMGR_GetLastError(IFileMgr *m) { return m->lastError; }

uint32_t IFILEMGR_GetFreeSpace(IFileMgr *m, uint32_t *total)
{
	(void)m;
	if (total)
		*total = 64u << 20;
	return 32u << 20;
}

int32_t IFILE_Read(IFile *f, void *buf, uint32_t n) { return f ? trap_FileRead(f->handle, buf, (int)n) : 0; }

/* BREW wrote to flash synchronously and a failed write set the IFileMgr's last error, which the save code
 * reads; the game only treats 0 as a failed write, so the engine returns 0 for a partial one too. */
uint32_t IFILE_Write(IFile *f, const void *buf, uint32_t n)
{
	int w;
	if (!f)
		return 0;
	w = trap_FileWrite(f->handle, buf, (int)n);
	if (w <= 0) {
		fileMgr.lastError = trap_FileLastError();
		return 0;
	}
	return (uint32_t)w;
}

int32_t IFILE_Size(IFile *f) { return f ? trap_FileSize(f->handle) : 0; }

/* SUCCESS / EFAILED; _SEEK_CURRENT with pos 0 returns the current position (File_tell 0x2958 in O&E). */
int IFILE_Seek(IFile *f, int seekType, int32_t pos)
{
	int r;
	if (!f)
		return EFAILED;
	r = trap_FileSeek(f->handle, seekType, pos);
	if (r == EFAILED)
		fileMgr.lastError = trap_FileLastError();
	return r;
}

void IFILE_Release(IFile *f)
{
	if (!f)
		return;
	trap_FileClose(f->handle);
	free(f);
}

/* ---------------------------------------------------------------- resources */

typedef struct ResCache {
	int id;
	uint8_t *data;
	size_t size;
} ResCache;

static ResCache *cache;
static int cacheCount, cacheCap;

int ISHELL_LoadResString(int resId, char *buf, int bufSize)
{
	int n;
	if (bufSize <= 0)
		return 0;
	n = trap_ResString(resId, buf, bufSize);
	return n < 0 ? 0 : n;
}

/* Decompressed resource bytes, read from the engine once and kept for the run (owned by the cache). */
const uint8_t *Brew_loadResource(int resId, size_t *size)
{
	uint8_t *d;
	int n;
	for (int i = 0; i < cacheCount; i++)
		if (cache[i].id == resId) {
			if (size)
				*size = cache[i].size;
			return cache[i].data;
		}
	n = trap_ResSize(resId);
	if (n < 0)
		return NULL;
	d = malloc((size_t)(n > 0 ? n : 1));
	if (!d)
		return NULL;
	if (trap_ResRead(resId, d, n) != n) {
		free(d);
		return NULL;
	}
	if (cacheCount == cacheCap) {
		int cap = cacheCap ? cacheCap * 2 : 64;
		ResCache *nc = realloc(cache, (size_t)cap * sizeof(*nc));
		if (!nc) {
			free(d);
			return NULL;
		}
		cache = nc;
		cacheCap = cap;
	}
	cache[cacheCount].id = resId;
	cache[cacheCount].data = d;
	cache[cacheCount].size = (size_t)n;
	cacheCount++;
	if (size)
		*size = (size_t)n;
	return d;
}

/* ---------------------------------------------------------------- misc */

uint32_t GETUPTIMEMS(void) { return Plat_uptimeMs(); }

/* Milliseconds since local midnight. Under the virtual clock (headless runs) the engine follows the uptime clock
 * so runs stay deterministic. */
uint32_t GETTIMEMS(void) { return (uint32_t)trap_TimeOfDayMs(); }

void GETRAND(uint8_t *buf, int n)
{
	/* BREW's GETRAND is a hardware/OS source; any good PRNG is equivalent here.
	 * The oe_seed cvar (developer use; the engine sets it from OE_SEED / OE2_SEED) seeds it with a fixed
	 * value so headless runs are reproducible. */
	static uint64_t s = 0;
	if (!s) {
		char seed[64];
		trap_Cvar_VariableStringBuffer("oe_seed", seed, sizeof(seed));
		if (seed[0])
			s = 0x9E3779B97F4A7C15ull ^ strtoull(seed, NULL, 0);
		else
			s = 0x9E3779B97F4A7C15ull ^ ((uint64_t)Plat_uptimeMs() << 17) ^ (uint64_t)(uintptr_t)&s;
		if (!s)
			s = 0x9E3779B97F4A7C15ull;
	}
	for (int i = 0; i < n; i++) {
		s ^= s << 13;
		s ^= s >> 7;
		s ^= s << 17;
		buf[i] = (uint8_t)(s >> 24);
	}
}

static int quitRequested;
void ISHELL_CloseApplet(void) { quitRequested = 1; }
int Brew_quitRequested(void) { return quitRequested; }

void ISOUND_Vibrate(int ms) { Plat_vibrate(ms); }
void ISOUND_StopVibrate(void) { Plat_vibrate(0); }

/* ---------------------------------------------------------------- lifecycle */

int Brew_init(const char *gameName, int screenW, int screenH)
{
	brewGameName = gameName;
	/* The engine has already checked the .bar and the .mod (size and CRC32 of the supported release). */
	if (ModData_load(&g_mod) != 0) {
		Oe_print("%s: the engine has no .mod for this game\n", brewGameName);
		return -1;
	}
	Brew_initScreen(screenW, screenH);
	return screen ? 0 : -1;
}

void Brew_shutdown(void)
{
	for (int i = 0; i < cacheCount; i++)
		free(cache[i].data);
	free(cache);
	cache = NULL;
	cacheCount = cacheCap = 0;
	ModData_close(&g_mod);
	if (screen) {
		free(screen->px);
		free(screen);
		screen = NULL;
	}
	quitRequested = 0;
	displayDirty = 0;
}
