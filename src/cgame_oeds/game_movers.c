/*
 * Game: moving wall blocks (script op 0x48/0x60), cinematic skipping, and the
 * small helpers script opcodes call (containers, traps, fire portals, notes).
 *
 * A moving block is a tile whose four edges are "moving" map lines (line flag
 * 0x80000, facing 0x800/0x1000/0x2000/0x4000). Moving it shifts those lines,
 * relinks them in the BSP and moves the tile's solid flag and world entity.
 */

#include "game.h"

#define MOD_STR_MAX_MOVING_BLOCKS  0x3049cu
#define MOD_STR_CRUSHED            0x3046cu
#define MOD_STR_FIREPIT_NO_DEST    0x3004cu
#define MOD_STR_FIREPORTAL_NO_DEST 0x30078u
#define MOD_STR_CHEST_CONTENTS     0x30168u
#define MOD_STR_RELIQUARY_CONTENTS 0x3017cu
#define MOD_STR_CONTAINER_BULLET   0x30194u /* (bullet glyph) */
#define MOD_STR_CONTAINER_GOLD     0x30198u
#define MOD_STR_CONTAINER_ITEM     0x301a4u
#define MOD_STR_CONTAINER_EOL      0x301b0u

#define MAX_MOVING_BLOCKS 40

#define TILEFLAG_SOLID     0x01
#define TILEFLAG_HAS_EVENT 0x08

/* Line.flags */
#define LINE_MOVING  0x80000u
#define LINE_FACE_N  0x800u  /* edge above the block origin */
#define LINE_FACE_S  0x1000u
#define LINE_FACE_E  0x2000u
#define LINE_FACE_W  0x4000u

/* Entity.info bits */
#define ENT_INFO_CRUSHED 0x120000u

/* Event opcodes read directly here */
#define OP_CONTAINER_ITEM  0x2f
#define OP_FIREPIT_DEST    0x3a
#define OP_NOTE            0x28

/* Container record types (arg1 & 0xff) */
#define CONTAINER_TEXT 0xff
#define CONTAINER_NOTE 0xfe

#define EV_FIRST(ev) (((ev) & 0xfffff) >> 9)	/* (DS: 11 bits from bit 9) */
#define EV_COUNT(ev) (((ev) & 0x7ffffff) >> 20)

/* 0x00026e4c
 * Finish the running cinematic at once: snap every mover and keep jumping the
 * camera to its key end (which resumes the script) until the script ends it. */
void Game_skipCinematic(Game *game)
{
	game->cinematicActive = 1;
	while (game->cinematicActive) {
		Game_snapMovingWalls(game, 1);
		Game_snapLerpSprites(game);
		ScriptThread_checkWait(game->activeCamera->thread, game->app->gameTime + 0x40000000);
		Camera_snapToKey(game->activeCamera, game->cameraKey);
	}
	if (game->canvas->state != 0x14)
		Canvas_updateMovement(game->canvas);
}

/* 0x00006d44 */
int Game_isIdle(Game *game)
{
	return game->numBlockingBlocks == 0;
}

/* 0x00026eb8 */
int Game_snapAllMovers(Game *game, int force)
{
	if (Game_snapMovingWalls(game, force) && Game_snapAllMonsterSprites(game, force))
		return 1;
	return 0;
}

/* 0x00027120
 * Finish all moving blocks (non-blocking ones only when forced). Unless forced,
 * only done when no blocking block is moving; returns 1 if it ran. */
int Game_snapMovingWalls(Game *game, int force)
{
	if (!force && !Game_isIdle(game))
		return 0;

	game->numBlocks = 0;
	game->numBlockingBlocks = 0;
	for (int i = 0; i < MAX_MOVING_BLOCKS; i++) {
		MovingBlock *b = &game->blocks[i];
		if (!b->active)
			continue;
		if (!b->nonBlocking || force) {
			b->active = 0;
			Game_moveBlock(game, b, b->dstX, b->dstY);
		} else {
			game->numBlocks++;
		}
	}
	return 1;
}

