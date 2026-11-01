/*
 * Game: monster bookkeeping — the inactive/active monster rings, turn
 * sequencing between the player and the monsters, monster sprite snapping,
 * the short-range path finder and a few monster spawning/waking helpers.
 *
 * Monster rings: Game.inactiveMonsters and Game.activeMonsters are circular
 * doubly-linked lists threaded through EntityMonster.nextOnList/prevOnList.
 * Game.combatMonsters is a singly linked queue (EntityMonster.nextAttacker) of
 * monsters that decided to attack this turn.
 */

#include "game.h"

/* Squared world distances of 1..8 tiles ((64*n)^2), int32 each. */
#define MOD_TILE_DIST_SQ 0x45ebcu

/* Entity.info bits used here */
#define INFO_IN_COMBAT    0x00010000u /* queued on Game.combatMonsters */
#define INFO_DEAD         0x00020000u
#define INFO_RAISE        0x00040000u
#define INFO_ACTIVE       0x00080000u /* on the active ring */
#define INFO_SNAP_BLOCK   0x00400000u
#define INFO_PICKED       0x02000000u
#define INFO_CORPSE       0x20000000u
#define INFO_AWAKE        0x40000000u

/* Entity types (EntityDef.eType) */
#define ETYPE_PLAYER  1
#define ETYPE_MONSTER 2
#define ETYPE_CORPSE  9

/* Mask of entity types that block a tile for monsters. */
#define BLOCKING_TYPES 0x7daf

static int tileDistSq(int tiles)
{
	return ModData_s32(&g_mod, MOD_TILE_DIST_SQ + (uint32_t)(tiles - 1) * 4u);
}

static Sprite *entitySprite(Entity *ent)
{
	return &g_app->render->sprites[(ent->info & 0xffff) - 1];
}

/* true when the monster's sprite has not yet reached its destination tile */
static int spriteBehindDest(Entity *ent)
{
	Sprite *spr = entitySprite(ent);
	return ent->monster->destX != spr->x || ent->monster->destY != spr->y;
}

/* 0x00004168 */
void Game_activate_4168(Game *game, Entity *ent)
{
	Game_activateEx(game, ent, 1, 0);
}

/* 0x00004174  Wake a monster: move it from the inactive ring to the active one. */
void Game_activateEx(Game *game, Entity *ent, int alert, int distCheck)
{
	Applet *app = g_app;
	EntityMonster *mon = ent->monster;
	int pos[2];

	if (distCheck != 0 && Entity_distSqFrom(ent, app->canvas->viewX, app->canvas->viewY) > tileDistSq(4))
		return;

	ent->info |= INFO_AWAKE;
	if (app->player->noclip != 0)
		return;

	if (ent->def->eSubType == 2) {
		if ((ent->info & 0x10000000) == 0)
			return;
		String_assign(&ent->name, &ent->def->name);
	}

	Entity_calcPosition(ent, pos);
	if (mon == NULL || ent->def == NULL)
		return;
	if (ent->def->eType != ETYPE_MONSTER || (ent->info & INFO_ACTIVE) != 0)
		return;

	/* unlink from the inactive ring */
	if (mon->nextOnList != NULL) {
		if (game->inactiveMonsters == ent && mon->nextOnList == ent) {
			game->inactiveMonsters = NULL;
		} else {
			if (game->inactiveMonsters == ent)
				game->inactiveMonsters = mon->nextOnList;
			mon->nextOnList->monster->prevOnList = mon->prevOnList;
			mon->prevOnList->monster->nextOnList = mon->nextOnList;
		}
	}

	/* append to the active ring */
	if (game->activeMonsters == NULL) {
		mon->nextOnList = ent;
		mon->prevOnList = ent;
		game->activeMonsters = ent;
	} else {
		Entity *head = game->activeMonsters;
		mon->prevOnList = head->monster->prevOnList;
		mon->nextOnList = head;
		head->monster->prevOnList->monster->nextOnList = ent;
		head->monster->prevOnList = ent;
	}
	ent->info |= INFO_ACTIVE;

	if (app->combat->target != ent)
		ent->monster->flags &= 0x8fff;

	mon->ce.x = mon->destX;
	mon->ce.y = mon->destY;

	if (alert != 0) {
		int sub = ent->def->eSubType;
		if (sub >= 0xb && sub <= 0xf && (ent->monster->flags & 0x100) == 0) {
			/* bosses wake through the reserved event tiles (0,1) / (1,1) */
			Game_executeTile0(game, sub == 0xc ? 1 : 0, 1, 0xf000);
		} else if ((mon->flags & 0x10) != 0) {
			Game_executeTile0(game, mon->destX >> 6, mon->destY >> 6, 0xf000);
		}
	}

	if (EntityMonster_getSound(ent->monster, 1) != -1) {
		if (game->alertEntity == NULL || (Random_nextInt(&app->random) & 1) != 0)
			game->alertEntity = ent;
	}
}

