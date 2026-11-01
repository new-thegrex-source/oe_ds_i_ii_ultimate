/*
 * Render: pixel-level code — the 13 span drawers (vertical texture columns
 * with a blend mode), palette selection and effects, floor/ceiling rows, 2D
 * bitshape sprites (first-person weapon), Render_drawImage with its per-pixel
 * blend ops (Pixel_*), the view fade and colour conversion.
 *
 * Everything writes straight into the RGB565 screen through Render.dib, an
 * IDIB describing Brew_screen() (see Render_syncScreenDib).
 */

#include "render_local.h"
#include "oe_hw.h"

/* Keep Render.dib describing the screen bitmap. The original obtained it once
 * in Render_startup with IBITMAP_QueryInterface(AEECLSID_DIB) on the device
 * bitmap (RGB565, nDepth 16). In the HD mode it describes the device pixels:
 * the renderer works at Brew_screenScale() times the game's resolution (see
 * Render_setScreenRect). */
void Render_syncScreenDib(Render *r)
{
	IBitmap *screen = Brew_screen();

	if (r->dib == NULL)
		r->dib = calloc(1, sizeof(IDIB));
	if (r->dib == NULL || screen == NULL)
		return;
	r->dib->pBmp = (uint8_t *)screen->px;
	r->dib->cx = (uint16_t)(screen->width * screen->scale);
	r->dib->cy = (uint16_t)(screen->height * screen->scale);
	r->dib->nPitch = (int16_t)(screen->pitch * 2);
	r->dib->nDepth = 16;
	r->dib->nColorScheme = 16; /* IDIB_COLORSCHEME_565 */
}

/* ======================================================================
 * Span drawers. One call draws n pixels down column x from row y of the 3D
 * view (Render.framebuffer). Texel = Render.texels[u >> 12] (u is a 64-bit
 * 20.12 value stepped by du), colour = Render.curPalette[texel]. The blend
 * palettes are pre-scaled by Render_setPalette.
 * ==================================================================== */

#define SPAN_SETUP(rv)                                              \
	Render *r = (rv);                                               \
	const uint16_t *pal = r->curPalette;                            \
	const uint8_t *texels = r->texels;                              \
	int stride = Render_pitch(r);                                   \
	uint16_t *dst = Render_viewPixel(r, x, y)

#define SPAN_TEXEL() pal[texels[(int32_t)(u >> 12)]]

/* 0x00029834 blend 0: plain copy (unrolled 8x in the original) */
void Span_drawNormal(void *rv, int x, int y, long long u, int du, int n)
{
	SPAN_SETUP(rv);

	while (--n >= 0) {
		*dst = SPAN_TEXEL();
		u += du;
		dst += stride;
	}
}

/* 0x000299e8 blends 7..10: saturating add (palette pre-scaled by 1, 3/4,
 * 1/2, 1/4 and with each channel's low bit cleared). The carries are smeared
 * back down into their channel. */
void Span_drawAdditive(void *rv, int x, int y, long long u, int du, int n)
{
	SPAN_SETUP(rv);

	while (--n >= 0) {
		uint32_t sum = (uint32_t)SPAN_TEXEL() + (*dst & 0xfffff7deu);
		uint32_t carry = sum & 0x10820;
		*dst = (uint16_t)((sum & 0xfffff7deu) | (carry >> 1) | (carry >> 2) | (carry >> 3));
		u += du;
		dst += stride;
	}
}

/* 0x00029a80 blend 3: dst &= colour */
void Span_drawAnd(void *rv, int x, int y, long long u, int du, int n)
{
	SPAN_SETUP(rv);

	while (--n >= 0) {
		*dst = (uint16_t)(SPAN_TEXEL() & *dst);
		u += du;
		dst += stride;
	}
}

