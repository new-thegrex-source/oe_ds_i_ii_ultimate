/*
 * Game: interaction with the world — automap reveal, locked doors, switches,
 * item pickups and drops, touching hazards, and the tile-walking trace used
 * for "what is in front of me" and attacks.
 */

#include <ctype.h>

#include "game.h"

/* Player-visible text, read from the user's .mod. */
#define MOD_STR_CANT_HOLD_MORE   0x300a8u
#define MOD_STR_GOT              0x300bcu
#define MOD_STR_EMPTY            0x300c4u /* (coin plural suffix for 1) */
#define MOD_STR_PLURAL_S         0x300c8u
#define MOD_STR_GOT_COINS        0x300ccu
#define MOD_STR_COINS_MAX        0x300dcu
#define MOD_STR_HEALTH_MAX       0x300f0u
#define MOD_STR_AMMO_MAX         0x30104u
#define MOD_STR_IT_BURNS         0x30114u
#define MOD_STR_IT_REALLY_BURNS  0x30120u
#define MOD_STR_FOUND_GEM        0x304c4u
#define MOD_STR_ACQUIRED         0x304ecu

#define MODSTR(off) ((char *)ModData_str(&g_mod, (off)))

/* Entity.info bits */
#define ENT_LINE        0x200000u
#define ENT_LINKED      0x4000000u
#define ENT_DIRTY       0x80000000u

/* Sprite.info bits */
#define SPRITE_HIDDEN     0x10000u
#define SPRITE_SIDE_MASK  0x780000u /* any wall-side orientation */
#define SPRITE_SIDE_XMASK 0x180000u
#define SPRITE_SIDE_YMASK 0x600000u
#define SPRITE_AUTOMAP    0x8000000u

/* Line.flags / mapFlags bits */
#define LINE_AUTOMAP   0x80u
#define LINE_LOCKED    0x400u
#define TILE_LINE_TEX  0x04 /* tile has a line texture 0x18 */
#define TILE_SEEN      0x10

/* Line textures for locked / unlocked doors */
#define TEX_DOOR_LOCKED   0x12
#define TEX_DOOR_UNLOCKED 0x13

/* EntityDef.eType values used here */
#define ETYPE_WORLD       0
#define ETYPE_DOOR        5
#define ETYPE_ITEM        6
#define ETYPE_RELIQUARY   7
#define ETYPE_FIRE        8
#define ETYPE_PICKUP_B    0xb
#define ETYPE_SWITCH      0xe
#define ETYPE_FIRE_PORTAL 0xf

/* Hud message timeout after a pickup (ms) */
#define PICKUP_MSG_MS 500

#define MAX_TRACE_HITS 32

static inline Sprite *entitySprite(Render *render, uint32_t info)
{
	return Render_spriteAt(render, (info & 0xffff));
}

/* 0x00013110  marks lines/sprites in the rectangle as seen on the automap.
 * The tile-flag pass keeps the original's bug: it only ever touches tile (x, y). */
void Game_revealAutomapRect(Game *game, int x, int y, int w, int h)
{
	int x2 = x + w;
	int y2 = y + h;

	int numLines = game->render->numLines;
	for (int i = 0; i < numLines; i++) {
		Line *ln = &game->render->lines[i];
		int mx = (((ln->x2 - ln->x1) >> 1) + ln->x1) >> 6;
		int my = (((ln->y2 - ln->y1) >> 1) + ln->y1) >> 6;
		if (x <= mx && mx < x2 && y <= my && my < y2)
			ln->flags |= LINE_AUTOMAP;
	}

	int numSprites = game->render->numMapSprites;
	for (int i = 0; i < numSprites; i++) {
		Sprite *s = &game->render->sprites[i];
		int sx = s->x >> 6;
		int sy = s->y >> 6;
		if (x <= sx && sx < x2 && y <= sy && sy < y2)
			s->info |= SPRITE_AUTOMAP;
	}

	int tile = y * 32 + x; /* BUG kept: the loops below never advance the index */
	for (int ty = y; ty < y2; ty++) {
		for (int tx = x; tx < x2; tx++) {
			uint8_t *f = &game->render->mapFlags[tile];
			if (*f & TILE_LINE_TEX)
				*f |= TILE_SEEN;
		}
	}
}

