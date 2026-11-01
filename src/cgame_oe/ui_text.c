/*
 * Canvas (Applet object 0x110) 2D drawing primitives: image regions with
 * anchors, the two bitmap fonts, icons, the scroll bar, the parchment dialog
 * frame and the soft-key bar. Everything ends up in Render_drawImage, which
 * blits into the screen at the canvas origin (Canvas.originX/Y).
 */

#include "ui_local.h"

/* soft-key labels */
#define MOD_STR_MENU   0x2d2a8u
#define MOD_STR_LEAVE  0x2d2b0u
#define MOD_STR_MAP    0x2d2b8u

/* Anchor bits (Canvas_drawRegion / Canvas_drawStringRange). */
enum {
	ANCHOR_BOTTOM = 0x02,
	ANCHOR_RIGHT = 0x08,
	ANCHOR_HCENTER = 0x10,
	ANCHOR_VCENTER = 0x20,
};

/* Text flags (Canvas_drawStringRange). */
enum {
	TEXT_LARGE = 0x01,      /* 13x17 font, 10 px advance */
	TEXT_TYPEWRITER = 0x02, /* last three characters fade in (blend 6/5/4) */
	TEXT_SHAKE = 0x04,      /* some words jitter */
	TEXT_CYCLE = 0x08,      /* colour cycles per character */
	TEXT_TINT = 0x10,       /* recolour with Canvas.textColor */
	TEXT_BLEND75 = 0x20,
	TEXT_BLEND50 = 0x40,
	TEXT_BLEND25 = 0x80,
};

/* Render_drawImage blend mode that replaces one key colour (Pixel_replaceColor). */
#define BLEND_REPLACE 0xe

/*
 * Render_drawImage with the original's pointer argument. Only blend 0xe reads
 * it: a short[2] {key, colour}, or Render.blendColors when it is 0. The port
 * passes the pair through Render.blendColors (saved and restored around the
 * call) so the argument never has to travel through an int on a 64-bit host;
 * the pixels written are the same.
 */
static void drawImageArg(Render *render, Image *img, int dx, int dy, int sx, int sy, int w, int h,
                         int mode, const int16_t *arg)
{
	int16_t saved0, saved1;

	if (arg == NULL || mode != BLEND_REPLACE) {
		Render_drawImage(render, img, dx, dy, sx, sy, w, h, mode, 0);
		return;
	}
	saved0 = render->blendColors[0];
	saved1 = render->blendColors[1];
	render->blendColors[0] = arg[0];
	render->blendColors[1] = arg[1];
	Render_drawImage(render, img, dx, dy, sx, sy, w, h, mode, 0);
	render->blendColors[0] = saved0;
	render->blendColors[1] = saved1;
}

/* 0x0000b238
 * Draws the (sx, sy, w, h) part of an image at (x, y) relative to the canvas
 * origin. w or h 0 means the whole image; a region larger than the image is
 * tiled in steps of the image size. */
void Canvas_drawRegion(Canvas *canvas, void *image, int sx, int sy, int w, int h, int transform, int x, int y,
                       int anchor, int mode, void *extra)
{
	Image *img = image;
	int left, curX, curY, remW;

	(void)transform; /* never used by the 1049 build */
	if (w == 0)
		w = img->width;
	if (h == 0)
		h = img->height;

	if (anchor & ANCHOR_HCENTER)
		x -= (int)((uint32_t)w >> 1);
	else if (anchor & ANCHOR_RIGHT)
		x -= w;
	if (anchor & ANCHOR_VCENTER)
		curY = y - (int)((uint32_t)h >> 1);
	else if (anchor & ANCHOR_BOTTOM)
		curY = y - h;
	else
		curY = y;

	left = x;
	do {
		remW = w;
		curX = left;
		do {
			drawImageArg(canvas->render, img, canvas->originX + curX, canvas->originY + curY, sx, sy, remW, h,
			             mode, extra);
			remW -= img->width;
			curX += img->width;
		} while (remW > 0);
		h -= img->height;
		curY += img->height;
	} while (h > 0);
}

/* 0x0000ac94 */
void Canvas_drawImage(Canvas *canvas, void *img, int x, int y, int anchor, int flags)
{
	Canvas_drawRegion(canvas, img, 0, 0, 0, 0, 0, x, y, anchor, flags, NULL);
}

/* 0x00009f04  12x12 status icons stacked in one column (5136). */
void Canvas_drawSmallIcon(Canvas *canvas, int idx, int x, int y, int anchor, int a, int b)
{
	Canvas_drawRegion(canvas, &canvas->buffIcons, 0, idx * 12, 12, 12, 0, x, y, anchor, a, (void *)(intptr_t)b);
}

