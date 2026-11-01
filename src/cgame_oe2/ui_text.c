/*
 * Canvas drawing helpers: image regions and icons, the bitmap font, labels,
 * dotted menu titles, the scroll bar, the parchment dialog frame and the
 * soft-key bar along the bottom of the screen.
 *
 * Everything is drawn in "view" coordinates: Canvas_drawRegion adds the view
 * origin (Canvas.viewRect x/y) before handing the blit to Render_drawImage.
 */

#include "game.h"

/* Small signed-byte table used for wobbling text and the bobbing yes/no cursor:
 * entries [0..3] are read as time-based offsets (index % 2 or % 4). */
#define MOD_WOBBLE_TABLE 0x47168u
/* One-glyph string used to pad dotted titles (the font's "dot" glyph). */
#define MOD_TITLE_PAD_GLYPH 0x45ddcu

/* Font metrics (pure layout constants of the two font sheets). */
enum {
	FONT_SMALL_ADVANCE = 7, FONT_SMALL_CELL_W = 9, FONT_SMALL_CELL_H = 12,
	FONT_LARGE_ADVANCE = 10, FONT_LARGE_CELL_W = 13, FONT_LARGE_CELL_H = 17,
};

/* Text drawing flags (Canvas_drawStringRange). */
enum {
	TEXT_LARGE = 0x01,      /* large font sheet */
	TEXT_TYPEWRITER = 0x02, /* last three glyphs fade in */
	TEXT_SHAKE = 0x04,      /* words after a "lucky" space wobble */
	TEXT_CYCLE = 0x08,      /* per-glyph colour cycling */
	TEXT_RECOLOR = 0x10,    /* recolour with Canvas.textColor */
	TEXT_FADE_75 = 0x20, TEXT_FADE_50 = 0x40, TEXT_FADE_25 = 0x80,
};

/* Render_drawImage blend mode that swaps one colour for another. */
#define BLEND_COLOR_KEY 10

/* 0x00005600 */
String *Canvas_addTextArgStr(Canvas *canvas, String *str)
{
	return String_assign(&canvas->textArgs[canvas->numTextArgs++], str);
}

/* 0x0000561c */
void Canvas_addTextArgInt(Canvas *canvas, int v)
{
	/* The int goes into the parallel int table; the String slot is emptied so
	 * Text_getString falls back to the number. */
	String empty;

	canvas->textIntArgs[canvas->numTextArgs] = v;
	String_ctor(&empty);
	String_assign(&canvas->textArgs[canvas->numTextArgs++], &empty);
	String_dtor(&empty, 0);
}

/*
 * 0x00010424
 * Draws a w x h region of img at (x, y) relative to the view. A region larger
 * than the image is tiled. `a` is the Render_drawImage blend mode.
 *
 * Port note: the original passed `b` straight through as Render_drawImage's
 * blend argument; the only non-zero value ever used was &canvas->renderParam
 * (the font's {key colour, replacement colour} pair for blend mode 10). The
 * port cannot carry that pointer in an int, so b != 0 means "use the Canvas
 * text colours", and we hand them to Render through its own colour-key pair,
 * which is what Render_drawImage reads when its argument is 0.
 */
void Canvas_drawRegion(Canvas *canvas, Image *img, int sx, int sy, int w, int h, int transform,
		       int x, int y, int anchor, int a, int b)
{
	Render *render = g_app->render;
	int imgW = img->width;
	int imgH = img->height;
	int16_t savedKey = 0, savedColor = 0;
	int remW, tileX;

	if (w == 0)
		w = imgW;
	if (h == 0)
		h = imgH;

	if (anchor & 0x10)
		x -= w / 2;
	else if (anchor & 8)
		x -= w;
	if (anchor & 0x20)
		y -= h / 2;
	else if (anchor & 2)
		y -= h;

	if (b != 0) {
		savedKey = render->blendArg;
		savedColor = render->blendArg2;
		render->blendArg = canvas->renderParam;
		render->blendArg2 = (int16_t)canvas->textColor;
	}

	/* Tile rows, then columns; each blit is given the remaining size and
	 * Render clips it to the image. */
	do {
		remW = w;
		tileX = x;
		do {
			Render_drawImage(render, img, canvas->viewRect[0] + tileX, canvas->viewRect[1] + y,
					 sx, sy, remW, h, a, 0);
			remW -= imgW;
			tileX += imgW;
		} while (remW > 0);
		h -= imgH;
		y += imgH;
	} while (h > 0);

	if (b != 0) {
		render->blendArg = savedKey;
		render->blendArg2 = savedColor;
	}
}

