/*
 * Entity.cpp (AI part): monster goals, path steps, movement interpolation and
 * attack choice. Same scheme as Doom II RPG: each monster keeps a MonsterGoal
 * {type, flags, turns, x, y, param} that is re-validated every turn.
 *
 * Goal types: 1 move to tile (x,y), 2 move to entity (param = index, 1 = the
 * player), 3 attack (param 1 = player), 4 flee / 5 evade for param turns,
 * 6 hold position (alert), 9 wait param turns, 10 raise a corpse.
 * Goal flags: 2 running, 4 attack when in reach after moving, 8 evade after
 * attacking, 0x10 re-plan after the move, 0x20 never moves, 0x80 stands guard.
 */
#include "game.h"

#define MOD_TILE_DIST_TABLE 0x45ebcu /* 15 x s32: squared world distance of k tiles */

/* Trace masks (bit per EntityDef.eType) */
#define MASK_MONSTER_MOVE  0x7daf
#define MASK_ATTACK_LOS    0x54af
#define MASK_ATTACK_REACH  0x74af

static Sprite *entSprite(Entity *e) { return &g_app->render->sprites[(e->info & 0xffff) - 1]; }
static Line *entLine(Entity *e) { return &g_app->render->lines[(e->info & 0xffff) - 1]; }
static void setSpriteFrame(Sprite *s, int frame) { s->info = (s->info & ~0xf00u) | ((uint32_t)frame << 8); }
static int tileDistSq(int tiles) { return ModData_s32(&g_mod, MOD_TILE_DIST_TABLE + (tiles - 1) * 4); }

/* 0x00007014: weapon (one of the def's two attacks) usable against target
 * from the monster's current tile, or -1 */
int Entity_aiWeaponForTarget(Entity *self, Entity *target)
{
	Applet *app = g_app;
	Combat *combat = app->combat;
	int w0 = Combat_getMonsterAttackField(combat, self->def, 0);
	int w1 = Combat_getMonsterAttackField(combat, self->def, 1);
	int chance = Combat_getMonsterAttackField(combat, self->def, 2);
	int tx = target->linkIndex % 32;
	int ty = target->linkIndex / 32;
	EntityMonster *mon = self->monster;
	int dist = Combat_WorldDistToTileDist(combat, Entity_distSqFrom(target, mon->ce.x, mon->ce.y));

	if (w0 != w1) {
		int ok0 = w0 != 0 && combat->weapons[w0].rangeMin <= dist && dist <= combat->weapons[w0].rangeMax;
		int ok1 = w1 != 0 && combat->weapons[w1].rangeMin <= dist && dist <= combat->weapons[w1].rangeMax;
		if (ok1) {
			/* both in range: the second attack is used when the roll beats the chance */
			if (!ok0 || (Random_nextInt(&app->random) & 0xff) > chance)
				w0 = w1;
		} else if (!ok0) {
			return -1;
		}
	}
	if (self->def->eSubType == 1 && (mon->flags & 8))
		w0 = w1;
	if (dist < combat->weapons[w0].rangeMin || dist > combat->weapons[w0].rangeMax)
		return -1;

	/* must share a row or column with the target */
	if (tx != mon->ce.x >> 6 && ty != mon->ce.y >> 6)
		return -1;
	int px, py;
	if (target->def->eType == 1) {
		px = app->game->destX;
		py = app->game->destY;
	} else {
		Sprite *spr = entSprite(target);
		px = spr->x;
		py = spr->y;
	}
	if (mon->ce.x != px && mon->ce.y != py)
		return -1;

	int sub = self->def->eSubType;
	int reach = (sub == 10 && w0 == 0x22) || (sub == 1 && w0 == 0xe); /* walk-in melee */
	Game_trace(app->game, mon->ce.x, mon->ce.y, px, py, self, reach ? MASK_ATTACK_REACH : MASK_ATTACK_LOS);
	if (app->game->traceEntity != target)
		return -1;
	if (!reach)
		return w0;

	/* the tile next to the target on our side must be free */
	int d = self->monster->ce.x - px;
	if (d > 0) {
		px += 0x40;
	} else if (d < 0) {
		px -= 0x40;
	} else {
		d = self->monster->ce.y - py;
		if (d > 0)
			py += 0x40;
		else if (d < 0)
			py -= 0x40;
	}
	if (Game_findEntityAt(app->game, px, py, 0x100) != NULL)
		return -1;
	return w0;
}