/* 0x0000ac48  24x24 item icons, 16 per row (5009). */
void Canvas_drawLargeIcon(Canvas *canvas, int idx, int x, int y, int anchor, int a, int b)
{
	int row = idx / 16;
	int col = idx - row * 16;

	Canvas_drawRegion(canvas, &canvas->largeIcons, col * 24, row * 24, 24, 24, 0, x, y, anchor, a,
	                  (void *)(intptr_t)b);
}

/* 0x0000bc4c */
void Canvas_drawString(Canvas *canvas, char *str, int x, int y, int anchor, int flags)
{
	Canvas_drawStringRange(canvas, str, x, y, anchor, 0, -1, flags);
}

/* 0x0000bc7c
 * Bitmap font text. Glyph g = (c + 0xdf) & 0xff, 16 glyphs per font row;
 * characters >= 0xc0 are 12x12 status icons; '\n' starts a new line. len -1
 * draws to the end of the string. */
void Canvas_drawStringRange(Canvas *canvas, char *str, int x, int y, int anchor, int start, int len, int flags)
{
	Image *font;
	int adv, cellW, cellH;
	int n, end, i, lineX, curX;
	int mode = 0;
	int16_t tint[2];
	const int16_t *tintArg = NULL;
	int wordSum = 0;  /* sum of glyph numbers of the current word */
	int shaky = 0;    /* once set, later words jitter (flag 4) */

	if (len == 0)
		return;

	if (flags & TEXT_LARGE) {
		font = &canvas->largeFont;
		adv = 10;
		cellW = 13;
		cellH = 17;
	} else {
		font = &canvas->smallFont;
		adv = 7;
		cellW = 9;
		cellH = 12;
	}

	n = (int)strlen(str) - start;
	if (len < n && len >= 0)
		n = len;

	if (anchor & ANCHOR_RIGHT) {
		x -= n * adv;
	} else if (anchor & ANCHOR_HCENTER) {
		int half = ((n - 1) << 16) / 512; /* (n-1)/2 in 8.8 */
		x -= cellW / 2 + ((adv * half) >> 8);
	}
	if (anchor & ANCHOR_BOTTOM)
		y -= cellH;
	else if (anchor & ANCHOR_VCENTER)
		y -= cellH >> 1;

	end = start + n;
	if (flags & TEXT_CYCLE)
		canvas->textColor = 0xe708;

	lineX = x;
	curX = x;
	for (i = start; i < end; i++) {
		int c = (uint8_t)str[i];
		int glyph, gx, gy;

		if (c == '\n') {
			y += cellH;
			curX = lineX;
			continue;
		}
		if (c == ' ') {
			/* Roughly one word boundary in ten (by time and the previous word) turns
			 * the shake on for the rest of the string. */
			if (((canvas->app->tickTime + (wordSum << 9)) >> 11) % 10 == 0)
				shaky = 1;
			wordSum = 0;
			curX += adv;
			continue;
		}
		if (c >= 0xc0) {
			Canvas_drawSmallIcon(canvas, c - 0xc0, curX, y, 0, 0, 0);
			curX += adv;
			continue;
		}

		glyph = (c + 0xdf) & 0xff;
		wordSum += glyph;

		if (flags & TEXT_TYPEWRITER) {
			int left = end - i;
			if (left > 3)
				mode = 0;
			else if (left == 3)
				mode = 6;
			else if (left <= 1)
				mode = 4;
			else
				mode = 5;
		} else if (flags & TEXT_TINT) {
			mode = BLEND_REPLACE;
			tintArg = tint;
		} else if (flags & TEXT_CYCLE) {
			int step = canvas->app->tickTime / (glyph * 4 + 100);
			uint32_t v = (uint32_t)(Ui_bob(step) * 4);

			mode = BLEND_REPLACE;
			canvas->textColor = (uint16_t)((v + 0x10) | (((v >> 1) + 0x1d) << 11) | ((v + 0x3a) << 5));
			tintArg = tint;
		} else if (flags & TEXT_BLEND75) {
			mode = 6;
		} else if (flags & TEXT_BLEND50) {
			mode = 5;
		} else if (flags & TEXT_BLEND25) {
			mode = 4;
		}

		gx = curX;
		gy = y;
		if ((flags & TEXT_SHAKE) && shaky) {
			int k = (glyph + i) * 4;
			int now = canvas->app->tickTime;
			gy = y + ModData_s8(&g_mod, MOD_BOB_TABLE + (uint32_t)((now / (k + 700)) % 2));
			gx = curX + ModData_s8(&g_mod, MOD_BOB_TABLE + (uint32_t)((now / (k + 1000)) % 2));
		}

		/* blend 0xe recolours key colour Canvas.tintKey with Canvas.textColor */
		tint[0] = canvas->tintKey;
		tint[1] = (int16_t)canvas->textColor;
		Canvas_drawRegion(canvas, font, cellW * (glyph & 0xf), cellH * (glyph >> 4), cellW, cellH, 0, gx, gy, 0,
		                  mode, (void *)tintArg);
		curX += adv;
	}
}