/* 0x0000fc4c */
void Canvas_drawImage(Canvas *canvas, Image *img, int x, int y, int anchor, int flags)
{
	Canvas_drawRegion(canvas, img, 0, 0, 0, 0, 0, x, y, anchor, flags, 0);
}

/* 0x0000fc20 */
void Canvas_drawImageSimple(Canvas *canvas, Image *img, int x, int y, int anchor)
{
	Canvas_drawImage(canvas, img, x, y, anchor, 0);
}

/* 0x0000e2d4  12x12 icons stacked vertically in Icons_Buffs. */
void Canvas_drawSmallIcon(Canvas *canvas, int idx, int x, int y, int anchor, int a, int b)
{
	Canvas_drawRegion(canvas, &canvas->imgIconsBuffs, 0, idx * 12, 12, 12, 0, x, y, anchor, a, b);
}

/* 0x0000fbac  24x24 icons, 16 per row. */
void Canvas_drawLargeIcon(Canvas *canvas, int idx, int x, int y, int anchor, int a, int b)
{
	int row = idx / 16;
	int col = idx - row * 16;

	Canvas_drawRegion(canvas, &canvas->imgIcons, col * 24, row * 24, 24, 24, 0, x, y, anchor, a, b);
}

/* 0x0000db34  Repeats the 20px-high bar segment across w pixels. */
void Canvas_drawSoftkeyBarBg(Canvas *canvas, int x, int y, int w)
{
	int segW = canvas->imgHudBarSegment.width;
	int i;

	for (i = 0; i < w; i += segW)
		Canvas_drawRegion(canvas, &canvas->imgHudBarSegment, 0, 20, segW, 20, 0, x + i, y, 0, 0, 0);
}

/* 0x000113e0 */
void Canvas_drawString(Canvas *canvas, String *str, int x, int y, int anchor)
{
	Canvas_drawStringFlags(canvas, str, x, y, anchor, 0);
}

/* 0x0001140c */
void Canvas_drawStringFlags(Canvas *canvas, String *str, int x, int y, int anchor, int flags)
{
	Canvas_drawStringRange(canvas, str, x, y, anchor, 0, str->len, flags);
}

/* String_charAt (0x96ac) reads an unsigned byte. */
static int charAt(const String *s, int i)
{
	return (uint8_t)s->buf[i];
}

/*
 * 0x00011468
 * Bitmap-font renderer. Glyph g = c - 0x21 sits at column g & 15, row g >> 4
 * of the font sheet; bytes >= 0xc0 are inline 12x12 icons; '\n' starts a new
 * line. Anchors apply to the first line only. len == -1 means "to the end".
 */
