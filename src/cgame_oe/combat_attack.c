/*
 * Combat: the attack state machines (player and monster), projectiles,
 * splash damage, the first-person weapon and raising the dead.
 *
 * Original: the Combat object at Applet+0x1b23d0 (Applet.combat).
 * Player-visible messages are read from the user's .mod at the offsets below.
 */

#include "game.h"

/* ---- strings in the .mod data area ---- */
#define MOD_STR_TOOK_DAMAGE_DIED 0x2ccdcu
#define MOD_STR_TOOK_DAMAGE      0x2ccf8u
#define MOD_STR_INTIMIDATED      0x2cd0cu
#define MOD_STR_LAST_SHOT        0x2cd20u
#define MOD_STR_1_SHOT_LEFT      0x2cd2cu
#define MOD_STR_2_SHOTS_LEFT     0x2cd3cu
#define MOD_STR_3_SHOTS_LEFT     0x2cd4cu
#define MOD_STR_OOPS             0x2cd5cu
#define MOD_STR_SHIELD_LOWERED   0x2cd64u
#define MOD_STR_PETRIFIED        0x2cd74u
#define MOD_STR_NO_EFFECT        0x2cd80u
#define MOD_STR_MISSED           0x2cd8cu
#define MOD_STR_IMMUNE           0x2cd94u
#define MOD_STR_DEFLECTED        0x2cda4u
#define MOD_STR_STUNNED          0x2cdb0u
#define MOD_STR_OUT_OF_RANGE     0x2cdc0u
#define MOD_STR_DODGED_NAME      0x2cdd0u
#define MOD_STR_CRIT_PREFIX      0x2cddcu
#define MOD_STR_DAMAGE           0x2cde8u
#define MOD_STR_HEALED           0x2cdf8u
#define MOD_STR_DIED_SUFFIX      0x2ce04u
#define MOD_STR_POISON_RESISTED  0x2ce10u
#define MOD_STR_POISONED         0x2ce24u
#define MOD_STR_DRAIN_RESISTED   0x2ce30u
#define MOD_STR_DRAINED          0x2ce40u
#define MOD_STR_DODGED           0x2ce4cu
#define MOD_STR_MISSED_NAME      0x2ce54u
#define MOD_STR_RAISE_FAILED     0x302d0u
#define MOD_STR_RAISES           0x302e4u

/* ---- tables in the .mod data area ---- */
#define MOD_JITTER_TABLE  0x2ca6cu /* 4 x s8: weapon shake / glow bob offsets */
#define MOD_WEAPON_POSE   0x2cc8du /* per weapon 4 x u8: idle x, idle y, attack x, attack y (+0x40 bias on y) */

static char *modStr(uint32_t off) { return (char *)ModData_str(&g_mod, off); }
static int jitter(int i) { return ModData_s8(&g_mod, MOD_JITTER_TABLE + (uint32_t)i); }

/* Sprite / line of an entity: the low 16 bits of Entity.info are a 1-based index. */
static Sprite *entSprite(Render *render, Entity *ent) { return Render_spriteAt(render, (ent->info & 0xffff)); }
static Line *entLine(Render *render, Entity *ent) { return &render->lines[(ent->info & 0xffff) - 1]; }

/*
 * The combat result block. The original passed Combat+0x3c to
 * CombatEntity_calcCombat/calcDamage, which use it as
 *   short [0] flags, [1] damage, [2] armour hit, byte +6 tile distance,
 *   int +8 crit chance, int +0xc hit chance.
 * The port's Combat struct only names +0x3c..+0x41 (hitFlags, rolledDamage,
 * armorDamage); the remaining bytes are scratch nobody else reads, so the
 * block lives on the stack and the three named shorts are copied in and out.
 */
typedef union CombatRes {
	short s[8];
	int32_t i[4];
} CombatRes;

static void resLoad(CombatRes *res, Combat *combat)
{
	memset(res, 0, sizeof *res);
	res->s[0] = (short)combat->hitFlags;
	res->s[1] = combat->rolledDamage;
	res->s[2] = combat->armorDamage;
}

static void resStore(Combat *combat, const CombatRes *res)
{
	combat->hitFlags = (uint16_t)res->s[0];
	combat->rolledDamage = res->s[1];
	combat->armorDamage = res->s[2];
}

/* ------------------------------------------------------------------ setup */

/* 0x0001db8c: start an attack; attacker/target NULL = the player */
void Combat_start(Combat *combat, void *attackerPtr, void *targetPtr, int tx, int ty)
{
	Entity *attacker = attackerPtr;
	Entity *target = targetPtr;
	Player *player = combat->player;

	combat->targetY = ty;
	combat->targetX = tx;

	if (attacker == NULL || target == NULL) {
		Player_calcStats(player);
		if (attacker != NULL)
			attacker->info |= 0x20000000;
	} else {
		attacker->info |= 0x20000000;
	}

	int playerFight = 0; /* player attacks a monster, or is attacked */
	int playerBreaks = 0; /* player attacks a breakable */
	if (target == NULL) {
		playerFight = 1;
	} else {
		uint32_t info = target->info;
		target->info = info | 0x20000000;
		if (target->monster != NULL && target->def->eType == 2 && !(info & 0x80000))
			Game_activateEx(combat->game, target, 1, 0);
		if (attacker == NULL) {
			if (target->def->eType == 2)
				playerFight = 1;
			else if (target->def->eType == 10)
				playerBreaks = 1;
		}
	}
	if (playerFight) {
		player->attackTurn = player->totalTurns;
		player->attacked = 1;
	}
	if (playerFight || playerBreaks) {
		Player_removeBuff(player, 5);
		Player_updateStats(player);
	}

	combat->attacker = attacker;
	if (attacker == NULL) {
		combat->attackerSubtype = 0;
		combat->attackerType = 0;
	} else {
		combat->attackerType = attacker->def->eType;
		combat->attackerSubtype = attacker->def->eSubType;
	}
	combat->target = target;
	if (target == NULL) {
		combat->targetSubtype = 0;
		combat->targetType = 0;
	} else {
		combat->targetType = target->def->eType;
		combat->targetSubtype = target->def->eSubType;
	}

	if (attacker == NULL) {
		combat->app->hud->numMessages = 0;
		combat->weaponId = player->ce.weapon;
	} else {
		if (attacker->monster->ce.weapon == 0x25)
			Entity_bossPickMinionSpots(attacker);
		int rangedWeapon = Combat_getMonsterAttackField(combat, combat->attacker->def, 1);
		int weapon = combat->attacker->monster->ce.weapon;
		combat->weaponId = weapon;
		if (weapon == rangedWeapon)
			combat->attackFrame = 4;
		else if (weapon == 0xe)
			combat->attackFrame = 3;
		else
			combat->attackFrame = 2;
		if (combat->attacker->monster->ce.weapon == 0x24)
			combat->attackFrame = 2;
	}

	combat->f24 = 0x30;
	combat->weapon = &combat->weapons[combat->weaponId];
	combat->state = 0;
	combat->resolveTime = 0;
	combat->nextShotTime = 0;
	combat->shotToggle = 0;
	combat->bonus = 0;
	combat->shotsLeft = combat->weapons[combat->weaponId].numShots;
	Entity *att = combat->attacker;
	if (att != NULL && att->def->eSubType == 6 && (att->info & 0x800000))
		combat->shotsLeft = 1;
	Canvas_setState(combat->canvas, 4);
}

/* 0x00022778 lives in combat.c (Combat_runFrame). */

/* ------------------------------------------------------------------ projectiles */

/* 0x000054c8: launch one missile sprite (max 3 in flight) */
void *Combat_launchMissile(Combat *combat, int sx, int sy, int dx, int dy, int vertical, int renderMode)
{
	if (combat->numMissiles == 3)
		return NULL;

	GameSprite *gs = Game_gspriteAlloc(combat->game, 0, combat->projectileTile, 0, 4);
	combat->missiles[combat->numMissiles] = gs;
	combat->numMissiles++;
	gs->lifetime = 0;
	gs->sprite->renderMode = (uint8_t)renderMode;
	gs->start[0] = sx;
	gs->sprite->x = sx;
	gs->start[1] = sy;
	gs->sprite->y = sy;
	gs->start[2] = 0x20;
	gs->sprite->z = 0x20;
	gs->dest[2] = 0x20;
	gs->dest[0] = dx;
	gs->dest[1] = dy;

	int speed = 0x140;
	switch (combat->projectileTile) {
	case 0xe0:
		speed = 0x100;
		break;
	case 0xe2:
	case 0xe4:
		speed = 0x180;
		break;
	}

	if (combat->projectileTile == 0) {
		gs->sprite->info |= 0x10000; /* nothing to see */
		return gs;
	}

	gs->vel[1] = 0;
	gs->vel[0] = 0;
	/* main = travel axis, side = the other one */
	int main = vertical ? 1 : 0;
	int side = vertical ? 0 : 1;
	gs->vel[main] = speed;
	int dist = gs->dest[main] - gs->start[main];
	if (dist < 0)
		dist = -dist;
	int duration = dist * 1000 / speed;
	gs->lerpDuration = duration;
	if (combat->hit == 0 && (combat->attacker != NULL || combat->targetType == 2)) {
		/* a miss drifts sideways towards the offset impact point */
		gs->vel[side] = duration != 0 ? (gs->dest[side] - gs->start[side]) * 1000 / duration : 0;
	} else {
		gs->vel[side] = 0;
		gs->dest[side] = gs->start[side];
	}
	if (gs->dest[main] < gs->start[main])
		gs->vel[main] = -gs->vel[main];
	Render_relinkSpriteAt(combat->render, gs->sprite, -1, -1);
	return gs;
}

