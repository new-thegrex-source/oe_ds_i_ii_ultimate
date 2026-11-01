/*
 * Full-screen Canvas states: logos (0), automap (5), story pages (9 / 0xf),
 * travel map (0xc), dying (0xd), credits scroller (0x11), error screen (0x13).
 */

#include "ui_local.h"

#define MOD_STR_BACK       0x2d5c8u
#define MOD_STR_MORE       0x2d5d0u
#define MOD_STR_ENTER      0x2d5a4u
#define MOD_STR_SORRY      0x2d638u
#define MOD_STR_CREDITS_TOO_BIG 0x2d2bcu
#define MOD_FMT_CREDITS    0x2d2dcu

#define RES_CREDITS        5142 /* credits.txt */
#define RES_MAP_BMP        5040 /* travel map */
#define RES_TRAVEL_ICONS   5013
#define SND_TRAVEL_STEP    0x138b
#define MUSIC_CREDITS      0x157c /* 5500 */
#define MUSIC_STORY        0x157d /* 5501 */
#define MENU_DIED          0x72

#define MAP_FIRST          6000
#define MAP_HUB            0x1778 /* 6008 */
#define MAP_MENU           0x1779 /* 6009 ui.bsp */

/* ---- automap ---- */

/* automap colours */
#define AM_FLOOR      ((int)0xfff1dfbdu)
#define AM_HATCH      ((int)0xff878787u)
#define AM_WALL       ((int)0xff746c59u)
#define AM_WALL_SPR   ((int)0xff8d8068u)
#define AM_CHEST      ((int)0xff33bb00u)
#define AM_NPC        ((int)0xff777777u)
#define AM_PORTAL     ((int)0xff3427feu)

/* map coordinate (64 units per tile) -> automap pixel offset; scale =
 * 0x4000 / cell, so this is coord * cell / 64 in 8.8 rounded */
static int amScale(int coord, int scale)
{
	return (int)((uint32_t)coord << 16) / scale;
}

/* 0x0002ad54
 * Marks the 3x3 tiles around the destination tile as seen on the automap,
 * except solid ones (flag 2) other than the centre. */
void Canvas_revealAutomap(Canvas *cv)
{
	int tx = cv->destX >> 6;
	int ty = cv->destY >> 6;
	int x, y;

	if (tx < 0 || tx >= 32 || ty < 0 || ty >= 32)
		return;
	for (y = ty - 1; y <= ty + 1; y++) {
		if (y < 0 || y >= 31)
			continue;
		for (x = tx - 1; x <= tx + 1; x++) {
			uint8_t *flags;
			if (x < 0 || x >= 31)
				continue;
			flags = &cv->render->mapFlags[y * 32 + x];
			if (!(*flags & 2) || (x == tx && y == ty))
				*flags |= 0x10;
		}
	}
}

/* 0x00008e84
 * The automap: a 32x32 grid scaled to the screen. full = 1 redraws the map
 * (parchment, seen floor, hatched event tiles, walls, doors and debug layers);
 * every frame draws the blinking player arrow. */