void Canvas_drawStringRange(Canvas *canvas, String *str, int x, int y, int anchor, int start, int len, int flags)
{
	Image *font;
	int advance, cellW, cellH;
	int firstLineLen = -1, count = 0;
	int end, i, penX;
	int blendMode = 0;  /* persists between glyphs, like the original */
	int useTextColors = 0;
	int wordSum = 0;    /* sum of glyph indices since the last space */
	int shaking = 0;    /* once set it stays set for the rest of the string */

	if (len == -1)
		len = str->len;
	if (len == 0)
		return;

	for (i = start; i < start + len && i < str->len; i++) {
		if (charAt(str, i) == '\n' && firstLineLen == -1)
			firstLineLen = count;
		count++;
	}
	if (firstLineLen == -1)
		firstLineLen = count;

	if (flags & TEXT_LARGE) {
		font = &canvas->imgFontLarge;
		advance = FONT_LARGE_ADVANCE;
		cellW = FONT_LARGE_CELL_W;
		cellH = FONT_LARGE_CELL_H;
	} else {
		font = &canvas->imgFont;
		advance = FONT_SMALL_ADVANCE;
		cellW = FONT_SMALL_CELL_W;
		cellH = FONT_SMALL_CELL_H;
	}
	if (count > len && len >= 0)
		count = len;

	if (anchor & 8) {
		x -= firstLineLen * advance;
	} else if (anchor & 0x10) {
		/* (n-1)*advance/2 in 8.8 fixed point, plus half a cell */
		int halfSpan = (firstLineLen - 1) * 65536 / 512;
		x -= cellW / 2 + ((advance * halfSpan) >> 8);
	}
	if (anchor & 2)
		y -= cellH;
	else if (anchor & 0x20)
		y -= cellH >> 1;

	end = start + count;
	if (flags & TEXT_CYCLE)
		canvas->textColor = 0xe708;

	penX = x;
	for (i = start; i < end; i++) {
		int c = charAt(str, i);
		int glyph, gx, gy;

		if (c == '\n') {
			y += cellH;
			penX = x;
			continue;
		}
		if (c == ' ') {
			/* Roughly every tenth word (time-dependent) starts the shake. */
			if ((((int)g_app->time + wordSum * 512) >> 11) % 10 == 0)
				shaking = 1;
			wordSum = 0;
			penX += advance;
			continue;
		}
		if (c >= 0xc0) {
			Canvas_drawSmallIcon(canvas, c - 0xc0, penX, y, 0, 0, useTextColors);
			penX += advance;
			continue;
		}

		glyph = c - 0x21;
		wordSum += glyph;
		if (flags & TEXT_TYPEWRITER) {
			int left = end - i;
			if (left > 3)
				blendMode = 0;
			else if (left == 3)
				blendMode = 3;
			else if (left == 2)
				blendMode = 2;
			else
				blendMode = 1;
		} else if (flags & TEXT_RECOLOR) {
			blendMode = BLEND_COLOR_KEY;
			useTextColors = 1;
		} else if (flags & TEXT_CYCLE) {
			int phase = (int)g_app->time / (glyph * 4 + 100);
			int v = ModData_s8(&g_mod, MOD_WOBBLE_TABLE + phase % 4) * 4;

			canvas->textColor = (uint16_t)((uint32_t)((v >> 1) + 0x1d) << 11 |
							(uint32_t)(v + 0x3a) << 5 | (uint32_t)(v + 0x10));
			blendMode = BLEND_COLOR_KEY;
			useTextColors = 1;
		} else if (flags & TEXT_FADE_75) {
			blendMode = 3;
		} else if (flags & TEXT_FADE_50) {
			blendMode = 2;
		} else if (flags & TEXT_FADE_25) {
			blendMode = 1;
		}

		gx = penX;
		gy = y;
		if ((flags & TEXT_SHAKE) && shaking) {
			int t = (int)g_app->time;
			int seed = glyph + i;

			gy = ModData_s8(&g_mod, MOD_WOBBLE_TABLE + (t / (seed * 4 + 700)) % 2) + y;
			gx = ModData_s8(&g_mod, MOD_WOBBLE_TABLE + (t / (seed * 4 + 1000)) % 2) + penX;
		}
		Canvas_drawRegion(canvas, font, cellW * (glyph & 0xf), cellH * ((glyph & 0xff) >> 4),
				  cellW, cellH, 0, gx, gy, 0, blendMode, useTextColors);
		penX += advance;
	}
}

/* 0x000118e8  Filled box with a border and small text inset by 3px. */
void Canvas_drawLabelBox(Canvas *canvas, String *str, int x, int y, int bgColor, int borderColor)
{
	Canvas *cv = g_app->canvas;
	int w = str->len * 7 + 6;

	Graphics_setColor(cv->graphics, bgColor);
	Graphics_fillRect(cv->graphics, x, y, w, 16);
	Graphics_setColor(cv->graphics, borderColor);
	Graphics_drawRect(cv->graphics, x, y, w, 16);
	Canvas_drawStringRange(cv, str, x + 3, y + 3, 0, 0, -1, 0);
}

/*
 * 0x00008190
 * Builds " title " centred in a row of dot glyphs into Canvas.scratch (width
 * textWidths[4] - 5 characters). Returns the Canvas, as the original did.
 */
void *Canvas_buildDottedTitle(void *x, String *str)
{
	Canvas *cv = g_app->canvas;
	String pad;
	int width, n;

	String_setLength(&cv->scratch, 0);
	n = (str->buf != NULL) ? str->len : 0; /* String_notEquals(str, empty) */
	width = cv->textWidths[4] - 5;
	if (n > 0) {
		String_appendCStr_76f0(&cv->scratch, " ");
		String_appendString(&cv->scratch, str);
		String_appendCStr_76f0(&cv->scratch, " ");
	}
	String_ctorCStr(&pad, (char *)ModData_str(&g_mod, MOD_TITLE_PAD_GLYPH));
	/* Pad alternately on the left and the right until the line is full. */
	while (n < width) {
		String_insert(&cv->scratch, 0, &pad);
		n++;
		if (n < width) {
			String_appendString(&cv->scratch, &pad);
			n++;
		}
	}
	String_dtor(&pad, 0);
	return cv;
}