/* 0x00005fd0: attack the player if possible, else walk towards him */
void Entity_aiCalcSimpleGoal(Entity *ent, MonsterGoal *goal)
{
	Applet *app = g_app;
	Game *game = app->game;
	Sprite *spr = entSprite(ent);
	Entity *hit;
	int w = Entity_aiWeaponForTarget(ent, &game->entities[1]);
	int sx = spr->x >> 6, sy = spr->y >> 6;
	int plink = game->entities[1].linkIndex;
	int py = plink / 32;
	int px = plink - py * 32;
	int aligned = sx == px || sy == py;

	if (w == -1) {
		/* ranged monsters: a destructible barrel in the way is a target too */
		int sub = ent->def->eSubType;
		if ((sub == 0 || sub == 6 || sub == 10) && aligned) {
			int x0 = ent->monster->ce.x, y0 = ent->monster->ce.y;
			int x1 = x0, y1 = y0;
			if (sx == px)
				y1 = y0 + (py - sy > 0 ? 0x40 : -0x40);
			else
				x1 = x0 + (px - sx > 0 ? 0x40 : -0x40);
			Game_trace(app->game, x0, y0, x1, y1, ent, MASK_ATTACK_LOS);
			hit = app->game->traceEntity;
			if (hit && hit->def->eType == 10 && hit->def->eSubType == 2) {
				int bw = Entity_aiWeaponForTarget(ent, hit);
				if (bw != -1) {
					ent->monster->ce.weapon = bw;
					goto attack;
				}
			}
		}
	} else {
		int dist = Combat_WorldDistToTileDist(app->combat, Entity_distSqFrom(ent, game->destX, game->destY));
		WeaponInfo *wp = &app->combat->weapons[w];
		ent->monster->ce.weapon = w;
		if (aligned && dist <= wp->rangeMax && wp->rangeMin <= dist) {
			game = app->game;
			Game_trace(game, ent->monster->ce.x, ent->monster->ce.y, game->destX, game->destY, ent, MASK_ATTACK_LOS);
			hit = app->game->traceEntity;
			if (hit) {
				int t = hit->def->eType;
				if ((t == 10 && hit->def->eSubType == 2) || t == 1)
					goto attack;
			}
		}
	}
	goal->type = 2;
	goal->param = 1;
	return;

attack:
	goal->type = 3;
	goal->param = 1;
	goal->param = Game_getEntityIndex(hit);
}

/* 0x00005f48: necromancers raise a nearby corpse when they can */
void Entity_aiCalcSimpleGoalRaise(Entity *ent, MonsterGoal *goal)
{
	if (ent->def->parm > 0) {
		int corpse = Game_pickNearbyMonster(ent, tileDistSq(4), 0, 0x100);
		if (corpse != -1) {
			ent->monster->ce.weapon = 0x13;
			Entity_attack(ent);
			goal->type = 10;
			goal->param = (int16_t)corpse;
			return;
		}
	}
	Entity_aiCalcSimpleGoal(ent, goal);
}

/* 0x00006238: subtype 6 stands guard; with its grab attack it holds when close */
void Entity_aiCalcGoalSubtype6(Entity *ent, MonsterGoal *goal)
{
	Applet *app = g_app;

	MonsterGoal_reset(goal);
	goal->flags |= 0x80;
	int w = Entity_aiWeaponForTarget(ent, &app->game->entities[1]);
	if (w == 0x17) {
		if (Entity_distSqFrom(ent, app->game->destX, app->game->destY) > tileDistSq(2)) {
			goal->type = 2;
			goal->param = 1;
		} else {
			goal->type = 6;
		}
	} else {
		Entity_aiCalcSimpleGoal(ent, goal);
		if (goal->type == 3 && goal->param != 1)
			return;
	}
	ent->monster->ce.weapon = w;
}