/* 0x0002597c */
void Game_setLineLocked(Game *game, int line, int locked, int force)
{
	Render *render = game->render;
	Line *ln = &render->lines[line];

	if (locked)
		ln->flags |= LINE_LOCKED;
	else
		ln->flags &= ~LINE_LOCKED;

	int defSub;
	if (locked && (force || ln->texture == TEX_DOOR_UNLOCKED)) {
		ln->texture = TEX_DOOR_LOCKED;
		defSub = 1;
	} else if (!locked && (force || ln->texture == TEX_DOOR_LOCKED)) {
		ln->texture = TEX_DOOR_UNLOCKED;
		Sound_play(game->app->sound, 0x1418, 0, 3, -1);
		defSub = 2;
	} else {
		return;
	}

	/* the door entity's definition follows: locked door (5,1) / door (5,2) */
	for (int i = game->firstDoor; i <= game->lastDoor; i++) {
		Entity *ent = &game->entities[i];
		if ((int)(ent->info & 0xffff) - 1 == line) {
			EntityDef *def = EntityDefs_find(game->app->entityDefs, ETYPE_DOOR, defSub, -1);
			ent->def = def;
			ent->name = def->name;
			return;
		}
	}
}

/* 0x00026458  levers: tiles 0xad/0xae, or 0xcc/0xcd (def parm 1) */
void Game_setSwitch(Game *game, void *entity, int on, int arg)
{
	Entity *ent = entity;
	Render *render = game->render;
	Sprite *s = entitySprite(render, ent->info);
	uint32_t tile = s->info & 0xff;
	uint32_t offTile, onTile;
	int parm;

	if (tile == 0xcc || tile == 0xcd) {
		offTile = 0xcc;
		onTile = 0xcd;
		parm = 1;
	} else {
		offTile = 0xad;
		onTile = 0xae;
		parm = 0;
	}

	uint32_t rest = s->info & 0xffffff00u;
	EntityDef *def;
	if (!on) {
		s->info = offTile | rest;
		def = EntityDefs_find(game->app->entityDefs, ETYPE_SWITCH, 2, parm);
	} else {
		s->info = rest | onTile;
		def = EntityDefs_find(game->app->entityDefs, ETYPE_SWITCH, 1, parm);
	}
	s->ent->def = def;
	s->ent->name = def->name;
}

/* 0x00026a3c  "acquired" dialog for a quest item / gem */
void Game_showItemAcquired(Game *game, int defSub, int defParam)
{
	char title[0x400];
	char text[0x400];
	EntityDef *def = EntityDefs_find(game->app->entityDefs, ETYPE_ITEM, defSub & 0xff, defParam & 0xff);

	/* the original upper-cased the long name in place and printed over the same buffer */
	size_t n = 0;
	for (const char *p = def->longName; *p && n < sizeof title - 1; p++)
		title[n++] = (char)toupper((unsigned char)*p);
	title[n] = '\0';

	const char *fmt = MODSTR(defSub == 8 ? MOD_STR_FOUND_GEM : MOD_STR_ACQUIRED);
	App_snprintf(text, 0x400, (char *)fmt, title, def->longName, def->description);
	Canvas_showDialog(game->canvas, NULL, text, 4, 1, 0);
	game->canvas->dialogItem = def;
}

/* 0x00027264  drops an item using the 8-slot drop-entity ring */
void Game_spawnDropItem(Game *game, int x, int y, int sprFlags, int eType, int subtype, int parm, int count)
{
	/* coins stack onto a coin pile already on the tile */
	if (eType == ETYPE_ITEM && subtype == 4) {
		for (Entity *e = Game_findEntityAt(game, x, y, 0x40); e != NULL; e = e->nextOnTile) {
			if (e->def->eType == ETYPE_ITEM && e->def->eSubType == 4) {
				e->param += count;
				return;
			}
		}
	}

	int slot = game->firstDrop + game->dropIndex;
	Entity *ent = &game->entities[slot];
	EntityDef *def = EntityDefs_find(game->app->entityDefs, eType, subtype, parm);
	ent->def = def;
	ent->name = def->name;

	game->dropIndex = (game->dropIndex + 1) % 8;
	/* never recycle a slot that holds a dropped quest item */
	EntityDef *nextDef = game->entities[game->firstDrop + game->dropIndex].def;
	if (nextDef != NULL && nextDef->eType == ETYPE_ITEM && nextDef->eSubType == 8)
		game->dropIndex = (game->dropIndex + 1) % 8;

	if (ent->info & ENT_LINKED)
		Game_unlinkEntity_2ae8c(game, ent);
	uint32_t info = ent->info;
	ent->info = info | ENT_DIRTY;

	Sprite *s = entitySprite(game->render, info);
	s->info = (s->info & 0xfffef000u) | (uint32_t)sprFlags;
	s->x = x;
	s->y = y;
	s->z = 0x20;
	s->ent = ent;
	ent->param = count;
	Render_relinkSpriteAt(game->render, &s->x, -1, -1);
	Game_linkEntity(game, ent, x >> 6, y >> 6);
}