/* 0x00005a50  End of the player's move: tick the player and start the monsters' turn. */
void Game_advanceTurn(Game *game)
{
	Applet *app = g_app;
	Player *player = app->player;
	int monstersAct;

	if (app->game->monstersMoving != 0) {
		/* The original walks the active ring here without doing anything. */
		return;
	}

	/* status effect 3 (slow?) lets the monsters act only every other turn */
	monstersAct = 0;
	if (player->statusEffects[3].turns < 1) {
		monstersAct = 1;
	} else {
		player->statusEffects[3].amount++;
		if ((player->statusEffects[3].amount & 1) == 0)
			monstersAct = 1;
	}

	Player_advanceTurn(app->player);

	game->monstersTurn = monstersAct ? 1 : 2;
	game->f9c08 = 0;
	game->turnTime = app->time;

	if (monstersAct) {
		for (int i = 0; i < game->numEntities; i++)
			Monster_updateEffects(&game->entities[i]);
		uint32_t facing = (uint32_t)Canvas_flagForFacingDir(app->canvas, 0xf000000);
		/* per-turn event on the reserved tile (2,0) */
		Game_executeTile0(game, 2, 0, (int)(game->eventFlags | facing | 0x1000f000u));
		Canvas_updateFacing(app->canvas);
	}

	app->statTurnsByRing[app->player->equippedRing]++;
	app->statMoves++;

	if (game->difficulty == 4)
		Game_pickNearbyMonster(Player_getPlayerEnt(app->player), tileDistSq(6), 0, 0x100);
}

/* 0x000095f0  Monster sprites may only be snapped when none is mid-action. */
int Game_canSnapMonsters(Game *game)
{
	if (game->disableAI == 0 && game->activeMonsters != NULL) {
		Entity *ent = game->activeMonsters;
		do {
			if ((ent->info & INFO_SNAP_BLOCK) != 0)
				return 0;
			ent = ent->monster->nextOnList;
		} while (ent != game->activeMonsters);
	}
	return 1;
}

/* 0x0000a924  Move an active monster back onto the inactive ring. */
void Game_deactivate(Game *game, Entity *entity)
{
	EntityMonster *mon = entity->monster;

	if ((entity->info & INFO_ACTIVE) == 0)
		return;

	if (mon->nextOnList != NULL) {
		if (game->activeMonsters == entity && mon->nextOnList == entity) {
			game->activeMonsters = NULL;
		} else {
			if (game->activeMonsters == entity)
				game->activeMonsters = mon->nextOnList;
			mon->nextOnList->monster->prevOnList = mon->prevOnList;
			mon->prevOnList->monster->nextOnList = mon->nextOnList;
		}
	}

	if (game->inactiveMonsters == NULL) {
		mon->nextOnList = entity;
		mon->prevOnList = entity;
		game->inactiveMonsters = entity;
	} else {
		Entity *head = game->inactiveMonsters;
		mon->prevOnList = head->monster->prevOnList;
		mon->nextOnList = head;
		head->monster->prevOnList->monster->nextOnList = entity;
		head->monster->prevOnList = entity;
	}
	entity->info &= ~(INFO_ACTIVE | INFO_SNAP_BLOCK);
}

/* 0x00014044  Familiar mode: if the familiar ended its move on the player's own
 * body, bounce it back to where it started. Always ends the monsters' turn. */