void Canvas_drawAutomap(Canvas *canvas, int full)
{
	Render *render = canvas->render;
	Game *game = canvas->game;
	int size, cell, ox, oy, scale, row, col, i, dir, sy, px, py;

	size = canvas->screenW - 8;
	if (canvas->softKeyBarY - 0x1c <= size)
		size = canvas->softKeyBarY - 0x1c;
	cell = size / 32;
	ox = canvas->screenW / 2 - cell * 16 - canvas->originX;
	oy = canvas->softKeyBarY / 2 - cell * 16 - canvas->originY;
	scale = 0x400000 / (cell << 8);

	if (full) {
		int thick;

		Ui_clearScreenRect(0, 0, canvas->screenW, canvas->softKeyBarY);
		Canvas_drawDialogFrame(canvas, -canvas->originX, -canvas->originY, canvas->screenW, canvas->softKeyBarY, 0);

		/* tiles and what stands on them */
		for (row = 0; row < 32; row++) {
			int cy = row * cell + oy;
			for (col = 0; col < 32; col++) {
				int idx = row * 32 + col;
				int cx = col * cell + ox;
				uint8_t flags = render->mapFlags[idx];
				int drawn = 0;
				Entity *ent;

				if (flags & 4) {
					/* lower-right half hatched: tiles with an event-line texture */
					int step = size >> 7, j, k;

					drawn = 1;
					Gfx_setColor(canvas->app, AM_FLOOR);
					Gfx_fillRect(canvas->app, cx, cy, cell, cell);
					if (step < 1)
						step = 1;
					for (j = 0; j < cell; j += step) {
						Gfx_setColor(canvas->app, (j & 1) ? UI_BLACK : AM_HATCH);
						for (k = 0; k < cell; k += step) {
							if (k + j >= cell - 1)
								Gfx_fillRect(canvas->app, cx + k, cy + j, step, step);
						}
					}
				} else if ((flags & 0x10) && !(flags & 1)) {
					drawn = 1;
					Gfx_setColor(canvas->app, AM_FLOOR);
					Gfx_fillRect(canvas->app, cx, cy, cell, cell);
				}

				for (ent = game->tileLists[idx]; ent != NULL; ent = ent->nextOnTile) {
					uint8_t eType;
					if (!drawn)
						continue;
					eType = ent->def->eType;
					if (eType == 3) {
						Gfx_setColor(canvas->app, AM_CHEST);
						Gfx_fillRect(canvas->app, cx + cell / 4, cy + cell / 4, cell / 2, cell / 2);
					} else if (eType == 7 || eType == 4) {
						Gfx_setColor(canvas->app, AM_NPC);
						Gfx_fillRect(canvas->app, cx + cell / 4, cy + cell / 4, cell / 2, cell / 2);
					} else if (eType == 0xf) {
						Gfx_setColor(canvas->app, AM_PORTAL);
						Gfx_fillRect(canvas->app, cx, cy, cell, cell);
					}
				}
			}
		}

		/* seen map walls */
		Gfx_setColor(canvas->app, AM_WALL);
		for (i = 0; i < render->numLines; i++) {
			Line *ln = &render->lines[i];
			if ((ln->flags & 0x80) && !(ln->flags & 0x40000) && !(ln->flags & 4))
				Gfx_drawLine(canvas->app, ((amScale(ln->x1, scale) + 0x80) >> 8) + ox,
				             ((amScale(ln->y1, scale) + 0x80) >> 8) + oy, ((amScale(ln->x2, scale) + 0x80) >> 8) + ox,
				             ((amScale(ln->y2, scale) + 0x80) >> 8) + oy);
		}

		/* wall-mounted sprites: walls of sprite type 0xc/0xd, doors 0xe */
		thick = size >> 6;
		for (row = 0; row < 32; row++) {
			int rowY = cell * row;
			for (col = 0; col < 32; col++) {
				int colX = cell * col;
				int idx = row * 32 + col;
				Entity *ent;

				for (ent = game->tileLists[idx]; ent != NULL; ent = ent->nextOnTile) {
					Sprite *spr;
					uint32_t info;
					uint8_t eType;

					if (ent->info & 0x200000)
						continue; /* line entity */
					spr = Render_spriteAt(render, (ent->info & 0xffff));
					info = spr->info;
					if (((info & 0x7fffff) >> 19) == 0 || !(info & 0x8000000))
						continue; /* not on a wall, or not seen */
					eType = ent->def->eType;
					if (eType == 0xc || eType == 0xd) {
						int x1, y1, x2, y2;

						Gfx_setColor(canvas->app, (info & 0x40000) ? AM_WALL : AM_WALL_SPR);
						info = spr->info;
						if (info & 0x80000) {
							x1 = amScale(spr->x - 0x20, scale);
							x2 = amScale(spr->x + 0x20, scale);
							y1 = y2 = amScale(spr->y, scale);
						} else if (info & 0x100000) {
							x1 = amScale(spr->x - 0x20, scale);
							x2 = amScale(spr->x + 0x20, scale);
							y1 = y2 = amScale(spr->y, scale);
						} else {
							x1 = x2 = amScale(spr->x, scale);
							y1 = amScale(spr->y - 0x20, scale);
							y2 = amScale(spr->y + 0x20, scale);
						}
						/* (the original rounds the first x with 0x81) */
						Gfx_drawLine(canvas->app, ((x1 + 0x81) >> 8) + ox, ((y1 + 0x80) >> 8) + oy,
						             ((x2 + 0x80) >> 8) + ox, ((y2 + 0x80) >> 8) + oy);
					} else if (eType == 0xe) {
						/* a bar on the wall side, if the tile on that side was seen */
						int x = colX, y = rowY, w = cell, h = cell;
						uint8_t side = 0x10;

						Gfx_setColor(canvas->app, ent->def->eSubType == 1 ? (int)0xffff00ffu : (int)0xff440044u);
						info = spr->info;
						if (info & 0x80000) {
							side = render->mapFlags[idx - 0x20];
							y = rowY - thick + 1;
							h = thick;
						} else if (info & 0x100000) {
							side = render->mapFlags[idx + 0x20];
							y = rowY + cell;
							h = thick;
						} else if (info & 0x200000) {
							/* the original reads the tile up and to the right here */
							side = render->mapFlags[idx - 0x1f];
							x = colX + cell;
							w = thick;
						} else if (info & 0x400000) {
							/* ... and the tile up and to the left here */
							side = render->mapFlags[idx - 0x21];
							x = colX - thick + 1;
							w = thick;
						}
						if (side & 0x10)
							Gfx_fillRect(canvas->app, ox + x, oy + y, w, h);
					}
				}
			}
		}

		/* doors (map line entities) */
		for (i = game->firstDoor; i <= game->lastDoor; i++) {
			Entity *ent = &game->entities[i];
			EntityDef *def = ent->def;
			Line *ln;
			int color;

			if (def->eType != 5 && def->eType != 10 && def->eSubType != 1)
				continue;
			ln = &render->lines[(ent->info & 0xffff) - 1];
			if (!(ln->flags & 0x80) || (ln->flags & 0x40))
				continue; /* not seen, or open */
			if (def->tileIndex == 0x115)
				color = (int)0xffff0000u;
			else if (def->tileIndex == 0x116)
				color = (int)0xff0000ffu;
			else if (def->tileIndex == 0x117)
				color = (int)0xff00ff00u;
			else
				color = (int)0xff4a3018u;
			Gfx_setColor(canvas->app, color);
			Gfx_drawLine(canvas->app, ((amScale(ln->x1, scale) + 0x80) >> 8) + ox,
			             ((amScale(ln->y1, scale) + 0x80) >> 8) + oy, ((amScale(ln->x2, scale) + 0x80) >> 8) + ox,
			             ((amScale(ln->y2, scale) + 0x80) >> 8) + oy);
		}

		/* debug: visible BSP leaves */
		if (canvas->showNodes) {
			Node *node;

			Gfx_setColor(canvas->app, (int)0xff0000ffu);
			for (node = canvas->app->render->leafSentinel.visNext; node != &canvas->app->render->leafSentinel;
			     node = node->visNext) {
				int x1 = ox + ((amScale(node->bbox[0], scale) + 0x80) >> 8);
				int y1 = oy + ((amScale(node->bbox[1], scale) + 0x80) >> 8);
				int x2 = ox + ((amScale(node->bbox[2], scale) + 0x80) >> 8);
				int y2 = oy + ((amScale(node->bbox[3], scale) + 0x80) >> 8);

				Gfx_drawLine(canvas->app, x1, y1, x2, y1);
				Gfx_drawLine(canvas->app, x1, y2, x2, y2);
				Gfx_drawLine(canvas->app, x1, y1, x1, y2);
				Gfx_drawLine(canvas->app, x2, y1, x2, y2);
			}
		}

		/* debug: monsters (inactive red, active green) */
		if (canvas->showMonsters) {
			static const uint32_t colors[2] = { 0xffff0000u, 0xff00ff00u };
			int l;

			for (l = 0; l < 2; l++) {
				Entity **ring = (l == 0) ? &game->inactiveMonsters : &game->activeMonsters;
				Entity *ent = *ring;

				Gfx_setColor(canvas->app, (int)colors[l]);
				if (ent == NULL)
					continue;
				do {
					if ((ent->info & 0x40020000) == 0) {
						Sprite *spr = Render_spriteAt(canvas->app->render, (ent->info & 0xffff));
						Gfx_fillRect(canvas->app, (spr->x >> 6) * cell + ox, (spr->y >> 6) * cell + oy, cell, cell);
					}
					ent = ent->monster->nextOnList;
				} while (*ring != ent);
			}
		}

		/* debug: lines drawn this frame */
		if (render->rShowLines) {
			for (i = 0; i < render->numLines; i++) {
				Line *ln = &render->lines[i];
				if (!(ln->flags & 0x800000))
					continue;
				Gfx_setColor(canvas->app, (ln->flags & 0x80000) ? (int)0xffff00ffu : (int)0xff00ff00u);
				Gfx_drawLine(canvas->app, ((amScale(ln->x1, scale) + 0x80) >> 8) + ox,
				             ((amScale(ln->y1, scale) + 0x80) >> 8) + oy, ((amScale(ln->x2, scale) + 0x80) >> 8) + ox,
				             ((amScale(ln->y2, scale) + 0x80) >> 8) + oy);
			}
		}
	}

	/* the player arrow, blinking every 333 ms */
	if (canvas->blinkTime < canvas->app->tickTime) {
		canvas->blinkTime = canvas->app->tickTime + 0x14d;
		canvas->blink = !canvas->blink;
	}
	switch (canvas->destAngle & 0x3ff) {
	case 0:
		dir = 2;
		break;
	case 0x200:
		dir = 3;
		break;
	case 0x300:
		dir = 1;
		break;
	default:
		dir = 0;
		break;
	}
	sy = dir * 6;
	if (canvas->blink)
		sy += 0x18;
	px = cell * (canvas->viewX - 0x20) / 64 + ox + cell / 2;
	py = cell * (canvas->viewY - 0x20) / 64 + oy + cell / 2;
	if (py < canvas->originY + canvas->viewH)
		Canvas_drawRegion(canvas, &canvas->automapCursor, 0, sy, 6, 6, 0, px, py, 0x30, 0, NULL);
}

