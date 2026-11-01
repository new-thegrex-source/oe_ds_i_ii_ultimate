/*
 * Game: monster bookkeeping — the inactive/active monster rings, turn
 * sequencing between the player and the monsters, monster sprite snapping,
 * the combat queue, the m08 boss reinforcements and a few monster helpers.
 *
 * Monster rings: Game.inactiveMonsters and Game.activeMonsters are circular
 * doubly-linked lists threaded through EntityMonster.nextOnList/prevOnList
 * (Entity.info 0x80000 = on the active ring). Game.combatMonsters is a singly
 * linked queue (EntityMonster.nextAttacker) of monsters that decided to attack
 * this turn.
 */

#include "game.h"

/* Two reinforcement spawn tiles on the boss map: int[2][2] {tx, ty}. */
#define MOD_BOSS_SPAWN_TILES 0x30318u

/* Entity.info bits */
#define INFO_SPRITE_MASK 0x0000ffffu
#define INFO_IN_COMBAT   0x00010000u /* queued on Game.combatMonsters */
#define INFO_REMOVED     0x00020000u
#define INFO_ACTIVE      0x00080000u /* on the active ring */
#define INFO_LERPING     0x00400000u /* blocks snapping */
#define INFO_PICKED      0x02000000u /* reserved by Game_pickNearbyMonster */
#define INFO_AGGRO       0x20000000u
#define INFO_CORPSE      0x40000000u
#define INFO_DIRTY       0x80000000u
#define INFO_DEAD_MASK   (INFO_CORPSE | INFO_REMOVED)

/* EntityMonster.flags bits */
#define MON_ALERT_EVENT  0x0010 /* run the tile event once when woken */
#define MON_KNOCKED      0x0040 /* sprite still moving (knockback) */
#define MON_DORMANT      0x0080
#define MON_NO_RAISE     0x0100

/* Entity types (EntityDef.eType) */
#define ETYPE_PLAYER  1
#define ETYPE_MONSTER 2

/* Mask of entity types that occupy a tile for monsters. */
#define BLOCKING_TYPES 0xfdaf

#define MAP_BOSS 0x1777 /* m08 */

static Sprite *entitySprite(Render *render, Entity *ent)
{
	return Render_spriteAt(render, (ent->info & INFO_SPRITE_MASK));
}

/* true when the monster's sprite has not reached its destination */
static int spriteBehindDest(Render *render, Entity *ent)
{
	Sprite *spr = entitySprite(render, ent);

	return ent->monster->destX != spr->x || ent->monster->destY != spr->y;
}

/* Remove `ent` from the ring whose head is *head. */
static void ringRemove(Entity **head, Entity *ent)
{
	EntityMonster *mon = ent->monster;

	if (mon->nextOnList == NULL)
		return;
	if (*head == ent && mon->nextOnList == *head) {
		*head = NULL;
	} else {
		if (*head == ent)
			*head = mon->nextOnList;
		mon->nextOnList->monster->prevOnList = mon->prevOnList;
		mon->prevOnList->monster->nextOnList = mon->nextOnList;
	}
}

/* Append `ent` at the tail of the ring whose head is *head. */
static void ringAppend(Entity **head, Entity *ent)
{
	EntityMonster *mon = ent->monster;

	if (*head == NULL) {
		mon->nextOnList = ent;
		mon->prevOnList = ent;
		*head = ent;
	} else {
		mon->prevOnList = (*head)->monster->prevOnList;
		mon->nextOnList = *head;
		(*head)->monster->prevOnList->monster->nextOnList = ent;
		(*head)->monster->prevOnList = ent;
	}
}

/* 0x00002e50  Wake a monster: move it from the inactive ring to the active one.
 * distCheck: only within 4 tiles of the view. A mimic (subtype 2) only wakes
 * once it is aggroed. alert: run its one-shot alert tile event. */