void Game_checkBobAfterWarp(Game *game, int a, int b, int c)
{
	Applet *app = g_app;

	if (app->player->isFamiliar != 0 &&
	    Game_findEntityAt(app->game, app->canvas->viewX, app->canvas->viewY, 4) != NULL) {
		Sound_play(app->sndSys, 0x65, 0, 4, -1);
		Canvas_resetPlayerPos(app->canvas, app->canvas->savedViewX, app->canvas->savedViewY);
	}
	game->monstersTurn = 0;
}

/* 0x00014ebc  Breadth-first search inside a 7x7 window (+-3 tiles) around (x,y)
 * towards (tx,ty). Cells are numbered (dy+4)*8 + (dx+4). If the target is not
 * reachable the visited cell closest to it is used. outDir receives the first
 * step {dx, dy}. Returns 0 when no step exists. */
int Game_findPathStep(Entity *entity, int x, int y, int tx, int ty, int mask, int *outDir)
{
	Game *game = g_app->game;
	int *visited = game->pathVisited;
	uint8_t *parent = game->pathParent;
	uint8_t *queue = game->pathQueue;
	int goalDx, goalDy, goalCell, best, bestDist, queued;

	if (x == tx && y == ty) {
		outDir[0] = 0;
		outDir[1] = 0;
		return 1;
	}

	memset(visited, 0, sizeof(game->pathVisited));
	memset(parent, 0xff, sizeof(game->pathParent));

	goalDx = tx - x;
	goalDy = ty - y;
	if (goalDx < -3) goalDx = -3; else if (goalDx > 3) goalDx = 3;
	if (goalDy < -3) goalDy = -3; else if (goalDy > 3) goalDy = 3;
	goalCell = goalDx + goalDy * 8 + 0x24;

	best = 0xff;
	bestDist = 0x7fffffff;
	queued = 1;
	queue[0] = 0x24; /* the start cell (0,0) */

/* push a neighbour of the head cell (the original has no bound check; the
 * window keeps the queue short, the guard only protects the next fields) */
#define PUSH(cell) do { \
		uint8_t from_ = queue[0], to_ = (uint8_t)(cell); \
		if (queued < (int)sizeof(game->pathQueue)) queue[queued++] = to_; \
		parent[to_] = from_; \
	} while (0)

	do {
		int cell = queue[0];
		if (cell == goalCell) {
			best = cell;
			break;
		}
		if (visited[cell] == 0) {
			int dx = (cell & 7) - 4;
			int dy = (cell >> 3) - 4;
			int d = (goalDy - dy) * (goalDy - dy) + (goalDx - dx) * (goalDx - dx);
			int cx = x + dx, cy = y + dy;

			visited[cell] = 1;
			if (d < bestDist && cell != 0x24) {
				bestDist = d;
				best = cell;
			}
			if (dx != -3 && visited[cell - 1] == 0 &&
			    Entity_checkLineOfSight(entity, cx, cy, cx - 1, cy, mask) != 0)
				PUSH(cell - 1);
			if (dx != 3 && visited[cell + 1] == 0 &&
			    Entity_checkLineOfSight(entity, cx, cy, cx + 1, cy, mask) != 0)
				PUSH(cell + 1);
			if (dy != 3 && visited[cell + 8] == 0 &&
			    Entity_checkLineOfSight(entity, cx, cy, cx, cy + 1, mask) != 0)
				PUSH(cell + 8);
			if (dy != -3 && visited[cell - 8] == 0 &&
			    Entity_checkLineOfSight(entity, cx, cy, cx, cy - 1, mask) != 0)
				PUSH(cell - 8);
		}
		queued--;
		memmove(queue, queue + 1, (size_t)queued);
	} while (queued > 0);
#undef PUSH

	if (best == 0xff)
		return 0;

	/* walk back to the cell whose parent is the start */
	int step = best, cur = best;
	while (parent[cur] != 0xff) {
		step = cur;
		cur = parent[cur];
	}
	outDir[0] = (step & 7) - 4;
	outDir[1] = (step >> 3) - 4;
	return 1;
}

/* 0x000153f8  Direction of the last move as event trigger bits:
 * moveMask = entering-from flags, eventFlags = direction flags. */