/* 0x00005830  Canvas state 5 */
void Canvas_automapState(Canvas *canvas)
{
	int full;

	Game_updateLerpSprites(canvas->game);
	if (canvas->f249 == 0) {
		/* first frame: render the (untextured) view once, then the full map */
		canvas->f249 = 1;
		canvas->render->rNoTexture = 1;
		Canvas_updateMovement(canvas);
		canvas->render->rNoTexture = 0;
		if (canvas->state != 5)
			return;
		full = 1;
	} else {
		full = 0;
	}
	Canvas_drawAutomap(canvas, full);
}

/* ---- story pages, credits ---- */

/* 0x00006db8  blacks out the view and steps the story page */
void Canvas_fadeStep(Canvas *canvas, int step)
{
	if (step < 0 && (int8_t)canvas->storyPage == 0)
		return;
	Gfx_setColor(canvas->app, UI_BLACK);
	Gfx_fillRect(canvas->app, 0, 0, canvas->viewW, canvas->viewH);
	Gfx_setColor(canvas->app, UI_WHITE);
	canvas->storyPage = (int8_t)(canvas->storyPage + step);
}

/* 0x0001af28
 * Intro (5165, 4 pages) / epilogue (5117, 5 pages): 176x176 pages stacked in
 * one image. */
void Canvas_initStoryScreen(Canvas *canvas, int resId, int a, int b)
{
	Sound_play(canvas->app->sound, MUSIC_STORY, 3, 5, -1);
	Image_load(canvas->app, resId, 0, &canvas->storyImage);
	canvas->storyPage = (int8_t)a;
	canvas->storyPageCount = (int8_t)b;
	Ui_clearScreenRect(0, 0, canvas->viewW, canvas->viewH);
	IDISPLAY_Update();
}