/* 0x0000b2e8
 * Vertical scroll bar with its right edge at x: arrows from the menu scroll
 * image, track and thumb. first/last/total are lines; only drawn when the text
 * does not fit (Canvas.linesPerPage < total). */
void Canvas_drawScrollBar(Canvas *canvas, int x, int y, int h, int first, int last, int total)
{
	int shown = last - first;
	int thumbH, maxFirst, pos, trackX, thumbY;
	Image *arrows;

	if (shown == 0 || canvas->linesPerPage >= total)
		return;

	thumbH = h / ((total + shown - 1) / shown);
	maxFirst = total - canvas->linesPerPage;
	if (maxFirst < first)
		maxFirst = first;
	pos = ((first << 16) / (maxFirst << 8)) * ((h - thumbH) - 14) * 0x100 >> 16;
	if (last == total)
		pos = (h - 14) - thumbH;

	arrows = &canvas->app->menuSystem->scrollArrows;
	Canvas_drawRegion(canvas, arrows, 0, 0, 7, 7, 0, x, y, 9, 0, NULL);
	Canvas_drawRegion(canvas, arrows, 0, 7, 7, 7, 0, x, y + h, 10, 0, NULL);

	trackX = x - 7;
	Gfx_setColor(canvas->app, (int)0xffb3aa93u);
	Gfx_fillRect(canvas->app, trackX, y + 7, 7, h - 14);
	Gfx_setColor(canvas->app, (int)0xffe7cfadu);
	thumbY = y + pos + 7;
	Gfx_fillRect(canvas->app, trackX, thumbY, 7, thumbH);
	Gfx_setColor(canvas->app, UI_BLACK);
	Gfx_drawRect(canvas->app, trackX, thumbY, 6, thumbH - 1);
	Gfx_drawRect(canvas->app, trackX, y, 6, h - 1);
}

/* 0x0000b434
 * Parchment frame: 60x13 corner pieces of the scroll (style 0) or barter
 * (style 1) image stretched by repeating 1-px slices, a flat fill, and on big
 * frames the stain and tear decals. */
void Canvas_drawDialogFrame(Canvas *canvas, int x, int y, int w, int h, int style)
{
	Image *bg, *stains, *tears;
	int fill, rightX, bottomY, i;

	if (style == 0) {
		tears = &canvas->scrollTears;
		bg = &canvas->scrollBg;
		stains = &canvas->scrollStains;
		fill = (int)0xffa8967eu;
	} else {
		tears = &canvas->barterTears;
		bg = &canvas->barterBg;
		stains = &canvas->barterStains;
		fill = (int)0xff99524bu;
	}

	rightX = x + w - 0x3c;
	bottomY = y + h - 0xd;
	Canvas_drawRegion(canvas, bg, 0, 0, 0x3c, 0xd, 0, x, y, 0, 0, NULL);
	Canvas_drawRegion(canvas, bg, 0x3c, 0, 0x3c, 0xd, 0, rightX, y, 0, 0, NULL);
	Canvas_drawRegion(canvas, bg, 0, 0xd, 0x3c, 0xd, 0, x, bottomY, 0, 0, NULL);
	Canvas_drawRegion(canvas, bg, 0x3c, 0xd, 0x3c, 0xd, 0, rightX, bottomY, 0, 0, NULL);
	for (i = 0; i < h - 0x1a; i++) {
		int rowY = y + i + 0xd;
		Canvas_drawRegion(canvas, bg, 0, 0xd, 0x3c, 1, 0, x, rowY, 0, 0, NULL);
		Canvas_drawRegion(canvas, bg, 0x3c, 0xd, 0x3c, 1, 0, rightX, rowY, 0, 0, NULL);
	}
	for (i = 0; i < w - 0x78; i++) {
		int colX = x + i + 0x3c;
		Canvas_drawRegion(canvas, bg, 0x3c, 0, 1, 0xd, 0, colX, y, 0, 0, NULL);
		Canvas_drawRegion(canvas, bg, 0x3c, 0xd, 1, 0xd, 0, colX, bottomY, 0, 0, NULL);
	}
	if (w > 0x78) {
		Gfx_setColor(canvas->app, fill);
		Gfx_fillRect(canvas->app, x + 0x3c, y + 0xd, w - 0x78, h - 0x1a);
	}
	if (w > 0xa0 && h > 0xa0) {
		Canvas_drawRegion(canvas, stains, 0, 0, 0x3b, 0x2d, 0, x + 0x1a, y + 0x26, 0, 0, NULL);
		Canvas_drawRegion(canvas, stains, 0x3c, 0, 0x2b, 0x26, 0, x + w - 0x44, y + 100, 0, 0, NULL);
		Canvas_drawRegion(canvas, stains, 0, 0x2e, 0x2b, 6, 0, x + 0x6c, y + 0x38, 0, 0, NULL);
		Canvas_drawRegion(canvas, stains, 0x3c, 0x27, 0x21, 0x1c, 0, x + 0x28, y + 0x68, 0, 0, NULL);
		Canvas_drawRegion(canvas, tears, 0, 0, 0xd, 0x1d, 0, x + 0x9e, y + 0x2b, 0, 0, NULL);
		Canvas_drawRegion(canvas, tears, 0xe, 0, 0xc, 0xd, 0, x + 0x9f, y + 0x55, 0, 0, NULL);
		Canvas_drawRegion(canvas, tears, 0xe, 0xe, 0xc, 0xc, 0, x + 7, y + 0x32, 0, 0, NULL);
		Canvas_drawRegion(canvas, tears, 0x1b, 0, 6, 0x12, 0, x + 0xa5, y + 0x71, 0, 0, NULL);
		Canvas_drawRegion(canvas, tears, 0x22, 0, 10, 0x13, 0, x + 7, y + h - 0x50, 0, 0, NULL);
		Canvas_drawRegion(canvas, tears, 0x2d, 0, 0xc, 0x1c, 0, x + 7, y + h - 0x41, 0, 0, NULL);
	}
}

