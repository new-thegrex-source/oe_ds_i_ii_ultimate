/*
 * Render: pixel-level code — span drawers (the original's 13 blend-mode
 * objects), palette effects, floor/ceiling rows, 2D bitshape sprites,
 * Render_drawImage blend modes and whole-screen effects (fade, tint).
 *
 * Everything writes straight into the RGB565 screen through Render.fbDib,
 * an IDIB describing Brew_screen() (see render_local.h).
 */

#include "render_local.h"
#include "oe_hw.h"

/* Keep Render.fbDib describing the screen bitmap. The original obtained it
 * once with IBITMAP_QueryInterface(AEECLSID_DIB) on the device bitmap. In the
 * HD mode it describes the device pixels: the renderer works at
 * Brew_screenScale() times the game's resolution (see Render_setScreenRect). */
void Render_syncScreenDib(Render *r)
{
	IBitmap *screen = Brew_screen();
	if (r->fbDib == NULL)
		r->fbDib = calloc(1, sizeof(IDIB));
	if (r->fbDib == NULL || screen == NULL)
		return;
	r->fbDib->pBmp = (uint8_t *)screen->px;
	r->fbDib->cx = (uint16_t)(screen->width * screen->scale);
	r->fbDib->cy = (uint16_t)(screen->height * screen->scale);
	r->fbDib->nPitch = (int16_t)(screen->pitch * 2);
	r->fbDib->nDepth = 16;
	r->fbDib->nColorScheme = 16; /* IDIB_COLORSCHEME_565 */
}

/* ======================================================================
 * Span drawers. One call draws n pixels of a vertical column starting at
 * (x, y) of the 3D view (Render.framebuffer). The texel for each pixel is
 * curTexels[u >> 12] (u is 20.12), looked up in curPalette (RGB565).
 * The blend palettes are pre-scaled by Render_setPalette.
 * ==================================================================== */

/* First destination pixel and the row stride in pixels. */
static inline uint16_t *span_dest(Render *r, int x, int y, int *stride)
{
	*stride = r->fbDib->nPitch >> 1;
	return r->framebuffer + rmul(*stride, y) + x;
}

/* 0x000403c4 */
void Span_drawAdditive(void *drawer, int x, int y, int u, int du, int n)
{
	Render *r = ((SpanDrawer *)drawer)->render;
	const uint8_t *tex = r->curTexels;
	const uint16_t *pal = r->curPalette;
	int stride;
	uint16_t *dst = span_dest(r, x, y, &stride);

	while (--n >= 0) {
		/* drop each channel's low bit, add, then smear the carries back
		 * down into their channel to saturate */
		uint32_t sum = (uint32_t)(*dst & ~0x821u) + pal[tex[u >> 12]];
		uint32_t carry = sum & 0x10820;
		u += du;
		sum = (sum & ~0x821u) | (carry >> 1) | (carry >> 2) | (carry >> 3);
		*dst = (uint16_t)sum;
		dst += stride;
	}
}

/* 0x00040464 */
void Span_drawMultiply(void *drawer, int x, int y, int u, int du, int n)
{
	Render *r = ((SpanDrawer *)drawer)->render;
	const uint8_t *tex = r->curTexels;
	const uint16_t *pal = r->curPalette;
	int stride;
	uint16_t *dst = span_dest(r, x, y, &stride);

	while (--n >= 0) {
		uint32_t d = *dst;
		uint32_t s = pal[tex[u >> 12]];
		uint32_t red = ((s >> 11) * (d >> 11) << 6) & 0xf800;
		uint32_t green = (((s >> 5) & 0x3f) * ((d >> 5) & 0x3f) >> 1) & 0x7e0;
		uint32_t blue = ((s & 0x1f) * (d & 0x1f)) >> 5;
		u += du;
		*dst = (uint16_t)(red | green | blue);
		dst += stride;
	}
}

/* 0x00040524 */
void Span_drawNone(void *drawer, int x, int y, int u, int du, int n)
{
	(void)drawer; (void)x; (void)y; (void)u; (void)du; (void)n;
}

/* 0x00040528 (unrolled 8x in the original) */
void Span_drawNormal(void *drawer, int x, int y, int u, int du, int n)
{
	Render *r = ((SpanDrawer *)drawer)->render;
	const uint8_t *tex = r->curTexels;
	const uint16_t *pal = r->curPalette;
	int stride;
	uint16_t *dst = span_dest(r, x, y, &stride);

	while (--n >= 0) {
		*dst = pal[tex[u >> 12]];
		u += du;
		dst += stride;
	}
}