/* 0x00029af8 blend 4: 3/4 screen + 1/4 texture (palette pre-scaled to 1/4) */
void Span_drawBlend75(void *rv, int x, int y, long long u, int du, int n)
{
	SPAN_SETUP(rv);

	while (--n >= 0) {
		uint32_t d = *dst;
		*dst = (uint16_t)(SPAN_TEXEL() + ((d >> 1) & 0xfbef) + ((d >> 2) & 0xf9e7));
		u += du;
		dst += stride;
	}
}

/* 0x00029b84 blend 5: 1/2 screen + 1/2 texture (palette pre-scaled to 1/2) */
void Span_drawBlend50(void *rv, int x, int y, long long u, int du, int n)
{
	SPAN_SETUP(rv);

	while (--n >= 0) {
		*dst = (uint16_t)(SPAN_TEXEL() + ((*dst >> 1) & 0xfbef));
		u += du;
		dst += stride;
	}
}

/* 0x00029c04 blend 6: 1/4 screen + 3/4 texture (palette pre-scaled to 3/4) */
void Span_drawBlend25(void *rv, int x, int y, long long u, int du, int n)
{
	SPAN_SETUP(rv);

	while (--n >= 0) {
		*dst = (uint16_t)(SPAN_TEXEL() + ((*dst >> 2) & 0xf9e7));
		u += du;
		dst += stride;
	}
}

/* 0x00029c8c blend 0xf: per-channel multiply */
void Span_drawMultiply(void *rv, int x, int y, long long u, int du, int n)
{
	SPAN_SETUP(rv);

	while (--n >= 0) {
		uint32_t d = *dst;
		uint32_t s = SPAN_TEXEL();
		uint32_t blue = ((s & 0x1f) * (d & 0x1f)) >> 5;
		uint32_t red = ((s >> 11) * (d >> 11) * 0x40) & 0xf800;
		uint32_t green = ((((s & 0x7ff) >> 5) * ((d & 0x7ff) >> 5)) >> 1) & 0x7e0;
		*dst = (uint16_t)(blue | red | green);
		u += du;
		dst += stride;
	}
}

/* 0x00029d38 blend 2: dst |= colour */
void Span_drawOr(void *rv, int x, int y, long long u, int du, int n)
{
	SPAN_SETUP(rv);

	while (--n >= 0) {
		*dst = (uint16_t)(SPAN_TEXEL() | *dst);
		u += du;
		dst += stride;
	}
}

/* 0x00029db0 blend 0xc: desaturate (and darken) what is already on screen;
 * the texture is not read and u is not stepped. */
void Span_drawGray(void *rv, int x, int y, long long u, int du, int n)
{
	SPAN_SETUP(rv);
	(void)pal;
	(void)texels;

	while (--n >= 0) {
		uint32_t c = *dst;
		int g = (int)((c >> 11) * 2 + ((c & 0x7ff) >> 5) + (c & 0x1f) * 2) / 6;
		g = (int8_t)g;
		*dst = (uint16_t)((((uint32_t)g >> 1) << 11) | ((uint32_t)g << 5) | (uint32_t)(g >> 1));
		dst += stride;
	}
}

/* 0x00029e0c blend 0xff and the r_noTexture debug switch: draw nothing */
void Span_drawNone(void *r, int x, int y, long long u, int du, int n)
{
	(void)r; (void)x; (void)y; (void)u; (void)du; (void)n;
}

/* 0x00029e10 blend 0xb: saturating subtract (palette with each channel's low
 * bit cleared). Borrow guard bits 0x10820 are set above each channel; a
 * channel whose guard bit was consumed is cleared. */
void Span_drawSubtract(void *rv, int x, int y, long long u, int du, int n)
{
	SPAN_SETUP(rv);

	while (--n >= 0) {
		int32_t diff = (int32_t)(((uint32_t)*dst | 0x10820) - SPAN_TEXEL());
		int32_t keep = diff | (int32_t)0xfffef7df;
		*dst = (uint16_t)((keep >> 1) & diff & (keep >> 2) & (keep >> 3) & 0xf7de);
		u += du;
		dst += stride;
	}
}

