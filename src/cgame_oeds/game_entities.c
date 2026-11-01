/*
 * Game: entity bookkeeping on the 32x32 tile grid.
 *
 * Every tile has a singly-walked, doubly-linked chain of entities
 * (Game.tileLists[ty*32+tx], links Entity.nextOnTile/prevOnTile, the tile index
 * cached in Entity.linkIndex). Entity 0 is the "world" dummy that marks solid
 * tiles; it is always appended at the tail of a chain and is never unlinked
 * through the normal path. Entity 1 is the player.
 *
 * Also here: building the entity table for a freshly loaded map
 * (Game_loadMapEntities), clearing it again (Game_unloadMap), placing the
 * player (Game_spawnPlayer), the event direction masks and the short-range
 * path finder used by the monster AI.
 */

#include "game.h"

/* "MAX_ENTITIES" / "MAX_MONSTERS" error strings (App_setErrorStr, error code 1). */
#define MOD_STR_MAX_ENTITIES 0x30428u
#define MOD_STR_MAX_MONSTERS 0x30438u

#define MAX_ENTITIES 400
#define MAX_MONSTERS 100

/* Entity.info bits */
#define INFO_SPRITE_MASK 0x0000ffffu /* 1-based Render sprite (or line) index */
#define INFO_HIDDEN_SPR  0x00010000u /* (sprite info) hidden */
#define INFO_ALIVE       0x00040000u
#define INFO_ACTIVE      0x00080000u /* on the active monster ring */
#define INFO_LINE_ENTITY 0x00200000u /* low 16 bits index Render.lines, not sprites */
#define INFO_LINKED      0x04000000u /* on a tile list */

/* Sprite.info bits used while spawning */
#define SPR_TILE_PLUS    0x00040000u /* tile id + 0x101 (wall-texture sprite) */
#define SPR_HAS_ENTITY   0x00020000u /* sprite without a def still gets an entity */
#define SPR_NO_ENTITY    0x08000000u /* skip (flag is consumed) */
#define SPR_SIDE_MASK    0x00780000u /* wall-side bits 19..22 */

/* Line.flags facing bits (door entity placement) */
#define LINE_FACE_N      0x0800u
#define LINE_FACE_S      0x1000u
#define LINE_FACE_E      0x2000u
#define LINE_FACE_W      0x4000u

/* Map ids */
#define MAP_HUB          6011   /* DS 6011 (BREW 6008): Gaya's hub / dragon map */

static Entity **tileSlot(Game *game, int index)
{
	return &game->tileLists[(int16_t)index];
}

/* Entity.info low 16 bits -> Render sprite */
static Sprite *entitySprite(Render *render, Entity *ent)
{
	return Render_spriteAt(render, (ent->info & INFO_SPRITE_MASK));
}

/* 0x000115f0  First entity on the tile at world (x, y) whose def type is in typeMask.
 * With bit 1 (player) set and (x, y) on the player's destination tile, the player entity wins. */
void *Game_findEntityAt(Game *game, int x, int y, int typeMask)
{
	Entity *ent;

	if ((typeMask & 2) && (x >> 6) == (game->canvas->destX >> 6) &&
	    (y >> 6) == (game->canvas->destY >> 6))
		return &game->entities[1];

	ent = Game_getTileEntities(game, x, y);
	while (ent != NULL && (arm_bit(ent->def->eType) & typeMask) == 0)
		ent = ent->nextOnTile;
	return ent;
}

/* 0x00011640  Head of the tile list at world (x, y); NULL off the map. */
void *Game_getTileEntities(Game *game, int x, int y)
{
	int tx = x >> 6, ty = y >> 6;

	if (tx < 0 || tx >= 32 || ty < 0 || ty >= 32)
		return NULL;
	return game->tileLists[ty * 32 + tx];
}

/* 0x00014738  1 if the world entity (def type 0) is on tile (tx, ty). */
int Game_tileHasWorldEntity(Game *game, int tx, int ty)
{
	Entity *ent;

	for (ent = *tileSlot(game, ty * 32 + tx); ent != NULL; ent = ent->nextOnTile) {
		if (ent->def->eType == 0)
			return 1;
	}
	return 0;
}

/* 0x00018774  Push an entity on the front of tile (tx, ty)'s list. Ignores the
 * world entity and entities that are already linked somewhere. */