/* 0x0000bab4  Canvas states 9 / 0xf */
void Canvas_drawStoryScreen(Canvas *canvas)
{
	char label[16];
	int page = (int8_t)canvas->storyPage; /* signed bytes (ldrsb) */
	int count = (int8_t)canvas->storyPageCount;
	int y, bob;

	if (page >= count)
		return;

	Ui_clearScreenRect(0, 0, canvas->screenW, canvas->screenH);
	Canvas_drawRegion(canvas, &canvas->storyScroll, 0, 0, 0xb0, 0xb0, 0, canvas->screenW >> 1, canvas->screenH >> 1,
	                  0x30, 0, NULL);
	Canvas_drawRegion(canvas, &canvas->storyImage, 0, page * 0xb0, 0xb0, 0xb0, 0, canvas->screenW >> 1,
	                  canvas->screenH >> 1, 0x30, 0xf, NULL);
	y = canvas->screenH - 0x15;
	bob = Ui_bob(canvas->app->tickTime / 100);
	if (page > 0)
		Canvas_drawString(canvas, Ui_str(MOD_STR_BACK), canvas->screenX + 0xe, y, 6, 0);
	if (page < count - 1)
		strcpy(label, Ui_str(MOD_STR_MORE));
	else
		strcpy(label, Ui_str(MOD_STR_CONTINUE));
	Canvas_drawImage(canvas, &canvas->app->menuSystem->cursor, canvas->screenW - (int)strlen(label) * 7 + bob - 0x12,
	                 y, 10, 0);
	Canvas_drawString(canvas, label, canvas->screenW - 0xe, y, 10, 0);
}

