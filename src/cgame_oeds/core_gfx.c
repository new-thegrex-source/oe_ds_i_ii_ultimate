/*
 * 2D drawing through the applet's IGraphics (all coordinates are relative to
 * the canvas view origin), colour conversion, the 64-bit square root, and BMP
 * image loading.
 */
#include "core_local.h"

/* Player-visible error format kept in the executable (read from the user's .mod) */
#define MOD_FMT_BAD_BPP 0x2fbb8u

/* ------------------------------------------------------------ IGraphics wrappers */

/* 0x00001368 */
void Gfx_resetClip(Applet *app)
{
	IGRAPHICS_SetClip(app->graphics, NULL, 0);
	app->clipSet = 0;
}

/* 0x0000138c */
void Gfx_setClip(Applet *app, int x, int y, int w, int h)
{
	AEEClip clip;
	clip.type = CLIPPING_RECT;
	clip.rect.x = (int16_t)(app->canvas->originX + x);
	clip.rect.y = (int16_t)(app->canvas->originY + y);
	clip.rect.dx = (int16_t)w;
	clip.rect.dy = (int16_t)h;
	IGRAPHICS_SetClip(app->graphics, &clip, 0);
	app->clipSet = 1;
}

/* 0x00001590 */
void Gfx_drawLine(Applet *app, int x1, int y1, int x2, int y2)
{
	AEELine ln;
	ln.sx = (int16_t)(app->canvas->originX + x1);
	ln.sy = (int16_t)(app->canvas->originY + y1);
	ln.ex = (int16_t)(app->canvas->originX + x2);
	ln.ey = (int16_t)(app->canvas->originY + y2);
	IGRAPHICS_DrawLine(app->graphics, &ln);
}

/* 0x000015dc  Outline covering (w+1) x (h+1) pixels, drawn with fill mode off. */
void Gfx_drawRect(Applet *app, int x, int y, int w, int h)
{
	AEERect rc;
	boolean fill;
	rc.x = (int16_t)(app->canvas->originX + x);
	rc.y = (int16_t)(app->canvas->originY + y);
	rc.dx = (int16_t)(w + 1);
	rc.dy = (int16_t)(h + 1);
	fill = app->graphics->fill; /* IGRAPHICS_GetFillMode */
	if (fill)
		IGRAPHICS_SetFillMode(app->graphics, FALSE);
	IGRAPHICS_DrawRect(app->graphics, &rc);
	if (fill)
		IGRAPHICS_SetFillMode(app->graphics, TRUE);
}

/* 0x00001648 */
void Gfx_fillCircle(Applet *app, int x, int y, int r)
{
	AEECircle c;
	boolean fill;
	c.cx = (int16_t)(app->canvas->originX + x);
	c.cy = (int16_t)(app->canvas->originY + y);
	c.r = (int16_t)r;
	fill = app->graphics->fill; /* IGRAPHICS_GetFillMode */
	if (!fill)
		IGRAPHICS_SetFillMode(app->graphics, TRUE);
	IGRAPHICS_DrawCircle(app->graphics, &c);
	if (!fill)
		IGRAPHICS_SetFillMode(app->graphics, FALSE);
}

/* 0x00001824  Solid rectangle in the background colour (IGRAPHICS_ClearRect). */
void Gfx_fillRect(Applet *app, int x, int y, int w, int h)
{
	AEERect rc;
	rc.x = (int16_t)(app->canvas->originX + x);
	rc.y = (int16_t)(app->canvas->originY + y);
	rc.dx = (int16_t)w;
	rc.dy = (int16_t)h;
	IGRAPHICS_ClearRect(app->graphics, &rc);
}

/* 0x000019a8 */
void Gfx_update(Applet *app)
{
	IGRAPHICS_Update(app->graphics);
}

/* 0x00002588  0xRRGGBB -> pen, background and fill colour. */
void Gfx_setColor(Applet *app, int rgb)
{
	uint8_t r = (uint8_t)(rgb >> 16), g = (uint8_t)(rgb >> 8), b = (uint8_t)rgb;
	IGRAPHICS_SetColor(app->graphics, r, g, b, 0xff);
	IGRAPHICS_SetBackground(app->graphics, r, g, b);
	IGRAPHICS_SetFillColor(app->graphics, r, g, b, 0xff);
}

/* 0x00008bc8  0xRRGGBB -> display pixel (Render_rgbToPixel takes b, g, r). */
int Color_rgb888to565(void *render, int rgb)
{
	uint32_t v = (uint32_t)rgb;
	return Render_rgbToPixel(render, (int)(v & 0xff), (int)((v >> 8) & 0xff), (int)((v >> 16) & 0xff));
}

/* ------------------------------------------------------------ math */

/* 0x000018c8  Square root of the 64-bit value hi:lo in 8.8 fixed point
 * (sqrt(x << 8)): Newton steps from (x/256 + 256) / 2, four unconditional
 * steps, then up to 12 more until the estimate stops changing. The original
 * returned the 64-bit result in r0:r1; every caller uses the low word. */
