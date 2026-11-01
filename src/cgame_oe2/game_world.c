/*
 * Game: the 32x32 entity tile grid (link/unlink/lookup), line traces, doors,
 * door locks and switches, item pickups and loot containers, dropped items.
 *
 * Ported from the Orcs & Elves II BREW engine (Game.cpp region of the module).
 */

#include "game.h"

/* ---- strings that live in the executable's data area (read from the .mod) ---- */
#define MOD_STR_LOOT_BULLET 0x47310u /* "\x88" inline bullet icon before each loot line */
#define MOD_STR_PIPE        0x4730cu /* "|" separator after a container title */

/* ---- Entity.info bits ---- */
#define ENT_SPRITE_MASK   0xffffu     /* 1-based sprite (or line) index */
#define ENT_IS_LINE       0x200000u   /* low16 is a line index (door entity) */
#define ENT_CORPSE_BITS   0x20020000u /* corpse states checked by hideExcessCorpses */
#define ENT_CORPSE_SHOWN  0x20000000u
#define ENT_CORPSE_HIDDEN 0x20000u
#define ENT_LINKED        0x4000000u  /* on a tile list */
#define ENT_DROPPED       0x40000000u /* spawned into a drop slot */

/* ---- Sprite.info bits ---- */
#define SPR_HIDDEN       0x10000u
#define SPR_WALL_MASK    0x780000u  /* wall-aligned sprite (any facing) */
#define SPR_WALL_X       0x180000u  /* faces along x: blocks horizontal traces */
#define SPR_WALL_Y       0x600000u  /* faces along y: blocks vertical traces */
#define SPR_WALL_NS      0x80000u
#define SPR_WALL_EW      0x400000u
#define SPR_DOOR_BITS    0x4040000u /* set on the door slab sprite (?) */
#define SPR_MIRROR       0x1000000u
#define SPR_DROP_KEEP    0xfffef000u /* bits kept when a drop slot sprite is reused */

/* ---- Line.flags bits ---- */
#define LINE_SLIDING     0x20u     /* slides sideways instead of swinging */
#define LINE_OPEN        0x40u
#define LINE_FACE_NS     0x100u
#define LINE_FACE_EW     0x200u
#define LINE_LOCKED      0x400u
#define LINE_SLIDE_POS_Y 0x800u
#define LINE_MIRROR      0x2000u
#define LINE_SLIDE_POS_X 0x4000u
#define LINE_WAS_OPENED  0x40000u

/* ---- GameSprite types used for doors ---- */
#define GS_DOOR_OPEN  1
#define GS_DOOR_CLOSE 2
#define GS_DOOR_SLIDE 4

/* Entity types (EntityDef.eType) referenced here. */
enum {
	ETYPE_WORLD = 0, ETYPE_MONSTER = 2, ETYPE_DOOR = 5, ETYPE_ITEM = 6, ETYPE_CONTAINER = 7,
	ETYPE_HAZARD = 8, ETYPE_LOOT_PILE = 0xa, ETYPE_PICKUP = 0xb, ETYPE_BLOCKER_C = 0xc,
	ETYPE_SWITCH = 0xe
};

/* Wall tile ids of a locked / unlocked door line. */
#define TILE_DOOR_LOCKED   0x12
#define TILE_DOOR_UNLOCKED 0x13

static Sprite *entitySprite(Entity *ent)
{
	return &g_app->render->sprites[(ent->info & ENT_SPRITE_MASK) - 1];
}

static int clampWorld(int v)
{
	if (v < 0)
		return 0;
	if (v > 0x7ff)
		return 0x7ff;
	return v;
}

static void playSound(int resId)
{
	Sound_play(g_app->sndSys, resId, 0, 2, -1);
}

/* 0x00019ce8 */
Entity *Game_findEntityAt(Game *game, int x, int y, int typeMask)
{
	Canvas *canvas = g_app->canvas;

	/* The player is not on the tile lists; bit 1 asks for him explicitly. */
	if ((typeMask & 2) != 0 && (x >> 6) == (canvas->destX >> 6) && (y >> 6) == (canvas->destY >> 6))
		return &game->entities[1];

	for (Entity *ent = Game_getEntitiesAt(game, x, y); ent != NULL; ent = ent->nextOnTile) {
		if ((typeMask & arm_bit(ent->def->eType)) != 0)
			return ent;
	}
	return NULL;
}

/* 0x00019d88 */
void *Game_getEntitiesAt(Game *game, int x, int y)
{
	int tx = x >> 6, ty = y >> 6;

	if (tx >= 0 && tx < 32 && ty >= 0 && ty < 32)
		return game->entityGrid[tx + ty * 32];
	return NULL;
}

/* 0x0001bca8 */
int Game_getEntityIndex(Entity *ent)
{
	return (int)(ent - g_app->game->entities);
}