void Game_linkEntity(Game *game, void *entity, int tx, int ty)
{
	Entity *ent = entity;
	int16_t index = (int16_t)(ty * 32 + tx);
	Entity *head;

	if (ent == &game->entities[0] || ent->nextOnTile != NULL || ent->prevOnTile != NULL ||
	    game->tileLists[index] == ent)
		return;

	head = game->tileLists[index];
	ent->nextOnTile = head;
	if (head != NULL)
		head->prevOnTile = ent;
	ent->prevOnTile = NULL;
	ent->linkIndex = index;
	game->tileLists[index] = ent;
	ent->info |= INFO_LINKED;
}

/* 0x0001883c  Append the world entity to tile (tx, ty) (unless a type-0 entity is
 * already there) and mark the tile solid. */
void Game_linkWorldEntity(Game *game, int tx, int ty)
{
	int16_t index = (int16_t)(ty * 32 + tx);
	Entity *ent, *last = NULL;

	for (ent = game->tileLists[index]; ent != NULL; ent = ent->nextOnTile) {
		if (ent->def->eType == 0)
			return;
		last = ent;
	}
	if (last == NULL)
		game->tileLists[index] = &game->entities[0];
	else
		last->nextOnTile = &game->entities[0];
	game->entities[0].nextOnTile = NULL;
	game->entities[0].prevOnTile = NULL;
	game->entities[0].linkIndex = index;
	game->render->mapFlags[index] |= 1;
}

/* 0x000188a0  Link the player entity at the canvas destination tile (if on the map). */
void Game_relinkAtViewDest(void *player)
{
	Player *p = player;
	int x = p->canvas->destX, y = p->canvas->destY;

	if (x >= 0 && x < 0x800 && y >= 0 && y < 0x800)
		Game_linkEntity(p->game, &p->game->entities[1], x >> 6, y >> 6);
}

/* 0x00020168 */
void Game_relinkEntity(void *player)
{
	Game_unlinkPlayerEntity(player);
	Game_relinkAtViewDest(player);
}

/* 0x000201d8  Hide an entity's sprite (not for line entities) and take it off its tile. */
void Game_unlinkEntity_201d8(Game *game, void *entity)
{
	Entity *ent = entity;
	uint32_t info = ent->info;

	if ((info & INFO_SPRITE_MASK) != 0 && !(info & INFO_LINE_ENTITY))
		Render_spriteAt(game->render, (info & INFO_SPRITE_MASK))->info |= INFO_HIDDEN_SPR;
	if (ent->info & INFO_LINKED)
		Game_unlinkEntity_2ae8c(game, ent);
}

/* 0x0002ae8c  Remove an entity from its tile list (never the world entity). */
void Game_unlinkEntity_2ae8c(Game *game, void *entity)
{
	Entity *ent = entity;

	if (ent == &game->entities[0])
		return;
	if (game->tileLists[ent->linkIndex] == ent)
		game->tileLists[ent->linkIndex] = ent->nextOnTile;
	else if (ent->prevOnTile != NULL)
		ent->prevOnTile->nextOnTile = ent->nextOnTile;
	if (ent->nextOnTile != NULL)
		ent->nextOnTile->prevOnTile = ent->prevOnTile;
	ent->nextOnTile = NULL;
	ent->prevOnTile = NULL;
	ent->info &= ~INFO_LINKED;
}

/* 0x0002af44  Take the world entity off tile (tx, ty) and clear the solid flag.
 * The world entity is always last on a chain, so cutting the chain before it is enough. */
void Game_unlinkPlayerFromTile(Game *game, int tx, int ty)
{
	int16_t index = (int16_t)(ty * 32 + tx);
	Entity *ent, *prev = NULL;

	if (game->tileLists[index] == &game->entities[0]) {
		game->tileLists[index] = NULL;
	} else {
		for (ent = game->tileLists[index]; ; ent = ent->nextOnTile) {
			if (ent == NULL)
				return;
			if (ent == &game->entities[0])
				break;
			prev = ent;
		}
		if (prev != NULL)
			prev->nextOnTile = NULL;
	}
	game->render->mapFlags[index] &= 0xfe;
}

/* 0x0002afa4 */
void Game_unlinkPlayerEntity(void *pl)
{
	Game *game = ((Player *)pl)->game;

	if (game->entities[1].info & INFO_LINKED)
		Game_unlinkEntity_2ae8c(game, &game->entities[1]);
}