/* 0x00040668 desaturate (and darken) what is already on screen; the texture
 * is not read and u is not advanced */
void Span_drawGray(void *drawer, int x, int y, int u, int du, int n)
{
	Render *r = ((SpanDrawer *)drawer)->render;
	int stride;
	uint16_t *dst = span_dest(r, x, y, &stride);

	while (--n >= 0) {
		uint32_t c = *dst;
		int gray = (int)(((c >> 5) & 0x3f) + ((c >> 11) << 1) + ((c & 0x1f) << 1)) / 6;
		gray = (int8_t)gray;
		*dst = (uint16_t)(((gray & ~1) << 10) | (gray << 5) | (gray >> 1));
		dst += stride;
	}
}

/* 0x000406f0 dst*3/4 + palette (palette pre-scaled to 1/4) */
void Span_drawBlend75(void *drawer, int x, int y, int u, int du, int n)
{
	Render *r = ((SpanDrawer *)drawer)->render;
	const uint8_t *tex = r->curTexels;
	const uint16_t *pal = r->curPalette;
	int stride;
	uint16_t *dst = span_dest(r, x, y, &stride);

	while (--n >= 0) {
		uint32_t d = *dst;
		uint32_t v = ((d & ~0x820u) >> 1) + ((d & ~0x1860u) >> 2);
		v += pal[tex[u >> 12]];
		u += du;
		*dst = (uint16_t)v;
		dst += stride;
	}
}

/* 0x0004077c dst/2 + palette (palette pre-scaled to 1/2) */
void Span_drawBlend50(void *drawer, int x, int y, int u, int du, int n)
{
	Render *r = ((SpanDrawer *)drawer)->render;
	const uint8_t *tex = r->curTexels;
	const uint16_t *pal = r->curPalette;
	int stride;
	uint16_t *dst = span_dest(r, x, y, &stride);

	while (--n >= 0) {
		uint32_t v = pal[tex[u >> 12]] + ((*dst & ~0x820u) >> 1);
		u += du;
		*dst = (uint16_t)v;
		dst += stride;
	}
}

/* 0x000407f8 dst/4 + palette (palette pre-scaled to 3/4) */
void Span_drawBlend25(void *drawer, int x, int y, int u, int du, int n)
{
	Render *r = ((SpanDrawer *)drawer)->render;
	const uint8_t *tex = r->curTexels;
	const uint16_t *pal = r->curPalette;
	int stride;
	uint16_t *dst = span_dest(r, x, y, &stride);

	while (--n >= 0) {
		uint32_t v = pal[tex[u >> 12]] + ((*dst & ~0x1860u) >> 2);
		u += du;
		*dst = (uint16_t)v;
		dst += stride;
	}
}

/* ======================================================================
 * Palette selection
 * ==================================================================== */

/* The 13 drawer slots Render.spanNormal..spanNone, indexed like the
 * original (Render + 4 + mode*4). Unused slots are NULL. */
static void *span_slot(Render *r, int mode)
{
	switch (mode) {
	case 0: return r->spanNormal;
	case 1: return r->spanBlend75;
	case 2: return r->spanBlend50;
	case 3: return r->spanBlend25;
	case 4: return r->spanAdditive;
	case 5: return r->span18;
	case 6: return r->span1c;
	case 7: return r->span20;
	case 8: return r->spanGray;
	case 9: return r->span28;
	case 10: return r->span2c;
	case 11: return r->spanMultiply;
	case 12: return r->spanNone;
	default: return NULL;
	}
}

/* 0x0003a818 Select palette `pal` (index into Render.palettes, already <<8),
 * apply Render.effectFlags colour effects and the blend mode `shade`
 * (pre-scaling the palette), and return the span drawer to use. */