/* 0x00006314 */
int Entity_aiChooseNewGoal(Entity *ent, MonsterGoal *goal)
{
	MonsterGoal_reset(goal);
	switch (ent->def->eSubType) {
	case 1:
		goal->flags |= 0x20;
		Entity_aiCalcSimpleGoal(ent, goal);
		break;
	case 2:
	case 9:
	case 13:
	case 14:
		Entity_aiCalcSimpleGoal(ent, goal);
		goal->flags |= 4;
		break;
	case 4:
		Entity_aiCalcSimpleGoalRaise(ent, goal);
		break;
	case 5:
		Entity_aiCalcSimpleGoal(ent, goal);
		goal->flags |= 0x14;
		break;
	case 6:
		Entity_aiCalcGoalSubtype6(ent, goal);
		break;
	case 7:
	case 12:
		Entity_aiCalcSimpleGoal(ent, goal);
		if (!(ent->monster->flags & 8) && goal->type == 3)
			goal->flags |= 8;
		goal->flags |= 4;
		break;
	default:
		Entity_aiCalcSimpleGoal(ent, goal);
		break;
	}
	return 1;
}

/* 0x00006420: goal 6, the alert pose; attack once the player stands in line */
void Entity_aiCheckNotice(Entity *ent, MonsterGoal *goal)
{
	Applet *app = g_app;
	Sprite *spr = entSprite(ent);

	if (goal->turns == 1) {
		setSpriteFrame(spr, 4);
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgStr(app->canvas, &ent->name);
		Hud_addMessage(app->hud, 0x9f);
		return;
	}

	int seen = 0;
	Game *game = app->game;
	if ((spr->x == game->destX || spr->y == game->destY) &&
	    Entity_distSqFrom(ent, game->destX, game->destY) <= tileDistSq(2)) {
		game = app->game;
		Game_trace(game, spr->x, spr->y, game->destX, game->destY, ent, MASK_ATTACK_LOS);
		Entity *hit = app->game->traceEntity;
		if (hit && hit->def->eType == 1)
			seen = 1;
	}
	MonsterGoal_reset(goal);
	if (!seen) {
		setSpriteFrame(spr, 0);
		return;
	}
	spr->ent->monster->frameTime = app->time + 1000;
	ent->monster->target = NULL;
	Entity_attack(ent);
}

/* 0x00008b90: the 4 step directions to try when fleeing from (tx,ty); the
 * first two lead away, randomised. outDirs = 4 x {dx, dy}; returns 4. */
