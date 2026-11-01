/*
 * Monster AI (Orcs & Elves, BREW 1049): goal selection, goal execution,
 * movement steps, boss minions and the hub wererat.
 *
 * Each monster turn runs Entity_aiThink: keep the current MonsterGoal while
 * Entity_aiIsValidGoal accepts it, otherwise choose a new one by monster
 * subtype (Entity_aiChooseNewGoal), then execute it (Entity_aiDoGoal).
 *
 * Goal types: 1 move to tile (x,y); 2 move towards entity `param` (1 = the
 * player); 3 attack entity `param`; 4/5 flee/evade for `param` turns; 6 Slam
 * (telegraphed for one turn); 8 teleport next to the player; 9 wait; 10
 * weapon-0x25 attack; 0xb Major Heal after `param` turns; 0xc raise corpse
 * `param`; 0xd disappear; 0xe wander while the player is invisible.
 * Goal flags: 2 running/charge, 4 attack after the move, 8 evade after
 * attacking, 0x10 re-plan after the move, 0x20 prefer the tile behind the
 * player's facing, 0x40 teleport after attacking, 0x80 guard.
 */

#include "game.h"

/* ---- player-visible strings in the .mod ---- */
#define MOD_STR_DEVOURS_CORPSE    0x302b8u
#define MOD_STR_IS_HEALED         0x30260u
#define MOD_STR_WILL_HEAL         0x30278u
#define MOD_STR_HEAL_SUFFIX_ONE   0x30274u
#define MOD_STR_HEAL_SUFFIX_MANY  0x30270u
#define MOD_STR_DISAPPEARS        0x30294u
#define MOD_STR_READIES_SLAM      0x302a4u
#define MOD_STR_READIES_HEAL      0x3014cu
#define MOD_STR_WERERAT_STATE     0x30244u

/* ---- tables in the .mod data area ---- */
#define MOD_HEALER_WEIGHTS   0x2ffd0u /* u8 [3 range bands][4 wound levels][5 behaviours] */
#define MOD_CORPSE_CHANCE    0x2ff40u /* s32 [4] by wound level, /256 */
#define MOD_TELEPORT_DIRS    0x2ff50u /* 4 x {s32 dx, s32 dy} */
#define MOD_MINION_DIRS      0x2ff70u /* 8 x {s32 dx, s32 dy} (octants) */
#define MOD_ADJACENT_DIRS    0x2ffb0u /* 4 x {s32 dx, s32 dy} */
#define MOD_DISAPPEAR_DIRS   0x3000cu /* 8 x {s32 dx, s32 dy} around tile (7,24) */
#define MOD_WERERAT_LAIRS    0x2ff1cu /* 2 x {u8 tx, u8 ty, u8 destIndex[3]} */
#define MOD_WERERAT_DESTS    0x2ff26u /* n x {u8 x0, u8 y0, u8 x1, u8 y1} random target box */

/* Trace / occupancy masks (one bit per entity type). */
#define MASK_BLOCKERS     0xfdaf
#define MASK_BLOCKERS_F   0xfdbf /* subtype-0xf healer boss */
#define MASK_CHARGE       0xfdad
#define MASK_ATTACK       0x54a7
#define MASK_ATTACK_RAISE 0x54a5

/* Centre of the healer boss's arena, tile (7,24). */
#define HEALER_ARENA_X 0x1e0
#define HEALER_ARENA_Y 0x620

/* Original address of Game.entities[0] inside the Applet (Applet+0x1a7cc8+0x8ec). */
#define ORIG_ENTITIES_OFS (0x1a7cc8 + 0x8ec)

static Sprite *entSprite(Entity *e)
{
	return Render_spriteAt(e->app->render, (e->info & 0xffff));
}

/* Sprite animation frame lives in info bits 8..11. */
static void setSpriteFrame(Sprite *s, int frame)
{
	s->info = (s->info & 0xfffff0ffu) + ((uint32_t)frame << 8);
}

static int dirX(uint32_t table, int i) { return ModData_s32(&g_mod, table + i * 8); }
static int dirY(uint32_t table, int i) { return ModData_s32(&g_mod, table + i * 8 + 4); }

/* Tile of an entity from its tile-list link index (x + y*32). */
static void linkTile(Entity *e, int *tx, int *ty)
{
	int link = e->linkIndex;
	*ty = link / 32;
	*tx = link - *ty * 32;
}

static int entityIndex(Game *game, Entity *e)
{
	return (int)(e - game->entities);
}

/* 0x000224b4 */
void MonsterGoal_reset(MonsterGoal *goal)
{
	goal->type = 0;
	goal->flags = 0;
	goal->turns = 0;
	goal->y = 0;
	goal->x = 0;
	goal->param = 0;
}

/* 0x00002dc8: a corpse-eating monster finishes its meal */
void Entity_devourCorpse(Entity *ent, int corpseIdx)
{
	char buf[128];
	Game *game = ent->app->game;
	CombatEntity *ce = &ent->monster->ce;

	Canvas_startShake(ent->app->canvas, 300, 1, 0);
	Entity_died(&game->entities[corpseIdx], 1);
	CombatEntity_setStrength(ce, CombatEntity_getStrength(ce) + 5);
	CombatEntity_setMaxHealth(ce, CombatEntity_getMaxHealth(ce) + 10);
	CombatEntity_setHealth(ce, CombatEntity_getMaxHealth(ce));
	App_snprintf(buf, sizeof buf, (char *)ModData_str(&g_mod, MOD_STR_DEVOURS_CORPSE), ent->name);
	Hud_addMessage(ent->app->hud, buf, 0);
}

/* 0x00003d08: healer boss (subtype 0xf). A weighted random choice between
 * attacking, evading, summoning its minions, raising a corpse and running to
 * a free tile next to its arena centre. The weights depend on how close and
 * aligned the player is and on the boss's wound level. */
