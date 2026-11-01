/*
 * Full-screen Canvas states and overlays: automap, intro/ending story pages,
 * scrolling credits, travel map, splash logos, loading screen and spinner,
 * the lock-pick progress row, error screen, death sequence, weapon/potion
 * quick menu and screen fades.
 *
 * Coordinates handed to Canvas_drawRegion/Graphics_* are relative to the view
 * origin (Canvas.viewRect x,y), exactly like the original.
 */

#include <stdio.h>

#include "game.h"

/* ---- tables read from the user's .mod ---- */
/* 8 signed bytes: small bob/jitter offsets used by blinking cursors */
#define MOD_BOB_OFFSETS        0x47168u
/* int32 per level (index mapId-1, 7 levels): travel-map path point of that level */
#define MOD_TRAVEL_LEVEL_POINT 0x46260u
/* int16 {x,y} per travel-map path point, relative to the map image */
#define MOD_TRAVEL_POINT_XY    0x4627cu
/* 3 x int32: height of each splash logo inside the logo strip image */
#define MOD_LOGO_HEIGHTS       0x46340u
/* " " separator string used by the quick menu */
#define MOD_STR_SPACE          0x463ccu
/* "\x87" (equipped-weapon glyph) used by the quick menu */
#define MOD_STR_EQUIPPED       0x46404u

#define NUM_TRAVEL_LEVELS 7

static int bobOffset(int i)
{
	return ModData_s8(&g_mod, MOD_BOB_OFFSETS + (uint32_t)i);
}

static int travelLevelPoint(int levelIdx)
{
	return ModData_s32(&g_mod, MOD_TRAVEL_LEVEL_POINT + (uint32_t)levelIdx * 4u);
}

static int travelPointX(int point)
{
	return ModData_s16(&g_mod, MOD_TRAVEL_POINT_XY + (uint32_t)point * 4u);
}

static int travelPointY(int point)
{
	return ModData_s16(&g_mod, MOD_TRAVEL_POINT_XY + (uint32_t)point * 4u + 2u);
}

/* String_appendInt (0x7750): snprintf "%d" then append. */
static void appendInt(String *s, int v)
{
	char buf[64];
	snprintf(buf, sizeof buf, "%d", v);
	String_appendCStr_40274(s, buf);
}

/* ======================================================================
 * Fades
 * ====================================================================== */

/* 0x0000964c  Clears the view to black and advances the story page by step
 * (a negative step only applies when not already on the first page). */
void Canvas_fadeStep(Canvas *canvas, int step)
{
	if (step >= 0 || canvas->storyPage != 0) {
		Graphics_setColor(canvas->graphics, (int)0xff000000);
		Graphics_fillRect(canvas->graphics, 0, 0, canvas->viewRect[2], canvas->viewRect[3]);
		canvas->storyPage += step;
	}
}

/* 0x00018da0  Timed framebuffer fade (camera/script fades). Returns 1 while
 * a fade is active. flags: 1 fade in (else out), 8 reverse once and snap the
 * view back to the saved position, 0x10 hold at the end. */
int Canvas_updateFade(Canvas *canvas)
{
	if (canvas->fadeStart == 0)
		return 0;

	if (canvas->fadeStart == -1)
		canvas->fadeStart = (int)GETUPTIMEMS();

	int elapsed = (int)GETUPTIMEMS() - canvas->fadeStart;
	if (elapsed > canvas->fadeDuration) {
		uint32_t flags = canvas->fadeFlags;
		if (flags & 0x10) {
			elapsed = canvas->fadeDuration;
		} else if ((flags & 8) && !(flags & 1)) {
			/* fade out finished: snap back and fade in again */
			canvas->fadeStart = (int)GETUPTIMEMS();
			canvas->fadeFlags |= 1;
			Canvas_resetPlayerPos(canvas, canvas->savedViewX, canvas->savedViewY);
			canvas->render->ceilHeight = canvas->savedViewZ;
			elapsed = 0;
		} else {
			canvas->fadeStart = 0;
			return 0;
		}
	}

	int ratio = (elapsed << 16) / (canvas->fadeDuration << 8);
	int level = ((ratio * 0xff00) >> 16) & 0xff;
	if ((canvas->fadeFlags & 1) == 0)
		level = (0xff - level) & 0xff;
	Render_fadeFramebuffer(canvas->render, level);
	return 1;
}

/* ======================================================================
 * Automap
 * ====================================================================== */

/* Map flags of a tile by linear index; neighbours of edge tiles can fall
 * outside the 32x32 grid (the original read the neighbouring fields). */
static uint16_t mapFlagAt(const Render *render, int idx)
{
	if (idx < 0 || idx >= 1024)
		return 0;
	return render->mapFlags[idx];
}

/* World coordinate -> automap pixel offset (16.8 fixed, rounded). */
static int amapScale(int v, int scale, int round)
{
	return ((v << 16) / scale + round) >> 8;
}

/* 0x00007a0c  State 5 per frame: the first frame renders the scene once
 * (forced span drawer) and paints the whole map; later frames only redraw the
 * blinking cursor. */
void Canvas_automapState(Canvas *canvas)
{
	Game_updateMovers(g_app->game);
	if (canvas->automapFirstFrame != 0) {
		Canvas_drawAutomap(canvas, 0);
		return;
	}
	canvas->automapFirstFrame = 1;
	canvas->render->dbgForceDrawer = 1;
	Canvas_updateMovement(canvas, 0, 0); /* original passed scratch registers; unused */
	canvas->render->dbgForceDrawer = 0;
	if (canvas->state == 5)
		Canvas_drawAutomap(canvas, 1);
}

/* Wall-sprite markers (eType 12/13 walls as lines, eType 14 as filled bars)
 * for the entities linked on tile idx. */