int Entity_aiStepDirs(Entity *self, int x, int y, int tx, int ty, int *outDirs)
{
	Random *rnd = &g_app->random;
	int dx = x - tx;
	int dy = y - ty;
	int *o = outDirs;

	if (dx != 0 && dy != 0) {
		if (Random_nextInt(rnd) & 1) {
			o[0] = dx >= 0 ? 1 : -1;
			o[1] = 0;
			o[2] = 0;
			o[3] = dy >= 0 ? 1 : -1;
		} else {
			o[0] = 0;
			o[1] = dy >= 0 ? 1 : -1;
			o[2] = dx >= 0 ? 1 : -1;
			o[3] = 0;
		}
		if (Random_nextInt(rnd) & 1) {
			o[4] = dx >= 0 ? -1 : 1;
			o[5] = 0;
			o[6] = 0;
			o[7] = dy >= 0 ? -1 : 1;
		} else {
			o[4] = 0;
			o[5] = dy >= 0 ? -1 : 1;
			o[6] = dx >= 0 ? -1 : 1;
			o[7] = 0;
		}
	} else if (dx != 0) {
		int r = Random_nextInt(rnd) & 1;
		o[0] = 0;
		o[1] = r ? -1 : 1;
		o[2] = 0;
		o[3] = r ? 1 : -1;
		if (Random_nextInt(rnd) & 1) {
			o[4] = -1;
			o[6] = 1;
		} else {
			o[4] = 1;
			o[6] = -1;
		}
		o[5] = 0;
		o[7] = 0;
	} else if (dy != 0) {
		int r = Random_nextInt(rnd) & 1;
		o[0] = r ? 1 : -1;
		o[1] = 0;
		o[2] = r ? -1 : 1;
		o[3] = 0;
		o[4] = 0;
		o[6] = 0;
		if (Random_nextInt(rnd) & 1) {
			o[5] = -1;
			o[7] = 1;
		} else {
			o[5] = 1;
			o[7] = -1;
		}
	} else {
		switch (Random_nextInt(rnd) & 3) {
		case 0:
			o[0] = 0; o[1] = -1; o[2] = 1; o[3] = 0; o[4] = 0; o[5] = 1; o[6] = -1; o[7] = 0;
			break;
		case 1:
			o[0] = 1; o[1] = 0; o[2] = 0; o[3] = 1; o[4] = -1; o[5] = 0; o[6] = 0; o[7] = -1;
			break;
		case 2:
			o[0] = 0; o[1] = 1; o[2] = -1; o[3] = 0; o[4] = 0; o[5] = -1; o[6] = 1; o[7] = 0;
			break;
		default:
			o[0] = -1; o[1] = 0; o[2] = 0; o[3] = -1; o[4] = 1; o[5] = 0; o[6] = 0; o[7] = 1;
			break;
		}
	}
	return 4;
}

/* 0x00009238: one path step from world (fx,fy) towards (tx,ty) */
int Entity_aiMoveStep(Entity *self, int fx, int fy, int tx, int ty)
{
	Applet *app = g_app;
	int step[2];

	if (!Game_findPathStep(self, fx >> 6, fy >> 6, tx >> 6, ty >> 6, MASK_MONSTER_MOVE, step))
		return 0;

	EntityMonster *mon = self->monster;
	mon->destX = fx + step[0] * 64;
	mon->destY = fy + step[1] * 64;
	self->info &= ~0x400000u;
	Game_unlinkEntity_43710(app->game, self);
	Game_linkEntity(app->game, self, self->monster->destX >> 6, self->monster->destY >> 6);
	int distSq = Entity_distSqFrom(self, app->game->viewX, app->game->viewY);
	if ((step[0] != 0 || step[1] != 0) && distSq < tileDistSq(4)) {
		/* close to the player next to a wall/door: animate the move */
		int fromWall = Render_tileTouchesWall(app->render, fx >> 6, fy >> 6);
		int toWall = Render_tileTouchesWall(app->render, self->monster->destX >> 6, self->monster->destY >> 6);
		if (fromWall || toWall)
			self->info |= 0x400000;
	}
	return 1;
}