/* 0x00029ea8 blend 1: dst ^= colour */
void Span_drawXor(void *rv, int x, int y, long long u, int du, int n)
{
	SPAN_SETUP(rv);

	while (--n >= 0) {
		*dst = (uint16_t)(SPAN_TEXEL() ^ *dst);
		u += du;
		dst += stride;
	}
}

/* ======================================================================
 * Palette selection
 * ==================================================================== */

/* 0x00025c58 Select palette `pal` (index into Render.palettes, already <<8),
 * apply the Render.segFlags colour effects (bit 2 grey, bit 3 brighten) and
 * the blend `mode` (pre-scaling the palette into Render.workPalette); stores
 * and returns the span drawer to use (Render.spanFunc). */
void *Render_setPalette(Render *render, int pal, int mode)
{
	uint16_t *work = render->workPalette;
	const uint16_t *src;
	SpanFn span = Span_drawNormal;
	int i;

	if (Hw_fastPass()) {   /* port: the GPU draws the view's pixels */
		render->curPalette = render->palettes + pal;
		render->spanFunc = (void *)Span_drawNone;
		return render->spanFunc;
	}

	render->curPalette = render->palettes + pal;

	if (render->segFlags & 4) { /* grey */
		for (i = 0; i < 256; i++) {
			uint32_t c = render->curPalette[i];
			uint32_t g = (((c & 0x7ff) >> 6) + (c >> 11) + (c & 0x1f)) / 3;
			work[i] = (uint16_t)((g & 0xff) | ((g & 0x7f) << 6) | ((g & 0xff) << 11));
		}
		render->curPalette = work;
	}
	if (render->segFlags & 8) { /* brighten */
		for (i = 0; i < 256; i++) {
			uint32_t c = render->curPalette[i];
			uint32_t red = (c >> 11) + 8;
			uint32_t green = ((c & 0x7ff) >> 5) + 0x10;
			uint32_t blue = (c & 0x1f) + 8;
			if (red > 0x1f) red = 0x1f;
			if (green > 0x3f) green = 0x3f;
			if (blue > 0x1f) blue = 0x1f;
			work[i] = (uint16_t)((red << 11) | (green << 5) | blue);
		}
		render->curPalette = work;
	}

	src = render->curPalette;
	switch (mode) {
	case 1: span = Span_drawXor; break;
	case 2: span = Span_drawOr; break;
	case 3: span = Span_drawAnd; break;
	case 4: /* 1/4 texture over 3/4 screen */
		span = Span_drawBlend75;
		for (i = 0; i < 256; i++)
			work[i] = (uint16_t)((src[i] >> 2) & 0xf9e7);
		render->curPalette = work;
		break;
	case 5: /* half and half */
		span = Span_drawBlend50;
		for (i = 0; i < 256; i++)
			work[i] = (uint16_t)((src[i] >> 1) & 0xfbef);
		render->curPalette = work;
		break;
	case 6: /* 3/4 texture over 1/4 screen */
		span = Span_drawBlend25;
		for (i = 0; i < 256; i++)
			work[i] = (uint16_t)(((src[i] >> 1) & 0xfbef) + ((src[i] >> 2) & 0xf9e7));
		render->curPalette = work;
		break;
	case 7: /* additive */
	case 0xb: /* subtractive */
		span = (mode == 7) ? Span_drawAdditive : Span_drawSubtract;
		for (i = 0; i < 256; i++)
			work[i] = (uint16_t)(src[i] & 0xf7de);
		render->curPalette = work;
		break;
	case 8: /* additive, 3/4 strength */
		span = Span_drawAdditive;
		for (i = 0; i < 256; i++)
			work[i] = (uint16_t)(((src[i] >> 1) & 0xf3ce) + ((src[i] >> 2) & 0xf1c6));
		render->curPalette = work;
		break;
	case 9: /* additive, 1/2 strength */
		span = Span_drawAdditive;
		for (i = 0; i < 256; i++)
			work[i] = (uint16_t)((src[i] >> 1) & 0xf3ce);
		render->curPalette = work;
		break;
	case 10: /* additive, 1/4 strength */
		span = Span_drawAdditive;
		for (i = 0; i < 256; i++)
			work[i] = (uint16_t)((src[i] >> 2) & 0xf1c6);
		render->curPalette = work;
		break;
	case 0xc: span = Span_drawGray; break;
	case 0xf: span = Span_drawMultiply; break;
	case 0xff: span = Span_drawNone; break;
	default: break; /* 0 and anything else: normal */
	}

	if (render->rNoTexture)
		span = Span_drawNone;
	render->spanFunc = (void *)span;
	return render->spanFunc;
}