/* 0x000182e4: one sparkle of the spell (weapon 9) pattern, 900 ms flight */
void *Combat_launchSpellSprite(Combat *combat, int *from, int *to)
{
	GameSprite *gs = Game_gspriteAlloc(combat->game, 4, 0xea, 0, 6);
	if (gs == NULL)
		return NULL;
	gs->start[0] = from[0];
	gs->sprite->x = from[0];
	gs->start[1] = from[1];
	gs->sprite->y = from[1];
	gs->start[2] = from[2];
	gs->sprite->z = from[2];
	gs->dest[0] = to[0];
	gs->dest[1] = to[1];
	gs->dest[2] = to[2];
	gs->lifetime = 900;
	gs->lerpDuration = 900;
	gs->frameDuration = 300;
	gs->sprite->renderMode = 7;
	int duration = gs->lerpDuration;
	gs->vel[0] = (to[0] - from[0]) * 1000 / duration;
	gs->vel[1] = (to[1] - from[1]) * 1000 / duration;
	gs->vel[2] = (to[2] - from[2]) * 1000 / duration;
	Render_relinkSpriteAt(combat->render, gs->sprite, -1, -1);
	return gs;
}

/* 0x00018394: launch the projectile of the current shot (if the weapon has one) */
void Combat_launchProjectile(Combat *combat)
{
	int renderMode = 0;
	uint8_t effect = combat->weapon->effect;

	switch (effect) {
	default:
		combat->done = 1; /* no projectile: resolve immediately */
		return;
	case 0x01:
		combat->projectileTile = 0xe1;
		renderMode = 7;
		break;
	case 0x05:
	case 0x06:
	case 0x07:
		combat->projectileTile = 0xe2;
		break;
	case 0x08:
		combat->projectileTile = 0xe3;
		renderMode = 7;
		break;
	case 0x0f:
		combat->projectileTile = 0xe4;
		break;
	case 0x10:
	case 0x11:
	case 0x12:
	case 0x13:
		combat->projectileTile = 0;
		renderMode = 4;
		break;
	case 0x14:
		combat->projectileTile = 0xe6;
		renderMode = 7;
		break;
	case 0x15:
		combat->projectileTile = 0xe5;
		break;
	case 0x16:
		combat->projectileTile = 0xe0;
		renderMode = 7;
		break;
	}

	Entity *attacker = combat->attacker;
	Render *render = combat->render;
	Canvas *canvas = combat->canvas;
	int dx, dy; /* impact point */

	if (attacker == NULL) {
		if (combat->targetType == 0 || combat->targetType == 0xc) {
			dx = combat->targetX;
			dy = combat->targetY;
		} else if (combat->target->info & 0x200000) {
			Line *line = entLine(render, combat->target);
			dx = (line->x2 - line->x1) / 2 + line->x1;
			dy = (line->y2 - line->y1) / 2 + line->y1;
		} else {
			Sprite *spr = entSprite(render, combat->target);
			EntityDef *def = spr->ent->def;
			if (def->eType == 7 && def->eSubType == 4) {
				dx = (spr->x >> 6) * 64 + 32;
				dy = (spr->y >> 6) * 64 + 32;
			} else {
				dx = spr->x;
				dy = spr->y;
			}
		}
	} else {
		Entity *tgt = attacker->monster->target;
		if (tgt == NULL) {
			dx = canvas->viewX;
			dy = canvas->viewY;
		} else {
			Sprite *spr = entSprite(render, tgt);
			dx = spr->x;
			dy = spr->y;
		}
	}

	int sx, sy, vertical;
	if (attacker == NULL) {
		sx = canvas->stepX / 2 + canvas->viewX;
		sy = canvas->stepY / 2 + canvas->viewY;
		int ddx = dx - sx;
		int ddy = dy - sy;
		int miss = combat->hitType == 0 && combat->targetType == 2;
		/* a missed shot lands 25 units to one side of the target */
		if (ddx != 0 && miss) {
			if (Random_nextByte(&combat->app->random) & 1)
				dy += 0x19;
			else
				dy -= 0x19;
		}
		if (ddy != 0 && miss) {
			if (Random_nextByte(&combat->app->random) & 1)
				dx += 0x19;
			else
				dx -= 0x19;
		}
		vertical = ddy != 0;
	} else {
		sx = attacker->monster->destX;
		sy = attacker->monster->destY;
		int spell = effect == 0x10 || effect == 0x11 || effect == 0x12 || effect == 0x13;
		int ddx = dx - sx;
		int ddy = dy - sy;
		int stop = combat->f24; /* hits stop short of the target's centre */
		if (ddx < 0) {
			if (spell)
				sx = stop + dx - 1;
			if (combat->hitType != 0)
				dx += stop;
			else if (Random_nextByte(&combat->app->random) & 1)
				dy += 0x19;
			else
				dy -= 0x19;
		} else if (ddx > 0) {
			if (spell)
				sx = dx - stop - 1;
			if (combat->hitType != 0)
				dx -= stop;
			else if (Random_nextByte(&combat->app->random) & 1)
				dy += 0x19;
			else
				dy -= 0x19;
		}
		if (ddy < 0) {
			if (spell)
				sy = stop + dy - 1;
			if (combat->hitType != 0)
				dy += stop;
			else if (Random_nextByte(&combat->app->random) & 1)
				dx += 0x19;
			else
				dx -= 0x19;
		} else if (ddy > 0) {
			if (spell)
				sy = dy - stop - 1;
			if (combat->hitType != 0)
				dy -= stop;
			else if (Random_nextByte(&combat->app->random) & 1)
				dx += 0x19;
			else
				dx -= 0x19;
		}
		vertical = ddy != 0;
	}

	GameSprite *gs = Combat_launchMissile(combat, sx, sy, dx, dy, vertical, renderMode);

	/* ring 5 bonus shot: a second projectile skimming the floor */
	if (combat->bonus && gs != NULL && gs->lerpDuration > 0) {
		GameSprite *extra = Game_gspriteAlloc(combat->game, 0, 0xe0, 0, 4);
		if (extra != NULL) {
			extra->dest[0] = gs->dest[0];
			extra->dest[1] = gs->dest[1];
			extra->dest[2] = gs->dest[2];
			extra->start[0] = gs->start[0];
			extra->start[1] = gs->start[1];
			extra->start[2] = gs->start[2];
			int z = combat->render->floorZ - 0x12;
			extra->start[2] = z;
			extra->dest[2] = z;
			extra->lerpDuration = gs->lerpDuration;
			extra->lifetime = gs->lerpDuration;
			extra->vel[0] = gs->vel[0];
			extra->vel[1] = gs->vel[1];
			extra->vel[2] = gs->vel[2];
			extra->sprite->x = extra->start[0];
			extra->sprite->y = extra->start[1];
			extra->sprite->z = extra->start[2];
			extra->sprite->renderMode = 7;
			extra->sprite->info |= 0x20000000;
			Render_relinkSpriteAt(combat->render, extra->sprite, -1, -1);
		}
	}
	combat->done = 0;
}