/* 0x00014b6c
 * Loads credits.txt under a " *** CREDITS *** " title (0x80 is a decorative
 * glyph) for the scroller and starts the credits music. */
void Canvas_initCredits(Canvas *canvas)
{
	InputStream is;
	int size, n;

	InputStream_ctor(&is, canvas->app, 1);
	if (!Resource_open(&is, RES_CREDITS, 1)) {
		App_setErrorCode(g_app, 0x16);
	} else if ((uint32_t)InputStream_getSize(&is) < 0x400) {
		n = App_snprintf(canvas->dialogText, 0x400, Ui_str(MOD_FMT_CREDITS), 0x80, 0x80, 0x80, 0x80, 0x80, 0x80);
		size = InputStream_getSize(&is);
		if (n + size > 0x3ff) /* the original does not check; keep inside the buffer */
			size = 0x3ff - n;
		memcpy(canvas->dialogText + n, InputStream_getBuffer(&is), (size_t)size);
		canvas->dialogText[n + size] = '\0';
		canvas->dialogText[0x3ff] = '\0';
		canvas->scrollStart = -1;
		canvas->scrollLines = Text_countLineBreaks(canvas->dialogText);
		Ui_clearScreen();
		IDISPLAY_Update();
		Sound_play(canvas->app->sound, MUSIC_CREDITS, 2, 5, -1);
	} else {
		App_setErrorCode(g_app, 1);
		App_setErrorStr(canvas->app, Ui_str(MOD_STR_CREDITS_TOO_BIG));
	}
	InputStream_dtor(&is, 0);
}

/* 0x0000a694
 * Canvas state 0x11: Canvas.dialogText scrolls up 1 px per 62 ms through a
 * 128-px window; 750 ms per line after the last one "Continue" appears. */
void Canvas_drawScrollingText(Canvas *canvas)
{
	int start, total, done;

	if (canvas->scrollStart == -1)
		canvas->scrollStart = (int)GETUPTIMEMS();
	start = canvas->scrollStart;
	total = canvas->scrollLines * 750 + 750;
	done = canvas->app->tickTime - start > total;
	if (done)
		start = canvas->app->tickTime - total; /* hold at the end */

	Ui_clearScreen();
	IDISPLAY_Update(); /* deferred (BREW UpdateEx FALSE): no black frame is ever shown */

	Canvas_drawString(canvas, canvas->dialogText, canvas->viewCenterX - 0x40,
	                  canvas->viewCenterY + 0x40 - (canvas->app->tickTime - start) / 0x3e, 0, 0);
	if (done) {
		Canvas_drawString(canvas, Ui_str(MOD_STR_CONTINUE), canvas->viewCenterX - 0x17, canvas->viewCenterY + 0x14,
		                  0x20, 8);
		Canvas_drawImage(canvas, &canvas->app->menuSystem->cursor,
		                 canvas->viewCenterX + Ui_bob(canvas->app->tickTime / 100) - 0x26, canvas->viewCenterY + 0x14,
		                 0x20, 0);
	}
	/* letterbox above and below the window */
	Gfx_setColor(canvas->app, 0);
	Gfx_fillRect(canvas->app, 0, -canvas->originY, canvas->viewW, canvas->originY + canvas->viewCenterY - 0x40);
	Gfx_fillRect(canvas->app, 0, canvas->viewCenterY + 0x40, canvas->viewW,
	             canvas->screenH - canvas->viewCenterY - 0x40);
}