/* 0x0000ea40  Remember the direction of the last step as script trigger bits
 * (A: 0x10 N, 0x20 E, 0x40 S, 0x80 W; B: 4 N, 8 E, 1 S, 2 W; (-1,-1) = all). */
void *Game_setMoveDirMask(Game *game, int x0, int y0, int x1, int y1)
{
	uint32_t a = 0, b = 0;

	if (x0 == -1 && y0 == -1) {
		a = 0xf0;
		b = 0xf;
	} else if (x0 < x1) {
		a = 0x20;
		b = 8;
	} else if (x1 < x0) {
		a = 0x80;
		b = 2;
	} else if (y1 < y0) {
		a = 0x10;
		b = 4;
	} else if (y0 < y1) {
		a = 0x40;
		b = 1;
	}
	game->moveDirMaskA = a;
	game->moveDirMaskB = b;
	return &game->f6c30;
}

/*
 * 0x0000e6a0  Breadth-first search in an 8x8 window around (x, y) (tile coords,
 * only offsets -3..3 are used; cell = (dy+4)*8 + (dx+4), start cell 0x24) towards
 * (tx, ty) clamped into the window. Neighbours are tried west, east, south, north;
 * a step is allowed when Entity_checkLineOfSight passes with `mask`. If the target
 * is unreachable the closest visited cell is used. Writes the first step
 * {dx, dy} to outDir and returns 1, or 0 if nothing was reachable.
 */
int Game_findPathStep(void *entity, int x, int y, int tx, int ty, int mask, int *outDir)
{
	Entity *ent = entity;
	Game *game;
	int32_t *visited;
	uint8_t *parent, *queue;
	int goalX, goalY, goalCell;
	int best = 0xff, bestDist = 0x7fffffff;
	int cell, prev;

	if (x == tx && y == ty) {
		outDir[0] = 0;
		outDir[1] = 0;
		return 1;
	}

	game = ent->app->game;
	visited = game->pathVisited;
	parent = game->pathParent;
	queue = game->pathQueue;
	memset(visited, 0, sizeof(game->pathVisited));
	memset(parent, 0xff, sizeof(game->pathParent));

	goalX = tx - x;
	if (goalX < -3)
		goalX = -3;
	else if (goalX > 3)
		goalX = 3;
	goalY = ty - y;
	if (goalY < -3)
		goalY = -3;
	else if (goalY > 3)
		goalY = 3;
	goalCell = goalY * 8 + goalX + 0x24;

	game->pathQueueLen = 1;
	queue[0] = 0x24;
	while (game->pathQueueLen > 0) {
		int len;

		if (queue[0] == goalCell) {
			best = queue[0];
			break;
		}
		if (visited[queue[0]] == 0) {
			int dx, dy, d, sx, sy;

			visited[queue[0]] = 1;
			cell = queue[0];
			dx = (cell & 7) - 4;
			dy = (cell >> 3) - 4;
			d = (goalX - dx) * (goalX - dx) + (goalY - dy) * (goalY - dy);
			if (d < bestDist && cell != 0x24) {
				bestDist = d;
				best = cell;
			}
			sx = x + dx;
			sy = y + dy;

/* Queue `next` (reached from the front cell); the original never checked the
 * 64-byte queue bound (overflow would hit pathQueueLen), the port drops the push. */
#define PATH_PUSH(next) do { \
		uint8_t n_ = (uint8_t)(next); \
		if (game->pathQueueLen < (int)sizeof(game->pathQueue)) { \
			queue[game->pathQueueLen] = n_; \
			parent[n_] = queue[0]; \
			game->pathQueueLen++; \
		} \
	} while (0)

			if (dx != -3 && visited[queue[0] - 1] == 0 &&
			    Entity_checkLineOfSight(ent, sx, sy, sx - 1, sy, mask))
				PATH_PUSH(queue[0] - 1);
			if (dx != 3 && visited[queue[0] + 1] == 0 &&
			    Entity_checkLineOfSight(ent, sx, sy, sx + 1, sy, mask))
				PATH_PUSH(queue[0] + 1);
			if (dy != 3 && visited[queue[0] + 8] == 0 &&
			    Entity_checkLineOfSight(ent, sx, sy, sx, sy + 1, mask))
				PATH_PUSH(queue[0] + 8);
			if (dy != -3 && visited[queue[0] - 8] == 0 &&
			    Entity_checkLineOfSight(ent, sx, sy, sx, sy - 1, mask))
				PATH_PUSH(queue[0] - 8);
#undef PATH_PUSH
		}
		/* pop the front */
		len = game->pathQueueLen;
		game->pathQueueLen = len - 1;
		memmove(queue, queue + 1, (size_t)(len - 1));
	}

	if (best == 0xff)
		return 0;

	/* walk back to the cell whose parent is the start cell */
	cell = best;
	prev = best;
	while (parent[cell] != 0xff) {
		prev = cell;
		cell = parent[cell];
	}
	outDir[0] = (prev & 7) - 4;
	outDir[1] = (prev >> 3) - 4;
	return 1;
}