/* 0x000082f4 */
void *Canvas_buildDottedTitleText(void *x, int group, int id)
{
	String text;
	void *ret;

	String_ctor(&text);
	Text_getString(g_app->canvas, group, id, &text);
	ret = Canvas_buildDottedTitle(x, &text);
	String_dtor(&text, 0);
	return ret;
}

/* 0x000082ac  Menu-strings variant. */
void *Canvas_buildDottedTitleId3(void *x, int id)
{
	return Canvas_buildDottedTitleText(x, 3, id);
}

/* 0x00008370  Second copy of the menu-strings variant. */
void *Canvas_buildDottedTitleId3b(void *x, int id)
{
	return Canvas_buildDottedTitleText(x, 3, id);
}

/*
 * 0x00010534
 * Vertical scroll bar with arrow caps (MenuSystem Scroll image). top/bottom
 * are the first/last visible rows, total the row count, view the visible rows.
 */
void Canvas_drawScrollBar(Canvas *canvas, int x, int y, int h, int top, int bottom, int total, int view)
{
	MenuSystem *ms = g_app->menuSystem;
	int thumbH, maxTop, thumbY;

	if (view == 0 || total <= view)
		return;

	thumbH = h / ((total + view - 1) / view);
	maxTop = total - view;
	if (maxTop < top)
		maxTop = top;
	thumbY = ((top * 65536) / (maxTop * 256)) * ((h - thumbH - 14) * 256) >> 16;
	if (bottom == total)
		thumbY = (h - 14) - thumbH;
	thumbY += 7;

	Canvas_drawRegion(canvas, &ms->imgScroll, 0, 0, 7, 7, 0, x, y, 9, 0, 0);
	Canvas_drawRegion(canvas, &ms->imgScroll, 0, 7, 7, 7, 0, x, y + h, 10, 0, 0);
	Graphics_setColor(canvas->graphics, (int)0xffb3aa93);
	Graphics_fillRect(canvas->graphics, x - 7, y + 7, 7, h - 14);
	Graphics_setColor(canvas->graphics, (int)0xffe7cfad);
	Graphics_fillRect(canvas->graphics, x - 7, y + thumbY, 7, thumbH);
	Graphics_setColor(canvas->graphics, (int)0xff000000);
	Graphics_drawRect(canvas->graphics, x - 7, y + thumbY, 6, thumbH - 1);
	Graphics_drawRect(canvas->graphics, x - 7, y, 6, h - 1);
}

/*
 * 0x00010710
 * Parchment frame: four 60x13 corners, stretched 1px edges, a flat fill and,
 * on big frames outside the automap, stains and tears. style 0 = scroll,
 * otherwise the barter scroll.
 */