/* 0x00024b48 */
void Game_removeEntitiesAtTile(Game *game, int tx, int ty, int typeMask)
{
	/* Clears up to two (non-boss) entities off a tile, e.g. before a teleport. */
	for (int i = 0; i < 2; i++) {
		Entity *ent = Game_findEntityAt(g_app->game, tx * 64 + 32, ty * 64 + 32, typeMask);
		if (ent != NULL && Entity_isBoss(ent) == 0)
			Entity_diedDefault(ent);
	}
}

/* 0x000254c4 */
void Game_linkEntity(Game *game, Entity *ent, int tx, int ty)
{
	if (ent == &game->entities[0])
		return;
	if (ent->nextOnTile != NULL || ent->prevOnTile != NULL)
		return;

	int16_t idx = (int16_t)(tx + ty * 32);
	if (game->entityGrid[idx] == ent)
		return;

	Entity *head = game->entityGrid[idx];
	ent->nextOnTile = head;
	if (head != NULL)
		head->prevOnTile = ent;
	ent->prevOnTile = NULL;
	ent->linkIndex = idx;
	game->entityGrid[idx] = ent;
	ent->info |= ENT_LINKED;
}

/* 0x00025540 */
void Game_linkWorldEntity(Game *game, int tx, int ty)
{
	int16_t idx = (int16_t)(tx + ty * 32);
	Entity *world = &game->entities[0];
	Entity *last = NULL;

	/* Append the world entity to the tail, unless a world-type entity is already there. */
	for (Entity *ent = game->entityGrid[idx]; ent != NULL; ent = ent->nextOnTile) {
		if (ent->def->eType == ETYPE_WORLD)
			return;
		last = ent;
	}
	if (last == NULL)
		game->entityGrid[idx] = world;
	else
		last->nextOnTile = world;
	world->nextOnTile = NULL;
	world->prevOnTile = NULL;
	world->linkIndex = idx;
	g_app->render->mapFlags[idx] |= 1;
}

/* 0x000255f4 */
void Game_relinkAtViewDest(Player *player)
{
	Entity *ent = Player_getPlayerEnt(player);
	Canvas *canvas = g_app->canvas;
	int x = canvas->destX, y = canvas->destY;

	if (x >= 0 && x <= 0x7ff && y >= 0 && y <= 0x7ff)
		Game_linkEntity(g_app->game, ent, x >> 6, y >> 6);
}

/* 0x00031a3c */
void Game_relinkEntity(Player *player)
{
	Game_unlinkEntityById(player);
	Game_relinkAtViewDest(player);
}

/* 0x00031bb8 */
void Game_unlinkEntity_31bb8(Game *game, Entity *entity)
{
	uint32_t info = entity->info;

	if ((info & ENT_SPRITE_MASK) != 0 && (info & ENT_IS_LINE) == 0)
		entitySprite(entity)->info |= SPR_HIDDEN;
	if ((entity->info & ENT_LINKED) != 0)
		Game_unlinkEntity_43710(game, entity);
}

/* 0x00043710 */
void Game_unlinkEntity_43710(Game *game, Entity *ent)
{
	if (ent == &game->entities[0])
		return;

	if (game->entityGrid[ent->linkIndex] == ent)
		game->entityGrid[ent->linkIndex] = ent->nextOnTile;
	else if (ent->prevOnTile != NULL)
		ent->prevOnTile->nextOnTile = ent->nextOnTile;
	if (ent->nextOnTile != NULL)
		ent->nextOnTile->prevOnTile = ent->prevOnTile;
	ent->nextOnTile = NULL;
	ent->prevOnTile = NULL;
	ent->info &= ~ENT_LINKED;
}

/* 0x0004377c */
void Game_unlinkPlayerFromTile(Game *game, int tx, int ty)
{
	/* Removes the world entity (always the tail, see Game_linkWorldEntity). */
	int16_t idx = (int16_t)(tx + ty * 32);
	Entity *world = &game->entities[0];

	if (game->entityGrid[idx] == world) {
		game->entityGrid[idx] = NULL;
	} else {
		Entity *prev = NULL, *ent;
		for (ent = game->entityGrid[idx]; ent != NULL; ent = ent->nextOnTile) {
			if (ent == world)
				break;
			prev = ent;
		}
		if (ent == NULL)
			return;
		if (prev != NULL)
			prev->nextOnTile = NULL;
	}
	g_app->render->mapFlags[idx] &= 0xfffe;
}

/* 0x00043828 */
void Game_unlinkEntityById(Player *player)
{
	Entity *ent = Player_getPlayerEnt(player);

	if ((ent->info & ENT_LINKED) != 0)
		Game_unlinkEntity_43710(g_app->game, ent);
}