void Game_setMoveDirMask(Game *game, int x0, int y0, int x1, int y1)
{
	uint32_t moveMask = 0, dirFlags = 0;

	if (x0 == -1 && y0 == -1) {
		moveMask = 0xf0; dirFlags = 0xf;
	} else if (x0 < x1) {
		moveMask = 0x20; dirFlags = 8;   /* east */
	} else if (x1 < x0) {
		moveMask = 0x80; dirFlags = 2;   /* west */
	} else if (y1 < y0) {
		moveMask = 0x10; dirFlags = 4;   /* north */
	} else if (y0 < y1) {
		moveMask = 0x40; dirFlags = 1;   /* south */
	}
	game->moveMask = moveMask;
	game->eventFlags = dirFlags;
}

/* 0x00019dc8  Pick up to 4 dead (corpse) inactive monsters near `entity`.
 * Normally one of them is chosen at random, flagged and its index returned;
 * on difficulty 4 around the player all candidates are raised at once. */
int Game_pickNearbyMonster(Entity *entity, int maxDist, int needFlags, int excludeFlags)
{
	Applet *app = g_app;
	Game *game = app->game;
	Entity *cand[4];
	int count = 0;
	int cx = app->canvas->viewX, cy = app->canvas->viewY;
	Entity *ent;

	if (entity->monster != NULL) {
		cx = entity->monster->destX;
		cy = entity->monster->destY;
	}

	ent = game->inactiveMonsters;
	if (ent == NULL)
		return -1;

	do {
		Entity *next = ent->monster->nextOnList;
		uint32_t info = ent->info;
		int16_t mflags = (int16_t)ent->monster->flags;

		if ((info & INFO_DEAD) == 0 && (info & INFO_PICKED) == 0 && (info & INFO_CORPSE) != 0 &&
		    Entity_isBoss(ent) == 0 &&
		    (needFlags == 0 || (mflags & needFlags) != 0) &&
		    (mflags & excludeFlags) == 0 &&
		    Entity_distSqFrom(ent, cx, cy) <= maxDist &&
		    Game_findEntityAt(app->game, entitySprite(ent)->x, entitySprite(ent)->y, BLOCKING_TYPES) == NULL &&
		    (app->game->difficulty != 4 || (ent->monster->monsterEffects & 0x10) == 0)) {
			cand[count++] = ent;
		}
		ent = next;
	} while (app->game->inactiveMonsters != ent && count != 4);

	if (app->game->difficulty == 4 && entity->def->eType == ETYPE_PLAYER) {
		for (int i = 0; i < count; i++) {
			cand[i]->info |= INFO_PICKED;
			Combat_raiseDead(entity, Game_getEntityIndex(cand[i]));
		}
	} else if (count != 0) {
		Entity *pick = cand[Random_nextInt(&app->random) % count];
		pick->info |= INFO_PICKED;
		return Game_getEntityIndex(pick);
	}
	return -1;
}

/* 0x00024aa0  Kill every ordinary monster flagged INFO_RAISE that is still alive
 * (flag 0x80 in monster flags marks it dead). */
void Game_activateMonsters(void)
{
	Game *game = g_app->game;

	for (int i = 0; i < game->numEntities; i++) {
		Entity *ent = &game->entities[i];
		EntityDef *def = ent->def;
		if (def != NULL && def->eType == ETYPE_MONSTER && def->eSubType < 0xb &&
		    (ent->info & INFO_RAISE) != 0 && (ent->monster->flags & 0x80) == 0)
			Entity_died(ent, 0);
	}
}

/* 0x0002bb7c  Let every active monster think once per turn, then keep
 * re-thinking monsters whose plans were invalidated until nothing changes. */