/* ---- travel map ---- */

static int travelX(int point)
{
	return ModData_u16(&g_mod, MOD_TRAVEL_PATH + (uint32_t)point * 4);
}

static int travelY(int point)
{
	return ModData_u16(&g_mod, MOD_TRAVEL_PATH + (uint32_t)point * 4 + 2);
}

/* path point of map (6000..6008) */
static int travelNode(int mapId)
{
	return ModData_s32(&g_mod, MOD_TRAVEL_NODES + (uint32_t)(mapId - MAP_FIRST) * 4);
}

/* 0x000178c4
 * Travel map between levels: the path runs from the previous map's node to
 * the new one (backwards when going to a lower map). */
void Canvas_initTravelMap(Canvas *canvas)
{
	Image_load(canvas->app, RES_MAP_BMP, 1, &canvas->travelMap);
	Image_load(canvas->app, RES_TRAVEL_ICONS, 1, &canvas->travelNodes);
	canvas->travelStep = (canvas->mapId < canvas->prevMapId) ? -1 : 1;
	canvas->travelTo = (canvas->mapId < MAP_MENU) ? travelNode(canvas->mapId) : -1;
	if (canvas->prevMapId == 0 || canvas->prevMapId > 0x1777 || canvas->mapId == MAP_HUB)
		canvas->travelCur = canvas->travelTo;
	else
		canvas->travelCur = travelNode(canvas->prevMapId);
	canvas->travelFrom = canvas->travelCur;
	canvas->travelStart = -1;
}

/* 0x0000c7c8
 * Canvas state 0xc: map picture, level name, the nodes of the maps reached,
 * and the path walked one point per 200 ms (a step sound each); the new
 * node blinks next to "Enter". */
void Canvas_drawTravelMap(Canvas *canvas)
{
	int span, steps, walked, level, maxMap, bob, ox, oy, blink, i, p;

	if (canvas->travelStart == -1)
		canvas->travelStart = (int)GETUPTIMEMS();

	span = canvas->travelTo - canvas->travelFrom;
	if (span < 1)
		span = -span;
	steps = (canvas->app->tickTime - canvas->travelStart) / 200;
	if (steps > span)
		steps = span;
	walked = canvas->travelCur - canvas->travelFrom;
	if (walked < 1)
		walked = -walked;
	if (walked < steps) {
		canvas->travelCur = canvas->travelFrom + steps * canvas->travelStep;
		if (canvas->travelFrom != canvas->travelTo)
			Sound_play(canvas->app->sound, SND_TRAVEL_STEP, 0, 2, -1);
	}

	level = canvas->mapId - MAP_FIRST;
	maxMap = canvas->app->player->maxMapReached;
	bob = Ui_bob(canvas->app->tickTime / 100);
	ox = (int)((uint32_t)(canvas->screenW - canvas->travelMap.width) >> 1) - canvas->originX;
	oy = (int)((uint32_t)(canvas->screenH - canvas->travelMap.height) >> 1) - canvas->originY;
	Canvas_drawImage(canvas, &canvas->travelMap, ox, oy, 0, 0);
	blink = ((canvas->app->tickTime / 200) & 1) ? 0xf : 0xff; /* 0xff = not drawn */
	Canvas_drawString(canvas, canvas->game->levelNames + level * 0x18, canvas->screenW / 2 - canvas->originX,
	                  5 - canvas->originY, 0x11, 0);
	Gfx_setColor(canvas->app, UI_BLACK);

	for (i = 0; i <= 8; i++) {
		int node = ModData_s32(&g_mod, MOD_TRAVEL_NODES + (uint32_t)i * 4);

		if (i <= maxMap - MAP_FIRST && (canvas->travelTo != node || canvas->travelCur != canvas->travelTo))
			Canvas_drawRegion(canvas, &canvas->travelNodes, 0, i * 9, 0xe, 9, 0, travelX(node) + ox,
			                  travelY(node) + oy, 0x30, 0xf, NULL);
		if (canvas->travelFrom != node)
			continue;
		/* the path from this node up to the current point */
		for (p = canvas->travelFrom; canvas->travelCur + canvas->travelStep != p; p += canvas->travelStep) {
			if (canvas->travelTo == p) {
				Canvas_drawString(canvas, Ui_str(MOD_STR_ENTER), canvas->screenW - canvas->originX - 0xe,
				                  canvas->screenH - canvas->originY - 3, 10, 0);
				Canvas_drawImage(canvas, &canvas->app->menuSystem->cursor,
				                 canvas->screenW - canvas->originX + bob - 0x35, canvas->screenH - canvas->originY - 4,
				                 10, 0);
				Canvas_drawRegion(canvas, &canvas->travelNodes, 0, level * 9, 0xe, 9, 0, travelX(p) + ox,
				                  travelY(p) + oy, 0x30, blink, NULL);
			} else if (canvas->travelFrom == p) {
				Canvas_drawRegion(canvas, &canvas->travelNodes, 0, i * 9, 0xe, 9, 0, travelX(node) + ox,
				                  travelY(node) + oy, 0x30, 0xf, NULL);
			} else {
				Gfx_fillCircle(canvas->app, travelX(p) + ox, travelY(p) + oy, 3);
			}
		}
	}
}