/* 0x0000658c: pick the next tile for a movement goal and start moving */
void Entity_aiGoal_MOVE(Entity *ent, MonsterGoal *goal)
{
	Applet *app = g_app;
	EntityMonster *mon = ent->monster;
	int x, y;

	if (mon->flags & 0x20) {
		MonsterGoal_reset(goal);
		return;
	}
	x = mon->destX;
	y = mon->destY;

	if (goal->type == 2 && goal->param == 1) {
		/* to the player: one of the 4 tiles around him, in a random order */
		Game *game = app->game;
		int around[8] = { game->viewStepX, game->viewStepY, -game->viewStepX, -game->viewStepY,
		                  game->viewRightStepX, game->viewRightStepY, -game->viewRightStepX,
		                  -game->viewRightStepY };
		int order[4];
		if (goal->flags & 0x20) {
			order[0] = 1;
			if (Random_nextInt(&app->random) & 1) {
				order[2] = 3;
				order[1] = 2;
			} else {
				order[1] = 3;
				order[2] = 2;
			}
			order[3] = 0;
		} else if (goal->flags & 0x80) {
			order[0] = 0;
			if (Random_nextInt(&app->random) & 1) {
				order[2] = 3;
				order[1] = 2;
			} else {
				order[1] = 3;
				order[2] = 2;
			}
			order[3] = 1;
		} else {
			switch (Random_nextInt(&app->random) % 4) {
			case 0: order[0] = 2; order[1] = 3; break;
			case 1: order[0] = 3; order[1] = 2; break;
			default: order[0] = 0; order[1] = 2; order[2] = 3; order[3] = 1; break;
			}
			if (order[0] != 0) {
				order[2] = 0;
				order[3] = 1;
			}
		}

		int i;
		for (i = 0; i < 4; i++) {
			game = app->game;
			x = game->destX + around[order[i] * 2];
			y = game->destY + around[order[i] * 2 + 1];
			Entity *occ = Game_findEntityAt(game, x, y, MASK_MONSTER_MOVE);
			if (occ == NULL || occ == ent)
				break;
		}
		if (i == 4) {
			x = app->game->destX;
			y = app->game->destY;
		}
		goal->x = x >> 6;
		goal->y = y >> 6;
	} else if (goal->type == 4 || goal->type == 5) {
		/* flee / evade: first free step away from the player */
		int dirs[8];
		int n = Entity_aiStepDirs(ent, x >> 6, y >> 6, app->game->destX >> 6, app->game->destY >> 6, dirs);
		int i;
		for (i = 0; i < n; i++) {
			int fx = ent->monster->destX, fy = ent->monster->destY;
			x = fx + dirs[i * 2] * 64;
			y = fy + dirs[i * 2 + 1] * 64;
			if (Entity_checkLineOfSight(ent, fx >> 6, fy >> 6, x >> 6, y >> 6, MASK_MONSTER_MOVE))
				break;
		}
		if (i == n) {
			if (goal->type == 5)
				MonsterGoal_reset(goal);
			return;
		}
	} else if (goal->type == 2) {
		int li = app->game->entities[goal->param].linkIndex;
		x = (li % 32) * 64 + 32;
		y = (li / 32) * 64 + 32;
		goal->x = x >> 6;
		goal->y = y >> 6;
	} else {
		x = goal->x * 64 + 32;
		y = goal->y * 64 + 32;
	}

	Sprite *spr = entSprite(ent);
	Entity_aiMoveStep(ent, spr->x, spr->y, x, y);
	app->game->monstersMoving = 1;
	Entity *door = app->game->watchEntity;
	if (door)
		Game_useDoor(app->game, 0x10, Render_getLineIndex(entLine(door)), 1);
}

/* 0x00006e14: arrived on the step's tile */
void Entity_aiReachedGoal_MOVE(Entity *self, MonsterGoal *goal)
{
	Applet *app = g_app;
	Sprite *spr = entSprite(self);

	self->monster->destX = spr->x;
	self->monster->ce.x = spr->x;
	self->monster->destY = spr->y;
	self->monster->ce.y = spr->y;
	self->info &= ~0x400000u;

	int w = self->monster->ce.weapon;
	if (w == 0xe || w == 0x22)
		return;

	setSpriteFrame(spr, ((spr->info & 0xfff) >> 8) == 0 ? 1 : 0); /* walk frame */
	spr->ent->monster->frameTime = app->time + (spr->ent->linkIndex & 0xff) * 8 + 4000;

	if (goal->flags & 4) {
		w = Entity_aiWeaponForTarget(self, &app->game->entities[1]);
		if (w != -1) {
			self->monster->target = NULL;
			self->monster->ce.weapon = w;
			Entity_attack(self);
			return;
		}
	}
	if (goal->flags & 0x10) {
		goal->flags &= ~0x10;
		Entity_aiCalcSimpleGoal(self, goal);
		if (goal->type == 1 || goal->type == 2) {
			if (Game_isMonsterBetween(app->game, goal->x, goal->y))
				MonsterGoal_reset(goal);
			else
				Entity_aiGoal_MOVE(self, goal);
		}
	}
}

/* 0x00006980: advance the sprite towards the monster's destination;
 * returns 0 when already there */