void Entity_aiCalcGoalSubtype15(Entity *ent, MonsterGoal *goal)
{
	enum { CHOICE_ATTACK, CHOICE_EVADE, CHOICE_SUMMON, CHOICE_RAISE, CHOICE_MOVE };
	int wound = Entity_getWoundLevel(ent);
	Canvas *cv = ent->app->canvas;
	int distSq = Entity_distSqFrom(ent, cv->destX, cv->destY);
	int aligned = ent->monster->destX == cv->destX || ent->monster->destY == cv->destY;
	int band;

	if (distSq <= 0x1000 && aligned)
		band = 0;
	else if (distSq > 0x1000 && aligned)
		band = 1;
	else
		band = 2;
	uint32_t row = MOD_HEALER_WEIGHTS + band * 20 + wound * 5;
#define WEIGHT(k) ModData_u8(&g_mod, row + (k))

	int kinds[5], weights[5];
	int n = 0;
	int moveX = 0, moveY = 0;
	int corpse = 0;
	int weapon = 0;

	if (WEIGHT(4) != 0 &&
	    Entity_aiFindFreeAdjacent(ent, HEALER_ARENA_X, HEALER_ARENA_Y, cv->viewX, cv->viewY, MASK_BLOCKERS,
	                              &moveX, &moveY)) {
		kinds[n] = CHOICE_MOVE;
		weights[n++] = WEIGHT(4);
	}
	if (WEIGHT(3) != 0) {
		corpse = Game_pickNearbyMonster(ent, 0x24000, 0, 0x100);
		if (corpse != -1) {
			kinds[n] = CHOICE_RAISE;
			weights[n++] = WEIGHT(3);
		}
	}
	if (WEIGHT(1) != 0) {
		kinds[n] = CHOICE_EVADE;
		weights[n++] = WEIGHT(1);
	}
	if (WEIGHT(0) != 0) {
		weapon = Entity_aiWeaponForTarget(ent, &ent->app->game->entities[1]);
		if (weapon != -1) {
			kinds[n] = CHOICE_ATTACK;
			weights[n++] = WEIGHT(0);
		}
	}
	if (WEIGHT(2) != 0) {
		/* summon only while one of the three minions stored after the boss is gone */
		int alive = 0;
		for (int i = 0; i < 3; i++) {
			Entity *minion = &ent[i + 1];
			if (!(entSprite(minion)->info & 0x10000) && !(minion->info & 0x40000000))
				alive++;
		}
		if (alive != 3) {
			kinds[n] = CHOICE_SUMMON;
			weights[n++] = WEIGHT(2);
		}
	}
#undef WEIGHT

	/* cumulative weights */
	int total = 0;
	for (int i = 0; i < n; i++) {
		total += weights[i];
		if (i > 0)
			weights[i] += weights[i - 1];
	}
	int r = Random_nextInt(&ent->app->random);
	int choice = -1;
	if (total != 0) { /* the original divides unguarded; every table row has a fallback weight */
		r %= total;
		for (int i = 0; i < n; i++) {
			int lo = i == 0 ? 0 : weights[i - 1];
			if (lo <= r && r < weights[i]) {
				choice = kinds[i];
				break;
			}
		}
	}

	switch (choice) {
	case CHOICE_ATTACK:
		ent->monster->ce.weapon = (uint8_t)weapon;
		ent->monster->target = NULL;
		Entity_attack(ent);
		MonsterGoal_reset(goal);
		break;
	case CHOICE_EVADE:
		goal->type = 5;
		goal->param = 1;
		break;
	case CHOICE_SUMMON:
		goal->type = 10;
		break;
	case CHOICE_RAISE:
		ent->monster->ce.weapon = 0x26;
		Entity_attack(ent);
		goal->type = 0xc;
		goal->param = (int16_t)corpse;
		break;
	case CHOICE_MOVE:
		goal->type = 1;
		goal->x = (uint8_t)(moveX >> 6);
		goal->y = (uint8_t)(moveY >> 6);
		goal->flags |= 2;
		break;
	}
	/* release the corpse Game_pickNearbyMonster reserved (also when none was asked for) */
	if (corpse != -1 && choice != CHOICE_RAISE)
		ent->app->game->entities[corpse].info &= ~0x10000000u;
}

/* 0x00003fac: subtype 10 */
void Entity_aiCalcGoalSubtype10(Entity *ent, MonsterGoal *goal)
{
	Entity_aiCalcSimpleGoal(ent, goal);
	if (goal->type == 3)
		goal->flags |= 0x40;
	goal->flags |= 0x24;
}

/* 0x00003fd0: subtype 5 (necromancer) raises a nearby corpse when it can */
void Entity_aiCalcSimpleGoalRaise(Entity *ent, MonsterGoal *goal)
{
	int corpse;

	if (ent->def->parm < 1 || (corpse = Game_pickNearbyMonster(ent, 0x10000, 0, 0x100)) == -1) {
		Entity_aiCalcSimpleGoal(ent, goal);
		return;
	}
	ent->monster->ce.weapon = 0x15;
	Entity_attack(ent);
	goal->type = 0xc;
	goal->param = (int16_t)corpse;
}

/* 0x00004014: attack the player when a weapon reaches and the line is clear,
 * otherwise walk towards the player */
void Entity_aiCalcSimpleGoal(Entity *ent, MonsterGoal *goal)
{
	Game *game = ent->app->game;
	Sprite *spr = entSprite(ent);
	int weapon = Entity_aiWeaponForTarget(ent, &game->entities[1]);

	if (weapon != -1) {
		Canvas *cv = ent->app->canvas;
		int dist = Combat_worldDistToTileDist(Entity_distSqFrom(ent, cv->destX, cv->destY));
		Combat *combat = ent->app->combat;
		int px, py;

		ent->monster->ce.weapon = (uint8_t)weapon;
		linkTile(&game->entities[1], &px, &py);
		if ((spr->x >> 6 == px || spr->y >> 6 == py) && dist <= combat->weapons[weapon].rangeMax &&
		    dist >= combat->weapons[weapon].rangeMin) {
			Game_trace(ent->app->game, spr->x, spr->y, cv->destX, cv->destY, ent, MASK_ATTACK);
			game = ent->app->game;
			Entity *hit = game->traceEntity;
			if (hit) {
				int idx = entityIndex(game, hit);
				uint8_t type = hit->def->eType;
				if ((type == 10 && hit->def->eSubType == 2) || type == 1) {
					goal->type = 3;
					goal->param = (int16_t)idx;
					return;
				}
			}
		}
	}
	goal->type = 2;
	goal->param = 1;
}

/* 0x00004138: subtype 4 (corpse eater). Depending on its wounds it looks for a
 * reachable corpse: next to it -> eat (weapon 0x13), else walk to it. */
void Entity_aiCalcGoalSubtype4(Entity *ent, MonsterGoal *goal)
{
	MonsterGoal_reset(goal);
	int wound = Entity_getWoundLevel(ent);
	if (Random_nextByte(&ent->app->random) <= ModData_s32(&g_mod, MOD_CORPSE_CHANCE + wound * 4)) {
		int best = 0x9000;
		Entity *cands[4];
		int count = 0;
		Entity *e = ent->app->game->inactiveMonsters;

		if (e) {
			do {
				Render *render = ent->app->render;
				Sprite *s = Render_spriteAt(render, (e->info & 0xffff));
				Entity *occ = Game_findEntityAt(ent->app->game, s->x, s->y, MASK_BLOCKERS);
				if (!(e->info & 0x20000) && (e->info & 0x40000000) && (occ == NULL || occ == ent)) {
					int d = Entity_distSqFrom(ent, s->x, s->y);
					if (Render_inSameLeaf(ent->app->render, ent->monster->destX, ent->monster->destY, s->x,
					                      s->y)) {
						if (d < best) {
							best = d;
							cands[0] = e;
							count = 1;
						} else if (d == best && count < 4) {
							cands[count++] = e;
						}
					}
				}
				e = e->monster->nextOnList;
			} while (ent->app->game->inactiveMonsters != e);

			if (count > 0) {
				Entity *corpse = cands[Random_nextByte(&ent->app->random) % count];
				int idx = entityIndex(ent->app->game, corpse);
				if (best >= 0x1000) {
					goal->type = 2;
					goal->param = (int16_t)idx;
					return;
				}
				goal->type = 3;
				goal->param = (int16_t)idx;
				ent->monster->ce.weapon = 0x13;
				ent->monster->target = NULL;
				Entity_attack(ent);
			}
		}
	}
	if (goal->type == 0)
		Entity_aiCalcSimpleGoal(ent, goal);
}