/* 0x0002ba04
 * Advance the moving blocks (game clock); returns how many are still moving. */
int Game_updateMovingWalls(Game *game)
{
	int moving = 0;
	if (game->numBlocks == 0)
		return 0;

	game->numBlockingBlocks = 0;
	for (int i = 0; i < MAX_MOVING_BLOCKS; i++) {
		MovingBlock *b = &game->blocks[i];
		if (!b->active)
			continue;
		int elapsed = game->app->tickTime - b->startTime;
		if (elapsed < b->duration) {
			if (b->blocking)
				game->numBlockingBlocks++;
			int frac = (elapsed * 0x10000) / (b->duration << 8); /* 8.8 */
			Game_moveBlock(game, b,
			               ((frac * (b->dstX - b->x) * 0x100) >> 16) + b->x,
			               ((frac * (b->dstY - b->y) * 0x100) >> 16) + b->y);
			moving++;
		} else {
			b->active = 0;
			Game_moveBlock(game, b, b->dstX, b->dstY);
		}
	}
	game->numBlocks = moving;
	return moving;
}

/* Attach one edge line to a block side. */
static void attachLine(MovingBlock *b, int side, int lineIdx, Line *line, int x, int y)
{
	b->lineIdx[side] = (uint8_t)lineIdx;
	b->lines[side] = line;
	b->ofs1[side * 2] = line->x1 - x;
	b->ofs1[side * 2 + 1] = line->y1 - y;
	b->ofs2[side * 2] = line->x2 - x;
	b->ofs2[side * 2 + 1] = line->y2 - y;
}

/* 0x00003464
 * Start moving the block at world (x, y) to (dstX, dstY) over `duration` ms.
 * With every slot busy, the slot with the smallest (elapsed - duration) is
 * finished at once and reused. `flag` = non-blocking (doesn't hold the turn). */
void Game_startMovingBlock(Game *game, int x, int y, int dstX, int dstY, int duration, int flag)
{
	int best = 100000;
	int slot = -1;

	for (int i = 0; i < MAX_MOVING_BLOCKS; i++) {
		MovingBlock *b = &game->blocks[i];
		if (!b->active) {
			slot = i;
			break;
		}
		int late = (game->app->tickTime - b->startTime) - b->duration;
		if (late < best) {
			best = late;
			slot = i;
		}
	}
	if (slot < 0) {
		App_setErrorStr(game->app, (char *)ModData_str(&g_mod, MOD_STR_MAX_MOVING_BLOCKS));
		App_setErrorCode(g_app, 1);
		return;
	}

	MovingBlock *b = &game->blocks[slot];
	if (!b->active)
		game->numBlocks++;
	else
		Game_moveBlock(game, b, b->dstX, b->dstY);

	b->active = 1;
	b->nonBlocking = (uint8_t)flag;
	b->blocking = 0;
	b->tileX = x >> 6;
	b->tileY = y >> 6;
	b->startTime = game->app->tickTime;
	b->duration = duration;
	b->x = x;
	b->y = y;
	b->dstX = dstX;
	b->dstY = dstY;

	int fromWall = Render_tileTouchesWall(game->render, b->tileX, b->tileY);
	int toWall = Render_tileTouchesWall(game->render, dstX >> 6, dstY >> 6);
	if (!b->nonBlocking && (toWall || fromWall)) {
		b->blocking = 1;
		game->numBlockingBlocks++;
	}

	/* find the four moving edge lines around (x, y) by their midpoints */
	for (int i = 0; i < game->render->numMovingLines; i = (i + 1) & 0xff) {
		Line *line = game->render->movingLines[i].line;
		int mx = (line->x1 + line->x2) >> 1;
		int my = (line->y1 + line->y2) >> 1;
		uint32_t fl = line->flags;

		if (!(fl & LINE_MOVING))
			continue;
		if ((fl & LINE_FACE_N) && mx == x && y <= my && my <= y + 0x20)
			attachLine(b, 2, i, line, x, y);
		else if ((fl & LINE_FACE_S) && mx == x && y - 0x20 <= my && my <= y)
			attachLine(b, 3, i, line, x, y);
		else if ((fl & LINE_FACE_E) && x - 0x20 <= mx && mx <= x && my == y)
			attachLine(b, 1, i, line, x, y);
		else if ((fl & LINE_FACE_W) && mx <= x + 0x20 && x <= mx && my == y)
			attachLine(b, 0, i, line, x, y);
	}
}