void Game_monsterAI(Game *game)
{
	int moved;
	Entity *ent, *next;

	if (game->disableAI != 0 || game->f9c08 != 0)
		return;

	game->f9c08 = 1;
	game->combatMonsters = NULL;
	if (game->activeMonsters == NULL)
		return;

	moved = 0;
	ent = game->activeMonsters;
	do {
		next = ent->monster->nextOnList;
		if ((ent->monster->monsterEffects & 2) == 0 &&
		    (game->monstersTurn == 1 ||
		     (game->monstersTurn == 2 && Entity_isMonsterType13or14(ent) != 0))) {
			Entity_aiThink(ent);
			if (spriteBehindDest(ent))
				moved = 1;
		}
		ent = next;
	} while (game->activeMonsters != next && game->activeMonsters != NULL);

	while (moved) {
		moved = 0;

		/* attackers whose attack is no longer valid re-think */
		ent = game->combatMonsters;
		while (ent != NULL) {
			next = ent->monster->nextAttacker;
			if (Entity_aiIsAttackValid(ent) == 0) {
				Game_removeFromCombat(ent);
				Entity_aiThink(ent);
				if (spriteBehindDest(ent))
					moved = 1;
			}
			ent = next;
		}

		/* walkers (subtypes 1 and 10) whose path got blocked re-think */
		ent = game->activeMonsters;
		if (ent == NULL)
			break; /* the original would dereference NULL here */
		do {
			EntityMonster *mon = ent->monster;
			next = mon->nextOnList;
			if ((ent->info & 0x10000) == 0 && (mon->monsterEffects & 2) == 0 &&
			    (game->monstersTurn == 1 ||
			     (game->monstersTurn == 2 && Entity_isMonsterType13or14(ent) != 0)) &&
			    (ent->def->eSubType == 1 || ent->def->eSubType == 10) &&
			    mon->goal.type == 1 && (mon->goal.flags & 2) != 0) {
				Game_trace(game, mon->destX, mon->destY, mon->goal.x * 64 + 32, mon->goal.y * 64 + 32,
				           ent, BLOCKING_TYPES);
				Entity *hit = game->traceEntity;
				if (hit != NULL && hit->def->eType != ETYPE_PLAYER) {
					MonsterGoal_reset(&ent->monster->goal);
					Entity_aiThink(ent);
					if (spriteBehindDest(ent))
						moved = 1;
				}
			}
			ent = next;
		} while (game->activeMonsters != next && game->activeMonsters != NULL);
	}
}

/* 0x0002be28  Advance every active monster's movement lerp one step. */
void Game_updateMonsterLerps(Game *game)
{
	int stillMoving = 0;

	if (game->monstersMoving == 0)
		return;

	Entity *ent = game->activeMonsters;
	if (ent != NULL) {
		do {
			if (Entity_aiStepLerp(ent) != 0)
				stillMoving = 1;
			ent->monster->ce.x = ent->monster->destX;
			ent->monster->ce.y = ent->monster->destY;
			ent = ent->monster->nextOnList;
		} while (game->activeMonsters != ent);
	}
	game->monstersMoving = stillMoving;
}

/* 0x0002c900  out[1] = monsters on the map (not flagged 0x80),
 * out[0] = how many of them are dead/corpses (info & 0x21020000). */
void Game_countMonsters(void *unused, int *out)
{
	Game *game = g_app->game;
	int total = 0, dead = 0;

	for (int i = 0; i < game->numEntities; i++) {
		EntityMonster *mon = game->entities[i].monster;
		if (mon != NULL && (mon->flags & 0x80) == 0) {
			total++;
			if ((game->entities[i].info & 0x21020000u) != 0)
				dead++;
		}
	}
	out[1] = total;
	out[0] = dead;
}

/* 0x000302cc  On entering a map: put monsters on their sprite (home) positions.
 * When revisiting a completed level with fewer than half its monsters alive,
 * up to 8 dead monsters are randomly resurrected where they lay. */