int Entity_aiStepLerp(Entity *ent)
{
	Applet *app = g_app;
	EntityMonster *mon = ent->monster;
	Sprite *spr = entSprite(ent);
	int dx = mon->destX, dy = mon->destY;

	if (spr->x == dx && spr->y == dy)
		return 0;

	int step = app->canvas->moveStep;
	int sub = ent->def->eSubType;
	if ((sub == 1 || sub == 10) && (mon->goal.flags & 2))
		step <<= 1; /* running */

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
	Render_relinkSprite(app->render, spr);

	if (spr->x == dx && spr->y == dy) {
		ent->info &= ~0x400000u;
		mon = ent->monster;
		if (mon->flags & 0x40)
			mon->flags &= ~0x40; /* knocked back: no goal step */
		else
			Entity_aiReachedGoal_MOVE(ent, &mon->goal);
	}
	return 1;
}

/* 0x00006adc: is the queued attack still possible? */
int Entity_aiIsAttackValid(Entity *ent)
{
	Applet *app = g_app;
	EntityMonster *mon = ent->monster;

	if (mon->ce.weapon == 0x13) {
		/* raise dead: the corpse tile must still be free */
		Entity *corpse = &app->game->entities[mon->goal.param];
		if (Game_findEntityAt(app->game, corpse->monster->destX, corpse->monster->destY, MASK_MONSTER_MOVE) == NULL) {
			app->game->entities[ent->monster->goal.param].info &= ~0x2000000u;
			return 1;
		}
	}

	Game *game = app->game;
	Game_trace(game, ent->monster->destX, ent->monster->destY, game->destX, game->destY, ent, MASK_ATTACK_LOS);
	Entity *hit = app->game->traceEntity;
	if (hit == NULL)
		return 0;
	Entity *target = ent->monster->target;
	if ((target == NULL && hit->def->eType == 1) || target == hit)
		return 1;
	return 0;
}

/* 0x00006bf8 */
int Entity_aiIsValidGoal(Entity *ent, MonsterGoal *goal)
{
	Applet *app = g_app;
	int turns = goal->turns;

	if (turns >= 30)
		return 0;
	switch (goal->type) {
	case 1:
		return goal->x + goal->y * 32 != ent->linkIndex;
	case 2: {
		if (goal->param == 1)
			return 0;
		Entity *target = &app->game->entities[goal->param];
		int li = target->linkIndex;
		Entity *occ = Game_findEntityAt(app->game, (li % 32) << 6, (li / 32) << 6, MASK_MONSTER_MOVE);
		if ((uint16_t)target->linkIndex == (uint16_t)ent->linkIndex)
			return 0;
		if (occ != NULL && occ != target)
			return 0;
		int dist = Combat_WorldDistToTileDist(
			app->combat, Entity_distSqFrom(target, ent->monster->destX, ent->monster->destY));
		return dist >= goal->turns;
	}
	case 4:
	case 5:
		return turns < goal->param;
	case 6:
		return 1;
	case 9:
		if (turns < goal->param)
			return 1;
		if (turns == goal->param) {
			MonsterGoal_reset(goal);
			return 1;
		}
		return 0;
	default:
		return 0;
	}
}

/* 0x00006d58 */
void Entity_aiDoGoal(Entity *ent, MonsterGoal *goal)
{
	switch (goal->type) {
	case 1:
	case 2:
	case 4:
	case 5:
		Entity_aiGoal_MOVE(ent, goal);
		break;
	case 3:
		ent->monster->target = goal->param == 1 ? NULL : &g_app->game->entities[goal->param];
		Entity_attack(ent);
		break;
	case 6:
		Entity_aiCheckNotice(ent, goal);
		break;
	default:
		break;
	}
}

/* 0x00006f94: one AI turn of a monster */
void Entity_aiThink(Entity *self)
{
	if (self->monster->flags & 4)
		return;
	if (self->info & 0x10000)
		self->info &= ~0x10000u;
	if (Entity_aiIsValidGoal(self, &self->monster->goal) || Entity_aiChooseNewGoal(self, &self->monster->goal)) {
		self->monster->goal.turns++;
		Entity_aiDoGoal(self, &self->monster->goal);
	}
}