static void drawAutomapWallSprites(Canvas *canvas, int idx, int row, int col, int cell,
                                   int left, int top, int scale)
{
	Render *render = g_app->render;
	Game *game = g_app->game;
	Graphics *g = canvas->graphics;
	int half = cell >> 1;
	int colX = col * cell;
	int rowY = row * cell;

	for (Entity *ent = game->entityGrid[idx]; ent != NULL && ent != &game->entities[0];
	     ent = ent->nextOnTile) {
		if (ent == &game->entities[1] || ent == &game->entities[2])
			continue;
		if (ent->info & 0x200000) /* line entity */
			continue;
		int sprIdx = (int)(ent->info & 0xffff);
		if (sprIdx == 0)
			continue; /* original would read the field before sprites[0] */
		const Sprite *spr = &render->sprites[sprIdx - 1];
		uint32_t sinfo = spr->info;
		if (!(sinfo & 0x780000) || !(sinfo & 0x8000000))
			continue;

		int eType = ent->def->eType;
		if (eType == 12 || eType == 13) {
			Graphics_setColor(g, (sinfo & 0x40000) ? (int)0xff746c59 : (int)0xff8d8068);
			int x1, y1, x2, y2;
			if (sinfo & 0x80000) {
				if (!(mapFlagAt(render, idx - 0x20) & 0x20))
					continue;
				x1 = ((spr->x - 0x20) << 16) / scale;
				x2 = ((spr->x + 0x20) << 16) / scale;
				y1 = y2 = (spr->y << 16) / scale;
			} else if (sinfo & 0x100000) {
				if (!(mapFlagAt(render, idx + 0x20) & 0x20))
					continue;
				x1 = ((spr->x - 0x20) << 16) / scale;
				x2 = ((spr->x + 0x20) << 16) / scale;
				y1 = y2 = (spr->y << 16) / scale;
			} else {
				int n = (sinfo & 0x400000) ? idx - 0x21 : idx - 0x1f;
				if (!(mapFlagAt(render, n) & 0x20))
					continue;
				x1 = x2 = (spr->x << 16) / scale;
				y1 = ((spr->y - 0x20) << 16) / scale;
				y2 = ((spr->y + 0x20) << 16) / scale;
			}
			/* x1 rounds with 0x81 in the original */
			Graphics_drawLine(g, left + ((x1 + 0x81) >> 8), top + ((y1 + 0x80) >> 8),
			                  left + ((x2 + 0x80) >> 8), top + ((y2 + 0x80) >> 8));
		} else if (eType == 14) {
			Graphics_setColor(g, ent->def->eSubType == 1 ? (int)0xffff00ff : (int)0xff440044);
			int x = colX, y = rowY, w = cell, h = cell;
			if (sinfo & 0x80000) {
				if (!(mapFlagAt(render, idx - 0x20) & 0x20))
					continue;
				y = rowY - half + 1;
				h = half;
			} else if (sinfo & 0x100000) {
				if (!(mapFlagAt(render, idx + 0x20) & 0x20))
					continue;
				y = rowY + cell;
				h = half;
			} else if (sinfo & 0x200000) {
				if (!(mapFlagAt(render, idx - 0x1f) & 0x20))
					continue;
				x = colX + cell;
				w = half;
			} else if (sinfo & 0x400000) {
				if (!(mapFlagAt(render, idx - 0x21) & 0x20))
					continue;
				x = colX - half + 1;
				w = half;
			}
			Graphics_fillRect(g, left + x, top + y, w, h);
		}
	}
}