/* 0x0002a044  touches fire on the tile (or everything if force); 1 if anything was touched */
int Game_touchTile(Game *game, int x, int y, int force)
{
	int touched = 0;
	Entity *e = Game_getTileEntities(game, x, y);
	while (e != NULL) {
		Entity *next = e->nextOnTile;
		if (force || e->def->eType == ETYPE_FIRE) {
			Game_touchEntity(e);
			touched = 1;
		}
		e = next;
	}
	return touched;
}

/* 0x0002a2ac  picks up an item entity (eType 6), by subtype */
void Game_pickupItem(void *entArg)
{
	Entity *ent = entArg;
	Applet *app = ent->app;
	Player *player = app->player;
	Hud *hud = app->hud;
	EntityDef *def = ent->def;
	char *buf;

	switch (def->eSubType) {
	case 0: { /* potion / consumable */
		if (!Player_give(player, 0, def->parm, 1, 0)) {
			buf = Hud_getMessageBuffer(hud, 0);
			App_snprintf(buf, 0x40, MODSTR(MOD_STR_CANT_HOLD_MORE), ent->def->name);
			Hud_finishMessageBuffer(hud);
			return;
		}
		Hud_startStatFlash(player);
		hud->msgTimeout = ent->app->tickTime + PICKUP_MSG_MS;
		buf = Hud_getMessageBuffer(hud, 1);
		int item = ent->def->parm;
		int snd;
		if (item == 0xb)
			snd = 0x141b;
		else if (item < 0 || item > 10)
			snd = 0x13c4;
		else
			snd = 0x1412;
		Sound_play(ent->app->sound, snd, 0, 3, -1);
		App_snprintf(buf, 0x40, MODSTR(MOD_STR_GOT), ent->def->longName);
		Hud_finishMessageBuffer(hud);
		break;
	}
	case 1: { /* weapon (+ its ammo) */
		int w = def->parm;
		if (!(player->weapons & arm_bit(w)))
			Player_selectWeapon(player, w & 0xff);
		Hud_startStatFlash(player);
		Player_give(player, 1, ent->def->parm, 1, 0);
		w = ent->def->parm;
		Weapon *wp = &ent->app->combat->weapons[w];
		if (wp->ammoUsage != 0)
			Player_give(player, 3, wp->ammoType, w, 0); /* amount = weapon index, as in the original */
		hud->msgTimeout = ent->app->tickTime + PICKUP_MSG_MS;
		buf = Hud_getMessageBuffer(hud, 1);
		App_snprintf(buf, 0x40, MODSTR(MOD_STR_GOT), ent->def->longName);
		Hud_finishMessageBuffer(hud);
		Hud_randomBark(hud, 7, 0);
		Player_showWeaponHelp(player, ent->def->parm, 0);
		break;
	}
	case 2: /* ring */
		Player_give(player, 2, def->parm, 1, 0);
		Player_showHelp(player, 2, 0);
		break;
	case 3: /* ammo; consumed even when full */
		if (!Player_give(player, 3, def->parm, 5, 0)) {
			Hud_addMessage(hud, MODSTR(MOD_STR_AMMO_MAX), 0);
			break;
		}
		Hud_startStatFlash(player);
		hud->msgTimeout = ent->app->tickTime + PICKUP_MSG_MS;
		buf = Hud_getMessageBuffer(hud, 1);
		App_snprintf(buf, 0x40, MODSTR(MOD_STR_GOT), ent->def->longName);
		Hud_finishMessageBuffer(hud);
		Sound_play(ent->app->sound, 0x13c4, 0, 3, -1);
		break;
	case 4: { /* coins: a dropped pile carries its count, a map pile sums its 0x2f event records */
		Player_showHelp(player, 9, 0);
		buf = Hud_getMessageBuffer(hud, 1);
		int coins = Entity_isDroppedEntity(ent) ? ent->param : Entity_sumTileEventOp2F(ent);
		App_snprintf(buf, 0x40, MODSTR(MOD_STR_GOT_COINS), coins,
			     MODSTR(coins == 1 ? MOD_STR_EMPTY : MOD_STR_PLURAL_S));
		Hud_finishMessageBuffer(hud);
		if (!Player_give(player, 4, 0, coins, 0)) {
			Hud_addMessage(hud, MODSTR(MOD_STR_COINS_MAX), 0);
			return;
		}
		Hud_startStatFlash(player);
		Sound_play(ent->app->sound, 0x1411, 0, 2, -1);
		break;
	}
	case 5: /* food: parm 1 heals 20, else 40; parm 0 food stays visible (only unlinked) */
		if (!Player_addHealth(player, def->parm == 1 ? 20 : 40)) {
			Hud_addMessage(hud, MODSTR(MOD_STR_HEALTH_MAX), 0);
			return;
		}
		Player_showHelp(player, 8, 0);
		Sound_play(ent->app->sound, 0x1419, 0, 2, -1);
		if (ent->def->parm == 0) {
			Game_unlinkEntity_2ae8c(ent->app->game, ent);
			return;
		}
		break;
	case 6:
		Sound_play(app->sound, 0x13b8, 0, 3, -1);
		break;
	case 8: /* quest item / gem */
		Player_give(player, 8, def->parm, 1, 0);
		Game_showItemAcquired(ent->app->game, 8, ent->def->parm);
		Sound_play(ent->app->sound, 0x1408, 0, 2, -1);
		break;
	default:
		break;
	}

	hud->msgTimeout = ent->app->tickTime + PICKUP_MSG_MS;
	Game_unlinkEntity_201d8(ent->app->game, ent);
}