void Game_resetMonsterPositions(Game *game)
{
	Applet *app = g_app;
	unsigned shift = (unsigned)(app->canvas->mapId - 1) & 0xff;
	uint32_t levelBit = shift < 32 ? (1u << shift) : 0;

	if (game->loadedFromSave == 0 && (app->player->completedLevels & levelBit) != 0) {
		int counts[2]; /* [0] dead, [1] total */
		int alive, half, revived = 0;

		Game_countMonsters(app->player, counts);
		alive = counts[1] - counts[0];
		half = counts[1] / 2;

		for (int i = 0; i < game->numEntities; i++) {
			Entity *ent = &game->entities[i];
			EntityMonster *mon = ent->monster;
			int stayDead;

			if (mon == NULL)
				continue;

			stayDead = (ent->info & 0x20020000u) == 0 || (mon->flags & 0x82) != 0 ||
			           half <= alive || revived > 7 ||
			           (Random_nextInt(&app->random) & 0xff) > 100 ||
			           ent->def->eSubType == 7 || ent->def->eSubType == 2 ||
			           Game_findEntityAt(game, mon->destX, mon->destY, BLOCKING_TYPES) != NULL;
			if (stayDead) {
				Sprite *spr = entitySprite(ent);
				mon->ce.x = mon->destX = spr->x;
				mon->ce.y = mon->destY = spr->y;
			} else {
				Entity_resurrect(ent, mon->destX, mon->destY, 0x20);
				alive++;
				ent->info &= ~INFO_AWAKE;
				revived++;
			}
		}
	} else {
		for (int i = 0; i < game->numEntities; i++) {
			EntityMonster *mon = game->entities[i].monster;
			if (mon != NULL) {
				Sprite *spr = entitySprite(&game->entities[i]);
				mon->ce.x = mon->destX = spr->x;
				mon->ce.y = mon->destY = spr->y;
			}
		}
	}
}

/* 0x0003ae58  Debug "respawn": drop 8 items at the player start, resurrect all
 * corpses, wake every monster and re-arm one-shot event commands. */
void Game_setRespawn(Game *game)
{
	Applet *app = g_app;
	Render *render;

	for (int i = 0; i < 8; i++) {
		uint32_t tile = (uint32_t)app->render->playerTile;
		Game_spawnDropItem(game, tile & 0x1f, (tile & 0x3ff) >> 5, 99, 6, 0, 1, 0);
	}

	for (int i = 0; i < game->numEntities; i++) {
		Entity *ent = &game->entities[i];
		if (ent->def == NULL)
			continue;
		if (ent->def->eType == ETYPE_CORPSE) {
			int link = ent->linkIndex;
			Entity_resurrect(ent, link % 32, link / 32, 0x20);
		} else if (ent->def->eType == ETYPE_MONSTER && (ent->info & INFO_ACTIVE) == 0) {
			Game_activateEx(game, ent, 0, 0);
		}
	}

	render = app->render;
	for (int i = 0; i < render->numByteCodes; i++) {
		if ((render->byteCodes[i * 3 + 2] & 0x200) != 0)
			render->byteCodes[i * 3 + 2] = 0;
	}
}

/* 0x0003c91c  Finish monster movement. During the monsters' turn this also runs
 * the AI and, once nobody is moving any more, starts the first queued attack. */
int Game_snapAllMonsterSprites(Game *game, int force)
{
	Applet *app = g_app;

	if (game->monstersTurn == 0) {
		int snapped = 0;
		if (force == 0)
			force = Game_canSnapMonsters(game);
		Entity *ent = game->activeMonsters;
		if (force != 0 && ent != NULL) {
			do {
				Entity *next = ent->monster->nextOnList;
				if ((ent->monster->flags & 0x40) != 0) {
					Game_snapMonsterSprite(ent);
					snapped = 1;
				}
				ent = next;
			} while (game->activeMonsters != ent);
			if (snapped)
				Canvas_updateFacing(app->canvas);
		}
		return force;
	}

	Game_monsterAI(game);
	if (force == 0)
		force = Game_canSnapMonsters(game);
	if (force != 0 && game->activeMonsters != NULL) {
		Entity *ent = game->activeMonsters;
		do {
			Game_snapMonsterSprite(ent);
			ent = ent->monster->nextOnList;
		} while (game->activeMonsters != ent);
		Canvas_updateFacing(app->canvas);
	}
	Game_updateMonsterLerps(game);

	Entity *attacker = game->combatMonsters;
	if (attacker != NULL && game->monstersMoving == 0) {
		Combat_start(app->combat, attacker, attacker->monster->target, 0, 0);
		return 0;
	}
	return force;
}

/* 0x0003ca54  Complete a monster's pending move instantly. Returns 1 if it moved. */
int Game_snapMonsterSprite(Entity *ent)
{
	Applet *app = g_app;
	Sprite *spr = entitySprite(ent);

	if (spr->x == ent->monster->destX && spr->y == ent->monster->destY)
		return 0;

	while (Entity_aiStepLerp(ent) != 0)
		;
	ent->monster->ce.x = ent->monster->destX;
	spr->x = ent->monster->destX;
	ent->monster->ce.y = ent->monster->destY;
	spr->y = ent->monster->destY;
	Render_relinkSprite(app->render, spr);
	return 1;
}