/* 0x000042b8: subtype 7 telegraphs its Slam (weapon 0x19) */
void Entity_aiCalcGoalSubtype7(Entity *ent, MonsterGoal *goal)
{
	MonsterGoal_reset(goal);
	goal->flags |= 0x80;
	int weapon = Entity_aiWeaponForTarget(ent, &ent->app->game->entities[1]);
	if (weapon == 0x19) {
		Canvas *cv = ent->app->canvas;
		if (Entity_distSqFrom(ent, cv->destX, cv->destY) <= 0x4000) {
			goal->type = 6;
		} else {
			goal->type = 2;
			goal->param = 1;
		}
	} else {
		Entity_aiCalcSimpleGoal(ent, goal);
	}
	ent->monster->ce.weapon = (uint8_t)weapon;
}

/* 0x00004328: subtype 1 charges (weapon 0xe) to the tile next to the player */
void Entity_aiCalcGoalCharge(Entity *ent, MonsterGoal *goal)
{
	Sprite *spr = entSprite(ent);
	Game *game = ent->app->game;
	int weapon = Entity_aiWeaponForTarget(ent, &game->entities[1]);

	goal->flags |= 0x20;
	if (weapon != 0xe) {
		Entity_aiCalcSimpleGoal(ent, goal);
		return;
	}
	Canvas *cv = ent->app->canvas;
	Game_trace(ent->app->game, spr->x, spr->y, cv->destX, cv->destY, ent, MASK_CHARGE);
	if (ent->app->game->traceEntity != NULL) {
		goal->type = 2;
		goal->param = 1;
		return;
	}
	ent->monster->ce.weapon = 0xe;
	int px, py;
	linkTile(&game->entities[1], &px, &py);
	int mx = spr->x >> 6;
	int my = spr->y >> 6;
	int tx, ty;
	if (mx == px) {
		tx = mx;
		ty = my >= py ? py + 1 : py - 1;
	} else {
		tx = mx >= px ? px + 1 : px - 1;
		ty = my;
	}
	goal->type = 1;
	goal->x = (uint8_t)tx;
	goal->y = (uint8_t)ty;
	goal->flags |= 2;
	setSpriteFrame(spr, 2);
}

/* 0x00004414 */
int Entity_aiChooseNewGoal(Entity *ent, MonsterGoal *goal)
{
	MonsterGoal_reset(goal);
	if (!Player_isVisible(ent->app->player) && !Entity_isBoss(ent)) {
		goal->type = 0xe;
		return 1;
	}
	switch (ent->def->eSubType) {
	case 1: {
		Sprite *hub = ent->app->render->hubSprite;
		if (hub != NULL && hub->ent == ent)
			Entity_updateWererat(ent, 0);
		else
			Entity_aiCalcGoalCharge(ent, goal);
		break;
	}
	case 2:
	case 0xb:
		Entity_aiCalcSimpleGoal(ent, goal);
		goal->flags |= 4;
		break;
	case 4:
		Entity_aiCalcGoalSubtype4(ent, goal);
		break;
	case 5:
		Entity_aiCalcSimpleGoalRaise(ent, goal);
		break;
	case 6:
		Entity_aiCalcSimpleGoal(ent, goal);
		goal->flags |= 0x14;
		break;
	case 7:
		Entity_aiCalcGoalSubtype7(ent, goal);
		break;
	case 8:
		Entity_aiCalcSimpleGoal(ent, goal);
		if (!(ent->monster->flags & 8) && goal->type == 3)
			goal->flags |= 8;
		goal->flags |= 4;
		break;
	case 10:
		Entity_aiCalcGoalSubtype10(ent, goal);
		break;
	case 0xf:
		Entity_aiCalcGoalSubtype15(ent, goal);
		break;
	default:
		Entity_aiCalcSimpleGoal(ent, goal);
		break;
	}
	return 1;
}

/* 0x00004518: goal 0xb, Major Heal counts down then restores full health */
void Entity_aiGoal_HEAL(Entity *ent, MonsterGoal *goal)
{
	char buf[64];

	if ((uint16_t)goal->turns == (uint16_t)goal->param) {
		CombatEntity_setHealth(&ent->monster->ce, CombatEntity_getMaxHealth(&ent->monster->ce));
		MonsterGoal_reset(goal);
		App_snprintf(buf, sizeof buf, (char *)ModData_str(&g_mod, MOD_STR_IS_HEALED), ent->name);
		Hud_addMessage(ent->app->hud, buf, 3);
		setSpriteFrame(entSprite(ent), 4);
		ent->monster->frameTime = ent->app->tickTime + 100;
	} else {
		int left = goal->param - goal->turns;
		const char *suffix = ModData_str(&g_mod, left == 1 ? MOD_STR_HEAL_SUFFIX_ONE : MOD_STR_HEAL_SUFFIX_MANY);
		App_snprintf(buf, sizeof buf, (char *)ModData_str(&g_mod, MOD_STR_WILL_HEAL), ent->name, left, suffix);
		Hud_addMessage(ent->app->hud, buf, 3);
	}
}

/* 0x000045e4: goal 10, the summoning attack (weapon 0x25) */
void Entity_aiGoal_10(Entity *ent, MonsterGoal *goal)
{
	ent->monster->ce.weapon = 0x25;
	Entity_attack(ent);
	MonsterGoal_reset(goal);
}

/* 0x00004600: goal 0xd, vanish to the free tile around the arena centre that
 * is farthest from the player */