/* 0x0001c7b4
 * Put the block at world (x, y). Crossing into a new tile crushes whatever
 * stands there, moves the solid flag / world entity, relinks the edge lines
 * and lets the player get pushed out. Then the four edge lines follow. */
void Game_moveBlock(Game *game, void *blockPtr, int x, int y)
{
	MovingBlock *b = blockPtr;
	int tx = x >> 6;
	int ty = y >> 6;
	int oldX = b->tileX;
	int oldY = b->tileY;

	if (oldX != tx || oldY != ty) {
		int tile = ty * 32 + tx;

		Game_tileHasWorldEntity(game, oldX, oldY); /* result unused by the original */
		Game_unlinkPlayerFromTile(game, b->tileX, b->tileY);
		game->render->mapFlags[oldY * 32 + oldX] &= (uint8_t)~TILEFLAG_SOLID;

		Entity *ent = game->tileLists[tile];
		while (ent != NULL) {
			Entity *next = ent->nextOnTile;
			if (ent != &game->entities[0] && ent != &game->entities[1]) {
				char *buf = Hud_getMessageBuffer(game->app->hud, 0);
				App_snprintf(buf, 0x40, (char *)ModData_str(&g_mod, MOD_STR_CRUSHED), ent->name);
				Hud_finishMessageBuffer(game->app->hud);
				ent->info |= ENT_INFO_CRUSHED;
				Entity_died(ent, 1);
			}
			ent = next;
		}

		Game_tileHasWorldEntity(game, tx, ty); /* result unused by the original */
		Game_linkWorldEntity(game, tx, ty);
		game->render->mapFlags[tile] |= TILEFLAG_SOLID;

		int cx = tx * 64 + 32;
		int cy = ty * 64 + 32;
		Render_relinkLine(game->render, b->lineIdx[2], cx, cy);
		Render_relinkLine(game->render, b->lineIdx[3], cx, cy);
		Render_relinkLine(game->render, b->lineIdx[1], cx, cy);
		Render_relinkLine(game->render, b->lineIdx[0], cx, cy);
		Player_crushed(game->player, b->dstX >> 6, b->dstY >> 6, b->dstX - b->x, b->dstY - b->y);
	}

	for (int side = 0; side < 4; side++) {
		Line *line = b->lines[side];
		if (line == NULL)
			continue; /* port guard: the original wrote through it regardless */
		line->x1 = b->ofs1[side * 2] + x;
		line->y1 = b->ofs1[side * 2 + 1] + y;
		line->x2 = b->ofs2[side * 2] + x;
		line->y2 = b->ofs2[side * 2 + 1] + y;
	}
	b->tileX = tx;
	b->tileY = ty;
}

/* 0x00027390
 * Trap effect aimed at the player: only when the trap at world (x, y) is in a
 * straight line in front of the player (same row or column, in the facing
 * direction). `arg` is the particle colour. */
void Game_trapAttack(Game *game, int kind, int x, int y, int arg)
{
	Canvas *cv = game->canvas;
	int dx = x - cv->viewX;
	int dy = y - cv->viewY;

	if (dx != 0 && dy != 0)
		return;
	if (dx == 0 && dy == 0)
		return;
	if (cv->stepX < 0 && dx >= 0)
		return;
	if (cv->stepX > 0 && dx <= 0)
		return;
	if (cv->stepY < 0 && dy >= 0)
		return;
	if (cv->stepY > 0 && dy <= 0)
		return;

	int dist = Combat_worldDistToTileDist(dx * dx + dy * dy);
	Combat *combat = game->app->combat;
	if (kind == 0) {
		combat->f7c = 0;
		Particles_spawnBlood(combat, dist, arg, 0x1e, 0);
	} else if (kind == 1) {
		Particles_spawnPreset3(combat, dist, arg);
	} else if (kind == 2) {
		Particles_spawnImage1(combat, arg);
	} else if (kind == 3) {
		Particles_spawnToTarget(combat, arg);
	}
}