/* 0x00018f30  Build the entity table for the map just loaded by Render:
 * world + player entities, one entity per map sprite with a def (monsters get a
 * monster slot and go on the inactive ring), the 8 drop slots, the hub's special
 * monster, door/line entities, and the world entity on every solid tile. */
void Game_loadMapEntities(Game *game)
{
	Applet *app = game->app;
	Render *render;
	Combat *combat;
	EntityDef *def, *defType0c, *defType0d;
	Entity *ent;
	int mapId = game->canvas->mapId;
	int progress = game->player->maxMapReached - 6000;
	int i;

	memset(game->vars, 0, sizeof(game->vars));
	game->alertMonster = NULL;
	game->entityMemory = App_getMemUsed(app);
	game->levelStartTime = app->gameTime;
	game->numMonsters = 0;
	game->numLerps = 0;
	for (i = 0; i < 16; i++)
		game->lerps[i].hSprite = 0;
	game->numThreads = 0;
	for (i = 0; i < 16; i++)
		game->threads[i].inUse = 0;

	/* The wand (weapon 0) takes the per-map variant 39..47. */
	combat = app->combat;
	if (progress < 12 && mapId != MAP_HUB)	/* (DS: 12 maps; BREW 9) */
		combat->weapons[0] = combat->weapons[39 + progress];
	else
		combat->weapons[0] = combat->weapons[47];

	/* entity 0: the world */
	game->numEntities = 1;
	def = EntityDefs_find(app->entityDefs, 0, 0, -1);
	game->entities[0].def = def;
	game->entities[0].name = def->name;

	/* entity 1: the player */
	i = game->numEntities++;
	def = EntityDefs_find(app->entityDefs, 1, 0, -1);
	game->entities[i].def = def;
	game->entities[i].name = def->name;
	game->entities[i].info = INFO_ALIVE;

	/* Defs for sprites without their own def but flagged to get an entity. */
	defType0c = EntityDefs_find(app->entityDefs, 0xc, 0, -1);
	defType0d = EntityDefs_find(app->entityDefs, 0xd, 0, -1);

	for (i = 0; render = game->render, i < render->numMapSprites; i++) {
		Sprite *spr = &render->sprites[i];
		uint32_t info = spr->info;
		int tile, x, y;
		uint32_t side;

		if (info & SPR_NO_ENTITY) {
			spr->info = info & ~SPR_NO_ENTITY;
			continue;
		}
		tile = info & 0xff;
		if (info & SPR_TILE_PLUS)
			tile += 0x101;
		x = spr->x;
		y = spr->y;
		/* Wall-mounted sprites sit on the wall line; move the link point into the tile. */
		side = (info & 0x7fffff) >> 19;
		if (side != 0xc && side != 3 && side != 0) {
			if (info & 0x200000)
				x -= 0x40;
			else if (info & 0x100000)
				y -= 0x40;
			else if (info & 0x80000)
				y += 0x20;
			else if (info & 0x400000)
				x += 0x20;
		}

		def = EntityDef_lookup(app->entityDefs, tile);
		if (def == NULL) {
			if (!(spr->info & SPR_HAS_ENTITY))
				continue;
			if (game->numEntities == MAX_ENTITIES)
				goto tooManyEntities;
			ent = &game->entities[game->numEntities++];
			ent->info = (uint32_t)(i + 1) & 0xffff;
			def = (tile == 0xb6 || tile == 0xa8) ? defType0d : defType0c;
			ent->def = def;
			ent->name = def->name;
			spr->ent = ent;
		} else {
			if (game->numEntities == MAX_ENTITIES)
				goto tooManyEntities;
			ent = &game->entities[game->numEntities++];
			ent->def = def;
			ent->info = (uint32_t)(i + 1) & 0xffff;
			if (def->eType == 2) {
				if (game->numMonsters == MAX_MONSTERS) {
					App_setErrorStr(app, "%s", ModData_str(&g_mod, MOD_STR_MAX_MONSTERS));
					App_setErrorCode(g_app, 1);
					return;
				}
				ent->monster = &game->monsters[game->numMonsters];
				EntityMonster_reset(&game->monsters[game->numMonsters]);
				game->numMonsters++;
				ent->info |= INFO_ACTIVE;
				Game_deactivate(game, ent);
			}
			Entity_initspawn(ent);
			spr->ent = ent;
		}
		if (!(spr->info & INFO_HIDDEN_SPR))
			Game_linkEntity(game, ent, x >> 6, y >> 6);
	}

	/* 8 drop slots, sprites hidden until used */
	game->firstDrop = game->numEntities;
	for (i = 0; i < 8; i++) {
		if (game->numEntities == MAX_ENTITIES)
			goto tooManyEntities;
		ent = &game->entities[game->numEntities++];
		ent->info = (uint32_t)(game->render->firstDropSprite + i + 1) & 0xffff;
		game->render->dropSprites[i]->info |= INFO_HIDDEN_SPR;
	}

	/* the hub's special monster (sprite 0x14, wererat?) with its first respawn time */
	if (game->canvas->mapId == MAP_HUB) {
		EntityMonster *mon;

		if (game->numEntities == MAX_ENTITIES)
			goto tooManyEntities;
		ent = &game->entities[game->numEntities++];
		ent->info = (uint32_t)((game->render->hubSprite - game->render->sprites) + 1) & 0xffff;
		ent->def = EntityDef_lookup(app->entityDefs, 0x14);
		mon = &game->monsters[game->numMonsters++];
		ent->monster = mon;
		EntityMonster_reset(mon);
		ent->info |= INFO_ACTIVE;
		Game_deactivate(game, ent);
		Entity_initspawn(ent);
		game->render->hubSprite->ent = ent;
		game->canvas->hubEventTime = (Random_nextInt(&app->random) % 180) * 1000 + 30000;
	}

	/* door / line entities, linked on the tile in front of the line */
	game->firstDoor = game->numEntities;
	for (i = 0; render = game->render, i < render->numLines; i++) {
		Line *line = &render->lines[i];
		int x, y;

		def = EntityDef_lookup(app->entityDefs, line->texture + 0x101);
		if (def == NULL)
			continue;
		if (game->numEntities == MAX_ENTITIES)
			goto tooManyEntities;
		ent = &game->entities[game->numEntities++];
		ent->def = def;
		ent->info = ((uint32_t)(i + 1) & 0xffff) + INFO_LINE_ENTITY;
		Entity_initspawn(ent);
		x = (line->x2 - line->x1) / 2 + line->x1;
		y = (line->y2 - line->y1) / 2 + line->y1;
		if (line->flags & LINE_FACE_N)
			y -= 1;
		else if (line->flags & LINE_FACE_E)
			x += 1;
		else if (line->flags & LINE_FACE_S)
			y += 1;
		else if (line->flags & LINE_FACE_W)
			x -= 1;
		Game_linkEntity(game, ent, x >> 6, y >> 6);
	}
	game->lastDoor = game->numEntities - 1;

	/* the world entity on every solid tile without a line entity */
	for (i = 0; i < 1024; i++) {
		if (game->render->mapFlags[i] & 1) {
			for (ent = game->tileLists[i]; ent != NULL; ent = ent->nextOnTile) {
				if (ent->info & INFO_LINE_ENTITY)
					break;
			}
			if (ent == NULL)
				Game_linkWorldEntity(game, i % 32, i / 32);
		}
	}

	game->entityMemory -= App_getMemUsed(app);
	Game_loadCameras(game);
	Game_initDebugVars(game);
	return;

tooManyEntities:
	App_setErrorStr(app, "%s", ModData_str(&g_mod, MOD_STR_MAX_ENTITIES));
	App_setErrorCode(g_app, 1);
}