void Game_activateEx(Game *game, Entity *ent, int alert, int distCheck)
{
	EntityMonster *mon = ent->monster;
	uint32_t info;

	if (distCheck != 0 && Entity_distSqFrom(ent, game->canvas->viewX, game->canvas->viewY) > 0x10000)
		return;

	info = ent->info;
	ent->info = info | INFO_DIRTY;
	if (game->player->noclip)
		return;
	if (ent->def->eSubType == 2) {
		if (!(info & INFO_AGGRO))
			return;
		ent->name = ent->def->name;
	}
	if (info & INFO_ACTIVE)
		return;

	ringRemove(&game->inactiveMonsters, ent);
	ringAppend(&game->activeMonsters, ent);
	ent->info |= INFO_ACTIVE;
	mon->ce.x = mon->destX;
	mon->ce.y = mon->destY;
	if (alert != 0 && (mon->flags & MON_ALERT_EVENT)) {
		Game_executeTile(game, mon->destX >> 6, mon->destY >> 6, 0xf000, 0);
		mon->flags &= ~MON_ALERT_EVENT;
	}
	/* The first (or a coin-flip later) woken monster with an alert sound barks. */
	if (Player_isVisible(game->player) && EntityMonster_getSound(ent->monster, 1) != 0 &&
	    (game->alertMonster == NULL || (Random_nextByte(&game->app->random) & 1)))
		game->alertMonster = ent;
}

/* 0x00007874  Put an active monster back on the inactive ring. */
void Game_deactivate(Game *game, void *entity)
{
	Entity *ent = entity;

	if (!(ent->info & INFO_ACTIVE))
		return;
	ringRemove(&game->activeMonsters, ent);
	ringAppend(&game->inactiveMonsters, ent);
	ent->info &= ~(INFO_ACTIVE | INFO_LERPING);
}

/* 0x00003a1c  End of the player's turn: count down petrification, haste
 * (every other turn the monsters skip), Player_advanceTurn, start the monster
 * phase and run the map's per-turn event at tile (2, 0). */
void Game_advanceTurn(Game *game)
{
	Applet *app = game->app;
	Player *p = game->player;
	int monstersMove;
	int i;

	for (i = 0; i < game->numEntities; i++) {
		EntityMonster *mon = game->entities[i].monster;

		if (mon != NULL && mon->petrifyTurns != 0) {
			mon->petrifyTurns--;
			game->entities[i].monster->frameTime = app->tickTime;
		}
	}

	/* status effect 4 (haste): the monsters only act on every second turn */
	monstersMove = 1;
	if (p->effects[4].turns != 0) {
		p->effects[4].amount++;
		if ((uint8_t)p->effects[4].amount & 1)
			monstersMove = 0;
	}
	Player_advanceTurn(game->player);

	game->monsterTurnMode = monstersMove ? 1 : 2;
	game->aiDone = 0;
	game->phaseTime = app->tickTime;
	if (monstersMove) {
		int flags = Canvas_getDirEventFlags(game->canvas, (int)0xf0000000);

		Game_executeTile(game, 2, 0, flags | (int)game->moveDirMaskB | 0xf000, 0);
		Canvas_updateFacing(game->canvas);
	}
	app->statRingTurns[game->player->ring]++;
	app->statTurns++;
	if (game->canvas->showSaveSize) {
		game->canvas->debugVar = Game_calcSaveSize(game);
		SaveFile_totalSize();
	}
}

/* 0x00006b28  m08 boss fight reinforcements (script op 0x5a).
 * op 0: init; op 1: once per turn, with chance roll% wake one of up to 4 dormant
 * hidden monsters at a free spawn tile (at most 3 times); op 2: kill the dormant
 * ones that are still alive. vars[0] spawned, [1] enabled, [2] spawns left in
 * the burst, [3] pause turns. */