/* 0x00042958 */
void Game_trace(Game *game, int x0, int y0, int x1, int y1, Entity *ignore, int typeMask)
{
	Applet *app = g_app;
	Entity *world = &game->entities[0];
	Entity *playerEnt = &game->entities[1];
	Entity *playerEnt2 = &game->entities[2];
	int dists[32];

	game->traceCount++;

	/* Only straight (row or column) traces of non-zero length are supported. */
	if ((x0 == x1 && y0 == y1) || (x0 != x1 && y0 != y1)) {
		game->traceEntity = NULL;
		game->numTraceEntities = 0;
		return;
	}
	x1 = clampWorld(x1);
	y1 = clampWorld(y1);
	x0 = clampWorld(x0);
	y0 = clampWorld(y0);
	game->numTraceEntities = 0;

	int dirX = x1 - x0, dirY = y1 - y0;
	int cx = x0, cy = y0;

	for (;;) {
		Entity *head = game->entityGrid[(cx >> 6) + (cy >> 6) * 32];
		if (head != NULL) {
			/* Pass 1: does anything on this tile stop the trace? */
			int blocked = 0;
			for (Entity *ent = head; ent != NULL && ent != world; ent = ent->nextOnTile) {
				if (ent->def == NULL)
					return;
				int type = ent->def->eType;
				uint32_t bit = arm_bit(type);
				if ((bit & (uint32_t)typeMask) == 0)
					continue;
				if ((bit & 0x3000) != 0) { /* types 12/13 always block */
					blocked = 1;
					break;
				}
				if ((ent->info & ENT_IS_LINE) != 0) {
					if (type == ETYPE_HAZARD) {
						blocked = 1;
						break;
					}
				} else if (ent != playerEnt && ent != playerEnt2 &&
				           (entitySprite(ent)->info & SPR_WALL_MASK) != 0) {
					blocked = 1;
					break;
				}
			}

			/* Pass 2: collect the entities the trace actually crosses. */
			int first = game->numTraceEntities;
			int numDists = 0;
			for (Entity *ent = head; ent != NULL; ent = ent->nextOnTile) {
				if (ent == ignore)
					continue;
				int type = ent->def->eType;
				if ((typeMask & arm_bit(type)) == 0 || ent == playerEnt2)
					continue;

				uint32_t info = ent->info;
				if ((info & ENT_SPRITE_MASK) != 0 && (info & ENT_IS_LINE) == 0) {
					Sprite *spr = &app->render->sprites[(info & ENT_SPRITE_MASK) - 1];

					/* A familiar can walk through type 0xc/1 blockers. */
					if (ignore != NULL && ignore->def->eType == 1 && type == ETYPE_BLOCKER_C &&
					    ent->def->eSubType == 1 && app->player->isFamiliar != 0)
						continue;

					uint32_t sinfo = spr->info;
					if ((sinfo & SPR_WALL_MASK) != 0) {
						/* Wall-aligned sprite: hit only if the trace crosses its plane. */
						if (x0 != x1) {
							if ((sinfo & SPR_WALL_X) != 0)
								continue;
							if (spr->x > x0 && spr->x > x1)
								continue;
							if (spr->x < x0 && spr->x < x1)
								continue;
						}
						if (y0 != y1) {
							if ((sinfo & SPR_WALL_Y) != 0)
								continue;
							if (spr->y > y0 && spr->y > y1)
								continue;
							if (spr->y < y0 && spr->y < y1)
								continue;
						}
					} else {
						/* Free-standing: must lie ahead of the start in the trace direction. */
						int px, py;
						if (ent->monster == NULL) {
							px = spr->x;
							py = spr->y;
						} else {
							px = ent->monster->destX;
							py = ent->monster->destY;
						}
						if (px - x0 < 0) {
							if (dirX >= 0)
								continue;
						} else if (dirX < 0) {
							continue;
						}
						if (py - y0 < 0) {
							if (dirY >= 0)
								continue;
						} else if (dirY < 0) {
							continue;
						}
					}
				}

				if (type == ETYPE_WORLD) {
					/* Wall hit: point just before the tile edge, then stop after this tile. */
					game->traceHitX = cx;
					game->traceHitY = cy;
					if (x0 > x1)
						game->traceHitX = (cx >> 6) * 64 + 0x41;
					else if (x0 < x1)
						game->traceHitX = (cx >> 6) * 64 - 1;
					if (y0 > y1)
						game->traceHitY = (cy >> 6) * 64 + 0x41;
					else if (y0 < y1)
						game->traceHitY = (cy >> 6) * 64 - 1;
					int dx = game->traceHitX - x0, dy = game->traceHitY - y0;
					if (numDists < 32)
						dists[numDists++] = dx * dx + dy * dy;
					cx = x1;
					cy = y1;
				} else if (blocked) {
					int d = Entity_distSqFrom(ent, x0, y0);
					if (numDists < 32)
						dists[numDists++] = d;
				}
				if (game->numTraceEntities < 32)
					game->traceEntities[game->numTraceEntities++] = ent;
			}

			/* Bubble-sort this tile's hits by distance. */
			if (numDists > 0) {
				int last = numDists - 1;
				for (int i = 0; i < last; i++) {
					for (int j = 0; j < last - i; j++) {
						if (dists[j + 1] < dists[j]) {
							int d = dists[j];
							dists[j] = dists[j + 1];
							dists[j + 1] = d;
							Entity *t = game->traceEntities[first + j];
							game->traceEntities[first + j] = game->traceEntities[first + j + 1];
							game->traceEntities[first + j + 1] = t;
						}
					}
				}
			}
		}

		if (cx == x1 && cy == y1)
			break;
		if (cx < x1) {
			cx += 64;
			if (cx > x1)
				cx = x1;
		} else if (cx > x1) {
			cx -= 64;
			if (cx < x1)
				cx = x1;
		}
		if (cy < y1) {
			cy += 64;
			if (cy > y1)
				cy = y1;
		} else if (cy > y1) {
			cy -= 64;
			if (cy < y1)
				cy = y1;
		}
	}

	game->traceEntity = game->numTraceEntities > 0 ? game->traceEntities[0] : NULL;
}