/* 0x0002a648 */
void Game_touchEntity(void *entArg)
{
	Entity *ent = entArg;
	Player *player = ent->app->player;
	Hud *hud = ent->app->hud;

	switch (ent->def->eType) {
	case ETYPE_ITEM:
	case ETYPE_PICKUP_B:
		Game_pickupItem(ent);
		if (Entity_isDroppedEntity(ent)) {
			/* free the drop slot: detach the sprite and clear the definition */
			entitySprite(ent->app->render, ent->info)->ent = NULL;
			ent->def = NULL;
		}
		break;
	case ETYPE_FIRE:
		if (ent->def->parm == 0) {
			Player_hurtFeedback(player, 0);
			Player_takeDamage(player, 3, ent);
			Hud_addMessage(hud, MODSTR(MOD_STR_IT_BURNS), 0);
		} else {
			Player_hurtFeedback(player, 0);
			Player_takeDamage(player, 20, ent);
			Hud_addMessage(hud, MODSTR(MOD_STR_IT_REALLY_BURNS), 0);
		}
		break;
	case ETYPE_FIRE_PORTAL:
		Game_useFirePortal(ent);
		break;
	default:
		break;
	}
}

/* Wall-side bits of an entity's sprite. Index 0 made the original read the
 * word before sprites[0] (inside the last, normally unused, map line); the port
 * treats it as "no sprite". */
static uint32_t spriteSideBits(Render *render, uint32_t info)
{
	if ((info & 0xffff) == 0)
		return 0;
	return entitySprite(render, info)->info & SPRITE_SIDE_MASK;
}

/* 0x0002a6f4  walks an axis-aligned segment tile by tile (64 units), collecting the
 * entities whose type is in typeMask into Game.traceHits, nearest first. A world
 * entity (eType 0, i.e. a wall) ends the walk and records the hit point. */