void Game_updateBossReinforcements(Game *game, int op, int roll)
{
	Random *rnd = &game->app->random;
	int i;

	if (game->canvas->mapId != MAP_BOSS)
		return;

	if (op == 0) {
		game->vars[1] = 1;
		game->vars[0] = 0;
		game->vars[2] = (int)((uint32_t)Random_nextByte(rnd) % 5u) + 5;
		game->vars[3] = 0;
	} else if (op == 2) {
		for (i = 0; i < game->numEntities; i++) {
			Entity *ent = &game->entities[i];
			EntityDef *def = ent->def;

			if (def != NULL && def->eType == ETYPE_MONSTER && (ent->monster->flags & MON_DORMANT) &&
			    (ent->info & INFO_DEAD_MASK) == 0)
				Entity_died(ent, 0);
		}
	} else if (op == 1) {
		Entity *cands[4];
		int numCands = 0;
		int freeTile[2];
		int spawn[2][2];
		Sprite *spr = NULL;
		Entity *ent, *next;
		int choice;

		if (game->vars[3] >= 1) {
			if (--game->vars[3] == 0)
				game->vars[2] = (int)((uint32_t)Random_nextByte(rnd) % 5u) + 3;
			return;
		}
		if ((int)((uint32_t)Random_nextByte(rnd) % 100u) > roll || game->vars[0] >= 3 ||
		    game->inactiveMonsters == NULL)
			return;

		/* Up to 4 hidden dormant monsters; `spr` ends up as the sprite of the
		 * last monster examined (the original uses it for the spawn effect). */
		ent = game->inactiveMonsters;
		do {
			int16_t flags;

			next = ent->monster->nextOnList;
			spr = entitySprite(game->render, ent);
			flags = (int16_t)ent->monster->flags;
			if ((spr->info & 0x10000) && !(flags & MON_NO_RAISE) && (flags & MON_DORMANT)) {
				if (numCands == 4)
					break;
				cands[numCands++] = ent;
			}
			ent = next;
		} while (game->inactiveMonsters != next);
		if (numCands == 0)
			return;

		if (--game->vars[2] == 0)
			game->vars[3] = (int)((uint32_t)Random_nextByte(rnd) % 5u) + 8;

		for (i = 0; i < 2; i++) {
			spawn[i][0] = ModData_s32(&g_mod, MOD_BOSS_SPAWN_TILES + (uint32_t)i * 8u);
			spawn[i][1] = ModData_s32(&g_mod, MOD_BOSS_SPAWN_TILES + (uint32_t)i * 8u + 4u);
			freeTile[i] = 0;
		}
		for (i = 0; i < 2; i++) {
			if (Game_findEntityAt(game, spawn[i][0] * 0x40 + 0x20, spawn[i][1] * 0x40 + 0x20,
					      BLOCKING_TYPES) == NULL)
				freeTile[i] = 1;
		}
		if (freeTile[0] == 0) {
			if (freeTile[1] == 0)
				return;
			choice = 1;
		} else if (freeTile[1] == 0) {
			choice = 0;
		} else {
			choice = Random_nextByte(rnd) & 1;
		}

		ent = cands[Random_nextByte(rnd) % numCands];
		Entity_resurrect(ent, spawn[choice][0] * 0x40 + 0x20, spawn[choice][1] * 0x40 + 0x20, 0x20);
		Game_activateEx(game, ent, 0, 0);
		game->vars[0]++;
		Game_gspriteAllocAnim(game, 0xc0, spr->x, spr->y, spr->z);
	}
}

/* 0x00006d58  0 while the hub monster or any active monster is mid-lerp. */
int Game_canSnapMonsters(Game *game)
{
	Sprite *hub;
	Entity *ent;

	if (game->disableAI != 0)
		return 1;
	hub = game->render->hubSprite;
	if (hub != NULL && hub->ent != NULL && (hub->ent->info & INFO_LERPING))
		return 0;
	ent = game->activeMonsters;
	if (ent != NULL) {
		do {
			if (ent->info & INFO_LERPING)
				return 0;
			ent = ent->monster->nextOnList;
		} while (game->activeMonsters != ent);
	}
	return 1;
}

/* 0x0000daf0 */
void Game_clearMonsterTurn(Game *game)
{
	game->monsterTurnMode = 0;
	Combat_checkSurrounded(game->app->combat);
}

/* 0x0001166c  Pick (randomly among the first 4 found) a corpse on the inactive
 * ring near `entity`'s destination whose tile is free; marks it reserved and
 * returns its entity index, or -1. Used by corpse-raising/eating monsters. */
int Game_pickNearbyMonster(void *entity, int maxDistSq, int needFlags, int excludeFlags)
{
	Entity *self = entity;
	Game *game = self->app->game;
	Entity *cands[4];
	Entity *ent, *next;
	Entity *picked;
	int n = 0;

	ent = game->inactiveMonsters;
	if (ent == NULL)
		return -1;
	do {
		Sprite *spr;
		uint32_t info = ent->info;
		int flags;

		next = ent->monster->nextOnList;
		spr = Render_spriteAt(self->app->render, (info & INFO_SPRITE_MASK));
		flags = (int16_t)ent->monster->flags;
		if (!(info & INFO_REMOVED) && !(info & INFO_PICKED) && (info & INFO_CORPSE) &&
		    !Entity_isBoss(ent) &&
		    (needFlags == 0 || (flags & needFlags) != 0) &&
		    (flags & excludeFlags) == 0 &&
		    Entity_distSqFrom(ent, self->monster->destX, self->monster->destY) <= maxDistSq &&
		    Game_findEntityAt(game, spr->x, spr->y, BLOCKING_TYPES) == NULL)
			cands[n++] = ent;
		if (game->inactiveMonsters == next) {
			if (n == 0)
				return -1;
			break;
		}
		ent = next;
	} while (n != 4);

	picked = cands[Random_nextByte(&self->app->random) % n];
	picked->info |= INFO_PICKED;
	return (int)(picked - game->entities);
}