/* 0x0002b78c: replace landed missiles by their impact effect; finish the attack */
void Combat_resolveProjectiles(Combat *combat)
{
	for (int i = 0; i < combat->numMissiles; i++) {
		GameSprite *gs = combat->missiles[i];
		if (gs->flags & 4)
			continue; /* still flying */

		int mode = 0;
		switch (combat->weapon->effect) {
		case 0x01:
			combat->projectileTile = 0xf3;
			mode = 7;
			break;
		case 0x08:
			combat->projectileTile = 0;
			Game_gspriteAllocAnim(combat->game, 0xec, combat->targetX, combat->targetY, 0x20);
			Game_gspriteAllocAnim(combat->game, 0xeb, combat->targetX, combat->targetY, 0x20);
			Canvas_startShake(combat->canvas, (short)gs->lerpDuration, combat->crit ? 3 : 1, 0);
			break;
		case 0x0f:
			combat->projectileTile = 0xfa;
			mode = 0xd;
			break;
		case 0x10:
			combat->projectileTile = 0xf8;
			mode = 5;
			break;
		case 0x11:
			combat->projectileTile = 0xf9;
			mode = 5;
			break;
		case 0x12:
			combat->projectileTile = 0xfa;
			mode = 5;
			break;
		case 0x13:
			combat->projectileTile = 0xfb;
			mode = 5;
			break;
		case 0x14:
			combat->projectileTile = 0xf2;
			mode = 7;
			break;
		case 0x15:
			combat->projectileTile = 0xf7;
			mode = 5;
			break;
		case 0x16:
			if (Entity_distSqFrom(combat->attacker, combat->canvas->destX, combat->canvas->destY) <= 0x1000) {
				combat->projectileTile = 0xfa;
				mode = 4;
			} else {
				combat->projectileTile = 0;
				mode = 0;
			}
			break;
		default:
			break;
		}

		int x = gs->dest[0];
		int y = gs->dest[1];
		int z = gs->dest[2];
		Game_gspriteDestroy(combat->game, gs);

		if (combat->projectileTile != 0 && (combat->attacker == NULL || combat->totalDamage != 0)) {
			GameSprite *fx = Game_gspriteAllocAnim(combat->game, combat->projectileTile, x, y, z);
			fx->sprite->renderMode = (uint8_t)mode;
			if (combat->attacker != NULL) {
				Sprite *spr = entSprite(combat->render, combat->attacker);
				if (spr->scale < 0x40)
					fx->sprite->scale = spr->scale;
				if (spr->info & 0x1000000)
					fx->sprite->info |= 0x1000000;
			}
		}

		/* remove from the list; like the original, the loop index still advances */
		for (int j = i; j + 1 < combat->numMissiles; j++)
			combat->missiles[j] = combat->missiles[j + 1];
		combat->numMissiles--;
		combat->done = 1;
	}

	if (combat->done) {
		if (combat->attacker == NULL)
			Combat_finishPlayerAttack(combat);
		else
			Combat_finishMonsterAttack(combat);
		combat->f39 = 1;
		combat->done = 0;
	}

	if (combat->numMissiles == 0) {
		if (combat->hitType == 0)
			combat->f39 = 1;
		if (combat->f39 && combat->shotsLeft == 0 && combat->shotToggle == 0)
			combat->resolveTime = combat->app->tickTime;
	}
}

/* ------------------------------------------------------------------ damage application */

/* 0x00010650: the player's shot landed: blood, damage, ring effects, knockback */
void Combat_finishPlayerAttack(Combat *combat)
{
	if (combat->hitType == 0)
		return;

	switch (combat->targetType) {
	case 2:
		break;
	case 10:
		if (combat->targetSubtype == 1) {
			Particles_spawnImage1(combat, (int)0xff845d39);
			entSprite(combat->render, combat->target)->info |= 0x10000;
			Game_unlinkEntity_2ae8c(combat->game, combat->target);
		} else {
			entSprite(combat->render, combat->target)->info |= 0x10000;
			Particles_spawnPreset1(combat, combat->targetTileDist);
		}
		return;
	case 8:
		Particles_spawnPreset3(combat, combat->targetTileDist, (int)0xffcccc00);
		return;
	case 9:
		entSprite(combat->render, combat->target)->info |= 0x10000;
		combat->f7c = 1;
		combat->bloodColor = Combat_getBloodColor(combat->target);
		Particles_spawnBlood(combat, combat->targetTileDist, combat->bloodColor, 0xf, 0x10);
		return;
	default:
		return;
	}

	Entity *target = combat->target;
	EntityMonster *mon = target->monster;

	/* shielded boss: sparks */
	if (combat->targetSubtype == 0xd && (mon->flags & 8) && mon->petrifyTurns == 0) {
		Particles_spawnBurst(combat, combat->targetTileDist, (int)0xff009fef, 0x20);
		return;
	}
	if (combat->targetSubtype == 0xf && combat->totalDamage == 0 && combat->weaponId != 0) {
		Particles_spawnBurst(combat, combat->targetTileDist, (int)0xffc502de, 0x20);
		return;
	}

	if (combat->totalDamage < 1) {
		if (combat->totalDamage >= 0)
			return;
		/* negative damage heals, up to max health */
		int hp = CombatEntity_getHealth(&target->monster->ce);
		if (hp - combat->totalDamage > CombatEntity_getMaxHealth(&target->monster->ce))
			combat->totalDamage = -(CombatEntity_getMaxHealth(&target->monster->ce) -
			                        CombatEntity_getHealth(&target->monster->ce));
		CombatEntity_setHealth(&target->monster->ce,
		                       CombatEntity_getHealth(&target->monster->ce) - combat->totalDamage);
		return;
	}

	Entity_takeDamage(target, combat->totalDamage);
	Particles_spawnBlood(combat, combat->targetTileDist, combat->bloodColor, -1, 0);
	if (CombatEntity_getHealth(&combat->target->monster->ce) < 1)
		return;

	int knock = 0;
	if (combat->bonus) {
		knock = 2;
	} else if (combat->player->ring == 7) {
		/* ring of intimidation */
		mon = combat->target->monster;
		if (mon->petrifyTurns == 0 && mon->goal.type != 4 && !Entity_isBoss(combat->target) &&
		    Random_nextByte(&combat->app->random) < 0x20) {
			MonsterGoal_reset(&combat->target->monster->goal);
			combat->target->monster->goal.type = 4;
			int r = Random_nextByte(&combat->app->random);
			combat->target->monster->goal.param = (int16_t)((unsigned)r % 3 + 3);
			char msg[0x40];
			App_snprintf(msg, 0x40, modStr(MOD_STR_INTIMIDATED), combat->target->name);
			Hud_addMessage(combat->app->hud, msg, 0);
		}
	}

	int doKnock = 1;
	if (combat->weaponId == 10) {
		if (combat->targetType == 2) {
			knock++;
			mon = combat->target->monster;
			if (combat->targetSubtype == 0xd && (mon->flags & 8) && mon->petrifyTurns != 0)
				mon->petrifyTurns = 1;
		}
	} else {
		if (combat->weaponId == 6 && Random_nextByte(&combat->app->random) < 0x28)
			knock++;
		/* stone does not move, except a shielded sub-0xd boss */
		mon = combat->target->monster;
		if (mon->petrifyTurns != 0 && (combat->targetSubtype != 0xd || !(mon->flags & 8)))
			doKnock = 0;
	}
	if (doKnock && knock > 0) {
		Game_unlinkPlayerEntity(combat->player);
		Entity_knockback(combat->target, combat->canvas->viewX, combat->canvas->viewY, knock);
		Game_relinkAtViewDest(combat->player);
	}

	/* the multi-part boss reacts to the hammer by disappearing or attacking */
	if (combat->target->def->eSubType != 0xf || combat->weaponId != 10)
		return;
	MonsterGoal *goal = &combat->target->monster->goal;
	MonsterGoal_reset(goal);
	mon = combat->target->monster;
	int ddx = combat->canvas->viewX - mon->destX;
	int ddy = combat->canvas->viewY - mon->destY;
	goal->type = (ddx * ddx + ddy * ddy <= 0x1000) ? 0xd : 0xa;
}

/* 0x00010948: a monster's shot reached the player: damage direction, pushes */
void Combat_finishMonsterAttack(Combat *combat)
{
	if (combat->target != NULL)
		return;

	Sprite *spr = entSprite(combat->render, combat->attacker);
	Canvas *canvas = combat->canvas;
	Hud *hud = combat->app->hud;
	/* the original passed the Player* as the (unused) first argument */
	hud->arrowSide = Dir_relativeQuadrant(0, canvas->viewX, canvas->viewY, canvas->viewAngle, spr->x, spr->y);
	hud->arrowUntil = combat->app->tickTime + 500;
	if (combat->hit == 0 || combat->totalDamage < 1) {
		combat->app->hud->arrowDir = -1;
		return;
	}

	Player_hurtFeedback(combat->player, combat->attacker != NULL);
	if (combat->attacker == NULL)
		return;

	Game *game = combat->game;
	Game_unlinkEntity_2ae8c(game, combat->attacker);
	EntityMonster *mon = combat->attacker->monster;
	int push = 0;
	switch (combat->weaponId) {
	case 0x19:
		push = combat->targetTileDist != 1 ? 1 : 2;
		break;
	case 0x20:
		push = (mon->flags & 8) ? 4 : 1;
		break;
	case 0x24:
		push = 3;
		break;
	}
	if (push != 0)
		Entity_knockback(&game->entities[1], mon->destX, mon->destY, push);

	int link = combat->attacker->linkIndex;
	int ty = link / 32;
	Game_linkEntity(combat->game, combat->attacker, link - ty * 32, ty);
}