void Canvas_drawDialogFrame(Canvas *canvas, int x, int y, int w, int h, int style)
{
	Image *frame, *tears, *stains;
	int fill, right, bottom, i;

	if (style == 0) {
		fill = (int)0xffa8967e;
		frame = &canvas->imgBgScroll;
		tears = &canvas->imgBgScrollTears;
		stains = &canvas->imgBgScrollStains;
	} else {
		fill = (int)0xff99524b;
		frame = &canvas->imgBgScrollBarter;
		tears = &canvas->imgBgScrollTearsBarter;
		stains = &canvas->imgBgScrollStainsBarter;
	}

	right = x + w - 60;
	bottom = y + h - 13;
	Canvas_drawRegion(canvas, frame, 0, 0, 60, 13, 0, x, y, 0, 0, 0);
	Canvas_drawRegion(canvas, frame, 60, 0, 60, 13, 0, right, y, 0, 0, 0);
	Canvas_drawRegion(canvas, frame, 0, 13, 60, 13, 0, x, bottom, 0, 0, 0);
	Canvas_drawRegion(canvas, frame, 60, 13, 60, 13, 0, right, bottom, 0, 0, 0);
	for (i = 0; i < h - 26; i++) {
		int row = y + i + 13;
		Canvas_drawRegion(canvas, frame, 0, 13, 60, 1, 0, x, row, 0, 0, 0);
		Canvas_drawRegion(canvas, frame, 60, 13, 60, 1, 0, right, row, 0, 0, 0);
	}
	for (i = 0; i < w - 120; i++) {
		int col = x + i + 60;
		Canvas_drawRegion(canvas, frame, 60, 0, 1, 13, 0, col, y, 0, 0, 0);
		Canvas_drawRegion(canvas, frame, 60, 13, 1, 13, 0, col, bottom, 0, 0, 0);
	}
	if (w > 120) {
		Graphics_setColor(canvas->graphics, fill);
		Graphics_fillRect(canvas->graphics, x + 60, y + 13, w - 120, h - 26);
	}

	if (canvas->state != 5 && w > 160 && h > 160) {
		Canvas_drawRegion(canvas, stains, 0, 0, 59, 45, 0, x + 26, y + 38, 0, 0, 0);
		Canvas_drawRegion(canvas, stains, 60, 0, 43, 38, 0, x + w - 68, y + 100, 0, 0, 0);
		Canvas_drawRegion(canvas, stains, 0, 46, 43, 6, 0, x + 108, y + 56, 0, 0, 0);
		Canvas_drawRegion(canvas, stains, 60, 39, 33, 28, 0, x + 40, y + 104, 0, 0, 0);
		Canvas_drawRegion(canvas, tears, 0, 0, 13, 29, 0, x + 158, y + 43, 0, 0, 0);
		Canvas_drawRegion(canvas, tears, 14, 0, 12, 13, 0, x + 159, y + 85, 0, 0, 0);
		Canvas_drawRegion(canvas, tears, 14, 14, 12, 12, 0, x + 7, y + 50, 0, 0, 0);
		Canvas_drawRegion(canvas, tears, 27, 0, 6, 18, 0, x + 165, y + 113, 0, 0, 0);
		Canvas_drawRegion(canvas, tears, 34, 0, 10, 19, 0, x + 7, y + h - 80, 0, 0, 0);
		Canvas_drawRegion(canvas, tears, 45, 0, 12, 28, 0, x + 7, y + h - 65, 0, 0, 0);
	}
}

/* 0x00010c68 helper: the original's "!String_equals(s, empty) && length > 0" test. */
static int hasText(const String *s)
{
	return s->buf != NULL && s->len > 0;
}

/*
 * 0x00010c68
 * Soft-key labels on the 20px bar below the view. The labels are remembered
 * in Canvas.softKeyLeft/Right; their separator x positions in softKeyLeftX/RightX.
 */
void Canvas_drawSoftKeys_10c68(Canvas *canvas, String *leftStr, String *rightStr, int drawSeparators)
{
	int originX, y;

	if (canvas->hasStatusBar == 0)
		return;

	if (leftStr->buf != NULL || rightStr->buf != NULL) {
		if (!hasText(leftStr))
			canvas->softKeyLeftX = 0;
		if (!hasText(rightStr))
			canvas->softKeyRightX = 0;
	}

	originX = -canvas->viewRect[0];
	y = canvas->usableHeight - canvas->viewRect[1];
	if (drawSeparators)
		Canvas_drawSoftkeyBarBg(canvas, originX, y, canvas->screenRect[2]);

	if (canvas->waitTime != 0)
		return;

	if (hasText(leftStr)) {
		if (String_notEquals(&canvas->softKeyLeft, leftStr)) {
			String_setLength(&canvas->softKeyLeft, 0);
			String_appendString(&canvas->softKeyLeft, leftStr);
		}
		canvas->softKeyLeftX = Text_approxWidth(0, &canvas->softKeyLeft) + 8;
		if (drawSeparators) {
			int sx = canvas->softKeyLeftX + originX;
			Graphics_setColor(canvas->graphics, (int)0xff313131);
			Graphics_drawLine(canvas->graphics, sx, y, sx, y + 19);
			Graphics_setColor(canvas->graphics, (int)0xff808591);
			Graphics_drawLine(canvas->graphics, sx + 1, y, sx + 1, y + 19);
		}
		Canvas_drawString(canvas, leftStr, canvas->softKeyLeftX / 2, y + 5, 0x11);
	} else {
		String_setLength(&canvas->softKeyLeft, 0);
	}

	if (hasText(rightStr)) {
		if (String_notEquals(&canvas->softKeyRight, rightStr))
			String_assign(&canvas->softKeyRight, rightStr);
		canvas->softKeyRightX = canvas->screenRect[2] - Text_approxWidth(0, &canvas->softKeyRight) - 8;
		if (drawSeparators) {
			int sx = canvas->softKeyRightX + originX;
			Graphics_setColor(canvas->graphics, (int)0xff313131);
			Graphics_drawLine(canvas->graphics, sx, y, sx, y + 19);
			Graphics_setColor(canvas->graphics, (int)0xff808591);
			Graphics_drawLine(canvas->graphics, sx + 1, y, sx + 1, y + 19);
		}
		Canvas_drawString(canvas, rightStr,
				  canvas->screenRect[2] - (canvas->screenRect[2] - canvas->softKeyRightX) / 2, y + 5, 0x11);
	} else {
		String_setLength(&canvas->softKeyRight, 0);
	}
}