/* 0x0001bf94  Let every active monster think (mode 1 all, mode 2 only the fast
 * subtypes), then keep re-thinking queued attackers whose attack became invalid
 * and runners whose path got blocked until nothing changes. */
void Game_monsterAI(Game *game)
{
	Entity *ent, *next;
	int moved;

	if (game->disableAI != 0 || game->aiDone != 0)
		return;
	game->aiDone = 1;
	game->combatMonsters = NULL;
	if (game->activeMonsters == NULL)
		return;

	moved = 0;
	ent = game->activeMonsters;
	do {
		next = ent->monster->nextOnList;
		if (ent->monster->petrifyTurns == 0 &&
		    (game->monsterTurnMode == 1 ||
		     (game->monsterTurnMode == 2 && Entity_isMonsterType13or14(ent)))) {
			Entity_aiThink(ent);
			if (spriteBehindDest(game->render, ent))
				moved = 1;
		}
	} while (game->activeMonsters != next && (ent = next, game->activeMonsters != NULL));

	while (moved) {
		moved = 0;
		for (ent = game->combatMonsters; ent != NULL; ent = next) {
			next = ent->monster->nextAttacker;
			if (!Entity_aiIsAttackValid(ent)) {
				Game_removeFromCombat(ent);
				Entity_aiThink(ent);
				if (spriteBehindDest(game->render, ent))
					moved = 1;
			}
		}

		ent = game->activeMonsters;
		if (ent == NULL)
			break; /* the original would dereference NULL here */
		do {
			EntityMonster *mon;

			next = ent->monster->nextOnList;
			mon = ent->monster;
			if (!(ent->info & INFO_IN_COMBAT) && mon->petrifyTurns == 0 &&
			    (game->monsterTurnMode == 1 ||
			     (game->monsterTurnMode == 2 && Entity_isMonsterType13or14(ent))) &&
			    ent->def->eSubType == 1 && mon->goal.type == 1 && (mon->goal.flags & 2)) {
				Game_trace(game, mon->destX, mon->destY, mon->goal.x * 0x40 + 0x20,
					   mon->goal.y * 0x40 + 0x20, ent, BLOCKING_TYPES);
				if (game->traceEntity != NULL && game->traceEntity->def->eType != ETYPE_PLAYER) {
					MonsterGoal_reset(&ent->monster->goal);
					Entity_aiThink(ent);
					if (spriteBehindDest(game->render, ent))
						moved = 1;
				}
			}
		} while (game->activeMonsters != next && (ent = next, game->activeMonsters != NULL));
	}
}

/* 0x0001c158  Step every active monster's sprite towards its destination;
 * Game.monstersMoving stays set while any is still moving. */
void Game_updateMonsterLerps(Game *game)
{
	Entity *ent;
	int moving = 0;

	if (game->monstersMoving == 0)
		return;
	if (game->disableAI != 0) {
		game->monstersMoving = 0;
		return;
	}
	ent = game->activeMonsters;
	if (ent != NULL) {
		do {
			if (Entity_aiStepLerp(ent))
				moving = 1;
			ent->monster->ce.x = ent->monster->destX;
			ent->monster->ce.y = ent->monster->destY;
			ent = ent->monster->nextOnList;
		} while (game->activeMonsters != ent);
	}
	game->monstersMoving = moving;
}

/* 0x0001c74c  Monsters on the map (not dormant) and how many are dead/removed.
 * A subtype-0xf monster is followed by 3 helper entities that are skipped. */
void Game_countMonsters(void *player, int *outKilled, int *outTotal)
{
	Game *game = ((Player *)player)->game;
	int killed = 0, total = 0;
	int i;

	for (i = 0; i < game->numEntities; i++) {
		Entity *ent = &game->entities[i];

		if (ent->monster == NULL || (ent->monster->flags & MON_DORMANT))
			continue;
		total++;
		if (ent->info & 0x51020000u)
			killed++;
		if (ent->def->eSubType == 0xf)
			i += 3;
	}
	*outKilled = killed;
	*outTotal = total;
}