/* 0x000076d4: splash damage on one tile (scale 16.16) */
void Combat_damageTile(Combat *combat, int tx, int ty, int scale)
{
	CombatRes res;
	char msg[0x80];

	memset(&res, 0, sizeof res);
	res.s[0] = 8; /* ignore range */

	if (combat->canvas->viewX >> 6 == tx && combat->canvas->viewY >> 6 == ty) {
		/* the player is caught in the blast */
		CombatEntity *ce = &combat->player->ce;
		CombatEntity_calcCombat(ce, ce, res.s);
		res.s[1] = (short)((uint32_t)(scale * res.s[1]) >> 16);
		Player_hurtFeedback(combat->player, 0);
		if (res.s[1] > 0) {
			Player_takeDamage(combat->player, res.s[1], NULL);
			CombatEntity_addArmor(&combat->player->ce, -res.s[2]);
		}
		return;
	}

	Entity *ent = Game_findEntityAt(combat->game, tx << 6, ty << 6, 0x76b5);
	if (ent == NULL)
		return;
	uint8_t eType = ent->def->eType;
	uint32_t info = ent->info;
	if (eType == 9 || !(info & 0x40000))
		return;

	EntityMonster *mon = ent->monster;
	if (mon == NULL) {
		if (eType == 8 || eType == 10)
			return;
	} else {
		if (ent->def->eSubType == 0xd && (mon->flags & 8) && mon->petrifyTurns == 0)
			return; /* shielded */
		ent->info = info | 0x20000000;
		if (!(info & 0x80000))
			Game_activateEx(combat->game, ent, 1, 0);
		CombatEntity_calcCombat(&combat->player->ce, &ent->monster->ce, res.s);
		res.s[1] = (short)((uint32_t)(scale * res.s[1]) >> 16);
		if (res.s[1] < 1)
			return;
		Entity_takeDamage(ent, res.s[1]);
		if (CombatEntity_getHealth(&ent->monster->ce) > 0) {
			App_snprintf(msg, 0x80, modStr(MOD_STR_TOOK_DAMAGE), ent->name, (int)res.s[1]);
			Hud_addMessage(combat->app->hud, msg, 0);
			if (ent->monster->petrifyTurns != 0)
				return;
			int snd = EntityMonster_getRandomSound(ent->monster, 6);
			if (snd != 0)
				Sound_play(combat->app->sound, snd, 0, 2, -1);
			return;
		}
		App_snprintf(msg, 0x80, modStr(MOD_STR_TOOK_DAMAGE_DIED), ent->name, (int)res.s[1], ent->def->name);
		Hud_addMessage(combat->app->hud, msg, 0);
	}
	Entity_died(ent, 1);
}

/* 0x0001fe24: a necromancer (sub 5) or the healer boss (sub 0xf) raises a corpse */
void Combat_raiseDead(void *casterPtr, int corpseIdx)
{
	Entity *caster = casterPtr;
	Applet *app = caster->app;
	Game *game = app->game;
	Entity *corpse = &game->entities[corpseIdx];
	Sprite *spr = entSprite(app->render, corpse);
	char msg[0x40];

	if (Game_findEntityAt(game, spr->x, spr->y, 0xfdaf) != NULL) {
		App_snprintf(msg, 0x40, modStr(MOD_STR_RAISE_FAILED), caster->name);
		Hud_addMessage(app->hud, msg, 0);
		corpse->info &= ~0x2000000u;
		return;
	}

	Entity_resurrect(corpse, spr->x, spr->y, spr->z);
	Game_activateEx(app->game, corpse, 0, 0);
	uint8_t sub = caster->def->eSubType;
	if (sub == 0xf) {
		corpse->info |= 0x10000000;
		Game_gspriteAllocAnim(app->game, 0xc0, spr->x, spr->y, spr->z);
	} else if (sub == 5) {
		/* raised as a shadow monster: name from Combat.monsterNames. Classes 2, 5, 11 and 12 (mimics,
		 * shadows, wraiths, Filch) have no shadow name: the original stored the table's 0 and drew whatever
		 * was at address 0. Crash guard: they keep the name Entity_resurrect gave them. */
		corpse->info |= 0x1000000;
		EntityDef *def = corpse->def;
		char *shadowName = app->combat->monsterNames[def->eSubType * 3 + def->parm];
		if (shadowName)
			corpse->name = shadowName;
		Game_gspriteAllocAnim(app->game, 0xc6, spr->x, spr->y, spr->z);
	}
	App_snprintf(msg, 0x40, modStr(MOD_STR_RAISES), caster->name, corpse->name);
	Hud_addMessage(app->hud, msg, 0);
	Sound_play(app->sound, 0x1406, 0, 3, -1);
}

/* ------------------------------------------------------------------ monster attack */