/* 0x0002b404
 * Enable/disable the fire portal entity (eType 15) with portal id `id`. */
void Game_setFirePortalEnabled(Game *game, int id, int on)
{
	for (int i = 0; i < game->numEntities; i++) {
		Entity *ent = &game->entities[i];
		if (ent->def != NULL && ent->def->eType == 0x0f && (ent->param & 0xff) == id) {
			if (on)
				ent->param |= 0x100;
			else
				ent->param &= ~0x100;
			return;
		}
	}
}

/* Bounded strcat (the original used an unbounded one into 0x200 bytes). */
static void appendText(char *dst, size_t size, const char *src)
{
	size_t len = strlen(dst);
	if (len + 1 >= size)
		return;
	strncat(dst, src, size - len - 1);
}

/* 0x0000d274
 * Open a chest (eType 10) or reliquary (7): give every op-0x2f record of the
 * entity tile's event and show the contents list (dialog style 4). Records:
 * arg1 b0 = type (0 item, 2 ring, 3 ammo, 4 gold, 8 gem/quest item,
 * 0xfe note, 0xff map text), b1 = index, b2 = amount (gold: bits 8..23). */
void Game_openContainer(void *entity)
{
	Entity *ent = entity;
	Applet *app = ent->app;
	int tile = ent->linkIndex;
	char text[0x200];
	char line[0x40];
	int gotGem = 0;

	text[0] = '\0';
	line[0] = '\0'; /* the original's stack line could be stale: re-appended below */

	if (ent->def->eType == 10)
		strcpy(text, ModData_str(&g_mod, MOD_STR_CHEST_CONTENTS));
	else if (ent->def->eType == 7)
		strcpy(text, ModData_str(&g_mod, MOD_STR_RELIQUARY_CONTENTS));

	if (!(app->render->mapFlags[tile] & TILEFLAG_HAS_EVENT))
		return;

	int evIdx = Render_findTileEvent(app->render, tile);
	uint32_t ev = app->render->tileEvents[evIdx];
	int first = (int)EV_FIRST(ev);
	int count = (int)EV_COUNT(ev);

	for (int i = 0; i < count; i++) {
		Command *cmd = &app->render->commands[first + i];
		if (cmd->op != OP_CONTAINER_ITEM)
			continue;

		int32_t arg = cmd->arg1;
		int type = arg & 0xff;

		if (type == CONTAINER_TEXT) {
			appendText(text, sizeof text, ModData_str(&g_mod, MOD_STR_CONTAINER_BULLET));
			appendText(text, sizeof text, app->render->mapStrings[arg >> 8]);
			appendText(text, sizeof text, ModData_str(&g_mod, MOD_STR_CONTAINER_EOL));
			continue;
		}
		if (type == CONTAINER_NOTE) {
			Player_addNotebookIndex(app->player, (int16_t)(arg >> 8));
			continue;
		}

		int idx = (arg >> 8) & 0xff;
		int amount = (int)(((uint32_t)arg >> 16) & 0xff);
		int giveType = -1;
		int giveAmount = amount;

		switch (type) {
		case 0:
			giveType = 0;
			break;
		case 2:
			giveType = 2;
			giveAmount = 1;
			break;
		case 3:
			giveType = 3;
			break;
		case 4: {
			int gold = (int)(((uint32_t)arg >> 8) & 0xffff);
			Player_give(app->player, 4, 0, gold, 0);
			App_snprintf(line, 0x40, (char *)ModData_str(&g_mod, MOD_STR_CONTAINER_GOLD), 0x88, gold);
			break;
		}
		case 8:
			Player_give(app->player, 8, idx, amount, 0);
			Game_showItemAcquired(app->game, 8, idx);
			gotGem = 1;
			break;
		default:
			/* other types only repeat the previous line */
			break;
		}

		if (giveType >= 0) {
			Player_give(app->player, giveType, idx, giveAmount, 0);
			EntityDef *def = EntityDefs_find(app->entityDefs, 6, type, idx);
			App_snprintf(line, 0x40, (char *)ModData_str(&g_mod, MOD_STR_CONTAINER_ITEM), 0x88, amount,
			             def ? def->longName : "");
		}
		appendText(text, sizeof text, line);
		appendText(text, sizeof text, ModData_str(&g_mod, MOD_STR_CONTAINER_EOL));
	}

	/* drop the final '|' */
	size_t len = strlen(text);
	if (len > 0)
		text[len - 1] = '\0';
	if (!gotGem)
		Canvas_showDialog(app->canvas, NULL, text, 4, 1, 0);
}