static void swapDoorEnds(GameSprite *gs)
{
	int t[3];

	gs->vel[0] = -gs->vel[0];
	gs->vel[1] = -gs->vel[1];
	gs->vel[2] = -gs->vel[2];
	memcpy(t, gs->dest, sizeof t);
	memcpy(gs->dest, gs->start, sizeof t);
	memcpy(gs->start, t, sizeof t);
}

/* 0x0002eae4 */
int Game_useDoor(Game *game, int op, int lineIdx, int animate)
{
	Applet *app = g_app;
	Render *render = app->render;
	Line *line = &render->lines[lineIdx];
	uint32_t flags = line->flags;
	int sliding = (flags & LINE_SLIDING) != 0;

	if ((flags & LINE_LOCKED) != 0)
		return 0;
	if (op == 0xf) {
		if ((flags & LINE_OPEN) != 0)
			return 0;
	} else if (op == 0x10 && (flags & LINE_OPEN) == 0) {
		return 0;
	}

	line->flags = flags ^ LINE_OPEN;

	/* Link (closed) or unlink (open) the door's line entity on its tile. */
	for (int i = game->firstLineEntity; i <= game->lastEntity; i++) {
		Entity *ent = &game->entities[i];
		if ((int)(ent->info & ENT_SPRITE_MASK) - 1 != lineIdx)
			continue;
		if ((line->flags & LINE_OPEN) != 0) {
			Game_unlinkEntity_43710(game, ent);
		} else {
			int tx = ent->linkIndex % 32, ty = ent->linkIndex / 32;
			for (Entity *e = Game_getEntitiesAt(app->game, tx << 6, ty << 6); e != NULL; e = e->nextOnTile) {
				if (e->def->eType == ETYPE_MONSTER && e->def->eSubType != 0x10) {
					/* A monster stands in the doorway: refuse to close. */
					line->flags ^= LINE_OPEN;
					game->watchEntity = ent;
					return 0;
				}
			}
			if (game->watchEntity == ent)
				game->watchEntity = NULL;
			Game_linkEntity(game, ent, ent->linkIndex % 32, ent->linkIndex / 32);
		}
		break;
	}

	if (app->canvas->state != 2) {
		if ((line->flags & LINE_OPEN) == 0)
			playSound(3);
		else
			playSound(sliding ? 0x13 : 5);
	}
	if (op == 0xf)
		line->flags |= LINE_WAS_OPENED;
	if (animate == 0)
		return 1;

	/* A door already moving is simply reversed. */
	GameSprite *gs = NULL;
	for (int i = 0; i < 48; i++) {
		if ((game->gsprites[i].flags & 1) != 0 && game->gsprites[i].data == line) {
			gs = &game->gsprites[i];
			break;
		}
	}
	if (gs != NULL) {
		swapDoorEnds(gs);
		gs->time = app->time - (gs->travelTime - (app->time - gs->time));
		gs->flags |= 0x20;
		if (op == 0xf) {
			gs->type = GS_DOOR_OPEN;
			Render_relinkSprite(app->render, gs->sprite);
		} else {
			gs->type = GS_DOOR_CLOSE;
			Render_relinkSpriteAt(app->render, gs->sprite, gs->dest[0], gs->dest[1]);
		}
		return 1;
	}

	gs = Game_gspriteAlloc(game, 0, line->texture, 0, 6);
	gs->data = line;
	uint32_t sinfo = gs->sprite->info | SPR_DOOR_BITS;
	gs->sprite->info = sinfo;
	sinfo &= ~SPR_WALL_MASK;
	gs->sprite->info = sinfo;
	if ((line->flags & LINE_FACE_NS) != 0)
		gs->sprite->info = sinfo | SPR_WALL_NS;
	else if ((line->flags & LINE_FACE_EW) != 0)
		gs->sprite->info = sinfo | SPR_WALL_EW;
	if ((line->flags & LINE_MIRROR) != 0 || (line->flags & LINE_SLIDE_POS_Y) != 0)
		gs->sprite->info |= SPR_MIRROR;

	gs->travelTime = 400;
	gs->f10 = 400;
	gs->duration = 0xfa;
	gs->flags |= 0x20;
	gs->type = sliding ? GS_DOOR_SLIDE : GS_DOOR_OPEN;
	gs->start[0] = (line->x1 + line->x2) >> 1;
	gs->start[1] = (line->y1 + line->y2) >> 1;
	gs->start[2] = 0x20;
	Vec3_copy(gs->dest, gs->start);
	gs->travelTime = sliding ? 1000 : 250;
	gs->f10 = gs->travelTime;

	/* Swinging doors rotate away from the player; sliding doors move half a tile. */
	int dist = -32000;
	flags = line->flags;
	if ((flags & LINE_FACE_NS) != 0) {
		if (sliding) {
			if ((flags & LINE_SLIDE_POS_Y) != 0) {
				dist = 32000;
				gs->dest[1] += 0x20;
			} else {
				gs->dest[1] -= 0x20;
			}
			gs->vel[1] = dist / gs->travelTime;
		} else {
			if (app->game->destY < gs->dest[1]) {
				gs->dest[1] -= 1;
				gs->start[1] -= 1;
			} else {
				gs->dest[1] += 1;
				gs->start[1] += 1;
			}
			gs->dest[0] += 0x40;
			gs->vel[0] = 64000 / gs->travelTime;
		}
	} else if ((flags & LINE_FACE_EW) != 0) {
		if (sliding) {
			if ((flags & LINE_SLIDE_POS_X) != 0) {
				dist = 32000;
				gs->dest[0] += 0x20;
			} else {
				gs->dest[0] -= 0x20;
			}
			gs->vel[0] = dist / gs->travelTime;
		} else {
			if (app->game->destX < gs->dest[0]) {
				gs->dest[0] -= 1;
				gs->start[0] -= 1;
			} else {
				gs->dest[0] += 1;
				gs->start[0] += 1;
			}
			gs->dest[1] += 0x40;
			gs->vel[1] = 64000 / gs->travelTime;
		}
	}

	if (op == 0x10) {
		swapDoorEnds(gs);
		gs->type = GS_DOOR_CLOSE;
		if (!sliding)
			Game_setTrackedSlot(game, lineIdx, 0);
	} else if (op == 0xf && !sliding) {
		Game_setTrackedSlot(game, lineIdx, 1);
	}

	gs->sprite->x = gs->start[0];
	gs->sprite->y = gs->start[1];
	gs->sprite->z = gs->start[2];
	if (gs->type != GS_DOOR_CLOSE)
		Render_relinkSprite(app->render, gs->sprite);
	else
		Render_relinkSpriteAt(app->render, gs->sprite, gs->dest[0], gs->dest[1]);
	return 1;
}