/* 0x0001f5d4  On revisiting a level, bring back up to 8 dead monsters (each with
 * a 101/256 chance) until at least half of the level's monsters live; every other
 * monster's home becomes its sprite position. */
void Game_resetMonsterPositions(Game *game)
{
	int mapBit = (game->canvas->mapId - 6000) & 0xff;
	int i;

	if (game->isLoaded == 0 && mapBit < 32 && (game->player->visitedMaps & (1u << mapBit))) {
		int killed, total, alive, raised = 0;

		Game_countMonsters(game->player, &killed, &total);
		alive = total - killed;
		for (i = 0; i < game->numEntities; i++) {
			Entity *ent = &game->entities[i];
			EntityMonster *mon = ent->monster;
			Render *render;
			uint32_t info;
			int sub;

			if (mon == NULL)
				continue;
			if (total / 2 <= alive || raised > 7)
				return;
			render = game->render;
			info = ent->info;
			if ((info & INFO_DEAD_MASK) != 0 && (mon->flags & 0x82) == 0 &&
			    (uint32_t)Random_nextByte(&game->app->random) <= 100u &&
			    (sub = ent->def->eSubType, sub != 8 && sub != 2) &&
			    Game_findEntityAt(game, mon->destX, mon->destY, BLOCKING_TYPES) == NULL) {
				Entity_resurrect(ent, mon->destX, mon->destY, 0x20);
				ent->info &= ~INFO_DIRTY;
				alive++;
				raised++;
			} else {
				Sprite *spr = Render_spriteAt(render, (info & INFO_SPRITE_MASK));

				mon->ce.x = mon->destX = spr->x;
				mon->ce.y = mon->destY = spr->y;
			}
		}
	} else {
		for (i = 0; i < game->numEntities; i++) {
			Entity *ent = &game->entities[i];
			EntityMonster *mon = ent->monster;
			Sprite *spr;

			if (mon == NULL)
				continue;
			spr = entitySprite(game->render, ent);
			mon->ce.x = mon->destX = spr->x;
			mon->ce.y = mon->destY = spr->y;
		}
	}
}

/* 0x00027028  Finish monster movement at once (skip key / cinematic skip).
 * Outside the monster phase only knocked-back sprites (and the hub monster) are
 * snapped; inside it the AI runs, everyone is snapped, and a queued attacker
 * starts its attack. Returns whether snapping was allowed (0 once combat starts). */
int Game_snapAllMonsterSprites(Game *game, int force)
{
	Entity *ent, *next;

	if (game->monsterTurnMode == 0) {
		Sprite *hub;
		int snapped = 0;

		if (force == 0)
			force = Game_canSnapMonsters(game);
		hub = game->render->hubSprite;
		if (force != 0 && hub != NULL && hub->ent->monster->goal.param < 2) {
			Game_snapMonsterSprite(hub->ent);
			snapped = 1;
		}
		if (force != 0 && game->activeMonsters != NULL) {
			ent = game->activeMonsters;
			do {
				next = ent->monster->nextOnList;
				if (ent->monster->flags & MON_KNOCKED) {
					Game_snapMonsterSprite(ent);
					snapped = 1;
				}
				ent = next;
			} while (game->activeMonsters != next);
		}
		if (snapped)
			Canvas_updateFacing(game->canvas);
	} else {
		Game_monsterAI(game);
		if (force == 0)
			force = Game_canSnapMonsters(game);
		if (force != 0 && game->activeMonsters != NULL) {
			ent = game->activeMonsters;
			do {
				Game_snapMonsterSprite(ent);
				ent = ent->monster->nextOnList;
			} while (game->activeMonsters != ent);
			Canvas_updateFacing(game->canvas);
		}
		Game_updateMonsterLerps(game);
		ent = game->combatMonsters;
		if (ent != NULL && game->monstersMoving == 0) {
			Combat_start(game->app->combat, ent, ent->monster->target, 0, 0);
			force = 0;
		}
	}
	return force;
}

/* 0x00027188  Run a monster's movement to completion and put its sprite on the
 * destination. Returns 1 if it had to move. */