/* 0x000131fc
 * Debug "Give notes": add every note (op 0x28) of the map to the quest log. */
void Game_giveAllNotes(Game *game)
{
	game->player->notebookCount = 0;
	for (int i = 0; i < game->render->numCommands; i++) {
		if (game->render->commands[i].op == OP_NOTE)
			Player_addNotebookIndex(game->player, (int16_t)game->render->commands[i].arg1);
	}
}

/* 0x0002a074
 * Step into an enabled fire portal (eType 15): pick one of the tile event's
 * op-0x3a "firePitDest" records at random, mark the portal used, queue the map
 * change and start the portal fade (state 0x15). */
void Game_useFirePortal(void *entPtr)
{
	Entity *ent = entPtr;
	Applet *app = ent->app;

	if (!(ent->param & 0x100))
		return;

	Render *render = app->render;
	Sprite *spr = Render_spriteAt(render, (ent->info & 0xffff));
	int tile = (spr->y >> 6) * 32 + (spr->x >> 6);

	if (!(render->mapFlags[tile] & TILEFLAG_HAS_EVENT)) {
		App_setErrorStr(app, (char *)ModData_str(&g_mod, MOD_STR_FIREPIT_NO_DEST), spr->x >> 6, spr->y >> 6);
		App_setErrorCode(g_app, -666);
		return;
	}

	uint32_t ev = app->render->tileEvents[Render_findTileEvent(render, tile)];
	int first = (int)EV_FIRST(ev);
	int count = (int)EV_COUNT(ev);
	int numDests = 0;
	int pick = 0;

	/* pass 0 counts the destinations, pass 1 takes the chosen one */
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1) {
			if (numDests == 0) {
				App_setErrorStr(app, (char *)ModData_str(&g_mod, MOD_STR_FIREPORTAL_NO_DEST),
				                spr->x >> 6, spr->y >> 6);
				App_setErrorCode(g_app, -666);
				return;
			}
			pick = Random_nextByte(&app->random) % numDests;
		}
		numDests = 0;
		for (int i = 0; i < count; i++) {
			Command *cmd = &app->render->commands[first + i];
			if (cmd->op != OP_FIREPIT_DEST)
				continue;
			numDests++;
			if (pass == 0 || numDests - 1 != pick)
				continue;

			Canvas *cv = app->canvas;
			app->player->firePortals |= arm_bit(cv->mapId - 6000);
			if (cv->mapId == 6000)
				app->player->visitedMaps |= 1;

			uint32_t arg = (uint32_t)cmd->arg1;
			uint32_t map = arg & 0xff;
			uint32_t sx = (arg >> 8) & 0xff;
			uint32_t sy = (arg >> 16) & 0xff;
			uint32_t dir = arg >> 24;
			Sound_play(app->sound, 0x1414, 0, 5, -1);
			/* map | spawn (x | y << 5 | dir << 10) << 8 */
			app->game->pendingMapChange = dir << 18 | sy << 13 | (sx << 8 | map);
			Canvas_startShake(cv, 750, 2, 0);
			cv->fadeStart = (int32_t)GETUPTIMEMS();
			cv->fadeDir = 0;
			Canvas_setState(cv, 0x15);
			return;
		}
	}
}