/* ======================================================================
 * Floor and ceiling
 * ==================================================================== */

/* 0x00014790 One horizontal run of floor/ceiling. `tex` is the 32x32 tile map
 * of 64x64 RGB565 flats (uint16_t *[1024]); u/v are world 16.16 positions.
 * perTile = 0 means the whole plane uses one flat (tex[0]). */
void Render_drawFlatSpan(Render *render, int x, int y, void *tex, int u, int v, int du, int dv, int count, int perTile)
{
	uint16_t *const *tiles = (uint16_t *const *)tex;
	uint16_t *dst = Render_viewPixel(render, x, y);
	const uint16_t *flat = NULL;

	if (perTile == 0)
		flat = tiles[0];
	while (--count >= 0) {
		if (perTile != 0)
			flat = tiles[(((uint32_t)u & 0x7ffffff) >> 22) + (((uint32_t)v >> 17) & 0x3e0)];
		*dst++ = flat[(((uint32_t)u & 0x3fffff) >> 16) + (((uint32_t)v >> 10) & 0xfc0)];
		u += du;
		v += dv;
	}
}

/* 0x00026510 Perspective-correct floor (row below the horizon) or ceiling
 * row: distance from the row's offset to the horizon and the plane height,
 * then the world step per pixel from the view matrix. */
void Render_drawFlatRow(Render *render, int x, int y, void *tex, int width, int perTile)
{
	int sinA = render->viewMatrix[3];
	int cosA = render->viewMatrix[4];
	int viewW = render->viewW;
	int y2 = y * 2;
	int rows, dz, dist, k, du, dv, center;

	if (y2 < render->viewH) { /* ceiling */
		rows = render->viewH - y2;
		dz = render->ceilZ - render->viewZ;
	} else { /* floor */
		rows = y2 - render->viewH;
		dz = render->viewZ - render->floorZ;
	}
	dist = rmul(rmul(viewW, dz), 8) / (rows + 1);
	k = rshl(dist, 19) / rshl(viewW, 5);
	du = rmul(k >> 8, sinA >> 8);
	dv = rmul(k >> 8, cosA >> 8);
	center = x * 2 - viewW + 1;

	Render_drawFlatSpan(render, (x * 2) >> 1, y2 >> 1, tex,
	                    (rmul(du, center) >> 1) + (rmul(cosA, dist) >> 3) + rshl(render->viewX, 16),
	                    (rmul(dv, center) >> 1) + (rmul(-sinA, dist) >> 3) + rshl(render->viewY, 16),
	                    du, dv, width, perTile);
}

/* 0x0000bf80 Fill the view: ceiling rows down to the horizon, then floor rows.
 * A flat flag (Render.ceilFlat/floorFlat) means one texture for the plane. */
void Render_drawFloorCeiling(Render *render)
{
	int y = render->clipY0;
	int x = render->clipX0;
	int width = render->clipX1 - x;
	int yEnd = render->clipY1 - y;
	uint16_t **ceil = &render->flatTile[0x400];
	uint16_t **floor = &render->flatTile[0];

	for (; y < render->halfH; y++)
		Render_drawFlatRow(render, x, y, ceil, width, render->ceilFlat == 0);
	for (; y < yEnd; y++)
		Render_drawFlatRow(render, x, y, floor, width, render->floorFlat == 0);
}