void *Render_setPalette(Render *render, int pal, int shade)
{
	uint16_t *tmp = render->tmpPalette;
	void *drawer;
	int i;

	if (shade == 12)
		return render->spanNone;

	render->curPalette = render->palettes + pal;
	if (Hw_fastPass())   /* port: the GPU draws the view's pixels */
		return render->spanNone;

	if (render->effectFlags & 4) { /* grayscale */
		for (i = 0; i < 256; i++) {
			uint32_t c = render->curPalette[i];
			uint32_t g = ((c >> 11) + (((c >> 5) & 0x3f) >> 1) + (c & 0x1f)) / 3;
			g &= 0xff;
			tmp[i] = (uint16_t)((g << 11) | (((g << 1) & 0xff) << 5) | g);
		}
		render->curPalette = tmp;
	}
	if (render->effectFlags & 8) { /* brighten */
		for (i = 0; i < 256; i++) {
			uint32_t c = render->curPalette[i];
			uint32_t red = (c >> 11) + 8;
			uint32_t green = ((((c >> 5) & 0x3f) + 0x10) & 0xff);
			uint32_t blue = ((c & 0x1f) + 8) & 0xff;
			if (red > 0x1f) red = 0x1f;
			if (green > 0x3f) green = 0x3f;
			if (blue > 0x1f) blue = 0x1f;
			tmp[i] = (uint16_t)((red << 11) | (green << 5) | blue);
		}
		render->curPalette = tmp;
	}
	if (render->effectFlags & 0x10) { /* blue tint */
		for (i = 0; i < 256; i++) {
			uint32_t c = render->curPalette[i];
			uint32_t blue = (c & 0x1f) + 8;
			if (blue > 0x1f) blue = 0x1f;
			tmp[i] = (uint16_t)(((c >> 11) << 11) | (((c >> 5) & 0x3f) << 5) | blue);
		}
		render->curPalette = tmp;
	}
	if (render->effectFlags & 0x20) { /* green tint */
		for (i = 0; i < 256; i++) {
			uint32_t c = render->curPalette[i];
			uint32_t green = ((c >> 5) & 0x3f) + 0x10;
			if (green > 0x3f) green = 0x3f;
			tmp[i] = (uint16_t)(((c >> 11) << 11) | (green << 5) | (c & 0x1f));
		}
		render->curPalette = tmp;
	}
	if (render->effectFlags & 0x40) { /* red tint */
		for (i = 0; i < 256; i++) {
			uint32_t c = render->curPalette[i];
			int red = (int)(c >> 11) + 8;
			int green = (int)((c >> 5) & 0x3f) - 2;
			int blue = (int)(c & 0x1f) - 1;
			if (red > 0x1f) red = 0x1f;
			if (green < 0) green = 0;
			if (blue < 0) blue = 0;
			tmp[i] = (uint16_t)((red << 11) | (green << 5) | (blue & 0xff));
		}
		render->curPalette = tmp;
	}

	drawer = span_slot(render, shade);
	if (shade >= 1 && shade <= 7) {
		const uint16_t *src = render->curPalette;
		switch (shade) {
		case 1: /* 75% screen + 25% texture */
			drawer = render->spanBlend75;
			for (i = 0; i < 256; i++)
				tmp[i] = (uint16_t)((src[i] & ~0x1860u) >> 2);
			break;
		case 2: /* 50/50 */
			drawer = render->spanBlend50;
			for (i = 0; i < 256; i++)
				tmp[i] = (uint16_t)((src[i] & ~0x820u) >> 1);
			break;
		case 3: /* 25% screen + 75% texture */
			drawer = render->spanBlend25;
			for (i = 0; i < 256; i++)
				tmp[i] = (uint16_t)(((src[i] & ~0x820u) >> 1) + ((src[i] & ~0x1860u) >> 2));
			break;
		case 4: /* additive */
			drawer = render->spanAdditive;
			for (i = 0; i < 256; i++)
				tmp[i] = (uint16_t)(src[i] & 0xf7de);
			break;
		case 5: /* additive, 3/4 strength */
			drawer = render->spanAdditive;
			for (i = 0; i < 256; i++)
				tmp[i] = (uint16_t)(((src[i] & ~0x1862u) >> 1) + ((src[i] & ~0x38e4u) >> 2));
			break;
		case 6: /* additive, 1/2 strength */
			drawer = render->spanAdditive;
			for (i = 0; i < 256; i++)
				tmp[i] = (uint16_t)((src[i] & ~0x1862u) >> 1);
			break;
		case 7: /* additive, 1/4 strength */
			drawer = render->spanAdditive;
			for (i = 0; i < 256; i++)
				tmp[i] = (uint16_t)((src[i] & ~0x38e4u) >> 2);
			break;
		}
		render->curPalette = tmp;
	}

	if (render->dbgForceDrawer != 0)
		return render->spanNone;
	/* Modes 5..7 and 9/10 have no drawer object in the original (a call
	 * through them would crash the phone); draw nothing instead. */
	if (drawer == NULL)
		drawer = render->spanNone;
	return drawer;
}