/* 0x0002afc0  Forget the current map's entities, sprites in flight and lists. */
void Game_unloadMap(Game *game)
{
	int i;

	game->numActiveGSprites = 0;
	game->numMovingGSprites = 0;
	game->numBlocks = 0;
	game->numBlockingBlocks = 0;
	game->goldPaid = 0;
	game->player->facing = NULL;
	game->app->combat->target = NULL;
	game->app->combat->attacker = NULL;
	game->app->hud->lastTarget = NULL;
	for (i = 0; i < 24; i++) {
		if (game->gsprites[i].flags & 1)
			Game_gspriteDestroy(game, &game->gsprites[i]);
	}
	game->inactiveMonsters = NULL;
	game->activeMonsters = NULL;
	game->combatMonsters = NULL;
	game->password[0] = '\0';
	for (i = 0; i < 1024; i++)
		game->tileLists[i] = NULL;
	for (i = 0; i < game->numEntities; i++)
		Entity_reset(&game->entities[i]);
	game->numCameraKeys = 0;
	game->numCameras = 0;
}

/* 0x0002745c  Place the player at the pending spawn point (Game.spawnParam:
 * x bits 0-4, y bits 5-9, dir bits 10-17) or the map's default spawn, run the
 * arrival tile event, and on entering the hub from a dungeon pull the mood
 * half way back to neutral. */