/* ======================================================================
 * Bitshapes (sprites as column run lists, Render.shapeData)
 *
 * Shape at shapeData[ofs]: [0],[1] texel offset (low, high half), [2]/[3]
 * first/last column, then per column the offset of its run list (relative to
 * ofs+2). A run is {start | len << 8, texel offset}; the list ends with a
 * word whose low byte is 0x7f.
 * ==================================================================== */

/* Width the first-person weapon is sized for: the view width, but at most
 * 1.5 view heights, so that on a wide (landscape) screen the weapon keeps the
 * size it has next to the view height on the phone instead of growing with the
 * width. Device pixels like the view. */
int Render_spriteRefW(Render *render)
{
	int cap = render->viewH * 3 / 2;
	return render->viewW > cap ? cap : render->viewW;
}

/* 0x00008be0 Draw a bitshape frame in 2D over the 3D view (the first-person
 * weapon): 0xb000/viewW shape units per pixel (1:1 on a 176-pixel view),
 * blend mode `blend`, colour effects `arg` (Render.segFlags). Follows the
 * screen shake and the drunk wobble. x, y are device pixels of the view. */
void Render_draw2DSprite(Render *render, int shapeIdx, int frame, int x, int y, int blend, int arg)
{
	const int16_t *shape = render->shapeData;
	int viewW = render->viewW;
	int colStep = 0xb000 / Render_spriteRefW(render); /* shape units per pixel, 8.8 */
	int pixPerUnit = 0x10000 / colStep; /* pixels per shape unit, 8.8 */
	int media = render->spriteBase[shapeIdx] + frame;
	int ofs = render->spriteMap[media].ofs;
	Canvas *canvas = render->canvas;
	int s = Brew_screenScale();
	int colAcc = 0, x0, x1, xs, xe, width, du;
	int64_t texBase;
	void *span;

	if (render->drunkLevel != 0) {
		x += render->drunkOfs[3] * s;
		y += render->drunkOfs[4] * s;
	}
	render->clipX0 = -canvas->shakeX * s;
	render->clipY0 = -canvas->shakeY * s;
	render->clipX1 = viewW - canvas->shakeX * s;
	render->clipY1 = render->viewH - canvas->shakeY * s;
	render->segFlags = arg;
	span = Render_setPalette(render, render->spriteMap[media].palette, blend);

	texBase = (int32_t)((uint32_t)(uint16_t)shape[ofs] | ((uint32_t)(uint16_t)shape[ofs + 1] << 16));
	x0 = (rmul(pixPerUnit, shape[ofs + 2]) >> 8) + x;
	x1 = (rmul(pixPerUnit, shape[ofs + 3]) >> 8) + x;
	width = x1 - x0;
	xs = x0;
	if (render->clipX0 > xs) {
		colAcc = -rmul(xs - render->clipX0, colStep);
		xs = render->clipX0;
	}
	xe = x1;
	if (render->clipX1 < xe)
		xe = render->clipX1;
	du = colStep << 4;

	for (; xs < xe; xs++) {
		int column = colAcc >> 8;
		int run;

		if (column >= width)
			break;
		colAcc += colStep;
		run = shape[ofs + 4 + column] + ofs + 2;
		for (;; run += 2) {
			int word = shape[run];
			int start = word & 0xff;
			int top, len;
			int64_t u;

			if (start == SHAPE_END)
				break;
			u = rlsl64(texBase + shape[run + 1], 12);
			len = rmul(pixPerUnit, word >> 8) >> 8;
			top = (rmul(pixPerUnit, start) >> 8) + y;
			if (render->clipY0 > top) {
				int d = top - render->clipY0;
				/* the original steps u back by colStep, not du, per clipped row */
				u -= rmul(d, colStep);
				len += d;
				top = render->clipY0;
			}
			if (top + len > render->clipY1)
				len = render->clipY1 - top;
			SPAN_CALL(span, render, xs, top, u, du, len);
		}
	}
}