/* ======================================================================
 * Floor and ceiling
 * ==================================================================== */

/* 0x0001f00c One horizontal row of floor/ceiling. tex is the 32x32 tile
 * map of 64x64 RGB565 flats (uint16_t *[1024]); u/v are world 16.16.
 * perTile=0 means the whole plane uses one flat (tex[0]). */
void Render_drawFlatSpan(Render *render, int x, int y, void *tex, int u, int v, int du, int dv, int count, int perTile)
{
	uint16_t *const *tiles = (uint16_t *const *)tex;
	uint16_t *dst = render->framebuffer + rmul(render->fbDib->nPitch >> 1, y) + x;
	uint32_t uu = (uint32_t)u, vv = (uint32_t)v;

	if (perTile == 0) {
		const uint16_t *flat = tiles[0];
		while (--count >= 0) {
			*dst++ = flat[((vv >> 10) & 0xfc0) + ((uu & 0x3fffff) >> 16)];
			uu += (uint32_t)du;
			vv += (uint32_t)dv;
		}
	} else {
		while (--count >= 0) {
			const uint16_t *flat = tiles[((vv >> 17) & 0x3e0) + ((uu & 0x7ffffff) >> 22)];
			*dst++ = flat[((vv >> 10) & 0xfc0) + ((uu & 0x3fffff) >> 16)];
			uu += (uint32_t)du;
			vv += (uint32_t)dv;
		}
	}
}

/* 0x0003b79c Perspective-correct floor (y >= height/2) or ceiling row. */
void Render_drawFlatRow(Render *render, int x, int y, void *tex64, int width, int mode)
{
	int vx16 = rshl(render->viewX, 16);
	int vy16 = rshl(render->viewY, 16);
	int y2 = y * 2;
	int x2 = x * 2;
	int viewW = render->width;
	int sinA = render->viewPlane[3];
	int cosA = render->viewPlane[4];
	int dz, rows, dist, k, du, dv, center;

	if (render->height <= y2) { /* floor */
		dz = render->viewZ - render->effFloor;
		rows = y2 - render->height;
	} else { /* ceiling */
		dz = render->effCeil - render->viewZ;
		rows = render->height - y2;
	}
	/* distance to the row on the plane, then per-pixel world step */
	dist = rmul(viewW, rshl(dz, 3)) / (rows + 1);
	k = rshl(dist, 19) / rshl(viewW, 5);
	dv = rmul(k >> 8, cosA >> 8);
	du = rmul(sinA >> 8, k >> 8);
	center = x2 - viewW + 1;

	Render_drawFlatSpan(render, x2 >> 1, y2 >> 1, tex64,
	                    vx16 + (rmul(cosA, dist) >> 3) + (rmul(du, center) >> 1),
	                    vy16 + (rmul(-sinA, dist) >> 3) + (rmul(dv, center) >> 1),
	                    du, dv, width, mode);
}

/* 0x000119c4 Fill the view with ceiling rows (above centerY) then floor rows. */
void Render_drawFloorCeiling(Render *render)
{
	int x = render->clipX1;
	int y = render->clipY1;
	int width = render->clipX2 - x;
	int yEnd = render->clipY2 - y;

	for (; y < render->centerY; y++)
		Render_drawFlatRow(render, x, y, render->ceilTiles, width, render->ceilUniform == 0);
	for (; y < yEnd; y++)
		Render_drawFlatRow(render, x, y, render->floorTiles, width, render->floorUniform == 0);
}

/* ======================================================================
 * Bitshapes (sprites as column run lists)
 *
 * Shape at shapeSpans[ofs]: [0],[1] texel offset (bits 30-31 of [1] = page),
 * [2]/[3] first/last column, then one entry per column giving the offset of
 * its run list (relative to ofs+2). A run is {start | len<<8, texel offset},
 * the list ends with a word whose low byte is 0x7f.
 * ==================================================================== */

/* Width the 2D bitshapes (the first-person weapon) are sized for: the view
 * width, but at most 1.5 view heights, so that on a wide (landscape) screen
 * they keep the size they have next to the view height on the phone instead of
 * growing with the width. Device pixels like the view. */
int Render_spriteRefW(Render *render)
{
	int cap = render->height * 3 / 2;
	return render->width > cap ? cap : render->width;
}