/* 0x0001c1ac: monster attack state machine; returns 1 when the attack is resolved */
int Combat_monsterAttackFrame(Combat *combat)
{
	Applet *app = combat->app;
	Hud *hud = app->hud;
	char buf[0x40];

	if (combat->state == 0) {
		combat->shotToggle = 0;
		combat->state = 1;
		combat->nextShotTime = app->tickTime;
		combat->animEnd = 0;
		combat->totalDamage = 0;
		combat->armorDamageTotal = 0;
		combat->hitType = 0;
		combat->hitFlags = 0;
		combat->f39 = 0;
		combat->crit = 0;
		combat->hit = 0;
		return 0;
	}

	if (combat->state == 2) {
		combat->shotToggle = 0;
		combat->attacker->info &= ~0x10000u;
		int w = combat->weaponId;
		if (w == 0x25) {
			Entity_bossPlaceMinions(combat->attacker, 1);
		} else if (w == 0x26 || w == 0x15) {
			Combat_raiseDead(combat->attacker, combat->attacker->monster->goal.param);
		} else if (w == 0x13) {
			Entity_devourCorpse(combat->attacker, combat->attacker->monster->goal.param);
		} else if (combat->target == NULL) {
			Sprite *spr = entSprite(combat->render, combat->attacker);
			Canvas *canvas = combat->canvas;
			combat->app->hud->arrowSide =
			    Dir_relativeQuadrant(0, canvas->viewX, canvas->viewY, canvas->viewAngle, spr->x, spr->y);
			combat->app->hud->arrowUntil = combat->app->tickTime + 500;

			const char *msg;
			if (combat->hit == 0) {
				combat->app->hud->arrowDir = 0;
				int level = combat->player->ce.level;
				if (Random_nextByte(&combat->app->random) < level * 25 - 25) {
					msg = modStr(MOD_STR_DODGED);
				} else {
					App_snprintf(buf, 0x40, modStr(MOD_STR_MISSED_NAME), combat->attacker->name);
					msg = buf;
				}
			} else {
				CombatEntity_addArmor(&combat->player->ce, -combat->armorDamageTotal);
				if (combat->totalDamage < 1) {
					combat->app->hud->arrowDir = 0;
					combat->totalDamage = 0;
				} else {
					Player_takeDamage(combat->player, combat->totalDamage, combat->attacker);
				}

				if (combat->weapon->effect == 0x15 && (Random_nextByte(&combat->app->random) & 1)) {
					/* poison */
					Player *p = combat->player;
					if (p->effects[3].turns == 0) {
						if (p->effects[1].turns == 0) {
							Player_addStatusEffect(p, 1, -6, 5);
							Player_updateStats(combat->player);
						} else {
							p->effects[1].turns = 5;
							combat->player->effects[1].stacks = 1;
						}
						Player_hurtFeedback(combat->player, combat->attacker != NULL);
						hud = combat->app->hud;
						msg = modStr(MOD_STR_POISONED);
					} else {
						hud = combat->app->hud;
						msg = modStr(MOD_STR_POISON_RESISTED);
					}
				} else if (combat->weapon->effect == 0x17) {
					/* drain: the attacker grows stronger */
					if (combat->player->effects[3].turns == 0) {
						Player_addStatusEffect(combat->player, 0xb, -5, 5);
						Player_updateStats(combat->player);
						CombatEntity *ace = &combat->attacker->monster->ce;
						CombatEntity_setStrength(ace, CombatEntity_getStrength(ace) + 3);
						CombatEntity_setDefense(ace, CombatEntity_getDefense(ace) + 3);
						CombatEntity_setAccuracy(ace, CombatEntity_getAccuracy(ace) + 3);
						CombatEntity_setHealth(ace, CombatEntity_getHealth(ace) + combat->totalDamage);
						hud = combat->app->hud;
						msg = modStr(MOD_STR_DRAINED);
					} else {
						hud = combat->app->hud;
						msg = modStr(MOD_STR_DRAIN_RESISTED);
					}
				} else {
					if (combat->totalDamage > 0)
						return 1;
					msg = modStr(MOD_STR_DEFLECTED);
				}
			}
			Hud_addMessage(hud, (char *)msg, 0);
		} else {
			Entity_died(combat->target, 1);
		}
		return 1;
	}

	if (combat->state != 1)
		return 0;

	/* all shots fired: wait for the projectiles and the resolve time */
	if (combat->resolveTime != 0 && combat->numMissiles == 0) {
		if (app->tickTime <= combat->nextShotTime)
			return 0;
		Entity *att = combat->attacker;
		Sprite *spr = entSprite(combat->render, att);
		if (att->def->eSubType == 0xf && ((spr->info & 0xfff) >> 8) > 1) {
			if (att->monster->frameTime == 0) {
				spr->info = (spr->info & 0xfffff0ff) + 0x300;
				combat->attacker->monster->frameTime = combat->app->tickTime + 150;
			}
		} else {
			spr->info &= 0xfffff0ff;
			combat->attacker->monster->frameTime = 0;
		}
		int now = combat->app->tickTime;
		if (now <= combat->resolveTime)
			return 0;
		if (now <= combat->attacker->monster->frameTime)
			return 0;
		combat->state = 2;
		combat->resolveTime = 0;
		combat->attacker->monster->frameTime = 0;
		return 0;
	}

	if (combat->animEnd == 0 && app->tickTime > combat->nextShotTime &&
	    (combat->shotsLeft > 0 || combat->shotToggle != 0)) {
		combat->shotToggle = combat->shotToggle == 0;
		if (combat->shotToggle) {
			/* fire one shot */
			Render *render = combat->render;
			Entity *att = combat->attacker;
			Sprite *spr = entSprite(render, att);
			if (att->def->eSubType == 6) {
				if (att->info & 0x800000)
					combat->attackFrame = 4;
				else if (combat->shotsLeft != 1)
					combat->attackFrame = 3;
				else
					combat->attackFrame = 2;
			} else if (combat->weaponId == 0x19) {
				Canvas_startShake(combat->canvas, 1000, 3, 0);
			}
			if (combat->attacker->monster->goal.type != 6)
				spr->info = (spr->info & 0xfffff0ff) | ((uint32_t)combat->attackFrame << 8);
			uint8_t sub = combat->attacker->def->eSubType;
			if (sub == 6 || sub == 8 || !(Random_nextByte(&combat->app->random) & 1))
				spr->info &= ~0x1000000u;
			else
				spr->info |= 0x1000000;

			int until = combat->app->tickTime + (combat->attacker->def->eSubType == 3 ? 350 : 150);
			combat->attacker->monster->frameTime = until;
			combat->animEnd = until;
			if (combat->attacker->def->eSubType == 8)
				combat->animEnd += 150;

			if (combat->weapon->dmgMax > 0) {
				if (combat->target == NULL) {
					CombatRes res;
					resLoad(&res, combat);
					CombatEntity_calcCombat(&combat->attacker->monster->ce, &combat->player->ce, res.s);
					resStore(combat, &res);
					/* protection charges absorb whole shots */
					int8_t *charges = &combat->player->effects[2].amount;
					if (*charges > 0) {
						combat->hitFlags = 0;
						*charges = (int8_t)(*charges - 1);
						if (*charges == 0)
							Player_removeBuff(combat->player, 2);
						Player_updateStats(combat->player);
					}
					if (combat->hitFlags & 3) {
						if (combat->hitFlags & 2) {
							combat->crit = 1;
							combat->hitType = 2;
						} else {
							combat->hitType = 1;
						}
						combat->hit = 1;
						combat->shotDamage = combat->rolledDamage;
					} else {
						combat->shotDamage = 0;
					}
				} else {
					/* monster vs monster always hits for 1 */
					combat->hitType = 1;
					combat->shotDamage = 1;
				}
				combat->totalDamage += combat->shotDamage;
				combat->armorDamageTotal += combat->armorDamage;
			}
			combat->shotsLeft--;
		} else if (app->tickTime < combat->nextShotTime) {
			entSprite(combat->render, combat->attacker)->info &= 0xfffff0ff;
		}
		combat->nextShotTime = combat->app->tickTime + combat->weapon->shotDelay * 10;
	}

	if (combat->animEnd != 0 && combat->animEnd < combat->app->tickTime) {
		int ranged = Combat_getMonsterAttackField(combat, combat->attacker->def, 1);
		int snd = EntityMonster_getSound(combat->attacker->monster, combat->weaponId == ranged ? 5 : 4);
		if (snd != 0)
			Sound_play(combat->app->sound, snd, 0, 3, -1);
		if (combat->weaponId == 0x19) {
			EntityMonster *mon = combat->attacker->monster;
			GameSprite *fx = Game_gspriteAllocAnim(combat->game, 0xdf, mon->destX, mon->destY, 0x20);
			fx->sprite->renderMode = 7;
		}
		if (combat->weaponId == 0x25) {
			Entity_bossPlaceMinions(combat->attacker, 0);
			combat->resolveTime = combat->app->tickTime;
		} else {
			Combat_launchProjectile(combat);
		}
		combat->animEnd = 0;
	}
	Combat_resolveProjectiles(combat);
	return 0;
}

/* ------------------------------------------------------------------ player attack */

/* Weapon 9 (spell) pattern of sparkles from in front of the player. */
static void launchSpellPattern(Combat *combat)
{
	Canvas *cv = combat->canvas;
	int from[3], to[3];

	from[0] = cv->viewX + (cv->stepX >> 1);
	from[1] = cv->viewY + (cv->stepY >> 1);
	from[2] = 0x24;
	int vx = cv->viewX;
	int ddx = (combat->targetX >> 6) - (vx >> 6);
	int ddy = (combat->targetY >> 6) - (cv->viewY >> 6);
	if (combat->targetType == 0 && ddx * ddx + ddy * ddy < 1) {
		/* casting at the own tile: a short pattern */
		to[0] = cv->stepX / 4 + vx + cv->rightX / 2;
		cv = combat->canvas;
		to[1] = cv->stepY / 4 + cv->viewY + cv->rightY / 2;
		to[2] = 0x20;
		Combat_launchSpellSprite(combat, from, to);
		to[0] -= combat->canvas->rightX;
		to[1] -= combat->canvas->rightY;
		Combat_launchSpellSprite(combat, from, to);
		to[0] = combat->canvas->stepX / 4 + combat->canvas->viewX;
		to[1] = combat->canvas->stepY / 4 + combat->canvas->viewY;
		to[2] = 0x40;
	} else {
		to[0] = cv->stepX / 2 + cv->stepX + vx + cv->rightX / 2;
		cv = combat->canvas;
		to[1] = cv->stepY / 2 + cv->viewY + cv->stepY + cv->rightY / 2;
		to[2] = 0;
		Combat_launchSpellSprite(combat, from, to);
		to[0] -= combat->canvas->rightX;
		to[1] -= combat->canvas->rightY;
		Combat_launchSpellSprite(combat, from, to);
		cv = combat->canvas;
		to[0] = cv->viewX + cv->stepX + cv->rightX;
		to[1] = cv->viewY + cv->stepY + cv->rightY;
		to[2] = 0x20;
		Combat_launchSpellSprite(combat, from, to);
		to[0] += combat->canvas->rightX * -2;
		to[1] += combat->canvas->rightY * -2;
		Combat_launchSpellSprite(combat, from, to);
		to[0] = combat->canvas->rightX / 2 + to[0];
		to[1] = combat->canvas->rightY / 2 + to[1];
		Combat_launchSpellSprite(combat, from, to);
		to[0] += combat->canvas->rightX;
		to[1] += combat->canvas->rightY;
		Combat_launchSpellSprite(combat, from, to);
		to[0] = combat->canvas->stepX / 2 + combat->canvas->viewX + combat->canvas->stepX;
		to[1] = combat->canvas->stepY / 2 + combat->canvas->viewY + combat->canvas->stepY;
		to[2] = 0x24;
	}
	Combat_launchSpellSprite(combat, from, to);
}