/* 0x0000cb24  Draws the 32x32 automap. full=0 only refreshes the cursor. */
void Canvas_drawAutomap(Canvas *canvas, int full)
{
	Render *render = g_app->render;
	Game *game = g_app->game;
	Graphics *g = canvas->graphics;

	int availW = canvas->screenRect[2] - 2;
	int availH = canvas->usableHeight - 0x16;
	int extent = availW >= availH ? availH : availW;
	int cell = extent / 32;
	int left = canvas->screenRect[2] / 2 - cell * 16 - canvas->viewRect[0];
	int top = canvas->usableHeight / 2 - cell * 16 - canvas->viewRect[1];
	int scale = 0x400000 / (cell << 8); /* world units per pixel, 8.8 */

	if (full) {
		Graphics_setColor(g, (int)0xff000000);
		Graphics_fillRect(g, 0, 0, canvas->screenRect[2], canvas->usableHeight);
		Canvas_drawDialogFrame(canvas, -canvas->viewRect[0], -canvas->viewRect[1],
		                       canvas->screenRect[2], canvas->usableHeight, 0);
	}

	if ((game->automapFlag == 0 || canvas->mapId != 7) && full) {
		int half = cell / 2;

		/* tiles */
		for (int row = 0; row < 32; row++) {
			int y = row * cell + top;
			for (int col = 0; col < 32; col++) {
				int idx = row * 32 + col;
				int x = col * cell + left;
				uint16_t f = render->mapFlags[idx];
				int showEnts = 0;

				if (f & 0x40) {
					Graphics_setColor(g, (int)0xff00ff00);
					Graphics_fillRect(g, x, y, cell, cell);
				} else if (f & 0x80) {
					/* yellow hatched tile */
					Graphics_setColor(g, (int)0xffffff00);
					Graphics_fillRect(g, x, y, cell, cell);
					Graphics_setColor(g, (int)0xff000000);
					for (int i = 0; i < cell; i += 2)
						Graphics_drawLine(g, x, y + i, x + cell, y + i);
					Graphics_drawLine(g, x, y, x, y + cell);
					Graphics_drawLine(g, x + cell, y, x + cell, y + cell);
				} else if (f & 0x20) { /* seen */
					if (f & 8) {
						Graphics_setColor(g, (int)0xffff0000);
						Graphics_fillRect(g, x, y, cell, cell);
					} else if (f & 0x400) {
						Graphics_setColor(g, (int)0xfff1dfbd);
						Graphics_fillRect(g, x, y, cell, cell);
						Graphics_setColor(g, (int)0xff777777);
						Graphics_fillRect(g, x + half, y + half, half, half);
					} else if (f & 0x200) {
						showEnts = 1;
						Graphics_setColor(g, (int)0xfff1dfbd);
						Graphics_fillRect(g, x, y, cell, cell);
						Graphics_setColor(g, (int)0xff746c59);
						Graphics_drawRect(g, x, y, cell, cell);
					} else if (!(f & 1)) {
						showEnts = 1;
						Graphics_setColor(g, (int)0xfff1dfbd);
						Graphics_fillRect(g, x, y, cell, cell);
						Graphics_setColor(g, (int)0xff746c59);
						/* edges against seen 0x200/0x800 neighbours above/left */
						if (row > 1) {
							uint16_t up = render->mapFlags[idx - 0x20];
							if ((up & 0x20) && (up & 0xa00))
								Graphics_drawLine(g, x, y, x + cell, y);
						}
						if (col > 1) {
							uint16_t lf = render->mapFlags[idx - 1];
							if ((lf & 0x20) && (lf & 0xa00))
								Graphics_drawLine(g, x, y, x, y + cell);
						}
					} else if (f & 0x800) {
						Graphics_setColor(g, (int)0xff746c59);
						Graphics_drawRect(g, x, y, cell, cell);
					}
				}

				/* items / pickups on walkable seen tiles */
				for (Entity *ent = game->entityGrid[idx]; ent != NULL; ent = ent->nextOnTile) {
					if (!showEnts)
						continue;
					int eType = ent->def->eType;
					int color;
					if (eType == 3)
						color = (int)0xff33bb00;
					else if ((eType == 7 && ent->def->eSubType != 7) || eType == 4)
						color = (int)0xff777777;
					else
						continue;
					Graphics_setColor(g, color);
					Graphics_fillRect(g, x + half, y + half, half, half);
				}
			}
		}

		/* walls that have been seen */
		Graphics_setColor(g, (int)0xff746c59);
		for (int i = 0; i < render->numLines; i++) {
			const Line *ln = &render->lines[i];
			if ((ln->flags & 0x80) && !(ln->flags & 0x40000) && !(ln->flags & 4)) {
				Graphics_drawLine(g, left + amapScale(ln->x1, scale, 0x80), top + amapScale(ln->y1, scale, 0x80),
				                  left + amapScale(ln->x2, scale, 0x80), top + amapScale(ln->y2, scale, 0x80));
			}
		}

		/* wall sprites (fences, bars, portcullises) */
		for (int row = 0; row < 32; row++)
			for (int col = 0; col < 32; col++)
				drawAutomapWallSprites(canvas, row * 32 + col, row, col, cell, left, top, scale);

		/* doors, coloured by key */
		for (int e = game->firstLineEntity; e <= game->lastEntity; e++) {
			Entity *ent = &game->entities[e];
			EntityDef *def = ent->def;
			if (def->eType != 5 && def->eType != 10 && def->eSubType != 1)
				continue;
			const Line *ln = &render->lines[(ent->info & 0xffff) - 1];
			if (!(ln->flags & 0x80) || (ln->flags & 0x40))
				continue;
			int color;
			uint16_t tile = (uint16_t)def->tileIndex;
			if (tile == 0x115)
				color = (int)0xffff0000;
			else if (tile == 0x116)
				color = (int)0xff0000ff;
			else if (tile == 0x117)
				color = (int)0xff00ff00;
			else
				color = (int)0xff4a3018;
			int x1 = amapScale(ln->x1, scale, 0x80), y1 = amapScale(ln->y1, scale, 0x80);
			int x2 = amapScale(ln->x2, scale, 0x80), y2 = amapScale(ln->y2, scale, 0x80);
			Graphics_setColor(g, color);
			Graphics_drawLine(g, left + x1, top + y1, left + x2, top + y2);
		}

		/* debug: boxes of the visible BSP leaves (list built by Render_walkBSP) */
		if (canvas->showNodes) {
			Graphics_setColor(g, (int)0xff0000ff);
			for (Node *n = render->leafSentinel.visNext; n != &render->leafSentinel; n = n->visNext) {
				int x1 = left + amapScale(n->x1, scale, 0x80);
				int y1 = top + amapScale(n->y1, scale, 0x80);
				int x2 = left + amapScale(n->x2, scale, 0x80);
				int y2 = top + amapScale(n->y2, scale, 0x80);
				Graphics_drawLine(g, x1, y1, x2, y1);
				Graphics_drawLine(g, x1, y2, x2, y2);
				Graphics_drawLine(g, x1, y1, x1, y2);
				Graphics_drawLine(g, x2, y1, x2, y2);
			}
		}

		/* debug: monsters (red inactive, green active) */
		if (canvas->showMonsters) {
			Graphics_setColor(g, (int)0xffff0000);
			Entity *ent = game->inactiveMonsters;
			if (ent != NULL) {
				do {
					const Sprite *spr = &render->sprites[(ent->info & 0xffff) - 1];
					if ((ent->info & 0x20020000) == 0)
						Graphics_fillRect(g, (spr->x >> 6) * cell + left, (spr->y >> 6) * cell + top, cell, cell);
					ent = ent->monster->nextOnList;
				} while (game->inactiveMonsters != ent);
			}
			Graphics_setColor(g, (int)0xff00ff00);
			ent = game->activeMonsters;
			if (ent != NULL) {
				do {
					const Sprite *spr = &render->sprites[(ent->info & 0xffff) - 1];
					if ((ent->info & 0x20020000) == 0)
						Graphics_fillRect(g, (spr->x >> 6) * cell + left, (spr->y >> 6) * cell + top, cell, cell);
					ent = ent->monster->nextOnList;
				} while (game->activeMonsters != ent);
			}
		}

		/* debug: lines flagged 0x800000 */
		if (render->dbgClearLineFlag) {
			for (int i = 0; i < render->numLines; i++) {
				const Line *ln = &render->lines[i];
				if (ln->flags & 0x800000) {
					Graphics_setColor(g, (int)0xff00ff00);
					Graphics_drawLine(g, left + amapScale(ln->x1, scale, 0x80), top + amapScale(ln->y1, scale, 0x80),
					                  left + amapScale(ln->x2, scale, 0x80), top + amapScale(ln->y2, scale, 0x80));
				}
			}
		}
	}

	/* blinking player cursor (toggles every 333 ms) */
	if (g_app->time > canvas->blinkTime) {
		canvas->blinkTime = g_app->time + 333;
		canvas->blinkState = ((unsigned)canvas->blinkState > 1) ? 0 : 1 - canvas->blinkState;
	}

	int px, py, angle;
	if (game->automapFlag != 0 && canvas->mapId != 7) {
		px = canvas->gotoX;
		py = canvas->gotoY;
		angle = canvas->gotoAngle;
	} else {
		px = canvas->viewX;
		py = canvas->viewY;
		angle = canvas->viewAngle;
	}
	int dir = 0;
	switch (angle & 0x3ff) {
	case 0x000: dir = 2; break;
	case 0x100: dir = 0; break;
	case 0x200: dir = 3; break;
	case 0x300: dir = 1; break;
	default: break;
	}
	int frameY = dir * 6;
	if (canvas->blinkState)
		frameY += 0x18;
	int cx = left + ((px - 0x20) * cell) / 64 + cell / 2;
	int cy = top + ((py - 0x20) * cell) / 64 + cell / 2;
	if (cy < canvas->viewRect[1] + canvas->viewRect[3])
		Canvas_drawRegion(canvas, &canvas->imgAutomapCursor, 0, frameY, 6, 6, 0, cx, cy, 0x30, 0, 0);
}