/* 0x0000c828 Draw a bitshape frame in 2D (weapons, HUD icons). scale is
 * 8.8 (0x100 = 1:1 on a 176-pixel-wide view); blend = Render.effectFlags.
 * x, y are device pixels of the view. */
void Render_draw2DSprite(Render *render, int sprite, int frame, int x, int y, int a, int blend, int scale, int flip)
{
	int width = render->width;
	int colStep = 0xb000 / Render_spriteRefW(render);     /* 16.16 shape columns per pixel, *scale */
	int vStep = rdiv64((int64_t)rshl(colStep, 8), scale);  /* 20.12 texels per pixel >> 4 */
	int k = 0x10000 / colStep;                             /* pixels per column, 16.16 */
	int idx = render->spriteRemap[sprite] + frame;
	int ofs = render->spriteMap[idx].ofs;
	const int16_t *shape = (const int16_t *)render->shapeSpans;
	void *drawer;
	int texBase, col0, col1, numCols, xs, xe, colAcc = 0;

	if (render->drunkLevel != 0) {
		x += render->oscOut[3] * Brew_screenScale();
		y += render->oscOut[4] * Brew_screenScale();
	}
	render->clipX1 = 0;
	render->clipY1 = 0;
	render->clipX2 = width;
	render->clipY2 = render->height;
	render->effectFlags = (uint32_t)blend;
	drawer = Render_setPalette(render, render->spriteMap[idx].palette, a);

	render->curTexels = render->texelPages + ((render->shapeSpans[ofs + 1] >> 14) & 3) * TEXEL_PAGE_SIZE;
	texBase = (int)(render->shapeSpans[ofs] | ((uint32_t)(render->shapeSpans[ofs + 1] & ~0xc000u) << 16));
	col0 = shape[ofs + 2];
	col1 = shape[ofs + 3];
	xs = x + (rmul(rmul(k, col0), scale) >> 16);
	xe = x + (rmul(rmul(k, col1), scale) >> 16);
	numCols = col1 - col0;

	if (render->clipX1 > xs) {
		colAcc = -rmul(xs - render->clipX1, colStep);
		xs = render->clipX1;
	}
	if (render->clipX2 < xe)
		xe = render->clipX2;
	if (flip) { /* mirrored: walk the columns right to left */
		int t = xe;
		xe = xs;
		xs = t;
	}

	while (xs != xe) {
		int col = colAcc / scale;
		int run;
		if (col >= numCols)
			break;
		colAcc += colStep;
		run = shape[ofs + 4 + col] + ofs + 2;
		for (;; run += 2) {
			int word = shape[run];
			int start = word & 0xff;
			int len, sy, u;
			if (start == SHAPE_END)
				break;
			len = rmul(rmul(k, word >> 8), scale) >> 16;
			u = rshl(shape[run + 1] + texBase, 12);
			sy = y + (rmul(rmul(k, start), scale) >> 16);
			if (render->clipY1 > sy) {
				int d = sy - render->clipY1;
				len += d;
				u -= rmul(vStep, d);
				sy = render->clipY1;
			}
			if (sy + len > render->clipY2)
				len = render->clipY2 - sy;
			SPAN_DRAW(drawer, xs, sy, u, rshl(vStep, 4), len);
		}
		if (flip)
			xs--;
		else
			xs++;
	}
}

/* 0x00041f0c Total texel count of a bitshape (sum of its run lengths), even. */
int Render_getShapeWidth(Render *r, int idx)
{
	int ofs = r->spriteMap[idx].ofs;
	const int16_t *shape = (const int16_t *)r->shapeSpans;
	int total = 0;
	int col;

	for (col = shape[ofs + 2]; col <= shape[ofs + 3]; col++) {
		int run = shape[ofs + 4 + (col - shape[ofs + 2])] + ofs;
		while ((shape[run + 2] & 0xff) != SHAPE_END) {
			total += shape[run + 2] >> 8;
			run += 2;
		}
	}
	return (total + 1) & ~1;
}

/* ======================================================================
 * Render_drawImage blend functions (dst, src -> new dst), RGB565.
 * In the original these are free functions at 0x7f80..0x80e0 picked
 * through a table; Color565_mul (0x80f4) is mode 11.
 * ==================================================================== */