/* 0x0003a208 */
void Game_setLineLocked(Game *game, int line, int locked, int force)
{
	Applet *app = g_app;
	Line *ln = &app->render->lines[line];
	int subType;

	if (locked == 0) {
		ln->flags &= ~LINE_LOCKED;
		if (force == 0 && ln->texture != TILE_DOOR_LOCKED)
			return;
		ln->texture = TILE_DOOR_UNLOCKED;
		playSound(6);
		subType = 2;
	} else {
		ln->flags |= LINE_LOCKED;
		if (force == 0 && ln->texture != TILE_DOOR_UNLOCKED)
			return;
		ln->texture = TILE_DOOR_LOCKED;
		subType = 1;
	}

	/* Swap the door entity's def (locked door / door); keep a custom name. */
	for (int i = game->firstLineEntity; i <= game->lastEntity; i++) {
		Entity *ent = &game->entities[i];
		if ((int)(ent->info & ENT_SPRITE_MASK) - 1 != line)
			continue;
		int defaultName = String_equals_40fc(&ent->def->name, &ent->name);
		EntityDef *def = EntityDefs_findType(app->entityDefs, ETYPE_DOOR, subType);
		ent->def = def;
		if (defaultName != 0)
			String_assign(&ent->name, &def->name);
		return;
	}
}

/* 0x0003b570 */
void *Game_setSwitch(Game *game, Entity *entity, int on)
{
	Applet *app = g_app;
	Sprite *spr = entitySprite(entity);

	spr->info = (spr->info & 0xffffff00u) | (on == 0 ? 0xad : 0xae);
	EntityDef *def = EntityDefs_find(app->entityDefs, ETYPE_SWITCH, on == 0 ? 2 : 1, 0);
	spr->ent->def = def;
	return String_assign(&spr->ent->name, &def->name);
}

/* Starts a loot line: clears the text arguments and passes the bullet icon as %1. */
static void beginLootLine(Canvas *canvas)
{
	String bullet;

	Canvas_resetEvents(canvas);
	String_ctorCStr(&bullet, (char *)ModData_str(&g_mod, MOD_STR_LOOT_BULLET));
	Canvas_addTextArgStr(canvas, &bullet);
	String_dtor(&bullet, 0);
}