/* 0x00043498  Marks the 3x3 tiles around the player's destination as seen
 * (automap), except solid tiles and tiles holding a closed wall object. */
void Canvas_revealAutomap(Canvas *cv)
{
	Render *render = g_app->render;
	if (cv->game->automapFlag != 0)
		return;

	int tx = cv->destX >> 6;
	int ty = cv->destY >> 6;
	if (tx < 0 || tx >= 32 || ty < 0 || ty >= 32)
		return;

	for (int y = ty - 1; y <= ty + 1; y++) {
		if (y < 0 || y >= 31)
			continue;
		for (int x = tx - 1; x <= tx + 1; x++) {
			if (x < 0 || x >= 31)
				continue;
			int idx = x + y * 32;
			int solid = render->mapFlags[idx] & 2;
			if (x == tx && y == ty) {
				/* the possessed familiar may stand inside a solid tile */
				if (solid && g_app->player->isFamiliar)
					continue;
			} else if (solid) {
				continue;
			}
			Entity *first = (Entity *)Game_getEntitiesAt(g_app->game, x << 6, y << 6);
			if (first != NULL && first->def->eType == 12 && first->def->eSubType == 1)
				continue;
			render->mapFlags[idx] |= 0x20;
		}
	}
}

/* ======================================================================
 * Story pages / credits
 * ====================================================================== */

/* 0x000298f0  Shared setup of the intro (state 8) and ending (state 0xd)
 * picture pages. */
void Canvas_initStoryScreen(Canvas *canvas, int bmpIdx, int a, int b)
{
	Sound_play(g_app->sndSys, 0x53, 3, 5, -1);
	Image_load(g_app, bmpIdx, &canvas->imgStoryPages, 1);
	canvas->storyPage = a;
	canvas->storyPageCount = b;
	Graphics_setColor(canvas->graphics, (int)0xff000000);
	Graphics_fillRect(canvas->graphics, 0, 0, canvas->viewRect[2], canvas->viewRect[3]);
}

/* 0x0001f4a4  Intro story: 4 pages from bitmap 0x20. */
void Canvas_initScrollScreen8(Canvas *canvas)
{
	Canvas_initStoryScreen(canvas, 0x20, 0, 4);
}

/* 0x0001f494  Ending story: 2 pages from bitmap 10. */
void Canvas_initScrollScreen13(Canvas *canvas)
{
	Canvas_initStoryScreen(canvas, 10, 0, 2);
}

/* 0x000111bc  One 176x176 story page on the scroll background with
 * "Back" and "More"/"Continue" prompts. */
void Canvas_drawStoryScreen(Canvas *canvas)
{
	if (canvas->storyPage >= canvas->storyPageCount)
		return;

	int sw = canvas->screenRect[2], sh = canvas->screenRect[3];
	Graphics_setColor(canvas->graphics, (int)0xff000000);
	Graphics_fillRect(canvas->graphics, 0, 0, sw, sh);
	Canvas_drawRegion(canvas, &canvas->imgScrollStory, 0, 0, 0xb0, 0xb0, 0, sw >> 1, sh >> 1, 0x30, 0, 0);
	Canvas_drawRegion(canvas, &canvas->imgStoryPages, 0, canvas->storyPage * 0xb0, 0xb0, 0xb0, 0,
	                  sw >> 1, sh >> 1, 0x30, 0xb, 0);

	int left = (sw >> 1) - 0x58;
	int y = (sh >> 1) + 0x58 - 8;
	int bob = bobOffset((g_app->time / 100) & 3);

	if (canvas->storyPage > 0) {
		String_setLength(&canvas->scratch, 0);
		Text_getString(canvas, 3, 0x4b, &canvas->scratch);
		TextUtil_dehyphenate(g_app->textUtil, &canvas->scratch);
		Canvas_drawString(canvas, &canvas->scratch, left + 0x14, y, 6);
	}

	String_setLength(&canvas->scratch, 0);
	Text_getString(canvas, 0, canvas->storyPage < canvas->storyPageCount - 1 ? 0x49 : 0x31, &canvas->scratch);
	TextUtil_dehyphenate(g_app->textUtil, &canvas->scratch);
	int right = left + 0x9c;
	Canvas_drawImageSimple(canvas, &g_app->menuSystem->imgCursor,
	                       right - canvas->scratch.len * 7 + bob - 5, y, 10);
	Canvas_drawString(canvas, &canvas->scratch, right, y, 10);
}