/* saturating add of an already-scaled source: clear low bits, add, smear carries */
static int blend_saturate(uint32_t sum)
{
	uint32_t carry = sum & 0x10820;
	return (int)((((sum & ~0x821u) | (carry >> 1) | (carry >> 2) | (carry >> 3))) & ~0x10000u);
}
static int blend_add25(int dst, int src, int arg) /* 0x00007f80 mode 7 */
{
	(void)arg;
	return blend_saturate((uint32_t)(dst & ~0x821) + ((uint32_t)(src & ~0x38e4) >> 2));
}
static int blend_add50(int dst, int src, int arg) /* 0x00007fbc mode 6 */
{
	(void)arg;
	return blend_saturate((uint32_t)(dst & ~0x821) + ((uint32_t)(src & ~0x1862) >> 1));
}
static int blend_add75(int dst, int src, int arg) /* 0x00007ff8 mode 5 */
{
	uint32_t sum = (uint32_t)(dst & ~0x821) + ((uint32_t)(src & ~0x1862) >> 1) + ((uint32_t)(src & ~0x38e4) >> 2);
	uint32_t carry = sum & 0x10820;
	(void)arg;
	return (int)(((sum & ~0x821u) | (carry >> 1) | (carry >> 2) | (carry >> 3)) & ~0x30000u);
}
static int blend_add(int dst, int src, int arg) /* 0x00008040 mode 4 */
{
	(void)arg;
	return blend_saturate((uint32_t)(dst & ~0x821) + (uint32_t)(src & ~0x821));
}
static int blend_mix25(int dst, int src, int arg) /* 0x0000807c mode 1: 3/4 dst + 1/4 src */
{
	(void)arg;
	return (int)((((uint32_t)(dst & ~0x820) >> 1) + ((uint32_t)(dst & ~0x1860) >> 2) +
	              ((uint32_t)(src & ~0x1860) >> 2)) & ~0x10000u);
}
static int blend_mix50(int dst, int src, int arg) /* 0x000080a4 mode 2 */
{
	(void)arg;
	return (int)(((uint32_t)(dst & ~0x820) >> 1) + ((uint32_t)(src & ~0x820) >> 1));
}
static int blend_mix75(int dst, int src, int arg) /* 0x000080b8 mode 3: 1/4 dst + 3/4 src */
{
	(void)arg;
	return (int)((((uint32_t)(dst & ~0x1860) >> 2) + ((uint32_t)(src & ~0x820) >> 1) +
	              ((uint32_t)(src & ~0x1860) >> 2)) & ~0x10000u);
}
/* 0x000080e0 mode 10: replace colour pair[0] by pair[1] (Render.blendArg/blendArg2) */
static uint16_t g_replacePair[2];
static int blend_replace(int dst, int src, int arg)
{
	(void)dst; (void)arg;
	return (g_replacePair[0] == (uint16_t)src) ? g_replacePair[1] : src;
}
static int blend_mul(int dst, int src, int arg) /* 0x000080f4 mode 11 */
{
	(void)arg;
	return Color565_mul(dst, src);
}

/* 0x00031d4c Draw part of an image at screen (dx, dy): blend 0/8/9 = plain
 * blit (colour-keyed if the image is transparent), 1..7/10/11 per-pixel
 * blend into the screen, 12 = nothing. */