void Entity_aiGoal_DISAPPEAR(Entity *ent, MonsterGoal *goal)
{
	Sprite *spr = entSprite(ent);
	int bestDist = 0;
	int best = -1;
	char buf[64];

	for (int i = 0; i < 8; i++) {
		int x = dirX(MOD_DISAPPEAR_DIRS, i) * 64 + HEALER_ARENA_X;
		int y = dirY(MOD_DISAPPEAR_DIRS, i) * 64 + HEALER_ARENA_Y;
		if (Game_findEntityAt(ent->app->game, x, y, MASK_BLOCKERS) == NULL) {
			Canvas *cv = ent->app->canvas;
			int dx = x - cv->viewX;
			int dy = y - cv->viewY;
			int d = dx * dx + dy * dy;
			if (d > bestDist) {
				best = i;
				bestDist = d;
			}
		}
	}
	if (best == -1) {
		Entity_aiCalcSimpleGoal(ent, goal);
		return;
	}

	Game_gspriteAllocAnim(ent->app->game, 0xf6, spr->x, spr->y, 0x20);
	Sound_play(ent->app->sound, 0x13c5, 0, 4, -1);
	App_snprintf(buf, sizeof buf, (char *)ModData_str(&g_mod, MOD_STR_DISAPPEARS), ent->name);
	Hud_addMessage(ent->app->hud, buf, 0);
	Game_unlinkEntity_2ae8c(ent->app->game, ent);
	int x = dirX(MOD_DISAPPEAR_DIRS, best) * 64 + HEALER_ARENA_X;
	ent->monster->ce.x = x;
	ent->monster->destX = x;
	spr->x = x;
	int y = dirY(MOD_DISAPPEAR_DIRS, best) * 64 + HEALER_ARENA_Y;
	ent->monster->ce.y = y;
	ent->monster->destY = y;
	spr->y = y;
	Game_linkEntity(ent->app->game, ent, spr->x >> 6, y >> 6);
	Render_relinkSpriteAt(ent->app->render, spr, -1, -1);
	ent->info &= ~0x20000000u;
	MonsterGoal_reset(goal);
}

/* 0x00004780: goal 6, "readies Slam" on the first turn, then slams if the
 * player is still aligned, close and in the line of fire */
void Entity_aiGoal_SLAM(Entity *ent, MonsterGoal *goal)
{
	Sprite *spr = entSprite(ent);
	char buf[64];

	if (goal->turns == 1) {
		setSpriteFrame(spr, 4);
		App_snprintf(buf, sizeof buf, (char *)ModData_str(&g_mod, MOD_STR_READIES_SLAM), ent->name);
		Hud_addMessage(ent->app->hud, buf, 0);
		return;
	}

	Canvas *cv = ent->app->canvas;
	int slam = 0;
	if ((spr->x == cv->destX || spr->y == cv->destY) && Entity_distSqFrom(ent, cv->destX, cv->destY) <= 0x4000) {
		cv = ent->app->canvas;
		Game_trace(ent->app->game, spr->x, spr->y, cv->destX, cv->destY, ent, MASK_ATTACK);
		Entity *hit = ent->app->game->traceEntity;
		if (hit != NULL && hit->def->eType == 1)
			slam = 1;
	}
	MonsterGoal_reset(goal);
	if (slam) {
		ent->monster->target = NULL;
		Entity_attack(ent);
	} else {
		setSpriteFrame(spr, 0);
	}
}

/* 0x00004878: goals 1/2/4/5/0xe pick the next tile and step towards it */
void Entity_aiGoal_MOVE(Entity *ent, MonsterGoal *goal)
{
	EntityMonster *mon = ent->monster;
	int steps[8];

	if (mon->flags & 0x20) { /* never moves */
		MonsterGoal_reset(goal);
		return;
	}
	int tx = mon->destX;
	int ty = mon->destY;

	if (goal->type == 2 && goal->param == 1) {
		/* approach the player: one of the 4 tiles around his destination */
		Canvas *cv = ent->app->canvas;
		int around[8] = { cv->stepX, cv->stepY, -cv->stepX, -cv->stepY,
		                  cv->rightX, cv->rightY, -cv->rightX, -cv->rightY };
		int order[4];
		if (goal->flags & 0x20) {
			order[0] = 1;
			if (Random_nextByte(&ent->app->random) & 1) {
				order[1] = 2;
				order[2] = 3;
			} else {
				order[1] = 3;
				order[2] = 2;
			}
			order[3] = 0;
		} else if (goal->flags & 0x80) {
			order[0] = 0;
			if (Random_nextByte(&ent->app->random) & 1) {
				order[1] = 2;
				order[2] = 3;
			} else {
				order[1] = 3;
				order[2] = 2;
			}
			order[3] = 1;
		} else {
			switch (Random_nextByte(&ent->app->random) & 3) {
			case 0:
				order[0] = 2; order[1] = 3; order[2] = 0; order[3] = 1;
				break;
			case 1:
				order[0] = 3; order[1] = 2; order[2] = 0; order[3] = 1;
				break;
			default:
				order[0] = 0; order[1] = 2; order[2] = 3; order[3] = 1;
				break;
			}
		}
		int i;
		for (i = 0; i < 4; i++) {
			cv = ent->app->canvas;
			tx = cv->destX + around[order[i] * 2];
			ty = cv->destY + around[order[i] * 2 + 1];
			Entity *occ = Game_findEntityAt(ent->app->game, tx, ty, MASK_BLOCKERS);
			if (occ == NULL || occ == ent)
				break;
		}
		if (i == 4) {
			cv = ent->app->canvas;
			tx = cv->destX;
			ty = cv->destY;
		}
		goal->x = (uint8_t)(tx >> 6);
		goal->y = (uint8_t)(ty >> 6);
	} else if (goal->type == 4 || goal->type == 5 || goal->type == 0xe) {
		int mx = tx >> 6, my = ty >> 6;
		int n;
		if (goal->type == 0xe) {
			n = Entity_aiStepDirs(ent, mx, my, mx, my, steps);
		} else {
			Canvas *cv = ent->app->canvas;
			n = Entity_aiStepDirs(ent, mx, my, cv->destX >> 6, cv->destY >> 6, steps);
		}
		int i;
		for (i = 0; i < n; i++) {
			int fx = ent->monster->destX;
			int fy = ent->monster->destY;
			tx = fx + steps[i * 2] * 64;
			ty = fy + steps[i * 2 + 1] * 64;
			if (Entity_checkLineOfSight(ent, fx >> 6, fy >> 6, tx >> 6, ty >> 6, MASK_BLOCKERS))
				break;
		}
		if (i == n) {
			if (goal->type == 5)
				MonsterGoal_reset(goal);
			return;
		}
	} else if (goal->type == 2) {
		int ex, ey;
		linkTile(&ent->app->game->entities[goal->param], &ex, &ey);
		tx = ex * 64 + 32;
		ty = ey * 64 + 32;
		goal->x = (uint8_t)(tx >> 6);
		goal->y = (uint8_t)(ty >> 6);
	} else {
		tx = goal->x * 64 + 32;
		ty = goal->y * 64 + 32;
	}

	Sprite *spr = entSprite(ent);
	Entity_aiMoveStep(ent, spr->x, spr->y, tx, ty);
	ent->app->game->monstersMoving = 1;
}