/* 0x000135c4 */
void Game_openContainer(Entity *entity)
{
	Applet *app = g_app;
	int tile = entity->linkIndex;
	String text;
	EntityDef *def = entity->def;

	String_ctor(&text);
	app->canvas->state = 7;

	/* Dialog title by container kind. */
	if (def->eType == ETYPE_LOOT_PILE) {
		Text_getString(app->canvas, 0, 0x95, &text);
	} else if (def->eType == ETYPE_ITEM) {
		if (def->eSubType == 4) {
			if (def->parm != 2) {
				Text_getString(app->canvas, 0, 0x9b, &text);
			} else if (app->canvas->mapId == 6) {
				Text_getString(app->canvas, 0, 0x1f, &text);
				Player_give(app->player, 1, 3, 1, 1);
				app->canvas->dialogPortrait = 7;
				Canvas_showDialogStr(app->canvas, 0, &text, 9, 1);
				String_dtor(&text, 0);
				return;
			} else {
				Text_getString(app->canvas, 0, 0x96, &text);
				tile = 0x20; /* this chest's contents live in the event of tile (0,1) */
			}
		}
	} else if (def->eType == ETYPE_CONTAINER) {
		int id;
		if (def->eSubType == 3)
			id = 0x97;
		else if (def->eSubType == 6)
			id = def->parm == 1 ? 0x98 : 0x99;
		else
			id = 0x9a;
		Text_getString(app->canvas, 0, id, &text);
	}

	if ((app->render->mapFlags[tile] & 0x10) == 0) {
		String_dtor(&text, 0);
		return;
	}

	/* The contents are the opcode-0x2f records of the tile's event. */
	uint32_t event = app->render->tileEvents[Render_findTileEvent(app->render, tile)];
	int numWords = (int)((event & 0x7ffffff) >> 20) * 3;
	int base = (int)((event & 0xfffff) >> 9) * 3;

	for (int i = 0; i < numWords; i += 3) {
		if (app->render->byteCodes[base + i] != 0x2f)
			continue;
		uint32_t arg = (uint32_t)app->render->byteCodes[base + i + 1];
		int kind = arg & 0xff;

		if (kind == 0xff) { /* sub-heading from the map's string group */
			String_appendCStr_76f0(&text, (char *)ModData_str(&g_mod, MOD_STR_LOOT_BULLET));
			Text_getString(app->canvas, app->canvas->mapStringGroup, (int)arg >> 16, &text);
			String_appendCStr_76f0(&text, (char *)ModData_str(&g_mod, MOD_STR_PIPE));
			continue;
		}
		if (kind == 0xfe) { /* notebook entry */
			Player_addNotebookIndex(app->player, (int16_t)(arg >> 8));
			continue;
		}

		int idx = (arg >> 8) & 0xff;
		int amount = (arg >> 16) & 0xff;
		EntityDef *item;
		beginLootLine(app->canvas);

		switch (kind) {
		case 0: /* gold / treasure */
		case 2: /* ring */
		case 3: /* ammo */
			if (kind == 2) {
				if ((app->player->rings & arm_bit(idx)) != 0)
					break;
				Player_give(app->player, 2, idx, 1, 0);
			} else {
				Player_give(app->player, kind, idx, amount, 0);
			}
			item = EntityDefs_find(app->entityDefs, ETYPE_ITEM, kind, idx);
			Canvas_addTextArgInt(app->canvas, amount);
			Canvas_addTextArgStr(app->canvas, &item->longName);
			Text_getString(app->canvas, 0, 0x9d, &text);
			break;
		case 1: /* weapon */
			Player_give(app->player, 1, idx, amount, 1);
			item = EntityDefs_find(app->entityDefs, ETYPE_ITEM, 1, idx);
			Canvas_addTextArgStr(app->canvas, &item->longName);
			Text_getString(app->canvas, 0, 0x9e, &text);
			if (idx == 2 && item->parm == 2) {
				/* Also list weapon 5 while the event at tile 0x243 is in state 1. */
				int ev = Render_findTileEvent(app->render, 0x243);
				if (((app->render->tileEvents[ev] << 2) >> 29) == 1) {
					beginLootLine(app->canvas);
					item = EntityDefs_find(app->entityDefs, ETYPE_ITEM, 1, 5);
					Canvas_addTextArgStr(app->canvas, &item->longName);
					Text_getString(app->canvas, 0, 0x9e, &text);
				}
			}
			break;
		case 4: { /* pouch: 16-bit count */
			int count = (int)((arg >> 8) & 0xffff);
			Player_give(app->player, 4, 0, count, 0);
			Canvas_addTextArgInt(app->canvas, count);
			Text_getString(app->canvas, 0, 0x9c, &text);
			break;
		}
		case 9: /* lock-pick level */
			if (app->player->lockLevel < idx) {
				Player_give(app->player, 9, idx, amount, 0);
				item = EntityDefs_find(app->entityDefs, ETYPE_ITEM, 9, idx);
				Canvas_addTextArgStr(app->canvas, &item->longName);
				Text_getString(app->canvas, 0, 0x9e, &text);
			}
			break;
		default:
			break;
		}
	}

	String_setLength(&text, text.len - 1); /* drop the trailing separator */
	Canvas_showDialogStr(app->canvas, 0, &text, 4, 1);

	def = entity->def;
	if (def->eType == ETYPE_ITEM && def->eSubType == 4 && def->parm == 2 && app->canvas->mapId == 3) {
		Game_executeTile(app->game, 0, 1, 0xf000, 1);
		Player_selectWeapon(app->player, 4);
	}
	String_dtor(&text, 0);
}