/* 0x0001f3ac  Loads the credits text (help group 2, entry 0) into the dialog
 * text with '|' turned into newlines and starts the scroller. */
void Canvas_initCredits(Canvas *canvas)
{
	String *s = &canvas->dialogText;
	String_setLength(s, 0);
	Text_readStringFromRes(canvas, 2, 0, s);
	TextUtil_dehyphenate(g_app->textUtil, s);
	for (int i = 0; i < s->len; i++) {
		if ((uint8_t)s->buf[i] == '|')
			String_setCharAt(s, i, '\n');
	}
	canvas->scrollStartTime = -1;
	canvas->scrollLines = TextUtil_countLines(g_app->textUtil, s) + 5;
	Graphics_setColor(canvas->graphics, (int)0xff000000);
	Graphics_fillRect(canvas->graphics, 0, 0, canvas->screenRect[2], canvas->screenRect[3]);
}

/* 0x0000f1dc  Credits/epilogue scroller: the text rises one pixel per 62 ms
 * inside a 128px window; after 750 ms per line it stops and shows the
 * blinking "Continue" prompt. */
void Canvas_drawScrollingText(Canvas *canvas)
{
	if (canvas->scrollStartTime == -1)
		canvas->scrollStartTime = App_getUpTimeMS();

	int start = canvas->scrollStartTime;
	int total = canvas->scrollLines * 750 + 750;
	int finished = 0;
	if (g_app->time - start > total) {
		start = g_app->time - total;
		finished = 1;
	}

	Graphics_setColor(canvas->graphics, (int)0xff000000);
	Graphics_fillRect(canvas->graphics, 0, 0, canvas->screenRect[2], canvas->screenRect[3]);
	int scroll = (g_app->time - start) / 62;
	Canvas_drawString(canvas, &canvas->dialogText, canvas->screenCenterX - 0x40,
	                  canvas->screenCenterY + 0x40 - scroll, 0);

	if (finished) {
		String_setLength(&canvas->scratch, 0);
		Text_getString(canvas, 0, 0x31, &canvas->scratch);
		TextUtil_dehyphenate(g_app->textUtil, &canvas->scratch);
		Canvas_drawStringFlags(canvas, &canvas->scratch, canvas->screenCenterX - 0x17,
		                       canvas->screenCenterY + 0x14, 0x20, 8);
		int bob = bobOffset((g_app->time / 100) % 4);
		Canvas_drawImageSimple(canvas, &g_app->menuSystem->imgCursor,
		                       canvas->screenCenterX + bob - 0x26, canvas->screenCenterY + 0x14, 0x20);
	}

	/* mask everything outside the scroll window */
	Graphics_setColor(canvas->graphics, 0);
	Graphics_fillRect(canvas->graphics, 0, -canvas->viewRect[1], canvas->viewRect[2],
	                  canvas->viewRect[1] + canvas->screenCenterY - 0x40);
	Graphics_fillRect(canvas->graphics, 0, canvas->screenCenterY + 0x40, canvas->viewRect[2],
	                  canvas->screenRect[3] - canvas->screenCenterY - 0x40);
}

/* ======================================================================
 * Travel map (state 0xb)
 * ====================================================================== */

/* 0x000240b0  Loads the world map, works out the path from the previous
 * level's point to the new one and plays the travel music. */
void Canvas_initTravelMap(Canvas *canvas)
{
	Image_load(g_app, 0x2a, &canvas->imgTravelMap, 1);
	Image_load(g_app, 0x2b, &canvas->imgMapNode, 1);

	int prev = canvas->prevMapId;
	int map = canvas->mapId;
	canvas->travelStep = map < prev ? -1 : 1;
	canvas->travelTo = map <= NUM_TRAVEL_LEVELS ? travelLevelPoint(map - 1) : -1;

	int from;
	if (prev == 0 || prev > NUM_TRAVEL_LEVELS)
		from = canvas->travelTo;
	else
		from = travelLevelPoint(prev - 1);
	canvas->travelNode = from;
	canvas->travelStartTime = -1;
	canvas->drawLoading = from; /* +0xc78 holds the start point in this state */
	Sound_play(g_app->sndSys, 0x53, 1, 2, -1);
}

/* 0x00012388  World map: visited level markers, the path walked one point
 * per 200 ms from the previous level, the level name and "Enter". */