/* 0x00004b80: goal 8, reappear on a free tile next to the player */
void Entity_aiGoal_TELEPORT(Entity *ent, MonsterGoal *goal)
{
	Sprite *spr = entSprite(ent);
	int order[4];
	char buf[64];

	int r = Random_nextByte(&ent->app->random) & 3;
	for (int i = 0; i < 4; i++)
		order[i] = (r + i) & 3;

	Canvas *cv = ent->app->canvas;
	int px = cv->destX;
	int py = cv->destY;
	int x = 0, y = 0;
	int i;
	Game *game = ent->app->game;
	for (i = 0; i < 4; i++) {
		int dx = dirX(MOD_TELEPORT_DIRS, order[i]);
		int dy = dirY(MOD_TELEPORT_DIRS, order[i]);
		x = dx * 64 + px;
		y = dy * 64 + py;
		/* trace from just beyond the tile towards the player */
		Game_trace(ent->app->game, dx + x, dy + y, px, py, NULL, MASK_BLOCKERS);
		game = ent->app->game;
		Entity *hit = game->traceEntity;
		if (hit == NULL || hit->def->eType == 1)
			break;
	}
	if (i != 4) {
		Game_gspriteAllocAnim(game, 0xf6, spr->x, spr->y, 0x20);
		App_snprintf(buf, sizeof buf, (char *)ModData_str(&g_mod, MOD_STR_DISAPPEARS), ent->name);
		Hud_addMessage(ent->app->hud, buf, 0);
		Sound_play(ent->app->sound, 0x1407, 0, 3, -1);
		Game_unlinkEntity_2ae8c(ent->app->game, ent);
		ent->monster->ce.x = x;
		ent->monster->destX = x;
		spr->x = x;
		ent->monster->ce.y = y;
		ent->monster->destY = y;
		spr->y = y;
		ent->info &= ~0x400000u;
		Game_linkEntity(ent->app->game, ent, spr->x >> 6, spr->y >> 6);
		Render_relinkSpriteAt(ent->app->render, spr, -1, -1);
		ent->info &= ~0x20000000u;
	}
	MonsterGoal_reset(goal);
}

/* 0x00004d40: slide the sprite towards the monster's destination; returns 0
 * when it was already there */
int Entity_aiStepLerp(Entity *ent)
{
	Sprite *spr = entSprite(ent);
	EntityMonster *mon = ent->monster;
	int dx = mon->destX;
	int dy = mon->destY;

	if (spr->x == dx && spr->y == dy)
		return 0;

	int step = ent->app->canvas->moveStep;
	if (ent->def->eSubType == 1 && (mon->goal.flags & 2))
		step <<= 1; /* charging */

	if (spr->x < dx) {
		spr->x += step;
		if (spr->x > dx)
			spr->x = dx;
	} else if (spr->x > dx) {
		spr->x -= step;
		if (spr->x < dx)
			spr->x = dx;
	}
	if (spr->y < dy) {
		spr->y += step;
		if (spr->y > dy)
			spr->y = dy;
	} else if (spr->y > dy) {
		spr->y -= step;
		if (spr->y < dy)
			spr->y = dy;
	}
	Render_relinkSpriteAt(ent->app->render, spr, -1, -1);

	if (spr->x == dx && spr->y == dy) {
		ent->info &= ~0x400000u;
		if (ent->monster->flags & 0x40)
			ent->monster->flags &= ~0x40; /* knocked back: no goal step */
		else
			Entity_aiReachedGoal_MOVE(ent, &ent->monster->goal);
	}
	return 1;
}

/* 0x00004e1c */
int Entity_aiIsAttackValid(Entity *ent)
{
	EntityMonster *mon = ent->monster;
	Game *game = ent->app->game;

	if (mon->ce.weapon == 0x15 || mon->ce.weapon == 0x26) {
		/* raise dead: the corpse's tile must still be free */
		EntityMonster *corpse = game->entities[mon->goal.param].monster;
		if (Game_findEntityAt(game, corpse->destX, corpse->destY, MASK_BLOCKERS) != NULL)
			return 0;
		ent->app->game->entities[ent->monster->goal.param].info &= ~0x2000000u;
	} else {
		Canvas *cv = ent->app->canvas;
		Game_trace(game, mon->destX, mon->destY, cv->destX, cv->destY, ent, MASK_ATTACK_RAISE);
		if (ent->app->game->traceEntity != NULL)
			return 0;
	}
	return 1;
}

/* 0x00004ec4 */
int Entity_aiIsValidGoal(Entity *ent, MonsterGoal *goal)
{
	if (goal->turns > 0x1d)
		return 0;
	if (!Player_isVisible(ent->app->player) && !Entity_isBoss(ent))
		return 0;

	switch (goal->type) {
	case 1:
		return goal->x + goal->y * 32 != ent->linkIndex;
	case 2: {
		int idx = goal->param;
		Game *game = ent->app->game;
		if (idx == 1)
			return 0;
		Entity *target = &game->entities[idx];
		int ex, ey;
		linkTile(target, &ex, &ey);
		Entity *occ = Game_findEntityAt(game, ex * 64, ey << 6, MASK_BLOCKERS);
		if (target->linkIndex != ent->linkIndex && (occ == NULL || occ == target)) {
			int dist = Combat_worldDistToTileDist(Entity_distSqFrom(target, ent->monster->destX, ent->monster->destY));
			if (goal->turns <= dist)
				return 1;
		}
		return 0;
	}
	case 4:
	case 5:
		return (int16_t)goal->turns < goal->param;
	case 6:
	case 8:
	case 10:
	case 0xd:
		return 1;
	case 9:
		if ((int16_t)goal->turns < goal->param)
			return 1;
		if ((uint16_t)goal->turns != (uint16_t)goal->param)
			return 0;
		MonsterGoal_reset(goal);
		return 1;
	case 0xb:
		return 1;
	}
	return 0;
}

/* 0x00004fb4 */
void Entity_aiDoGoal(Entity *ent, MonsterGoal *goal)
{
	switch (goal->type) {
	case 1:
	case 2:
	case 4:
	case 5:
	case 0xe:
		Entity_aiGoal_MOVE(ent, goal);
		break;
	case 3:
		ent->monster->target = goal->param == 1 ? NULL : &ent->app->game->entities[goal->param];
		Entity_attack(ent);
		break;
	case 6:
		Entity_aiGoal_SLAM(ent, goal);
		break;
	case 8:
		Entity_aiGoal_TELEPORT(ent, goal);
		break;
	case 10:
		Entity_aiGoal_10(ent, goal);
		break;
	case 0xb:
		Entity_aiGoal_HEAL(ent, goal);
		break;
	case 0xd:
		Entity_aiGoal_DISAPPEAR(ent, goal);
		break;
	}
}