/* 0x00029f40 Total run length of a sprite media's bitshape (all columns),
 * rounded up to even: the number of texels it needs. */
int Render_getShapeWidth(Render *r, int idx)
{
	const int16_t *shape = r->shapeData;
	int ofs = r->spriteMap[idx].ofs;
	int total = 0;
	int col;

	for (col = shape[ofs + 2]; col <= shape[ofs + 3]; col++) {
		int run = shape[ofs + 4 + (col - shape[ofs + 2])] + ofs;
		while ((shape[run + 2] & 0xff) != SHAPE_END) {
			total += shape[run + 2] >> 8;
			run += 2;
		}
	}
	return (int)((uint32_t)(total + 1) & ~1u);
}

/* ======================================================================
 * Per-pixel blend ops of Render_drawImage (RGB565 in the low 16 bits).
 * Saturating adds clear each channel's low bit, add, and smear the carry
 * bits (0x10820) back down into their channel.
 * ==================================================================== */

static int addSaturate(uint32_t dst, uint32_t addend)
{
	uint32_t sum = (dst & 0xfffff7deu) + addend;
	uint32_t carry = sum & 0x10820;
	return (int)(uint16_t)((sum & 0xfffff7deu) | (carry >> 1) | (carry >> 2) | (carry >> 3));
}

/* 0x00005b60 blit mode 10: dst + src/4 */
int Pixel_addQuarter(int dst, int src)
{
	return addSaturate((uint32_t)dst, ((uint32_t)src >> 2) & 0xfffff1c6u);
}

/* 0x00005b94 blit mode 9: dst + src/2 */
int Pixel_addHalf(int dst, int src)
{
	return addSaturate((uint32_t)dst, ((uint32_t)src >> 1) & 0xfffff3ceu);
}

/* 0x00005bc8 blit mode 8: dst + src*3/4 */
int Pixel_addThreeQuarter(int dst, int src)
{
	uint32_t s = (uint32_t)src;
	uint32_t sum = ((uint32_t)dst & 0xfffff7deu) + ((s >> 1) & 0xfffff3ceu) + ((s >> 2) & 0xfffff1c6u);
	uint32_t carry = sum & 0x10820;
	return (int)(uint16_t)((sum & 0xfffff7deu) | (carry >> 1) | (carry >> 2) | (carry >> 3));
}

/* 0x00005c0c blit mode 7: dst + src */
int Pixel_add(int dst, int src)
{
	return addSaturate((uint32_t)dst, (uint32_t)src & 0xfffff7deu);
}

/* 0x00005c38 blit mode 4: 3/4 dst + 1/4 src */
int Pixel_blend25(int dst, int src)
{
	uint32_t d = (uint32_t)dst, s = (uint32_t)src;
	return (int)(uint16_t)(((d >> 1) & ~0x410u) + ((d >> 2) & ~0x618u) + ((s >> 2) & ~0x618u));
}

/* 0x00005c58 blit mode 5: dst/2 + src/2 (not truncated to 16 bits) */
int Pixel_blend50(int dst, int src)
{
	uint32_t d = (uint32_t)dst, s = (uint32_t)src;
	return (int)(((d >> 1) & ~0x410u) + ((s >> 1) & ~0x410u));
}

/* 0x00005c68 blit mode 6: 1/4 dst + 3/4 src */
int Pixel_blend75(int dst, int src)
{
	uint32_t d = (uint32_t)dst, s = (uint32_t)src;
	return (int)(uint16_t)(((d >> 2) & ~0x618u) + ((s >> 1) & ~0x410u) + ((s >> 2) & ~0x618u));
}

/* 0x00005c8c blit mode 0xe: key[0] -> key[1] colour replacement, else src */
int Pixel_replaceColor(int dst, int src, short *key)
{
	if ((uint16_t)key[0] == (uint32_t)src)
		return (uint16_t)key[1];
	return src;
}