void Canvas_drawTravelMap(Canvas *canvas)
{
	Graphics *g = canvas->graphics;
	int from = canvas->drawLoading;

	Graphics_setColor(g, (int)0xff000000);
	Graphics_fillRect(g, 0, 0, canvas->screenRect[2], canvas->screenRect[3]);
	if (canvas->travelStartTime == -1)
		canvas->travelStartTime = App_getUpTimeMS();

	/* advance the walker */
	int dist = Math_abs(canvas->travelTo - from);
	int steps = (g_app->time - canvas->travelStartTime) / 200;
	if (steps > dist)
		steps = dist;
	if (Math_abs(canvas->travelNode - from) < steps)
		canvas->travelNode = from + canvas->travelStep * steps;

	int levelIdx = canvas->mapId - 1;
	int lastVisited = g_app->player->highestMap - 1;
	int bob = bobOffset((g_app->time / 200) % 4);

	int ox = (canvas->screenRect[2] - canvas->imgTravelMap.width) / 2 - canvas->viewRect[0];
	int oy = (canvas->screenRect[3] - canvas->imgTravelMap.height) / 2 - canvas->viewRect[1];
	Canvas_drawImageSimple(canvas, &canvas->imgTravelMap, ox, oy, 0);

	int blinkMode = ((g_app->time / 300) & 1) == 0 ? 0xc : 0;
	Graphics_setColor(g, (int)0xff000000);

	for (int i = 0; i < NUM_TRAVEL_LEVELS; i++) {
		int pt = travelLevelPoint(i);
		if (i <= lastVisited && !(canvas->travelTo == pt && canvas->travelNode == canvas->travelTo)) {
			Canvas_drawRegion(canvas, &canvas->imgMapNode, 0, 0, 0x14, 0x14, 0,
			                  travelPointX(pt) + ox, travelPointY(pt) + oy, 0x30, 0, 0);
		}
		if (canvas->drawLoading != pt)
			continue;
		for (int k = canvas->drawLoading; canvas->travelNode + canvas->travelStep != k; k += canvas->travelStep) {
			if (canvas->travelTo == k) {
				Canvas_drawRegion(canvas, &canvas->imgMapNode, 0, 0, 0x14, 0x14, 0,
				                  travelPointX(k) + ox, travelPointY(k) + oy, 0x30, blinkMode, 0);
			} else if (canvas->drawLoading == k) {
				Canvas_drawRegion(canvas, &canvas->imgMapNode, 0, 0, 0x14, 0x14, 0,
				                  travelPointX(pt) + ox, travelPointY(pt) + oy, 0x30, 0, 0);
			}
		}
	}

	String_setLength(&canvas->scratch, 0);
	Text_getString(canvas, 3, g_app->game->levelNames[levelIdx], &canvas->scratch);
	TextUtil_dehyphenate(g_app->textUtil, &canvas->scratch);
	Canvas_drawString(canvas, &canvas->scratch, ox + canvas->imgTravelMap.width / 2, oy + 4, 0x11);

	for (int i = 0; i < NUM_TRAVEL_LEVELS; i++) {
		int k = canvas->drawLoading;
		if (k != travelLevelPoint(i))
			continue;
		for (; canvas->travelNode + canvas->travelStep != k; k += canvas->travelStep) {
			if (canvas->travelTo == k) {
				String_setLength(&canvas->scratch, 0);
				Text_getString(canvas, 0, 0x30, &canvas->scratch);
				TextUtil_dehyphenate(g_app->textUtil, &canvas->scratch);
				int bx = canvas->imgTravelMap.width + ox;
				int by = canvas->imgTravelMap.height + oy - 4;
				Canvas_drawString(canvas, &canvas->scratch, bx - 7, by, 10);
				Canvas_drawImageSimple(canvas, &g_app->menuSystem->imgCursor, bx + bob - 0x2e, by, 10);
			} else if (canvas->drawLoading != k) {
				Graphics_fillCircle(g, travelPointX(k) + ox, travelPointY(k) + oy, 3);
			}
		}
	}
}

/* 0x0000c13c  Stops the music and frees the travel-map images. */
void Canvas_freeIntroImages(Canvas *canvas)
{
	Sound_requestStopAll(g_app->sndSys);
	App_freeImage(g_app, &canvas->imgTravelMap);
	App_freeImage(g_app, &canvas->imgMapNode);
}

/* ======================================================================
 * Logos, loading, errors
 * ====================================================================== */

/* 0x0002b49c  Three splash logos, 2 s each (a key press skips via +0x378),
 * then the main menu (or the error state when +0xcd4 is set). */
int Canvas_updateLogos(Canvas *canvas, ...)
{
	if (canvas->logoTime == 0) {
		canvas->logoTime = g_app->time;
	} else if (canvas->f378 != 0 || g_app->time - canvas->logoTime > 2000) {
		canvas->logoIdx++;
		canvas->logoTime = g_app->time;
		canvas->f378 = 0;
	}

	if (canvas->logoIdx > 2) {
		App_freeImage(g_app, &canvas->imgLogos);
		if (canvas->fcd4 == 0)
			Canvas_backToMain(canvas, 1);
		else
			Canvas_setState(canvas, 0x10);
		return 0;
	}

	int heights[3];
	for (int i = 0; i < 3; i++)
		heights[i] = ModData_s32(&g_mod, MOD_LOGO_HEIGHTS + (uint32_t)i * 4u);
	int w = canvas->imgLogos.width;
	int sy = 0;
	for (int i = 0; i < canvas->logoIdx; i++)
		sy += heights[i];

	Graphics_setColor(canvas->graphics, (int)0xff000000);
	Graphics_fillRect(canvas->graphics, 0, 0, canvas->screenRect[2], canvas->screenRect[3]);
	int h = heights[canvas->logoIdx];
	Canvas_drawRegion(canvas, &canvas->imgLogos, 0, sy, w, h, 0, canvas->screenCenterX - 0x40,
	                  canvas->screenRect[3] / 2 - h / 2, 0, 0, 0);
	return 0;
}

/* 0x00010188  Eight-box progress spinner, one step per 75 ms (or every call
 * when forced); pushes the screen to the display itself. */
void Canvas_drawLoadingSpinner(Canvas *canvas, int x, int y, int force)
{
	if (!force) {
		int now = App_getUpTimeMS();
		if (now - canvas->spinnerTime < 75)
			return;
		canvas->spinnerTime = now;
	}
	Graphics_setColor(canvas->graphics, (int)0xff000000);
	Graphics_fillRect(canvas->graphics, x - 0x1b, y, 0x38, 7);
	Graphics_setColor(canvas->graphics, (int)0xffffffff);
	for (int i = 0; i < 8; i++) {
		if (canvas->spinnerFrame == i)
			Graphics_fillRect(canvas->graphics, i * 7 + x - 0x1b, y, 5, 7);
		else
			Graphics_drawRect(canvas->graphics, i * 7 + x - 0x1b, y, 4, 6);
	}
	if (++canvas->spinnerFrame > 7)
		canvas->spinnerFrame = 0;
	IDISPLAY_Update();
}