void Render_drawImage(Render *render, Image *img, int dx, int dy, int sx, int sy, int w, int h, int blend, int arg)
{
	Canvas *canvas = g_app->canvas;
	int (*fn)(int dst, int src, int arg);
	const uint8_t *srcBits;
	int srcPitch, dstPitch, i, j;

	if (dx < 0) {
		w += dx;
		sx -= dx;
		dx = 0;
	} else if (canvas->screenRect[2] < dx) {
		return;
	}
	if (dx + w > canvas->screenRect[2])
		w = canvas->screenRect[2] - dx;
	if (dy < 0) { /* note: the original does not move sy here */
		h += dy;
		dy = 0;
	} else if (canvas->screenRect[3] < dy) {
		return;
	}
	if (dy + h > canvas->screenRect[3])
		h = canvas->screenRect[3] - dy;

	switch (blend) {
	case 1: fn = blend_mix25; break;
	case 2: fn = blend_mix50; break;
	case 3: fn = blend_mix75; break;
	case 4: fn = blend_add; break;
	case 5: fn = blend_add75; break;
	case 6: fn = blend_add50; break;
	case 7: fn = blend_add25; break;
	case 10:
		/* the original passes a pointer to a colour pair; callers pass 0,
		 * which selects Render.blendArg/blendArg2 */
		if (arg != 0)
			OE2_UNIMPLEMENTED("Render_drawImage mode 10 with an explicit colour pair");
		g_replacePair[0] = (uint16_t)render->blendArg;
		g_replacePair[1] = (uint16_t)render->blendArg2;
		fn = blend_replace;
		break;
	case 11: fn = blend_mul; break;
	case 12: return;
	default: { /* 0, 8, 9 and anything else: IBITMAP_BltIn */
		IBitmap *dst = g_app->deviceBitmap ? g_app->deviceBitmap : Brew_screen();
		if (img->bitmap == NULL)
			return; /* the original fell back to the image's IDIB */
		IBITMAP_BltIn(dst, dx, dy, w, h, img->bitmap, sx, sy,
		              img->transparent == 0 ? AEE_RO_COPY : AEE_RO_TRANSPARENT);
		return;
	}
	}

	if (img->bitmap != NULL && img->bitmap->scale != 1 && Brew_screen() != NULL) {
		/* port: a mod's HD picture (Brew_hdBitmap), blended at the screen's resolution */
		IBitmap *scr = Brew_screen();
		int s = scr->scale, r, k;
		for (j = 0; j < h; j++) {
			for (r = 0; r < s; r++) {
				uint16_t *d = scr->px + ((dy + j) * s + r) * scr->pitch + dx * s;
				for (i = 0; i < w; i++) {
					for (k = 0; k < s; k++, d++) {
						uint16_t v = Brew_hdPixel(img->bitmap, sx + i, sy + j, k, r, s);
						if (img->transparent == 0 || v != 0xf81f)
							*d = (uint16_t)fn(*d, v, arg);
					}
				}
			}
		}
		return;
	}
	/* the original reads the source through the image's IDIB */
	if (img->dib != NULL) {
		srcBits = img->dib->pBmp;
		srcPitch = img->dib->nPitch >> 1;
	} else if (img->bitmap != NULL) {
		srcBits = (const uint8_t *)img->bitmap->px;
		srcPitch = img->bitmap->pitch;
	} else {
		return;
	}
	dstPitch = render->fbDib->nPitch >> 1;

	if (Brew_screenScale() > 1) {
		/* HD screen: each source pixel blends into a scale x scale block */
		int s = Brew_screenScale(), r, k;
		for (j = 0; j < h; j++) {
			const uint16_t *src = (const uint16_t *)srcBits + rmul(srcPitch, sy + j) + sx;
			for (r = 0; r < s; r++) {
				uint16_t *d = (uint16_t *)render->fbDib->pBmp + (ptrdiff_t)dstPitch * ((dy + j) * s + r) + dx * s;
				for (i = 0; i < w; i++, d += s) {
					if (img->transparent != 0 && src[i] == 0xf81f)
						continue;
					for (k = 0; k < s; k++)
						d[k] = (uint16_t)fn(d[k], src[i], arg);
				}
			}
		}
		return;
	}

	for (i = 0; i < w; i++) {
		const uint16_t *s = (const uint16_t *)srcBits + rmul(srcPitch, sy) + sx + i;
		uint16_t *d = (uint16_t *)render->fbDib->pBmp + rmul(dstPitch, dy) + dx + i;
		for (j = 0; j < h; j++) {
			if (img->transparent == 0 || *s != 0xf81f)
				*d = (uint16_t)fn(*d, *s, arg);
			d += dstPitch;
			s += srcPitch;
		}
	}
}

/* ======================================================================
 * Whole-view effects
 * ==================================================================== */