/* One player shot against a monster: hit roll, special rules, statistics. */
static void playerShotVsMonster(Combat *combat)
{
	Applet *app = combat->app;
	Entity *target = combat->target;
	CombatRes res;

	if (target->info & 0x10000000)
		combat->hitFlags |= 0x200;
	if (target->info & 0x1000000)
		combat->hitFlags |= 0x100;
	if (target->monster->flags & 8)
		combat->hitFlags |= 0x800;

	resLoad(&res, combat);
	CombatEntity_calcCombat(&combat->player->ce, &target->monster->ce, res.s);
	resStore(combat, &res);

	int w = combat->weaponId;
	if (w == 8) {
		/* the petrify weapon */
		if (combat->targetSubtype == 0xd) {
			if (++combat->game->vars[2] == 1) {
				combat->hitFlags |= 1;
				int dmin = combat->weapon->dmgMin;
				combat->rolledDamage = (short)((combat->weapon->dmgMax - dmin) / 2 + dmin);
			}
		} else if (combat->targetSubtype == 0xf || combat->targetSubtype == 5 ||
		           (combat->target->info & 0x1000000)) {
			combat->hitFlags = 0x80;
		}
	} else if ((w == 9 || w == 7) && !(combat->hitFlags & 0x400) && !(combat->hitFlags & 3)) {
		/* area weapons always graze for half damage */
		combat->hitFlags = 1;
		res.s[0] = 1;
		int dmg = CombatEntity_calcDamage(&combat->player->ce, &combat->target->monster->ce, res.s);
		resStore(combat, &res);
		combat->rolledDamage = (short)(dmg / 2);
		if (combat->rolledDamage == 0)
			combat->rolledDamage = 1;
	}

	if (!(combat->hitFlags & 3)) {
		app->statMissesByWeapon[combat->weaponId]++;
		return;
	}

	combat->player->missStreak = 0;
	if (combat->targetSubtype == 0xf) {
		if (combat->weaponId == 0) {
			/* the wand stuns the healer boss */
			MonsterGoal *goal = &combat->target->monster->goal;
			if (goal->type != 9) {
				MonsterGoal_reset(goal);
				goal->type = 9;
			}
			goal->param = (int16_t)(goal->param + 2);
			combat->rolledDamage = 0;
		} else if (combat->weaponId == 10) {
			int maxHp = CombatEntity_getMaxHealth(&combat->target->monster->ce) - combat->rolledDamage / 2;
			if (maxHp < 1)
				maxHp = 1;
			CombatEntity_setMaxHealth(&combat->target->monster->ce, maxHp);
		} else {
			combat->rolledDamage = 0;
			combat->hitFlags |= 0x80;
		}
	}

	if (combat->hitFlags & 2) {
		combat->crit = 1;
		combat->hitType = 2;
	} else {
		combat->hitType = 1;
	}
	combat->hit = 1;
	combat->shotDamage = combat->rolledDamage;
	int hpBefore = CombatEntity_getHealth(&combat->target->monster->ce);

	if (combat->weaponId == 8) {
		EntityMonster *mon = combat->target->monster;
		if (mon->petrifyTurns == 0) {
			mon->petrifyTurns = (uint8_t)(combat->rolledDamage + combat->armorDamage);
			combat->target->monster->flags |= 0x200;
			combat->bloodColor = (int)0xffaaaaaa;
			MonsterGoal_reset(&combat->target->monster->goal);
		} else {
			combat->hit = 0;
		}
		combat->shotDamage = 0;
	} else if (combat->targetSubtype == 0xd) {
		EntityMonster *mon = combat->target->monster;
		if ((mon->flags & 8) && mon->petrifyTurns == 0) {
			/* shield up: the blow is deflected */
			combat->shotDamage = 0;
			combat->target->monster->flashUntil = combat->app->tickTime + 300;
			Sound_play(combat->app->sound, 0x13e4, 0, 3, -1);
			if (combat->game->vars[2] < 3) {
				int n = ++combat->game->vars[1];
				if (n % 6 == 0)
					Game_executeTile(combat->game, 0, 3, 0xf000, 0);
			}
		}
	} else if (combat->targetSubtype == 0xf && combat->shotDamage == 0) {
		Sound_play(combat->app->sound, 0x13e4, 0, 3, -1);
	}

	int charge = combat->chargeLevel;
	if (combat->weaponId == 10 && charge > 0 && charge < 4)
		combat->shotDamage -= ((4 - charge) * 0x40 * combat->shotDamage * 0x100) >> 16;

	combat->targetHpAfter = hpBefore - combat->shotDamage;
	if (combat->shotDamage > 0 && combat->hit) {
		app = combat->app;
		app->statDamageByMonster[combat->target->def->eSubType] += combat->shotDamage;
		app->statDamage += combat->shotDamage;
		app->statDamageByWeapon[combat->weaponId] += combat->shotDamage;
		app->statHitsByWeapon[combat->weaponId]++;
		if (app->statMaxHit < combat->shotDamage)
			app->statMaxHit = combat->shotDamage;
		if (combat->player->ring == 5 && Random_nextByte(&combat->app->random) < 0x20)
			combat->bonus = 1;
		return;
	}
	combat->app->statMissesByWeapon[combat->weaponId]++;
}

/* Message for an attack that did no damage (state 2). */
static void playerNoDamageMessage(Combat *combat, Hud *hud)
{
	char buf[0x40];
	const char *msg = modStr(MOD_STR_NO_EFFECT);
	int flags = 0;

	if (combat->targetType == 2 && combat->target->monster->petrifyTurns != 0) {
		EntityMonster *mon = combat->target->monster;
		if (combat->hit) {
			if (combat->targetSubtype == 0xd && (mon->flags & 8)) {
				flags = 3;
				msg = modStr(MOD_STR_SHIELD_LOWERED);
			} else {
				msg = modStr(MOD_STR_PETRIFIED);
			}
			Hud_addMessage(hud, (char *)msg, flags);
			Sound_play(combat->app->sound, 0x143f, 0, 3, -1);
			return;
		}
		if (combat->weaponId != 8)
			msg = modStr(MOD_STR_MISSED);
		Hud_addMessage(hud, (char *)msg, 0);
		return;
	}

	if (combat->player->effects[0].turns != 0)
		return; /* fumbled: "Oops..." already said it */
	if (combat->targetType != 2) {
		Hud_addMessage(hud, (char *)msg, 0);
		return;
	}

	if (combat->hitFlags & 0x80) {
		App_snprintf(buf, 0x40, modStr(MOD_STR_IMMUNE), combat->target->name);
		Hud_addMessage(hud, buf, 0);
		return;
	}
	if (combat->targetSubtype == 0xd) {
		EntityMonster *mon = combat->target->monster;
		if (combat->weaponId != 8 && (mon->flags & 8) && mon->petrifyTurns == 0) {
			Hud_addMessage(hud, modStr(MOD_STR_DEFLECTED), 0);
			return;
		}
	} else if (combat->targetSubtype == 0xf && combat->weaponId == 0 && combat->hit) {
		App_snprintf(buf, 0x40, modStr(MOD_STR_STUNNED), combat->target->name);
		Hud_addMessage(hud, buf, 3);
		return;
	}

	/* dodge or plain miss: compare the player's accuracy with the monster's evasion */
	int tier = combat->target->def->parm;
	int mult = tier == 2 ? 400 : tier == 1 ? 0x140 : 0x100;
	int acc = CombatEntity_getAccuracy(&combat->player->ce);
	int mAcc = CombatEntity_getAccuracy(&combat->target->monster->ce);
	int mDef = CombatEntity_getDefense(&combat->target->monster->ce);
	int dodged;
	if (acc - ((mult * (mAcc + mDef / 4) * 0x100) >> 16) < 0)
		dodged = Random_nextByte(&combat->app->random) > 0xb;
	else
		dodged = Random_nextByte(&combat->app->random) <= 0xb;

	if (combat->hitFlags & 0x400) {
		Hud_addMessage(hud, modStr(MOD_STR_OUT_OF_RANGE), 0);
		return;
	}
	if (dodged)
		App_snprintf(buf, 0x40, modStr(MOD_STR_DODGED_NAME), combat->target->name);
	else
		strcpy(buf, modStr(MOD_STR_MISSED));
	Hud_addMessage(hud, buf, 0);
}