/* 0x0000502c: the sprite arrived at the monster's destination tile */
void Entity_aiReachedGoal_MOVE(Entity *ent, MonsterGoal *goal)
{
	char buf[64];

	if ((goal->flags & 2) && ent->def->eSubType == 1) {
		Entity_aiReachedGoal_CHARGE(ent, goal);
		return;
	}

	Render *render = ent->app->render;
	Sprite *spr = entSprite(ent);
	ent->monster->destX = spr->x;
	ent->monster->ce.x = spr->x;
	ent->monster->destY = spr->y;
	ent->monster->ce.y = spr->y;
	ent->info &= ~0x400000u;
	/* toggle the walk frame (0 <-> 1) */
	spr->info = (spr->info & 0xfffff0ffu) | (((spr->info & 0xf00) == 0) ? 0x100u : 0u);
	/* The original adds the low byte of the Entity's address, times 8, to the
	 * idle-animation time. Use the address the entity had in the original
	 * Applet layout (relative to a 256-aligned Applet). */
	Entity *owner = spr->ent;
	uint32_t origAddr = (uint32_t)(ORIG_ENTITIES_OFS + entityIndex(ent->app->game, owner) * 0x24);
	owner->monster->frameTime = ent->app->tickTime + (int)(origAddr & 0xff) * 8 + 4000;

	if (render->hubSprite == spr)
		ent->app->canvas->hubEventTime = Random_nextInt(&ent->app->random) % 2500 + ent->app->tickTime + 500;

	int weapon;
	if ((goal->flags & 4) && (weapon = Entity_aiWeaponForTarget(ent, &ent->app->game->entities[1])) != -1) {
		ent->monster->target = NULL;
		ent->monster->ce.weapon = (uint8_t)weapon;
		Entity_attack(ent);
		return;
	}
	if (goal->flags & 0x10) {
		goal->flags &= ~0x10;
		Entity_aiCalcSimpleGoal(ent, goal);
		if (goal->type == 1 || goal->type == 2) {
			if (!Game_isMonsterBetween(ent->app->game, goal->x, goal->y))
				Entity_aiGoal_MOVE(ent, goal);
			else
				MonsterGoal_reset(goal);
		}
	}
	if ((goal->flags & 2) && ent->def->eSubType == 0xf && spr->x >> 6 == goal->x && spr->y >> 6 == goal->y) {
		/* healer boss reached its spot: start Major Heal */
		MonsterGoal_reset(goal);
		goal->type = 0xb;
		goal->param = 6;
		App_snprintf(buf, sizeof buf, (char *)ModData_str(&g_mod, MOD_STR_READIES_HEAL), ent->name);
		Hud_addMessage(ent->app->hud, buf, 3);
		setSpriteFrame(spr, 4);
	}
}

/* 0x000051c8: a charging monster arrived on a tile */
void Entity_aiReachedGoal_CHARGE(Entity *ent, MonsterGoal *goal)
{
	Sprite *spr = entSprite(ent);

	ent->monster->destX = spr->x;
	ent->monster->ce.x = spr->x;
	ent->monster->destY = spr->y;
	ent->monster->ce.y = spr->y;
	if (spr->x >> 6 == goal->x && spr->y >> 6 == goal->y) {
		ent->monster->ce.weapon = 0xe;
		ent->monster->target = NULL;
		Entity_attack(ent);
	} else if (Game_findEntityAt(ent->app->game, goal->x << 6, goal->y << 6, MASK_BLOCKERS) == NULL) {
		setSpriteFrame(spr, 2);
		spr->ent->monster->frameTime = ent->app->tickTime + 250;
		goal->param = 0;
		Entity_aiGoal_MOVE(ent, goal);
	}
}

/* 0x0000526c */
void Entity_aiThink(Entity *ent)
{
	if (ent->monster->flags & 4)
		return;
	if (ent->info & 0x10000)
		ent->info &= ~0x10000u;
	if (Entity_aiIsValidGoal(ent, &ent->monster->goal) || Entity_aiChooseNewGoal(ent, &ent->monster->goal)) {
		ent->monster->goal.turns++;
		Entity_aiDoGoal(ent, &ent->monster->goal);
	}
}

/* 0x000052bc: which of the monster's two attacks can hit `target` now; -1 if none */
int Entity_aiWeaponForTarget(Entity *ent, Entity *target)
{
	Sprite *spr = entSprite(ent);
	Combat *combat = ent->app->combat;
	int w0 = Combat_getMonsterAttackField(ent->app->combat, ent->def, 0);
	int w1 = Combat_getMonsterAttackField(ent->app->combat, ent->def, 1);
	int chance = Combat_getMonsterAttackField(ent->app->combat, ent->def, 2);
	int tx, ty;
	linkTile(target, &tx, &ty);
	int dist = Combat_worldDistToTileDist(Entity_distSqFrom(target, spr->x, spr->y));
	int weapon = w0;

	if (w0 != w1) {
		Weapon *first = NULL;
		combat = ent->app->combat;
		if (w0 != 0 && dist >= combat->weapons[w0].rangeMin && dist <= combat->weapons[w0].rangeMax)
			first = &combat->weapons[w0];
		combat = ent->app->combat;
		if (w1 != 0 && dist >= combat->weapons[w1].rangeMin && dist <= combat->weapons[w1].rangeMax) {
			if (first == NULL || Random_nextByte(&ent->app->random) > chance)
				weapon = w1;
		} else if (first == NULL) {
			return -1;
		}
	}
	if (ent->def->eSubType == 1 && (ent->monster->flags & 8))
		weapon = w1;

	Applet *app = ent->app;
	if (dist < app->combat->weapons[weapon].rangeMin || dist > app->combat->weapons[weapon].rangeMax)
		return -1;
	if (spr->x >> 6 != tx && spr->y >> 6 != ty)
		return -1;
	int gx, gy;
	if (target->def->eType == 1) {
		gx = app->canvas->destX;
		gy = app->canvas->destY;
	} else {
		Sprite *ts = Render_spriteAt(app->render, (target->info & 0xffff));
		gx = ts->x;
		gy = ts->y;
	}
	if (spr->x != gx && spr->y != gy)
		return -1;
	Game_trace(app->game, spr->x, spr->y, gx, gy, ent, MASK_ATTACK);
	if (ent->app->game->traceEntity != target)
		return -1;
	return weapon;
}

/* 0x000056ac: queue the monster on the combat list (subtype 10 attacks last) */
void Entity_attack(Entity *ent)
{
	if (ent->info & 0x10000)
		return;
	ent->info |= 0x10000;
	if (ent->def->eSubType != 10) {
		ent->monster->nextAttacker = ent->app->game->combatMonsters;
		ent->app->game->combatMonsters = ent;
		return;
	}
	Game *game = ent->app->game;
	Entity *tail = NULL;
	for (Entity *e = game->combatMonsters; e != NULL; e = e->monster->nextAttacker)
		tail = e;
	if (tail == NULL)
		game->combatMonsters = ent;
	else
		tail->monster->nextAttacker = ent;
	ent->monster->nextAttacker = NULL;
}

/* 0x00006244: the 4 step directions to try when fleeing from (tx,ty); the
 * first two lead away, randomised. out = 4 x {dx, dy}; returns 4. */