/* 0x00005c98 blit mode 0xf: per-channel multiply, rounded up */
int Pixel_multiply(int dst, int src)
{
	uint32_t d = (uint32_t)dst, s = (uint32_t)src;
	uint32_t red = (((s >> 11) * (d >> 11) + 0x1f) << 6) & 0xf800;
	uint32_t green = ((((d << 21) >> 26) * ((s << 21) >> 26) + 0x3f) >> 1) & 0x7e0;
	uint32_t blue = (((d << 27) >> 27) * ((s << 27) >> 27) + 0x1f) >> 5;
	return (int)(red | green | blue);
}

/* ======================================================================
 * 2D blits and screen effects
 * ==================================================================== */

typedef int (*PixelOp)(int dst, int src, void *arg);

/* The blend ops take (dst, src); Pixel_replaceColor also takes its key. */
static int op_addQuarter(int d, int s, void *a) { (void)a; return Pixel_addQuarter(d, s); }
static int op_addHalf(int d, int s, void *a) { (void)a; return Pixel_addHalf(d, s); }
static int op_addThreeQuarter(int d, int s, void *a) { (void)a; return Pixel_addThreeQuarter(d, s); }
static int op_add(int d, int s, void *a) { (void)a; return Pixel_add(d, s); }
static int op_blend25(int d, int s, void *a) { (void)a; return Pixel_blend25(d, s); }
static int op_blend50(int d, int s, void *a) { (void)a; return Pixel_blend50(d, s); }
static int op_blend75(int d, int s, void *a) { (void)a; return Pixel_blend75(d, s); }
static int op_replaceColor(int d, int s, void *a) { return Pixel_replaceColor(d, s, (short *)a); }
static int op_multiply(int d, int s, void *a) { (void)a; return Pixel_multiply(d, s); }

/* 0x000202d4 Blit (sx, sy, w, h) of an Image to (dx, dy) on the screen,
 * clipped to the canvas screen size. Blend modes 4..10, 0xe and 0xf go pixel
 * by pixel (magenta 0xf81f skipped on transparent images); 0xff draws
 * nothing; anything else is an IBITMAP_BltIn (transparent or copy). `arg`
 * is the colour key pair for mode 0xe (default Render.blendColors).
 * Note: clipping at the top does not advance sy (as in the original). */