/* 0x0000b80c
 * Bottom soft-key bar (only on screens tall enough, Canvas.softkeyBar): label
 * boxes separated by a two-pixel divider. NULL hides a label. The labels are
 * remembered for Canvas_refreshSoftKeys. */
void Canvas_drawSoftKeys(Canvas *canvas, char *left, char *right)
{
	int ox, y, lx;

	if (!canvas->softkeyBar)
		return;

	if (right != NULL || left != NULL) {
		if (left == NULL)
			canvas->softkeySepL = 0;
		if (right == NULL)
			canvas->softkeySepR = 0;
	}
	ox = -canvas->originX;
	y = canvas->softKeyBarY - canvas->originY;
	Hud_drawSoftkeyBarBg(canvas->app->hud, ox, y, canvas->screenW);

	if (left != NULL) {
		if (canvas->leftLabel != left)
			strcpy(canvas->leftLabel, left);
		canvas->softkeySepL = Text_approxWidth(0, canvas->leftLabel) + 8;
		Gfx_setColor(canvas->app, (int)0xff313131u);
		lx = canvas->softkeySepL + ox;
		Gfx_drawLine(canvas->app, lx, y, lx, y + 0x13);
		Gfx_setColor(canvas->app, (int)0xff808591u);
		lx = canvas->softkeySepL + ox + 1;
		Gfx_drawLine(canvas->app, lx, y, lx, y + 0x13);
		Canvas_drawString(canvas, left, canvas->softkeySepL / 2, y + 5, 0x11, 0);
	}
	if (right != NULL) {
		if (canvas->rightLabel != right)
			strcpy(canvas->rightLabel, right);
		canvas->softkeySepR = canvas->screenW - Text_approxWidth(0, canvas->rightLabel) - 8;
		Gfx_setColor(canvas->app, (int)0xff313131u);
		lx = canvas->softkeySepR + ox;
		Gfx_drawLine(canvas->app, lx, y, lx, y + 0x13);
		Gfx_setColor(canvas->app, (int)0xff808591u);
		lx = canvas->softkeySepR + ox + 1;
		Gfx_drawLine(canvas->app, lx, y, lx, y + 0x13);
		Canvas_drawString(canvas, right, canvas->screenW - (canvas->screenW - canvas->softkeySepR) / 2, y + 5,
		                  0x11, 0);
	}
}

/* 0x0000b19c */
void Canvas_drawPlayingSoftKeys(Canvas *canvas)
{
	Canvas_drawSoftKeys(canvas, Ui_str(MOD_STR_MENU), Ui_str(canvas->state == 5 ? MOD_STR_LEAVE : MOD_STR_MAP));
}

/* 0x000200b4 */
void Canvas_refreshSoftKeys(Canvas *canvas)
{
	Canvas_drawSoftKeys(canvas, canvas->leftLabel, canvas->rightLabel);
}