int Entity_aiStepDirs(Entity *ent, int x, int y, int tx, int ty, int *out)
{
	Random *rnd = &ent->app->random;
	int dx = x - tx;
	int dy = y - ty;

	if (x != tx && y != ty) {
		if (Random_nextByte(rnd) & 1) {
			out[0] = dx < 0 ? -1 : 1;
			out[1] = 0;
			out[2] = 0;
			out[3] = dy < 0 ? -1 : 1;
		} else {
			out[0] = 0;
			out[1] = dy < 0 ? -1 : 1;
			out[2] = dx < 0 ? -1 : 1;
			out[3] = 0;
		}
		if (Random_nextByte(&ent->app->random) & 1) {
			out[4] = dx < 0 ? 1 : -1;
			out[5] = 0;
			out[6] = 0;
			out[7] = dy < 0 ? 1 : -1;
		} else {
			out[4] = 0;
			out[5] = dy < 0 ? 1 : -1;
			out[6] = dx < 0 ? 1 : -1;
			out[7] = 0;
		}
	} else if (x != tx) {
		out[0] = 0;
		if (Random_nextByte(rnd) & 1) {
			out[1] = -1;
			out[2] = 0;
			out[3] = 1;
		} else {
			out[1] = 1;
			out[2] = 0;
			out[3] = -1;
		}
		if (Random_nextByte(&ent->app->random) & 1) {
			out[4] = -1;
			out[5] = 0;
			out[6] = 1;
		} else {
			out[4] = 1;
			out[5] = 0;
			out[6] = -1;
		}
		out[7] = 0;
	} else if (y != ty) {
		if (Random_nextByte(rnd) & 1) {
			out[0] = 1;
			out[1] = 0;
			out[2] = -1;
		} else {
			out[0] = -1;
			out[1] = 0;
			out[2] = 1;
		}
		out[3] = 0;
		out[4] = 0;
		if (Random_nextByte(&ent->app->random) & 1) {
			out[5] = -1;
			out[6] = 0;
			out[7] = 1;
		} else {
			out[5] = 1;
			out[6] = 0;
			out[7] = -1;
		}
	} else {
		switch (Random_nextByte(rnd) & 3) {
		case 0:
			out[0] = 0; out[1] = -1; out[2] = 1; out[3] = 0; out[4] = 0; out[5] = 1; out[6] = -1; out[7] = 0;
			break;
		case 1:
			out[0] = 1; out[1] = 0; out[2] = 0; out[3] = 1; out[4] = -1; out[5] = 0; out[6] = 0; out[7] = -1;
			break;
		case 2:
			out[0] = 0; out[1] = 1; out[2] = -1; out[3] = 0; out[4] = 0; out[5] = -1; out[6] = 1; out[7] = 0;
			break;
		default:
			out[0] = -1; out[1] = 0; out[2] = 0; out[3] = -1; out[4] = 1; out[5] = 0; out[6] = 0; out[7] = 1;
			break;
		}
	}
	return 4;
}

/* 0x000063ec: a free tile next to world (x,y). The "farther from (fx,fy)"
 * comparison of the original is loop-invariant, so this returns the first
 * free one of the 4 directions. Returns 1 if found. */
int Entity_aiFindFreeAdjacent(Entity *ent, int x, int y, int fx, int fy, int mask, int *ox, int *oy)
{
	int dist = (x - fx) * (x - fx) + (y - fy) * (y - fy);
	int bestDist = 0;
	int best = -1;

	for (int i = 0; i < 4; i++) {
		if (bestDist < dist &&
		    Game_findEntityAt(ent->app->game, dirX(MOD_ADJACENT_DIRS, i) * 64 + x, dirY(MOD_ADJACENT_DIRS, i) * 64 + y,
		                      mask) == NULL) {
			best = i;
			bestDist = dist;
		}
	}
	if (best != -1) {
		*ox = dirX(MOD_ADJACENT_DIRS, best) * 64 + x;
		*oy = dirY(MOD_ADJACENT_DIRS, best) * 64 + y;
	}
	return best != -1;
}

/* 0x0000668c: choose up to 3 free tiles around the boss (starting with the
 * player's octant) for its dead/hidden minions; directions go in nibbles of
 * boss->param. No spot at all: the boss uses weapon 0x24 instead. */
void Entity_bossPickMinionSpots(Entity *ent)
{
	Canvas *cv = ent->app->canvas;
	int dx = cv->destX - ent->monster->destX;
	int dy = cv->destY - ent->monster->destY;
	int oct;

	if (dx > 0 && dy < 0)
		oct = 5; /* the original maps both mixed-sign diagonals to 5 */
	else if (dx > 0 && dy > 0)
		oct = 7;
	else if (dx < 0 && dy < 0)
		oct = 3;
	else if (dx < 0 && dy > 0)
		oct = 5;
	else if (dx < 0)
		oct = 4;
	else if (dx > 0)
		oct = 0;
	else if (dy < 0)
		oct = 6;
	else
		oct = 2;

	ent->param = 0;
	int placed = 0;
	for (int k = 0; k < 8; k++) {
		int dir;
		if (k == 1) {
			dir = (oct - 1) % 8;
			if (dir < 0)
				dir = 7;
		} else if (k == 0) {
			dir = oct % 8;
		} else {
			dir = (oct + k - 1) % 8;
		}
		if (Game_findEntityAt(ent->app->game, ent->monster->destX + dirX(MOD_MINION_DIRS, dir) * 64,
		                      ent->monster->destY + dirY(MOD_MINION_DIRS, dir) * 64, MASK_BLOCKERS) != NULL)
			continue;
		int slot;
		for (slot = 0; slot < 3; slot++) {
			Entity *minion = &ent[slot + 1];
			Sprite *ms = Render_spriteAt(ent->app->render, (minion->info & 0xffff));
			if ((ent->param & (0xf << (slot * 4))) == 0 && ((ms->info & 0x10000) || (minion->info & 0x40000000)))
				break;
		}
		if (slot == 3)
			break;
		ent->param |= (dir + 1) << (slot * 4);
		placed++;
	}
	if (placed == 0)
		ent->monster->ce.weapon = 0x24;
}

/* 0x00006810: one path step from world (fx,fy) towards (tx,ty) */
int Entity_aiMoveStep(Entity *ent, int fx, int fy, int tx, int ty)
{
	int mask = ent->def->eSubType == 0xf ? MASK_BLOCKERS_F : MASK_BLOCKERS;
	int step[2];

	if (!Game_findPathStep(ent, fx >> 6, fy >> 6, tx >> 6, ty >> 6, mask, step))
		return 0;

	ent->monster->destX = step[0] * 64 + fx;
	ent->monster->destY = step[1] * 64 + fy;
	ent->info &= ~0x400000u;
	Game_unlinkEntity_2ae8c(ent->app->game, ent);
	Game_linkEntity(ent->app->game, ent, ent->monster->destX >> 6, ent->monster->destY >> 6);
	Canvas *cv = ent->app->canvas;
	int distSq = Entity_distSqFrom(ent, cv->viewX, cv->viewY);
	if ((step[0] != 0 || step[1] != 0) && distSq < 0x10000) {
		int fromWall = Render_tileTouchesWall(ent->app->render, fx >> 6, fy >> 6);
		int toWall = Render_tileTouchesWall(ent->app->render, ent->monster->destX >> 6, ent->monster->destY >> 6);
		if (toWall || fromWall)
			ent->info |= 0x400000;
	}
	return 1;
}