/* Splash damage of weapons 7 (3x3 blast) and 9 (spell line). */
static void playerSplash(Combat *combat)
{
	if (combat->weaponId == 7) {
		int tx = combat->targetX >> 6;
		int ty = combat->targetY >> 6;
		if (combat->targetType != 2) {
			tx = (combat->targetX + (combat->canvas->stepX >> 2)) >> 6;
			ty = (combat->targetY + (combat->canvas->stepY >> 2)) >> 6;
			Combat_damageTile(combat, tx, ty, 0x10000);
		}
		Combat_damageTile(combat, tx - 1, ty, 0x8000);
		Combat_damageTile(combat, tx + 1, ty, 0x8000);
		Combat_damageTile(combat, tx, ty - 1, 0x8000);
		Combat_damageTile(combat, tx, ty + 1, 0x8000);
		Combat_damageTile(combat, tx - 1, ty + 1, 0x4000);
		Combat_damageTile(combat, tx + 1, ty + 1, 0x4000);
		Combat_damageTile(combat, tx + 1, ty - 1, 0x4000);
		Combat_damageTile(combat, tx - 1, ty - 1, 0x4000);
		Sound_play(combat->app->sound, 0x1409, 0, 5, -1);
	} else if (combat->weaponId == 9) {
		int tx = combat->targetX >> 6;
		int ty = combat->targetY >> 6;
		Canvas *cv = combat->canvas;
		int sx = cv->stepX >> 6;
		int sy = cv->stepY >> 6;
		int x = cv->viewX >> 6;
		int y = cv->viewY >> 6;
		if (combat->targetType == 0 && (tx - x) * (tx - x) + (ty - y) * (ty - y) < 1) {
			Combat_damageTile(combat, x, y, 0x8000);
		} else {
			x += sx;
			y += sy;
		}
		if (x != tx && y != ty)
			Combat_damageTile(combat, x, y, 0x10000);
		int x2 = x + sx;
		if (x2 != tx || y + sy != ty)
			Combat_damageTile(combat, x2, y + sy, 0x10000);
		if (sx != 0) {
			Combat_damageTile(combat, x, y + 1, 0x8000);
			Combat_damageTile(combat, x, y - 1, 0x8000);
			Combat_damageTile(combat, x2, y + 1, 0x4000);
			Combat_damageTile(combat, x2, y - 1, 0x4000);
		} else if (sy != 0) {
			Combat_damageTile(combat, x + 1, y, 0x8000);
			Combat_damageTile(combat, x - 1, y, 0x8000);
			y += sy;
			Combat_damageTile(combat, x + 1, y, 0x4000);
			Combat_damageTile(combat, x - 1, y, 0x4000);
		}
	}
}

/* Swing/shot sound when the attack animation ends (state 1). */
static void playerWeaponSound(Combat *combat)
{
	Applet *app = combat->app;
	int w = combat->weaponId;
	int snd = 0;

	if (w == 0) {
		snd = 0x13b4;
	} else if (w >= 1 && w <= 3) {
		/* melee: 0 swing at range, 1 hit flesh-less/small, 2 hit flesh, 3 hit wall */
		int kind;
		if (combat->hitType == 0) {
			int ddx = combat->canvas->viewX - combat->targetX;
			int ddy = combat->canvas->viewY - combat->targetY;
			if (ddx * ddx + ddy * ddy <= 0x1000) {
				if (combat->targetType == 2)
					kind = combat->targetSubtype != 2 ? 2 : 1;
				else if (combat->targetType != 10)
					kind = 3;
				else
					kind = 1;
			} else {
				kind = 0;
			}
		} else if ((combat->targetType != 2 && combat->targetType != 9) || combat->targetSubtype == 2) {
			kind = 1;
		} else {
			kind = 2;
		}
		static const int sounds[3][4] = {
			{0x1401, 0x1404, 0x1405, 0x1403},
			{0x1400, 0x1421, 0x1420, 0x1442},
			{0x1402, 0x13c3, 0x1402, 0x1402},
		};
		snd = sounds[w - 1][kind];
	} else if (w >= 4 && w <= 6) {
		snd = 0x13b6;
	} else if (w == 7) {
		snd = 0x13b7;
	} else if (w == 8) {
		snd = 0x13b8;
	} else if (w == 9) {
		snd = 0x13b9;
	} else if (w == 10) {
		if (combat->chargeLevel < 1)
			snd = 0x13ba;
		else if (combat->chargeLevel == 4)
			snd = 0x13d3;
	}
	if (snd != 0)
		Sound_play(app->sound, snd, 0, 5, -1);
}

/* 0x0001e0e8: player attack state machine; returns 1 when the attack is resolved */
int Combat_playerAttackFrame(Combat *combat)
{
	Applet *app = combat->app;
	Hud *hud = app->hud;

	switch (combat->state) {
	case 0: {
		combat->shotToggle = 0;
		combat->f7c = 0;
		combat->state = 1;
		combat->nextShotTime = app->tickTime;
		combat->animEnd = 0;
		combat->totalDamage = 0;
		combat->armorDamageTotal = 0;
		combat->hitType = 0;
		combat->targetHpAfter = 0;
		combat->f39 = 0;
		combat->crit = 0;
		combat->fumble = 0;
		combat->hit = 0;
		combat->numMissiles = 0;
		combat->attackFrameUntil = 0;
		combat->hitFlags = 0;
		if (combat->weaponId == 10 && combat->chargeLevel == 0 && combat->surrounded)
			combat->chargeLevel = 4;
		Combat_calcTargetDist(combat);
		if (combat->targetType == 2) {
			combat->bloodColor = Combat_getBloodColor(combat->target);
			return 0;
		}
		/* ammo warnings (non-monster targets only, as in the original) */
		int ammoType = combat->weapon->ammoType;
		if (ammoType == 0)
			return 0;
		int ammo = combat->player->ammo[ammoType];
		int use = combat->weapon->ammoUsage;
		uint32_t msg;
		if (ammo < use)
			msg = MOD_STR_LAST_SHOT;
		else if (ammo < use * 2)
			msg = MOD_STR_1_SHOT_LEFT;
		else if (ammo < use * 3)
			msg = MOD_STR_2_SHOTS_LEFT;
		else if (ammo < use * 4)
			msg = MOD_STR_3_SHOTS_LEFT;
		else
			return 0;
		Hud_addMessage(hud, modStr(msg), 0);
		return 0;
	}

	case 1:
		break;

	case 2: {
		combat->shotToggle = 0;
		if (combat->fumble) {
			Hud_addMessage(hud, modStr(MOD_STR_OOPS), 0);
			Player_addStatusEffect(combat->player, 0, 0, 5);
			Player_updateStats(combat->player);
			Sound_play(combat->app->sound, 0x143f, 0, 3, -1);
			combat->fumble = 0;
		}
		if (combat->resolveTime != 0) {
			if (combat->app->tickTime <= combat->resolveTime)
				return 0;
			combat->resolveTime = 0;
			combat->state = 3;
			return 1;
		}

		if (combat->totalDamage == 0) {
			playerNoDamageMessage(combat, hud);
		} else if (combat->targetType == 2) {
			char *buf = Hud_getMessageBuffer(hud, 0);
			int total = combat->totalDamage;
			if (total > 0)
				App_snprintf(buf, 0x40, modStr(MOD_STR_DAMAGE), combat->crit ? modStr(MOD_STR_CRIT_PREFIX) : "",
				             total);
			else if (total < 0)
				App_snprintf(buf, 0x40, modStr(MOD_STR_HEALED), -total);
			if (CombatEntity_getHealth(&combat->target->monster->ce) > 0) {
				Hud_finishMessageBuffer(hud);
				EntityMonster *mon = combat->target->monster;
				if (mon->petrifyTurns == 0) {
					int snd = EntityMonster_getRandomSound(mon, 6);
					if (snd != 0)
						Sound_play(combat->app->sound, snd, 0, 2, -1);
				}
				if (combat->crit)
					Hud_randomBark(hud, 0, 0);
			} else {
				int len = (int)strlen(buf);
				App_snprintf(buf + len, 0x40 - len, modStr(MOD_STR_DIED_SUFFIX), combat->target->name);
				Hud_finishMessageBuffer(hud);
				if (combat->targetTileDist == 1)
					Hud_randomBark(hud, 2, 0);
				else if (combat->targetTileDist <= 3)
					Hud_randomBark(hud, 3, 0);
				Entity_died(combat->target, 1);
			}
		} else if (combat->hitType != 0) {
			Entity_died(combat->target, 1);
		}

		playerSplash(combat);
		combat->resolveTime = combat->app->tickTime;
		if (combat->targetType == 2 && CombatEntity_getHealth(&combat->target->monster->ce) > 0)
			return 0;
		return 1;
	}

	case 3:
		return 1;

	default:
		return 0;
	}

	/* state 1: shots */
	if (combat->resolveTime != 0) {
		if (app->tickTime <= combat->nextShotTime)
			return 0;
		if (combat->shotToggle)
			combat->shotToggle = 0;
		if (app->tickTime <= combat->resolveTime)
			return 0;
		combat->state = 2;
		combat->resolveTime = 0;
		return 0;
	}

	if (combat->animEnd == 0 && combat->nextShotTime < app->tickTime &&
	    (combat->shotsLeft > 0 || combat->shotToggle != 0)) {
		combat->shotToggle = combat->shotToggle == 0;
		if (combat->shotToggle) {
			combat->animEnd = app->tickTime;
			int frameUntil = app->tickTime;
			if (combat->chargeLevel < 1)
				frameUntil += combat->weapon->shotDelay * 10;
			combat->attackFrameUntil = frameUntil;
			combat->shotDamage = 0;

			if (combat->weaponId == 8 && combat->render->drunkTarget != 0) {
				/* drunk with the petrify weapon: may fumble */
				int roll = Random_nextByte(&app->random);
				int threshold = combat->render->drunkTarget + 0x98;
				int acc = CombatEntity_getAccuracy(&combat->player->ce);
				if (acc > CombatEntity_getAccuracy(&combat->player->baseCe)) {
					acc = CombatEntity_getAccuracy(&combat->player->ce);
					threshold -= (acc - CombatEntity_getAccuracy(&combat->player->baseCe)) * 5;
				}
				if (roll < threshold) {
					combat->fumble = 1;
					Sound_play(combat->app->sound, 0x143e, 0, 3, -1);
				}
			} else if (combat->weaponId == 9) {
				combat->hitFlags |= 0x20;
				launchSpellPattern(combat);
			}

			if (combat->fumble == 0 && combat->targetType == 2) {
				playerShotVsMonster(combat);
			} else if (combat->fumble == 0) {
				combat->hitType = Combat_targetInWeaponRange(combat, combat->target);
				int dmin = combat->weapon->dmgMin;
				combat->shotDamage = (int16_t)((combat->weapon->dmgMax - dmin) / 2 + dmin);
			}
			combat->totalDamage += combat->shotDamage;
			combat->armorDamageTotal += combat->armorDamage;
			combat->shotsLeft--;
		}

		/* next shot: haste shortens the delay */
		int delay;
		if (combat->player->effects[4].turns == 0)
			delay = combat->shotToggle ? combat->weapon->shotDelay * 10 : combat->weapon->shotDelay * 5;
		else
			delay = combat->shotToggle ? combat->weapon->shotDelay * 5 : combat->weapon->shotDelay * 2;
		combat->nextShotTime = combat->app->tickTime + delay;
		if (combat->chargeLevel > 0) {
			combat->nextShotTime = combat->app->tickTime;
			if (combat->chargeLevel == 4)
				combat->nextShotTime += 0x4b;
		}
	}

	if (combat->animEnd != 0 && combat->app->tickTime > combat->animEnd) {
		playerWeaponSound(combat);
		Combat_launchProjectile(combat);
		combat->animEnd = 0;
	}
	Combat_resolveProjectiles(combat);
	return 0;
}