/* ---- dying, error, logos ---- */

/* 0x0000d76c
 * Canvas state 0xd: the eye sinks to the floor over 750 ms, then the view
 * fades to black over 2250 ms; then the "you died" menu. */
void Canvas_updateDying(Canvas *canvas)
{
	Applet *app = canvas->app;
	int now = app->tickTime;
	int start = canvas->stateStartTime;

	if (now < start + 750) {
		int t = ((now - start) << 16) / 0x2ee00; /* 0..256 */
		canvas->viewZ = 0x24 - ((t * (0x24 - app->render->floorHeight) * 0x100) >> 16);
		Canvas_updateMovement(canvas);
		Canvas_updateShake(canvas);
		Hud_draw(canvas->hud);
		IDISPLAY_Update();
	} else if (now < start + 3000) {
		int t = ((now - start - 750) << 16) / 0x8ca00; /* 0..256 */
		Render_fadeView(canvas->render, (0xff - ((t * 0xff00) >> 16)) & 0xff);
		Canvas_updateShake(canvas);
		Hud_draw(canvas->hud);
		IDISPLAY_Update();
	} else {
		MenuSystem_setMenu(app->menuSystem, MENU_DIED);
	}
}

/* 0x0000e588  Canvas state 0x13 */
void Canvas_drawErrorScreen(Canvas *canvas)
{
	Ui_clearScreenRect(0, 0, canvas->viewW, canvas->viewH);
	Canvas_drawDialogFrame(canvas, 0, 0, canvas->viewW, canvas->viewH, 0);
	Canvas_drawString(canvas, Ui_str(MOD_STR_SORRY), canvas->viewCenterX - 0x40, canvas->viewCenterY - 0x40, 0, 0);
	Canvas_drawString(canvas, canvas->printMsg, canvas->viewCenterX - 0x40, canvas->viewCenterY - 0x28, 0, 0);
}

/* 0x0001bce0
 * Canvas state 0: four 128x128 logo frames, 1.5 s each or until a key; then
 * the main menu (or the error screen). */
void Canvas_updateLogos(Canvas *canvas)
{
	Applet *app = canvas->app;

	if (canvas->logoTime == 0) {
		canvas->logoTime = app->tickTime;
	} else if (canvas->keySkip || app->tickTime - canvas->logoTime > 0x5dc) {
		canvas->logoFrame++;
		canvas->logoTime = app->tickTime;
		canvas->keySkip = 0;
	}
	if (canvas->logoFrame < 4) {
		Ui_clearScreenRect(0, 0, canvas->screenW, canvas->screenH);
		Canvas_drawRegion(canvas, &canvas->logos, 0, canvas->logoFrame << 7, 0x80, 0x80, 0,
		                  canvas->viewCenterX - 0x40, canvas->viewCenterY - 0x40, 0, 0, NULL);
	} else {
		App_freeImage(app, &canvas->logos);
		if (canvas->errorFlag == 0)
			Canvas_backToMain(canvas, 1);
		else
			Canvas_setState(canvas, 0x13);
	}
}