int Math_fixedSqrt64(int lo, int hi)
{
	int64_t x = (int64_t)(((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo);
	int64_t v, r, next;
	if (x == 0)
		return 0;
	v = (int64_t)((uint64_t)x << 8);
	r = (v / 0x100 + 0x100) >> 1;
	for (int i = 0; i < 3; i++) {
		if (r == 0) /* (only reachable for negative input) */
			return 0;
		r = (v / r + r) >> 1;
	}
	for (int i = 0; i < 12; i++) {
		if (r == 0)
			return 0;
		next = (v / r + r) >> 1;
		if (next == r)
			break;
		r = next;
	}
	return (int)r;
}

/* ------------------------------------------------------------ images */

/* 0x000128e0  Release an image's IDIB view and bitmap; the heap delta is
 * taken off the image memory statistic. */
void App_freeImage(Applet *app, void *imagePair)
{
	Image *img = imagePair;
	int before = App_getMemUsed(app);
	if (img != NULL && img->bitmap != NULL) {
		free(img->dib); /* our IDIB views are plain allocations */
		IBITMAP_Release(img->bitmap);
		img->dib = NULL;
		img->bitmap = NULL;
	}
	app->imageMem -= before - App_getMemUsed(app);
}

/* 0x00018c14  Load BMP resource resId (4 or 8 bpp, uncompressed, bottom-up)
 * into a device-compatible bitmap. The original built a palettised IDIB from
 * the BMP rows and blitted it (AEE_RO_COPY) into a new compatible bitmap,
 * which converted every palette index to a display colour; here the palette is
 * converted once and the indices are expanded directly. With `transparent`,
 * magenta (0xff00ff) becomes the bitmap's transparent colour. Afterwards
 * image->dib is the bitmap's own IDIB view (the original's QueryInterface
 * 0x1001045), RGB565 with the pitch in bytes. */
void Image_load(Applet *app, int resId, int transparent, void *image)
{
	Image *img = image;
	InputStream res;
	const uint8_t *bmp;
	int pixelOfs, width, height, bpp, numColors, rowBits, srcStride;
	uint16_t palette[256];
	IBitmap *bitmap;
	AEEBitmapInfo info;
	int memBefore = App_getMemUsed(app);

	InputStream_ctor(&res, app, 1);
	if (!Resource_open(&res, (int16_t)resId, 1)) {
		g_app->errorCode = 0x16;
		goto out;
	}
	bmp = InputStream_getBuffer(&res);
	pixelOfs = Bytes_readInt((void *)bmp, 10);
	width = Bytes_readInt((void *)bmp, 0x12) & 0xffff;
	height = Bytes_readInt((void *)bmp, 0x16) & 0xffff;
	bpp = (uint16_t)Util_shortAt((char *)bmp, 0x1c);
	numColors = Bytes_readInt((void *)bmp, 0x2e);
	if (bpp != 4 && bpp != 8) {
		g_app->errorCode = 1;
		App_setErrorStr(app, (char *)ModData_str(&g_mod, MOD_FMT_BAD_BPP), resId, bpp);
		goto out;
	}
	/* IDIB.cntRGB: biClrUsed, or the full palette when 0 */
	numColors = (uint16_t)(numColors == 0 ? 1 << bpp : numColors);

	/* palette: BGRx quads after the 40-byte info header -> display colours
	 * (what IBITMAP_BltIn did from the palettised IDIB) */
	memset(palette, 0, sizeof(palette));
	for (int i = 0; i < numColors && i < 256; i++) {
		const uint8_t *q = bmp + 0x36 + i * 4;
		palette[i] = (uint16_t)IBITMAP_RGBToNative(app->deviceBitmap, MAKE_RGB(q[2], q[1], q[0]));
	}
	img->transparent = transparent;

	/* BMP rows are padded to 32 bits and stored bottom-up */
	rowBits = bpp * width;
	if (rowBits & 0x1f)
		rowBits = (rowBits / 32) * 32 + 32;
	srcStride = rowBits / 8;

	if (IBITMAP_CreateCompatibleBitmap(app->deviceBitmap, &img->bitmap, width, height) != SUCCESS) {
		InputStream_close(&res);
		g_app->errorCode = 1;
		goto out;
	}
	bitmap = img->bitmap;
	for (int y = 0; y < height; y++) {
		const uint8_t *row = bmp + pixelOfs + srcStride * (height - 1 - y);
		uint16_t *dst = bitmap->px + y * bitmap->pitch;
		for (int x = 0; x < width; x++) {
			int idx = bpp == 8 ? row[x] : (x & 1 ? row[x >> 1] & 0x0f : row[x >> 1] >> 4);
			dst[x] = palette[idx];
		}
	}
	InputStream_close(&res);
	Brew_hdBitmap(resId, bitmap);   /* port: a mod's picture (or the id Tech 3 font) in its place */

	if (IBITMAP_GetInfo(bitmap, &info) != SUCCESS) {
		g_app->errorCode = 1;
		goto out;
	}
	img->width = (int32_t)info.cx;
	img->height = (int32_t)info.cy;
	img->depth = (int32_t)info.nDepth;
	if (transparent != 0 &&
	    IBITMAP_SetTransparencyColor(bitmap, IBITMAP_RGBToNative(bitmap, MAKE_RGB(0xff, 0x00, 0xff))) != SUCCESS) {
		g_app->errorCode = 1;
		goto out;
	}

	/* the bitmap's IDIB view */
	img->dib = calloc(1, sizeof(IDIB));
	if (img->dib == NULL) {
		g_app->errorCode = 1;
	} else {
		img->dib->pBmp = (uint8_t *)bitmap->px;
		img->dib->cx = (uint16_t)bitmap->width;
		img->dib->cy = (uint16_t)bitmap->height;
		img->dib->nPitch = (int16_t)(bitmap->pitch * 2);
		img->dib->nDepth = 16;
		img->dib->nColorScheme = 16; /* IDIB_COLORSCHEME_565 */
		img->dib->ncTransparent = bitmap->transparent;
	}
	app->imageMem += App_getMemUsed(app) - memBefore;
out:
	InputStream_dtor(&res, 0);
}