/* ------------------------------------------------------------------ first-person weapon */

/* 0x0000cdb4: draw the player's weapon (bob offsets in 1/64 of the view) */
void Combat_drawWeapon(Combat *combat, int bobX, int bobY)
{
	Applet *app = combat->app;
	Player *player = app->player;
	int state = combat->canvas->state;

	if (state == 0xd || player->weapons == 0)
		return;

	int frame = 0;
	int flags = 0;
	if (app->tickTime < app->hud->arrowUntil && app->hud->arrowDir > 0)
		flags = 8; /* hurt flash */

	int weapon = player->ce.weapon;
	uint32_t pose = MOD_WEAPON_POSE + (uint32_t)weapon * 4;
	int px, py;
	if (combat->fumble == 0 && player->effects[0].turns == 0) {
		if (state == 4 && combat->attacker == NULL && combat->state != 2 && combat->shotToggle) {
			px = ModData_u8(&g_mod, pose + 2);
			py = ModData_u8(&g_mod, pose + 3) - 0x40;
			if (app->tickTime < combat->attackFrameUntil)
				frame = 1;
		} else {
			px = ModData_u8(&g_mod, pose + 0);
			py = ModData_u8(&g_mod, pose + 1) - 0x40;
		}
	} else {
		if (player->effects[0].turns != 0)
			flags |= 4;
		px = ModData_u8(&g_mod, pose + 2);
		py = ModData_u8(&g_mod, pose + 3) - 0x40;
		frame = 2;
	}

	if (weapon == 10) {
		if (combat->chargeLevel < 1) {
			if (combat->surrounded) {
				px += jitter(Random_nextByte(&app->random) & 3) * 2;
				py += jitter(Random_nextByte(&combat->app->random) & 3);
			}
		} else {
			px = 9;
			frame = 2;
			py = -4;
			if (combat->chargeLevel == 4 || (uint32_t)(GETUPTIMEMS() - combat->canvas->lastTurnTime) < 100) {
				px = jitter(Random_nextByte(&combat->app->random) & 3) * 5 + 9;
				py += jitter(Random_nextByte(&combat->app->random) & 3) * 3;
			}
		}
	}

	/* the renderer works in device pixels (s per game pixel in the HD mode) */
	int s = Brew_screenScale();
	int centerX = combat->canvas->viewCenterX * s;
	int scale = (Render_spriteRefW(combat->render) * 0x10000) / 0x8000;
	int ox = px + bobX;
	int oy = py + bobY;
	int x = centerX + ((scale * (ox - 0x20) * 0x100 + 0xff00) >> 16);
	int yOfs = (scale * (0x40 - oy) * 0x100 + 0xff00) >> 16;
	int y = combat->render->viewH - yOfs;
	int palette = 0;
	if (!Player_isVisible(player))
		palette = 9;

	int shape;
	weapon = player->ce.weapon;
	if (weapon == 7) {
		Render_draw2DSprite(combat->render, 0xd8, frame + 5, x, y, palette, flags);
		if (frame != 0)
			return;
		Weapon *w = &combat->weapons[weapon];
		if (player->ammo[w->ammoType] < w->ammoUsage)
			return;
		/* loaded: the glowing charge bobs about */
		int now = combat->app->tickTime;
		int a = now / 200;
		int b = now / 350;
		x = centerX + ((scale * (ox + jitter(a % 4) * 2 - 0x1a) * 0x100 + 0xff00) >> 16);
		y = combat->render->viewH - ((scale * (0x40 - (oy + jitter(b % 4) * 2 - 0x18)) * 0x100 + 0xff00) >> 16);
		flags &= ~8;
		Render_draw2DSprite(combat->render, player->ce.weapon + 0xd1, 0, x, y, palette, flags);
		int c = combat->app->tickTime / 200;
		frame = c % 4 + 1;
		palette = 7;
		shape = player->ce.weapon + 0xd1;
	} else if (weapon == 2 || weapon == 3) {
		Render_draw2DSprite(combat->render, weapon + 0xd1, frame, x, y, palette, flags);
		if (frame == 0) {
			frame = (combat->app->tickTime / 200) % 4 + 2;
			if (player->ce.weapon == 2) {
				x = centerX + ((scale * (ox - 0x13) * 0x100 + 0xff00) >> 16);
				yOfs = (scale * (0x40 - (oy - 0xd)) * 0x100 + 0xff00) >> 16;
				y = combat->render->viewH - yOfs;
			} else if (player->ce.weapon == 3) {
				x = centerX + ((scale * (ox - 0x13) * 0x100 + 0xff00) >> 16);
				y = combat->render->viewH - yOfs;
			}
		} else if (frame == 1) {
			frame = 6;
		}
		flags &= ~8;
		palette = 7;
		shape = player->ce.weapon + 0xd1;
	} else if (weapon == 4 || weapon == 5 || weapon == 6) {
		Weapon *w = &combat->weapons[weapon];
		if (player->ammo[w->ammoType] < w->ammoUsage)
			frame = 1; /* empty */
		shape = weapon + 0xd1;
	} else if (weapon == 0) {
		Render_draw2DSprite(combat->render, 0xd1, frame, x, y, palette, flags);
		int glow = (uint8_t)combat->canvas->glowPalette;
		palette = glow;
		if (glow != 0xff) {
			if (frame == 0) {
				Render_draw2DSprite(combat->render, player->ce.weapon + 0xd1, 3, x + 10 * s, y - 5 * s, glow, flags & ~8);
				return;
			}
			Render_draw2DSprite(combat->render, player->ce.weapon + 0xd1, frame + 3, x, y, glow, flags & ~8);
		}
		if (frame != 1)
			return;
		Render_draw2DSprite(combat->render, player->ce.weapon + 0xd1, 2, x - 5 * s, y, 7, flags & ~8);
		return;
	} else {
		shape = weapon + 0xd1;
	}
	Render_draw2DSprite(combat->render, shape, frame, x, y, palette, flags);
}