/* 0x0003cce4 */
Entity *Game_spawnDropItem(Game *game, int x, int y, int sprFlags, int type, int subtype, int parm, int count)
{
	Applet *app = g_app;

	/* Gold-like pouches (6/4, parm 0/1) stack onto one already on the tile. */
	if (type == ETYPE_ITEM && subtype == 4 && parm < 2) {
		for (Entity *e = Game_findEntityAt(game, x, y, 1 << ETYPE_ITEM); e != NULL; e = e->nextOnTile) {
			if (e->def->eType == ETYPE_ITEM && e->def->eSubType == 4) {
				e->param += count;
				return e;
			}
		}
	}

	/* Otherwise recycle the next of the 8 drop slots. */
	Entity *ent = &game->entities[game->firstDropEntity + game->dropIndex];
	EntityDef *def = EntityDefs_find(app->entityDefs, type, subtype, parm);
	ent->def = def;
	String_assign(&ent->name, &def->name);
	game->dropIndex = (game->dropIndex + 1) % 8;
	if (ent->def == NULL)
		return NULL;

	if ((ent->info & ENT_LINKED) != 0)
		Game_unlinkEntity_43710(game, ent);
	ent->info |= ENT_DROPPED;
	Sprite *spr = entitySprite(ent);
	spr->info = (spr->info & SPR_DROP_KEEP) | (uint32_t)sprFlags;
	spr->z = 0x20;
	spr->ent = ent;
	spr->x = x;
	spr->y = y;
	ent->param = count;
	Render_relinkSprite(app->render, spr);
	Game_linkEntity(game, ent, x >> 6, y >> 6);
	return ent;
}

/* 0x00042128 */
int Game_touchTile(Game *game, int x, int y, int force)
{
	Applet *app = g_app;
	int touched = 0;

	/* While controlling the familiar, the player's body spot is not touched. */
	if (app->player->isFamiliar != 0 && app->canvas->savedViewX == x && app->canvas->savedViewY == y)
		return 0;

	Entity *ent = Game_getEntitiesAt(game, x, y);
	while (ent != NULL) {
		Entity *next = ent->nextOnTile;
		int type = ent->def->eType;
		/* Without force only hazards are touched. */
		if (force != 0 || type == ETYPE_HAZARD) {
			if (type == ETYPE_HAZARD && (entitySprite(ent)->info & SPR_WALL_MASK) != 0)
				return 0;
			Game_touchEntity(ent);
			touched = 1;
		}
		ent = next;
	}
	return touched;
}