void Game_trace(Game *game, int x0, int y0, int x1, int y1, void *ignore, int typeMask)
{
	game->traceCount++;
	if (x0 != x1 && y0 != y1) {
		game->traceEntity = NULL;
		game->numTraceHits = 0;
		return;
	}

#define CLAMP_COORD(v) ((v) < 0 ? 0 : (v) > 0x7ff ? 0x7ff : (v))
	const int endX = CLAMP_COORD(x1);
	const int endY = CLAMP_COORD(y1);
	const int startX = CLAMP_COORD(x0);
	const int startY = CLAMP_COORD(y0);
#undef CLAMP_COORD
	const int dx = endX - startX;
	const int dy = endY - startY;
	Render *render = game->render;

	game->numTraceHits = 0;
	int cx = startX, cy = startY;
	for (;;) {
		Entity *head = game->tileLists[(cx >> 6) + (cy >> 6) * 32];
		if (head != NULL) {
			/* sort by distance only when the tile holds something that is not a
			 * plain floor sprite (eType 12/13, or a wall-mounted sprite) */
			int sortTile = 0;
			for (Entity *e = head; e != NULL; e = e->nextOnTile) {
				uint32_t bit = arm_bit(e->def->eType);
				if (!(typeMask & bit))
					continue;
				if (bit & 0x3000) {
					sortTile = 1;
					break;
				}
				if (e->info & ENT_LINE)
					continue;
				if (spriteSideBits(render, e->info)) {
					sortTile = 1;
					break;
				}
			}

			int firstHit = game->numTraceHits;
			int numDist = 0;
			int dist[MAX_TRACE_HITS];
			for (Entity *e = head; e != NULL; e = e->nextOnTile) {
				if (e == ignore)
					continue;
				EntityDef *def = e->def;
				int type = def->eType;
				if (!(arm_bit(type) & typeMask))
					continue;

				uint32_t info = e->info;
				if ((info & 0xffff) != 0 && !(info & ENT_LINE)) {
					Sprite *s = entitySprite(render, info);
					uint32_t sinfo = s->info;
					if (sinfo & SPRITE_SIDE_MASK) {
						/* wall-mounted sprite: must lie within the segment and face along it */
						if (startX != endX) {
							if (sinfo & SPRITE_SIDE_XMASK)
								continue;
							if (s->x > startX && s->x > endX)
								continue;
							if (s->x < startX && s->x < endX)
								continue;
						}
						if (startY != endY) {
							if (sinfo & SPRITE_SIDE_YMASK)
								continue;
							if (s->y > startY && s->y > endY)
								continue;
							if (s->y < startY && s->y < endY)
								continue;
						}
					} else {
						/* free-standing: must be on the forward side of the start */
						int px, py;
						if (e->monster != NULL) {
							px = e->monster->destX;
							py = e->monster->destY;
						} else if (type == ETYPE_RELIQUARY && def->eSubType == 4) {
							px = (s->x >> 6) * 64 + 32;
							py = (s->y >> 6) * 64 + 32;
						} else {
							px = s->x;
							py = s->y;
						}
						if ((px - startX < 0) != (dx < 0))
							continue;
						if ((py - startY < 0) != (dy < 0))
							continue;
					}
				}

				if (type == ETYPE_WORLD) {
					/* wall: hit point just outside the tile on the near side */
					game->traceX = cx;
					game->traceY = cy;
					if (startX > endX)
						game->traceX = (cx >> 6) * 64 + 0x41;
					else if (startX < endX)
						game->traceX = (cx >> 6) * 64 - 1;
					if (startY > endY)
						game->traceY = (cy >> 6) * 64 + 0x41;
					else if (startY < endY)
						game->traceY = (cy >> 6) * 64 - 1;
					int hx = game->traceX - startX;
					int hy = game->traceY - startY;
					if (numDist < MAX_TRACE_HITS)
						dist[numDist++] = hx * hx + hy * hy;
					/* stop after this tile */
					cx = endX;
					cy = endY;
				} else if (sortTile) {
					int d = Entity_distSqFrom(e, startX, startY);
					if (numDist < MAX_TRACE_HITS)
						dist[numDist++] = d;
				}
				/* the original had no bound here; 32 hits never happen in practice */
				if (game->numTraceHits < MAX_TRACE_HITS)
					game->traceHits[game->numTraceHits++] = e;
			}

			/* bubble sort this tile's hits by distance */
			for (int i = 0; i < numDist - 1; i++) {
				for (int j = 0; j < numDist - 1 - i; j++) {
					if (dist[j + 1] < dist[j]) {
						int t = dist[j];
						dist[j] = dist[j + 1];
						dist[j + 1] = t;
						Entity *te = game->traceHits[firstHit + j + 1];
						game->traceHits[firstHit + j + 1] = game->traceHits[firstHit + j];
						game->traceHits[firstHit + j] = te;
					}
				}
			}
		}

		if (cx == endX && cy == endY)
			break;
		if (cx < endX) {
			cx += 64;
			if (cx > endX)
				cx = endX;
		} else if (cx > endX) {
			cx -= 64;
			if (cx < endX)
				cx = endX;
		}
		if (cy < endY) {
			cy += 64;
			if (cy > endY)
				cy = endY;
		} else if (cy > endY) {
			cy -= 64;
			if (cy < endY)
				cy = endY;
		}
	}

	game->traceEntity = game->numTraceHits > 0 ? game->traceHits[0] : NULL;
}