void Render_drawImage(Render *render, void *imgp, int dx, int dy, int sx, int sy, int w, int h, int blend, intptr_t arg)
{
	Image *img = imgp;
	Canvas *canvas = render->canvas;
	int screenW = canvas->screenW;
	PixelOp op;
	void *opArg = NULL;
	const uint16_t *srcPx;
	int srcStride, dstStride, i;

	if (dx < 0) {
		w += dx;
		sx -= dx;
		dx = 0;
	} else if (dx > screenW) {
		return;
	}
	if (dx + w > screenW)
		w = screenW - dx;
	if (dy < 0) {
		h += dy;
		dy = 0;
	} else if (dy > canvas->screenH) {
		return;
	}
	if (dy + h > canvas->screenH)
		h = canvas->screenH - dy;

	switch (blend) {
	case 4: op = op_blend25; break;
	case 5: op = op_blend50; break;
	case 6: op = op_blend75; break;
	case 7: op = op_add; break;
	case 8: op = op_addThreeQuarter; break;
	case 9: op = op_addHalf; break;
	case 10: op = op_addQuarter; break;
	case 0xe:
		op = op_replaceColor;
		/* arg is a pointer to a {from, to} colour pair in the original */
		opArg = arg != 0 ? (void *)arg : (void *)render->blendColors;
		break;
	case 0xf: op = op_multiply; break;
	case 0xff:
		return;
	default: {
		IBitmap *dst = render->app->deviceBitmap ? render->app->deviceBitmap : Brew_screen();
		if (img->bitmap != NULL)
			IBITMAP_BltIn(dst, dx, dy, w, h, img->bitmap, sx, sy,
			              img->transparent ? AEE_RO_TRANSPARENT : AEE_RO_COPY);
		return;
	}
	}

	if (img->bitmap != NULL && img->bitmap->scale != 1 && Brew_screen() != NULL) {
		/* port: a mod's HD picture (Brew_hdBitmap), blended at the screen's resolution */
		Brew_blendHd(Brew_screen(), dx, dy, w, h, img->bitmap, sx, sy, img->transparent != 0, op, opArg);
		return;
	}
	/* the original reads the source through its IDIB (Image.dib) */
	if (img->dib != NULL && img->dib->pBmp != NULL) {
		srcPx = (const uint16_t *)img->dib->pBmp;
		srcStride = img->dib->nPitch >> 1;
	} else if (img->bitmap != NULL) {
		srcPx = img->bitmap->px;
		srcStride = img->bitmap->pitch;
	} else {
		return;
	}
	dstStride = Render_pitch(render);
	if (Brew_screenScale() > 1) {
		/* HD screen: each source pixel blends into a scale x scale block */
		int s = Brew_screenScale(), j, r, k;
		for (j = 0; j < h; j++) {
			const uint16_t *src = srcPx + rmul(srcStride, sy + j) + sx;
			for (r = 0; r < s; r++) {
				uint16_t *dst = (uint16_t *)render->dib->pBmp + (ptrdiff_t)dstStride * ((dy + j) * s + r) + dx * s;
				for (i = 0; i < w; i++, dst += s) {
					if (img->transparent != 0 && src[i] == 0xf81f)
						continue;
					for (k = 0; k < s; k++)
						dst[k] = (uint16_t)op(dst[k], src[i], opArg);
				}
			}
		}
		return;
	}
	for (i = 0; i < w; i++) {
		const uint16_t *src = srcPx + rmul(srcStride, sy) + sx + i;
		uint16_t *dst = (uint16_t *)render->dib->pBmp + i + dx + rmul(dstStride, dy);
		int j;

		for (j = 0; j < h; j++) {
			if (img->transparent == 0 || *src != 0xf81f)
				*dst = (uint16_t)op(*dst, *src, opArg);
			dst += dstStride;
			src += srcStride;
		}
	}
}

/* 0x00010a48 Fade the 3D view toward black: every channel is clamped to
 * level (0..255) scaled to its width. */
void Render_fadeView(Render *render, int level)
{
	uint8_t *px = render->dib->pBmp;
	int pitch = render->dib->nPitch;
	int base = pitch * render->viewportY + render->viewportX * 2;
	uint32_t maxRB = (uint32_t)level >> 3;
	uint32_t maxG = (uint32_t)level >> 2;
	int x, y;

	for (y = 0; y < render->viewH; y++)
		for (x = 0; x < render->viewW; x++) {
			uint8_t *p = px + y * pitch + base + x * 2;
			uint32_t c = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
			uint32_t red = c >> 11, green = (c & 0x7ff) >> 5, blue = c & 0x1f;

			if (red > maxRB) red = maxRB;
			if (green > maxG) green = maxG;
			if (blue > maxRB) blue = maxRB;
			c = (red << 11) | (green << 5) | blue;
			p[0] = (uint8_t)c;
			p[1] = (uint8_t)(c >> 8);
		}
	Hw_fadeScene(level);   /* port: the GPU's view darkens the same way */
}

/* 0x00008b90 RGB -> native pixel: 565, or 555 when the display is 15-bit. */
int Render_rgbToPixel(Render *render, int b, int g, int r)
{
	int gBits = (render->dib->nColorScheme == 0xf) ? 5 : 6;
	int hi = (int16_t)((((uint32_t)g >> (8 - gBits)) << 5) | (((uint32_t)r >> 3) << (gBits + 5)));
	return hi | (int)((uint32_t)b >> 3);
}

/* 0x00025878 Colour pair used by blit mode 0xe (0xRRGGBB each). */
void Render_setBlendColors(Render *render, int a, int b)
{
	render->blendColors[0] = (int16_t)Color_rgb888to565(render, a);
	render->blendColors[1] = (int16_t)Color_rgb888to565(render, b);
}