/* 0x00042218 */
int Game_pickupItem(Entity *ent)
{
	Applet *app = g_app;
	Hud *hud = app->hud;
	Player *player = app->player;
	EntityDef *def = ent->def;
	String tmp;
	String *msg;
	int amount;

	String_ctor(&tmp);

	switch (def->eSubType) {
	case 0: /* treasure / gold */
		if (player->isFamiliar != 0 && def->parm >= 10)
			goto familiarRefuses;
		if (Player_give(player, 0, def->parm, 1, 0) == 0) {
			msg = Hud_getMessageBufferDefault(hud);
			Canvas_resetEvents(app->canvas);
			Canvas_addTextArgStr(app->canvas, &ent->def->name);
			Text_getString(app->canvas, 0, 0x8c, msg);
			Hud_finishMessageBuffer(hud);
			goto notTaken;
		}
		Hud_startStatFlash();
		hud->msgExpire = app->time + 500;
		if (ent->def->parm == 0xd)
			playSound(0x58);
		else if (ent->def->parm >= 0 && ent->def->parm < 10)
			playSound(0x5b);
		else
			playSound(0x5a);
		msg = Hud_getMessageBuffer(hud, 1);
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgStr(app->canvas, &ent->def->longName);
		Text_getString(app->canvas, 0, 0x8d, msg);
		Hud_finishMessageBuffer(hud);
		break;

	case 1: { /* weapon (+ its ammo) */
		if (player->isFamiliar != 0)
			goto familiarRefuses;
		if ((player->weapons & arm_bit(def->parm)) == 0)
			Player_selectWeapon(player, def->parm);
		Hud_startStatFlash();
		Player_give(app->player, 1, ent->def->parm, 1, 0);
		int weapon = ent->def->parm;
		WeaponInfo *w = &app->combat->weapons[weapon];
		if (w->ammoUsage != 0)
			Player_give(app->player, 3, w->ammoType, weapon, 0); /* amount = weapon id (sic) */
		hud->msgExpire = app->time + 500;
		msg = Hud_getMessageBuffer(hud, 1);
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgStr(app->canvas, &ent->def->longName);
		Text_getString(app->canvas, 0, 0x8d, msg);
		Hud_finishMessageBuffer(hud);
		Hud_randomBark(hud, 6);
		Player_showWeaponHelp(app->player, ent->def->parm, 0);
		break;
	}

	case 3: /* ammo */
		if (player->isFamiliar != 0 && def->parm != 3 && def->parm != 4 && def->parm != 5)
			goto familiarRefuses;
		if (Entity_isDroppedEntity(ent) != 0)
			amount = ent->param;
		else
			amount = Random_nextInt(&app->random) % 4 + 2;
		if (Player_give(app->player, 3, ent->def->parm, amount, 0) == 0) {
			Hud_addMessage(hud, 0x91);
			break;
		}
		Hud_startStatFlash();
		hud->msgExpire = app->time + 500;
		msg = Hud_getMessageBuffer(hud, 1);
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgInt(app->canvas, amount);
		Canvas_addTextArgStr(app->canvas, &ent->name);
		Text_getString(app->canvas, 0, 0x90, msg);
		Hud_finishMessageBuffer(hud);
		playSound(0x5a);
		if (Entity_isDroppedEntity(ent) != 0 && ent->def->parm == 4 && amount == 5) {
			String_setLength(&tmp, 0);
			Text_getString(app->canvas, 0, 0x20, &tmp);
			app->canvas->dialogPortrait = 6;
			Canvas_showDialogStr(app->canvas, 0, &tmp, 9, 1);
		}
		break;

	case 4: /* gold pouch, or a container */
		if (def->parm < 2) {
			Player_showHelp(app->player, 5, 0);
			if (Entity_isDroppedEntity(ent) != 0)
				amount = ent->param;
			else
				amount = Game_tileEventParamSum(ent);
			msg = Hud_getMessageBuffer(hud, 1);
			if (amount == 1) {
				Text_getString(app->canvas, 0, 0x8e, msg);
			} else {
				Canvas_resetEvents(app->canvas);
				Canvas_addTextArgInt(app->canvas, amount);
				Text_getString(app->canvas, 0, 0x8f, msg);
			}
			Hud_finishMessageBuffer(hud);
			if (Player_give(app->player, 4, 0, amount, 0) == 0) {
				Hud_addMessage(hud, 0x33);
				goto notTaken;
			}
			Hud_startStatFlash();
			playSound(0xd);
		} else {
			if (def->parm != 3 && app->player->isFamiliar != 0)
				goto familiarRefuses;
			Game_openContainer(ent);
			playSound(0x5a);
		}
		break;

	case 5: /* food */
		if (player->isFamiliar != 0)
			goto familiarRefuses;
		if (Player_addHealth(player, def->parm == 1 ? 0x14 : 0x28) == 0) {
			Hud_addMessage(hud, 0x3b);
			goto notTaken;
		}
		Player_showHelp(app->player, 4, 0);
		playSound(0x59);
		if (ent->def->parm == 0) {
			/* Reusable (fountain-like) food stays visible. */
			Game_unlinkEntity_43710(app->game, ent);
			String_dtor(&tmp, 0);
			return 1;
		}
		break;

	default: /* subtype 2 and others: taken without effect */
		break;
	}

	hud->msgExpire = app->time + 500;
	Game_unlinkEntity_31bb8(app->game, ent);
	String_dtor(&tmp, 0);
	return 1;

familiarRefuses:
	Hud_addMessage(hud, 0x92);
notTaken:
	String_dtor(&tmp, 0);
	return 0;
}

/* 0x00042838 */
int Game_touchEntity(Entity *ent)
{
	Applet *app = g_app;
	Hud *hud = app->hud;
	int type = ent->def->eType;

	if (type == ETYPE_HAZARD) {
		int kind = ent->def->parm;
		if (kind == 0) {
			if (app->player->isFamiliar != 0) {
				/* The familiar is dispelled by the hazard. */
				Player_setFamiliar(app->player, 0, 1, 0);
				return 0;
			}
			Player_hurtFeedback(app->player, 0);
			Player_takeDamage(app->player, 3, ent);
			Hud_addMessage(hud, 0x8a);
		} else if (kind != 2) {
			Player_hurtFeedback(app->player, 0);
			Player_takeDamage(app->player, 0x14, ent);
			Hud_addMessage(hud, 0x8b);
		}
		return 0;
	}
	if (type != ETYPE_ITEM && type != ETYPE_PICKUP)
		return 0;

	/* A picked-up drop slot entity is released. */
	if (Game_pickupItem(ent) != 0 && Entity_isDroppedEntity(ent) != 0) {
		entitySprite(ent)->ent = NULL;
		ent->def = NULL;
	}
	return 0;
}

/* 0x00043318 */
void Game_hideExcessCorpses(Game *game, int x, int y)
{
	Applet *app = g_app;
	Entity *ent = app->game->inactiveMonsters;
	int count = 0;

	if (ent == NULL)
		return;
	do {
		Sprite *spr = entitySprite(ent);
		if (spr->x == x && spr->y == y && (ent->info & ENT_CORPSE_BITS) != 0 &&
		    (spr->info & SPR_HIDDEN) == 0 && ++count > 2) {
			spr->info |= SPR_HIDDEN;
			ent->info = (ent->info & ~ENT_CORPSE_SHOWN) | ENT_CORPSE_HIDDEN;
			Game_unlinkEntity_43710(app->game, ent);
		}
		ent = ent->monster->nextOnList;
	} while (app->game->inactiveMonsters != ent);
}