/* 0x00044370  Loading screen: box with the loading text (or the default
 * "Loading" string), a second line and the spinner. Redrawn at most every
 * 80 ms (30 ms with fast loading). */
void Canvas_drawLoading(Canvas *cv)
{
	int interval = cv->fastLoading ? 30 : 80;
	int elapsed = App_getUpTimeMS() - cv->spinnerTime;
	if (elapsed < interval)
		return;
	cv->spinnerTime += elapsed;

	Graphics_setColor(cv->graphics, (int)0xff000000);
	Graphics_fillRect(cv->graphics, 0, 0, cv->screenRect[2], cv->screenRect[3]);
	Graphics_setColor(cv->graphics, (int)0xffffffff);
	Graphics_drawRect(cv->graphics, cv->screenCenterX - 0x36, cv->screenCenterY - 0x11, 0x6c, 0x2a);

	String *title;
	if (cv->loadingText.len > 0) {
		title = &cv->loadingText;
	} else {
		String_setLength(&cv->scratch, 0);
		Text_getString(cv, 0, 0x4a, &cv->scratch);
		TextUtil_dehyphenate(g_app->textUtil, &cv->scratch);
		title = &cv->scratch;
	}
	Canvas_drawString(cv, title, cv->screenCenterX, cv->screenCenterY - 0xc, 0x11);

	String_setLength(&cv->scratch, 0);
	Text_getString(cv, 0, 0x4b, &cv->scratch);
	TextUtil_dehyphenate(g_app->textUtil, &cv->scratch);
	Canvas_drawString(cv, &cv->scratch, cv->screenCenterX, cv->screenCenterY, 0x11);
	Canvas_drawLoadingSpinner(cv, cv->screenCenterX, cv->screenCenterY + 0xd, 1);
}

/* 0x00028fdc  Saves the config, sets the loading text and enters state 6. */
void Canvas_startLoading(Canvas *canvas, int type, int group, int strId)
{
	canvas->loadType = type;
	Game_saveConfig(g_app->game);
	String_setLength(&canvas->loadingText, 0);
	Text_getString(canvas, group, strId, &canvas->loadingText);
	TextUtil_dehyphenate(g_app->textUtil, &canvas->loadingText);
	Canvas_setState(canvas, 6);
}

/* 0x0002b19c  Lock-pick state (0x13) frame: the playing view, a "Stop"
 * soft key and one box per combination digit (green = solved, yellow = first
 * hit of the current digit). When the lock is open: finish the use, back to
 * play and award XP. */
void Canvas_drawSavingProgress(Canvas *canvas)
{
	if (canvas->lockDone) {
		Canvas_finishUse(canvas);
		Canvas_clearSoftKeys(canvas);
		Canvas_setState(canvas, 3);
		Player *player = g_app->player;
		uint32_t a = (uint32_t)player->lockpickTries, b = (uint32_t)player->f270;
		int num = (int)(((a & 0xff00ffffu) - (b & 0xff00ffffu)) << 8);
		int den = (player->lockpickTries >> 16) - (player->f270 >> 16);
		Player_addXP(player, (num / den >> 6) + 1);
		canvas->frameSkip += 5;
		return;
	}

	Canvas_updatePlaying(canvas);
	String_setLength(&canvas->scratch, 0);
	Text_getString(canvas, 0, 0x3f, &canvas->scratch);
	TextUtil_dehyphenate(g_app->textUtil, &canvas->scratch);
	String empty;
	String_ctor(&empty);
	Canvas_drawSoftKeys_10c68(canvas, &canvas->scratch, &empty, 1);
	String_dtor(&empty, 0);

	int digits = TextUtil_strlenZ(g_app->textUtil, &g_app->game->lockCombination);
	if (canvas->lockSlot >= digits)
		return;
	int half = digits * 8 / 2;
	int y = canvas->renderRect[1] + 10;
	for (int i = 0; i < digits; i++) {
		Graphics_setColor(canvas->graphics, (int)0xff000000);
		if (canvas->lockSlot > i)
			Graphics_setColor(canvas->graphics, (int)0xff00ff00);
		else if (canvas->lockCorrectHits == 1 && canvas->lockSlot == i)
			Graphics_setColor(canvas->graphics, (int)0xffffff00);
		int x = canvas->screenCenterX - half + i * 8;
		Graphics_fillRect(canvas->graphics, x, y, 7, 10);
		Graphics_setColor(canvas->graphics, (int)0xffaaaaaa);
		Graphics_drawRect(canvas->graphics, x, y, 7, 10);
	}
}

/* 0x00014dbc  State 0x10: "Sorry!" box with the error text in the scratch
 * string. */
void Canvas_drawErrorScreen(Canvas *canvas)
{
	String title;
	String_ctor(&title);
	Text_getString(canvas, 0, 0x3d, &title);
	TextUtil_dehyphenate(g_app->textUtil, &title);
	Graphics_setColor(canvas->graphics, (int)0xff000000);
	Graphics_fillRect(canvas->graphics, 0, 0, canvas->screenRect[2], canvas->screenRect[3]);
	Canvas_drawDialogFrame(canvas, 0, 0, canvas->viewRect[2], canvas->viewRect[3], 0);
	Canvas_drawString(canvas, &title, canvas->screenCenterX - 0x40, canvas->screenCenterY - 0x40, 0);
	Canvas_drawString(canvas, &canvas->scratch, canvas->screenCenterX - 0x40, canvas->screenCenterY - 0x28, 0);
	String_dtor(&title, 0);
}