/* 0x000089b4: place == 0 shows the summoning effect on the chosen spots;
 * otherwise the minions rise there */
void Entity_bossPlaceMinions(Entity *boss, int place)
{
	for (int slot = 0; slot < 3; slot++) {
		int nib = (boss->param >> (slot * 4)) & 0xf;
		if (nib == 0)
			continue;
		int x = boss->monster->destX + dirX(MOD_MINION_DIRS, nib - 1) * 64;
		int y = boss->monster->destY + dirY(MOD_MINION_DIRS, nib - 1) * 64;
		if (place == 0) {
			GameSprite *gs = Game_gspriteAllocAnim(boss->app->game, 0xfd, x, y, 0x20);
			if (gs != NULL) { /* the original does not check */
				int t = gs->lifetime * 3 + 250;
				gs->lifetime = t;
				gs->lerpDuration = t;
			}
		} else {
			Entity *minion = &boss[slot + 1];
			Entity_resurrect(minion, x, y, -0x20);
			minion->param = Random_nextInt(&boss->app->random) % 500 + boss->app->tickTime;
			minion->monster->flags |= 0x180;
			Game_activateEx(boss->app->game, minion, 0, 0);
		}
	}
	if (place != 0)
		Canvas_updateFacing(boss->app->canvas);
}

/* 0x00014804: one of the two hub lairs at random */
void Wererat_pickRandomLair(void *entity, char *outIdx, int *tx, int *ty)
{
	Entity *ent = entity;
	int lair = Random_nextByte(&ent->app->random) & 1;
	*outIdx = (char)lair;
	*tx = ModData_u8(&g_mod, MOD_WERERAT_LAIRS + lair * 5);
	*ty = ModData_u8(&g_mod, MOD_WERERAT_LAIRS + lair * 5 + 1);
}

/* 0x00014838: the hub lair nearest to the entity */
void Wererat_findNearestLair(void *entity, int *tx, int *ty)
{
	int best = -1;
	int bestDist = 0x7fffffff;

	for (int i = 0; i < 2; i++) {
		int d = Entity_distSqFrom(entity, ModData_u8(&g_mod, MOD_WERERAT_LAIRS + i * 5) * 64 + 32,
		                          ModData_u8(&g_mod, MOD_WERERAT_LAIRS + i * 5 + 1) * 64 + 32);
		if (d < bestDist) {
			bestDist = d;
			best = i;
		}
	}
	*tx = ModData_u8(&g_mod, MOD_WERERAT_LAIRS + best * 5);
	*ty = ModData_u8(&g_mod, MOD_WERERAT_LAIRS + best * 5 + 1);
}

/* 0x00014894: the hub wererat (goal.param = state: 0 roam, 1 flee to a lair,
 * 2 respawn, 3 hidden until Canvas.hubEventTime). goal.x/y = target tile. */
void Entity_updateWererat(Entity *entity, int moving)
{
	EntityMonster *mon = entity->monster;
	int state = mon->goal.param;
	int tx = mon->goal.x;
	int ty = mon->goal.y;
	Applet *app = entity->app;
	Render *render = app->render;
	Sprite *spr = Render_spriteAt(render, (entity->info & 0xffff));

	if (!(entity->info & 0x40000) || (entity->info & 0x40000000))
		return;

	if (state == 3) {
		if (app->tickTime < app->canvas->hubEventTime)
			return;
		state = 2;
	}
	if (state == 2) {
		int lx, ly;
		Wererat_pickRandomLair(entity, (char *)&entity->app->canvas->hubEventState, &lx, &ly);
		spr->info &= ~0x10000u;
		spr->x = lx * 64 + 32;
		entity->monster->destX = spr->x;
		spr->y = ly * 64 + 32;
		entity->monster->destY = spr->y;
		entity->info &= 0xdfbfffffu;
		Render_relinkSpriteAt(entity->app->render, spr, -1, -1);
		Game_unlinkEntity_2ae8c(entity->app->game, entity);
		Game_linkEntity(entity->app->game, entity, tx, ty); /* the old target tile, as in the original */
		entity->monster->goal.param = 0;
		entity->monster->goal.x = (uint8_t)lx;
		entity->monster->goal.y = (uint8_t)ly;
		Game_activateEx(entity->app->game, entity, 1, 0);
		return;
	}

	if (moving && (spr->x != mon->destX || spr->y != mon->destY))
		return;
	if (Game_snapMonsterSprite(entity))
		return;

	Canvas *cv = entity->app->canvas;
	int dx = spr->x - cv->destX;
	int dy = spr->y - cv->destY;
	if ((dx * dx + dy * dy <= 0x9000 || (entity->info & 0x20000000)) && state != 1) {
		state = 1;
		Wererat_findNearestLair(entity, &tx, &ty);
	}

	if (spr->x >> 6 == tx && spr->y >> 6 == ty) {
		if (state == 1) {
			/* reached the lair: hide for 30..179 s */
			state = 3;
			spr->info |= 0x10000;
			Game_unlinkEntity_2ae8c(entity->app->game, entity);
			entity->app->canvas->hubEventTime =
			    (Random_nextInt(&entity->app->random) % 150) * 1000 + entity->app->tickTime + 30000;
			goto done;
		}
		if (state != 0) {
			App_setErrorCode(g_app, -0x29a);
			App_setErrorStr(entity->app, (char *)ModData_str(&g_mod, MOD_STR_WERERAT_STATE), state);
			return;
		}
		/* roaming: a random tile inside one of this lair's 3 target boxes */
		int pick = Random_nextByte(&entity->app->random) % 3;
		uint32_t box = MOD_WERERAT_DESTS +
		               ModData_u8(&g_mod, MOD_WERERAT_LAIRS + 2 + pick + entity->app->canvas->hubEventState * 5) * 4;
		int x0 = ModData_u8(&g_mod, box), y0 = ModData_u8(&g_mod, box + 1);
		int x1 = ModData_u8(&g_mod, box + 2), y1 = ModData_u8(&g_mod, box + 3);
		tx = x0;
		if (x1 != x0)
			tx += Random_nextByte(&entity->app->random) % (x1 - x0);
		ty = y0;
		if (y1 != y0)
			ty += Random_nextByte(&entity->app->random) % (y1 - y0);
	} else if (state == 3) {
		goto done;
	}

	if (Entity_aiMoveStep(entity, spr->x, spr->y, tx * 64 + 32, ty * 64 + 32))
		entity->app->game->monstersMoving = 1;
done:
	entity->monster->goal.x = (uint8_t)tx;
	entity->monster->goal.y = (uint8_t)ty;
	entity->monster->goal.param = (int16_t)state;
}