static inline uint32_t fb_read(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static inline void fb_write(uint8_t *p, uint32_t c)
{
	p[0] = (uint8_t)c;
	p[1] = (uint8_t)(c >> 8);
}

/* 0x00018ca4 Darken the 3D view: clamp every channel to `level` (0..255). */
void Render_fadeFramebuffer(Render *render, int level)
{
	IDIB *dib = render->fbDib;
	int base = rmul(dib->nPitch, render->fbOffsetY) + render->fbOffsetX * 2;
	uint32_t max5 = (uint32_t)level >> 3;
	uint32_t max6 = (uint32_t)level >> 2;
	int x, y;

	for (y = 0; y < render->height; y++) {
		for (x = 0; x < render->width; x++) {
			uint8_t *p = dib->pBmp + rmul(y, dib->nPitch) + x * 2 + base;
			uint32_t c = fb_read(p);
			uint32_t red = c >> 11, green = (c >> 5) & 0x3f, blue = c & 0x1f;
			if (red > max5) red = max5;
			if (green > max6) green = max6;
			if (blue > max5) blue = max5;
			fb_write(p, (red << 11) | (green << 5) | blue);
		}
	}
	Hw_fadeScene(level);   /* port: the GPU's view darkens the same way */
}

/* 0x00030f08 Purple tint over the 3D view (red +10, blue +14, saturating). */
void Render_tintScreen(Render *render)
{
	IDIB *dib = render->fbDib;
	int base = rmul(dib->nPitch, render->fbOffsetY) + render->fbOffsetX * 2;
	int x, y;

	for (y = 0; y < render->height; y++) {
		for (x = 0; x < render->width; x++) {
			uint8_t *p = dib->pBmp + rmul(y, dib->nPitch) + x * 2 + base;
			uint32_t c = fb_read(p);
			uint32_t red = (c >> 11) + 10;
			uint32_t blue = (c & 0x1f) + 14;
			if (red > 0x1f) red = 0x1f;
			if (blue > 0x1f) blue = 0x1f;
			fb_write(p, (red << 11) | (((c >> 5) & 0x3f) << 5) | blue);
		}
	}
}

/* 0x00039c8c Colour pair used by Render_drawImage mode 10 (find, replace). */
void Render_setBlendColors(Render *render, int a, int b)
{
	render->blendArg = (int16_t)Color_rgb888to565(render, a);
	render->blendArg2 = (int16_t)Color_rgb888to565(render, b);
}

/* 0x00015294 amplitude (16.16) * sin(time * freq + phase), rounded to an
 * integer. time is in ms (only the low word is used), freq is 6.10 angle
 * units (1024 per turn) per second, phase in angle units. */
int Render_sinWave(Render *render, int timeLo, int timeHi, int amplitude, int freq, int phase)
{
	/* seconds as 16.16, with a 64-bit integer part */
	int64_t t = rlsl64((int64_t)timeLo, 32) / 0x3e80000;
	int64_t tInt = t >> 16;
	int32_t tFrac = (int32_t)(t & 0xffff);
	int32_t f = rshl(freq, 10); /* 16.16 */
	int32_t fHi = f >> 16;
	int32_t fLo = (int32_t)((uint32_t)f & 0xffff);
	int64_t mid;
	int32_t angle, idx;
	int64_t v;
	(void)timeHi;

	/* integer part of t * f (both 16.16), built from 16-bit halves exactly
	 * like the original: tInt*fHi + (tInt*fLo + tFrac*fHi + (tFrac*fLo >> 16)) >> 16 */
	mid = (rmul64(rlsl64(tInt, 16), fLo) >> 16) + (rmul64(rlsl64((int64_t)fHi, 16), tFrac) >> 16);
	mid += rmul(tFrac, fLo) >> 16;
	angle = (int32_t)(uint32_t)((uint64_t)(mid >> 16) + (uint64_t)rmul64(tInt, fHi) + (uint64_t)(int64_t)phase);

	idx = angle % 1024;
	if (idx < 0) /* the phone indexed before the table here; never happens in play */
		idx &= 0x3ff;
	v = ((int64_t)render->sinTable[idx] * amplitude) >> 16;
	return (int32_t)((v + 0x8000) >> 16);
}

/* 0x0001bbd0 Torch flicker frame (1..5) for a time in ms. */
int Render_getFlickerFrame(Render *render, int timeMs)
{
	int r = (timeMs / 300) % 36;
	int v = r + 1;

	if (v < 30)
		return (v == 10 || v == 15 || v == 25) ? 2 : 1;
	if (v < 33)
		return r - 27;
	return r - 30;
}

/* 0x00018f28 Mark every column of the view as uncovered. */
void Render_clearColumnDepth(Render *render)
{
	int x = render->width;
	while (--x >= 0)
		render->columnDepth[x] = DEPTH_EMPTY;
}

/* 0x0002cc64 Occluder pass: mark the columns covered by a projected solid wall. */
void Render_occludeColumns(Render *render, Line *seg)
{
	int x;
	if (seg->x2 - seg->x1 <= 0)
		return;
	for (x = (seg->x1 + 0xffff) >> 16; x < (seg->x2 + 0xffff) >> 16; x++)
		render->columnDepth[x] = DEPTH_SOLID;
}