/* 0x00041fa8  Is a queued attacker standing strictly between the player and
 * tile (tx,ty) on the same row or column? (weapon 0x13 attackers excluded) */
int Game_isMonsterBetween(Game *game, int tx, int ty)
{
	Applet *app = g_app;
	int px = app->canvas->destX >> 6;
	int py = app->canvas->destY >> 6;

	if (tx != px && ty != py)
		return 0;

	for (Entity *ent = game->combatMonsters; ent != NULL; ent = ent->monster->nextAttacker) {
		EntityMonster *mon = ent->monster;
		int mx, my, lo, hi;

		if (mon->ce.weapon == 0x13)
			continue;
		mx = mon->destX >> 6;
		my = mon->destY >> 6;
		if (mx != tx && my != ty)
			continue;
		if (mx != px) {
			lo = mx < px ? mx : px;
			hi = mx < px ? px : mx;
			if (lo < tx && tx < hi)
				return 1;
		}
		if (my != py) {
			lo = my < py ? my : py;
			hi = my < py ? py : my;
			if (lo < ty && ty < hi)
				return 1;
		}
	}
	return 0;
}

/* 0x00043600  Remove a monster from the attack queue and cancel its goal. */
void Game_removeFromCombat(Entity *ent)
{
	Applet *app = g_app;
	uint32_t info = ent->info;
	int weapon;
	Entity *prev, *cur;

	if ((info & INFO_IN_COMBAT) == 0)
		return;
	ent->info = info & ~INFO_IN_COMBAT;

	weapon = ent->monster->ce.weapon;
	if (weapon == 0x13) {
		/* release the entity this attack had claimed */
		app->game->entities[ent->monster->goal.param].info &= ~INFO_PICKED;
	} else if (weapon == 0xe || weapon == 0x17 || weapon == 0x22) {
		/* back to the idle frame */
		app->render->sprites[(info & 0xffff) - 1].info &= 0xfffff0ffu;
	}

	MonsterGoal_reset(&ent->monster->goal);

	prev = NULL;
	cur = app->game->combatMonsters;
	while (cur != NULL && cur != ent) {
		prev = cur;
		cur = cur->monster->nextAttacker;
	}
	if (prev == NULL)
		app->game->combatMonsters = ent->monster->nextAttacker;
	else
		prev->monster->nextAttacker = ent->monster->nextAttacker;
}

/* 0x00044ba0 */
void Game_endPlayerTurn(Game *game)
{
	Applet *app = g_app;

	Game_monsterAI(game);
	Game_updateMonsterLerps(game);
	if (game->monstersMoving != 0)
		return;

	Canvas_updateFacing(app->canvas);
	Entity *attacker = game->combatMonsters;
	if (attacker == NULL) {
		Game_checkBobAfterWarp(game, 0, 0, 0);
		Canvas_drawPlayingSoftKeys(app->canvas);
	} else {
		Combat_start(app->combat, attacker, attacker->monster->target, 0, 0);
	}
}

/* 0x00045860  A noise at (x,y): on maps after the 4th, the first inactive
 * ordinary monster within 4 tiles that is set to react (flags 0x7000) wakes up
 * with probability 85/256. */
void Game_wakeNearbyMonsters(Game *game, int x, int y)
{
	Applet *app = g_app;
	Entity *ent, *next;

	if (app->canvas->mapId <= 4)
		return;
	ent = app->game->inactiveMonsters;
	if (ent == NULL)
		return;

	do {
		next = ent->monster->nextOnList;
		if (ent->def->eType == ETYPE_MONSTER && (ent->monster->flags & 0x7000) != 0 &&
		    ent->def->eSubType < 0xb && Entity_distSqFrom(ent, x, y) < 0x10000 &&
		    (Random_nextInt(&app->random) & 0xff) < 0x55) {
			Game_activate_4168(app->game, ent);
			return;
		}
		ent = next;
	} while (app->game->inactiveMonsters != next && next != NULL);
}