void Game_spawnPlayer(Game *game)
{
	Canvas *cv = game->canvas;
	uint32_t param = game->spawnParam;
	int tx, ty, dir, angle;

	if (param == 0) {
		int tile = game->render->spawnTile;

		ty = tile / 32;
		tx = tile - ty * 32;
		dir = game->render->spawnDir;
	} else {
		ty = (int)((param & 0x3ff) >> 5);
		tx = (int)(param & 0x1f);
		dir = (int)((param & 0x3ffff) >> 10);
		game->spawnParam = 0;
	}

	if (dir == 0)
		angle = 0x200;
	else if (dir == 1)
		angle = 0;
	else if (dir == 3)
		angle = 0x300;
	else
		angle = 0x100;

	cv->destX = cv->viewX = tx * 0x40 + 0x20;
	cv->destY = cv->viewY = ty * 0x40 + 0x20;
	cv->viewZ = 0x24;
	cv->destAngle = cv->viewAngle = angle;
	game->player->ce.x = cv->viewX;
	game->player->ce.y = cv->viewY;
	Game_relinkEntity(game->player);
	game->render->f24 = 4;
	game->phaseTime = game->app->tickTime;

	if (game->isLoaded == 0) {
		cv->prevX = cv->destX;
		cv->prevY = cv->destY;
		Game_executeTile(game, cv->viewX >> 6, cv->viewY >> 6, 0x40f, 1);
		Canvas_finishRotation(cv);
		if (cv->mapId == MAP_HUB && cv->prevMapId < MAP_HUB) {
			Player *p = game->player;
			Hud *hud = game->app->hud;
			int mood = (((int)p->mood - 0x80) >> 1) + 0x80;

			p->mood = (uint8_t)mood;
			hud->moodTarget = mood & 0xff;
			hud->moodShown = p->mood;
			hud->moodTime = 0;
		}
		Canvas_dequeueHelpDialog(cv, 1);
	}
	Combat_checkSurrounded(game->app->combat);
}

/* 0x00025f80  Debug "respawn" (developer tools): drop 8 items at the spawn tile,
 * resurrect every corpse where it lies, wake every dormant monster and re-arm
 * one-shot script commands (flag 0x200 -> the whole flags word is cleared). */
void Game_setRespawn(Game *game)
{
	Render *render;
	int i;

	for (i = 0; i < 8; i++) {
		int tile = game->render->spawnTile;

		Game_spawnDropItem(game, tile & 0x1f, (tile & 0x3ff) >> 5, 99, 6, 0, 1, 0);
	}
	for (i = 0; i < game->numEntities; i++) {
		Entity *ent = &game->entities[i];
		EntityDef *def = ent->def;

		if (def == NULL)
			continue;
		if (def->eType == 9) {
			int link = ent->linkIndex;
			int y = link / 32;

			Entity_resurrect(ent, link - y * 32, y, 0x20);
		} else if (def->eType == 2 && !(ent->info & INFO_ACTIVE)) {
			Game_activateEx(game, ent, 0, 0);
		}
	}
	for (i = 0; render = game->render, i < render->numCommands; i++) {
		if (render->commands[i].arg2 & 0x200)
			render->commands[i].arg2 = 0;
	}
}