/* 0x00013ed8  State 0xc: the camera sinks to the floor over 750 ms, the
 * screen fades out over 2250 ms, then the death menu (0x74). */
void Canvas_updateDying(Canvas *canvas)
{
	int now = g_app->time;
	int start = canvas->dyingTime;

	if (now < start + 750) {
		int t = ((now - start) << 16) / (750 << 8);
		int drop = t * ((0x24 - canvas->render->floorHeight) << 8);
		canvas->viewZ = 0x24 - (drop >> 16);
		Canvas_updateMovement(canvas, 0, 0);
		Hud_draw(canvas->hud);
		IDISPLAY_Update();
		return;
	}
	if (now >= start + 3000) {
		MenuSystem_setMenu(g_app->menuSystem, 0x74);
		return;
	}
	int t = ((now - start - 750) << 16) / (2250 << 8);
	Render_fadeFramebuffer(canvas->render, (0xff - (((t * 0xff00) >> 16) & 0xff)) & 0xff);
	Hud_draw(canvas->hud);
	IDISPLAY_Update();
}

/* ======================================================================
 * Quick weapon/potion menu (states 0x14 / 0x15)
 * ====================================================================== */

/* 0x000310f8  Numbered list over the playing view: weapons (state 0x14) or
 * potions (state 0x15). Paired weapon slots (2/3, 5/6) show only the better
 * one owned. */
void Canvas_drawQuickMenu(Canvas *canvas)
{
	Player *player = g_app->player;
	char *space = (char *)ModData_str(&g_mod, MOD_STR_SPACE);

	Canvas_updatePlaying(canvas);
	String_setLength(&canvas->scratch, 0);
	Text_getString(canvas, 3, 0x4b, &canvas->scratch);
	TextUtil_dehyphenate(g_app->textUtil, &canvas->scratch);
	String empty;
	String_ctor(&empty);
	Canvas_drawSoftKeys_10c68(canvas, &canvas->scratch, &empty, 1);
	String_dtor(&empty, 0);

	int y = 0x21;
	if (canvas->state == 0x14) {
		int number = 1;
		for (int w = 0; w < 11; w++) {
			uint32_t bit = arm_bit(w);
			uint32_t owned = player->weapons;
			EntityDef *def = NULL;

			if (owned & bit) {
				int defIdx = w;
				if (bit & 0xc) {
					if (owned & 8) {
						if (w == 2)
							continue;
						defIdx = 3;
					}
				} else if ((bit & 0x60) && (owned & 0x40)) {
					if (w == 5)
						continue;
					defIdx = 6;
				}
				def = EntityDefs_find(g_app->entityDefs, 6, 1, defIdx);
			} else {
				/* unowned slot: one blank numbered line per pair */
				if ((bit & 0xc) && ((owned & 0xc) || w == 3))
					continue;
				if ((bit & 0x60) && ((owned & 0x60) || w == 6))
					continue;
			}

			String_setLength(&canvas->scratch, 0);
			appendInt(&canvas->scratch, number);
			String_appendCStr_76f0(&canvas->scratch, space);
			if (def != NULL) {
				if (player->ce.weapon == w) {
					String_appendCStr_76f0(&canvas->scratch, (char *)ModData_str(&g_mod, MOD_STR_EQUIPPED));
					String_appendCStr_76f0(&canvas->scratch, space);
				}
				String_appendString(&canvas->scratch, w == 4 ? &def->name : &def->longName);
				TextUtil_dehyphenate(g_app->textUtil, &canvas->scratch);
			}
			Canvas_drawString(canvas, &canvas->scratch, 3, y, 5);
			number++;
			y += 0xc;
		}
		String_setLength(&canvas->scratch, 0);
		Text_getString(canvas, 3, 0x131, &canvas->scratch);
		Canvas_drawString(canvas, &canvas->scratch, 3, y, 5);
		return;
	}

	String line;
	String_ctor(&line);
	int item = 0;
	for (int number = 1; number < 10; number++) {
		EntityDef *def = EntityDefs_find(canvas->entityDefs, 6, 0, item);
		String_setLength(&canvas->scratch, 0);
		String_setLength(&line, 0);
		appendInt(&canvas->scratch, number);
		String_appendCStr_76f0(&canvas->scratch, space);
		/* slot 1 lists the small potion, or the large one when only that is held */
		if (player->inventory[item] != 0 || (item == 0 && player->inventory[1] != 0))
			String_appendString(&canvas->scratch, &def->name);
		TextUtil_wrapText(g_app->textUtil, &canvas->scratch, 999, &line);
		Canvas_drawString(canvas, &line, 3, y, 5);
		item = item == 0 ? 2 : item + 1;
		y += 0xc;
	}
	String_setLength(&canvas->scratch, 0);
	Text_getString(canvas, 3, 0x132, &canvas->scratch);
	Canvas_drawString(canvas, &canvas->scratch, 3, y, 5);
	String_dtor(&line, 0);
}

/* ======================================================================
 * Text tables
 * ====================================================================== */

/* 0x00043a68  Frees a loaded strings.bin group and takes its size off the
 * string memory counter (Render.memStats[2..3], int64). */
void Canvas_unloadStringGroup(Canvas *cv, int group)
{
	if (cv->strData[group] == NULL)
		return;
	Render *render = g_app->render;
	int64_t used = (int64_t)(((uint64_t)(uint32_t)render->memStats[3] << 32) | (uint32_t)render->memStats[2]);
	used -= (int64_t)(cv->strDataSize[group] + cv->strCount[group] * 2);
	render->memStats[2] = (int32_t)(uint32_t)used;
	render->memStats[3] = (int32_t)(uint32_t)((uint64_t)used >> 32);
	free(cv->strData[group]);
	cv->strData[group] = NULL;
	free(cv->strOffsets[group]);
	cv->strOffsets[group] = NULL;
}