int Game_snapMonsterSprite(void *entity)
{
	Entity *ent = entity;
	Render *render = ent->app->render;
	Sprite *spr = entitySprite(render, ent);
	EntityMonster *mon = ent->monster;

	if (spr->x == mon->destX && spr->y == mon->destY)
		return 0;
	while (Entity_aiStepLerp(ent) != 0)
		;
	mon = ent->monster;
	mon->ce.x = mon->destX;
	spr->x = mon->destX;
	mon->ce.y = mon->destY;
	spr->y = mon->destY;
	Render_relinkSpriteAt(render, spr, -1, -1);
	return 1;
}

/* 0x00029fb4  1 if a queued attacker (not one of the ranged-through weapons
 * 0x13/0x15/0x25/0x26) stands on the same row/column as tile (tx, ty) with that
 * tile strictly between it and the player. */
int Game_isMonsterBetween(Game *game, int tx, int ty)
{
	int px = game->canvas->destX >> 6;
	int py = game->canvas->destY >> 6;
	Entity *ent;

	if (tx != px && ty != py)
		return 0;
	for (ent = game->combatMonsters; ent != NULL; ent = ent->monster->nextAttacker) {
		EntityMonster *mon = ent->monster;
		int w = mon->ce.weapon;
		int mx, my, lo, hi;

		if (w == 0x15 || w == 0x26 || w == 0x25 || w == 0x13)
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

/* 0x0002addc  Take a monster off the combat queue: reset its attack frame (or
 * release the corpse it was going for), clear its goal. */
void Game_removeFromCombat(void *entity)
{
	Entity *ent = entity;
	Game *game = ent->app->game;
	uint32_t info = ent->info;
	Entity *cur, *prev;

	if (!(info & INFO_IN_COMBAT))
		return;
	ent->info = info & ~INFO_IN_COMBAT;

	switch (ent->monster->ce.weapon) {
	case 0x0e:
	case 0x19:
		Render_spriteAt(ent->app->render, (info & INFO_SPRITE_MASK))->info &= 0xfffff0ffu;
		break;
	case 0x15:
	case 0x26:
		/* corpse raise/eat: goal.param holds the corpse's entity index */
		game->entities[ent->monster->goal.param].info &= ~INFO_PICKED;
		break;
	default:
		break;
	}
	MonsterGoal_reset(&ent->monster->goal);

	/* unlink from the queue (if absent, the tail's link is overwritten, as in the original) */
	prev = NULL;
	for (cur = game->combatMonsters; cur != NULL && cur != ent; cur = cur->monster->nextAttacker)
		prev = cur;
	if (prev == NULL)
		game->combatMonsters = ent->monster->nextAttacker;
	else
		prev->monster->nextAttacker = ent->monster->nextAttacker;
}

/* 0x0002b9a4  Run the monsters' turn; once nobody is moving either start the
 * first queued attack or hand control back to the player. */
void Game_endPlayerTurn(Game *game)
{
	Entity *attacker;

	Game_monsterAI(game);
	Game_updateMonsterLerps(game);
	if (game->monstersMoving != 0)
		return;
	Canvas_updateFacing(game->canvas);
	attacker = game->combatMonsters;
	if (attacker == NULL) {
		Game_clearMonsterTurn(game);
		Canvas_drawPlayingSoftKeys(game->canvas);
	} else {
		Combat_start(game->app->combat, attacker, attacker->monster->target, 0, 0);
	}
}

/* 0x0002acb8  Called from Entity_died with the dying entity: when a third (or
 * later) visible corpse lies on world position (x, y), hide and unlink it. */
void Game_hideExcessCorpses(void *entity, int x, int y)
{
	Applet *app = ((Entity *)entity)->app;
	Game *game = app->game;
	Entity *ent;
	int count = 0;

	ent = game->inactiveMonsters;
	if (ent == NULL)
		return;
	do {
		Sprite *spr = Render_spriteAt(app->render, (ent->info & INFO_SPRITE_MASK));

		if (spr->x == x && spr->y == y && (ent->info & INFO_DEAD_MASK) != 0 &&
		    !(spr->info & 0x10000) && ++count > 2) {
			spr->info |= 0x10000;
			ent->info = (ent->info & ~INFO_CORPSE) | INFO_REMOVED;
			Game_unlinkEntity_2ae8c(app->game, ent);
		}
		ent = ent->monster->nextOnList;
	} while (app->game->inactiveMonsters != ent);
}