/* 0x00009ab8 */
void Canvas_clearSoftKeys(Canvas *canvas)
{
	String left, right;

	String_ctor(&left);
	String_ctor(&right);
	Canvas_drawSoftKeys_10c68(canvas, &left, &right, 1);
	String_dtor(&right, 0);
	String_dtor(&left, 0);
}

/* 0x000318f8 */
void Canvas_refreshSoftKeys(Canvas *canvas)
{
	Canvas_drawSoftKeys_10c68(canvas, &canvas->softKeyLeft, &canvas->softKeyRight, 1);
}

/* 0x000102bc  In-game soft keys: menu on the left, automap/back on the right. */
void Canvas_drawPlayingSoftKeys(Canvas *canvas)
{
	Menu *menu = g_app->menu;
	int rightId;

	String_setLength(&canvas->scratch, 0);
	String_setLength(&menu->scratch0, 0);
	Text_getString(canvas, 0, 0x46, &canvas->scratch);
	TextUtil_dehyphenate(g_app->textUtil, &canvas->scratch);

	if (canvas->state == 5) {
		rightId = 0x47;
	} else if (g_app->player->isFamiliar) {
		String none;

		String_ctor(&none);
		Canvas_drawSoftKeys_10c68(canvas, &canvas->scratch, &none, 1);
		String_dtor(&none, 0);
		return;
	} else {
		rightId = 0x48;
	}
	Text_getString(canvas, 0, rightId, &menu->scratch0);
	TextUtil_dehyphenate(g_app->textUtil, &menu->scratch0);
	Canvas_drawSoftKeys_10c68(canvas, &canvas->scratch, &menu->scratch0, 1);
}

/* 0x0002b7bc  Paints the menu, then its soft keys (back/exit + context action). */
void Canvas_drawSoftKeys_2b7bc(Canvas *canvas)
{
	MenuSystem *ms;
	Menu *menu = g_app->menu;
	uint32_t itemFlags;
	int rightId = -1;
	int drawSeparators = 1;

	MenuSystem_paint(g_app->menuSystem);
	ms = canvas->menuSystem;
	itemFlags = ms->items[ms->selectedIndex].flags;

	if (itemFlags & 0x20) {
		rightId = 0x42;
	} else if (ms->type == 5 || ms->type == 7 || ms->menu == 400) {
		/* text pages and item details: no right key */
	} else if (ms->menu == 0x18) {
		drawSeparators = 0;
	} else if (ms->menu == 0x135) {
		rightId = 0x40;
	} else if (ms->menu == 0x97 || (ms->menu == 0x98 && ms->selectedIndex > 1)) {
		rightId = 0x41;
	} else if ((itemFlags & 0x400) == 0) {
		rightId = 0xcf;
	}

	String_setLength(&canvas->scratch, 0);
	if (canvas->menuSystem->menu == 3 || canvas->menuSystem->menu == 0x14) {
		Text_getString(canvas, 3, 0x162, &canvas->scratch);
	} else if (canvas->menuSystem->backMenu != -1 && canvas->menuSystem->backMenu != canvas->menuSystem->menu) {
		Text_getString(canvas, 3, 0x4b, &canvas->scratch);
		TextUtil_dehyphenate(g_app->textUtil, &canvas->scratch);
	}

	String_setLength(&menu->scratch0, 0);
	if (rightId != -1) {
		Text_getString(canvas, 0, rightId, &menu->scratch0);
		TextUtil_dehyphenate(g_app->textUtil, &menu->scratch0);
	}
	Canvas_drawSoftKeys_10c68(canvas, &canvas->scratch, &menu->scratch0, drawSeparators);
}
