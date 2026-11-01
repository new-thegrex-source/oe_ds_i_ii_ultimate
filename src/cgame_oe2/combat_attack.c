/*
 * Combat.cpp (part 2): the attack state machines (player -> target,
 * monster -> player), missiles, impact effects, splash damage, status
 * effects inflicted by weapons, raise dead.
 *
 * Combat.state: 0 init, 1 firing shots, 2 resolve/report, 3 done.
 */
#include "game.h"

#define MOD_CORPSE_NAME_IDS 0x461b4u /* s16 [subtype*3 + parm]: name string id of an undead/raised monster */

#define MAX_ACTIVE_MISSILES 3

/* calcCombat result block: Combat.hitFlags .. resHitChance were one int[6] */
static void resLoad(Combat *c, int res[6])
{
	res[0] = (int)c->hitFlags;
	res[1] = c->rolledDamage;
	res[2] = c->perShotAdd;
	res[3] = c->resTileDist;
	res[4] = c->resCritChance;
	res[5] = c->resHitChance;
}

static void resStore(Combat *c, const int res[6])
{
	c->hitFlags = (uint32_t)res[0];
	c->rolledDamage = res[1];
	c->perShotAdd = res[2];
	c->resTileDist = res[3];
	c->resCritChance = res[4];
	c->resHitChance = res[5];
}

static Sprite *entSprite(Entity *e) { return &g_app->render->sprites[(e->info & 0xffff) - 1]; }
static Line *entLine(Entity *e) { return &g_app->render->lines[(e->info & 0xffff) - 1]; }
static int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

static void setSpriteFrame(Sprite *s, int frame) { s->info = (s->info & ~0xf00u) | ((uint32_t)frame << 8); }

/* ------------------------------------------------------------------ setup */

/* 0x0002e8d4 */
void Combat_start(Combat *combat, Entity *attacker, Entity *target, int tx, int ty)
{
	Player *player = g_app->player;

	combat->attacker = attacker;
	combat->target = target;
	combat->targetY = ty;
	combat->targetX = tx;

	if (!(attacker && target))
		Player_calcStats(player);
	if (attacker)
		attacker->info |= 0x10000000;

	int playerInvolved;
	if (target) {
		target->info |= 0x10000000;
		playerInvolved = !attacker && target->def->eType == 2;
	} else {
		playerInvolved = 1;
	}
	if (playerInvolved) {
		player->lastCombatTurn = player->totalMoves;
		player->inCombat = 1;
	}

	if (attacker) {
		combat->attackerType = attacker->def->eType;
		combat->attackerSubtype = attacker->def->eSubType;
	} else {
		combat->attackerSubtype = 0;
		combat->attackerType = 0;
	}
	if (target) {
		combat->targetType = target->def->eType;
		combat->targetSubtype = target->def->eSubType;
	} else {
		combat->targetSubtype = 0;
		combat->targetType = 0;
	}

	if (combat->attacker == NULL) {
		combat->weaponId = player->ce.weapon;
	} else {
		int second = Combat_getMonsterAttackField(combat, combat->attacker->def, 1);
		int w = combat->attacker->monster->ce.weapon;
		combat->weaponId = w;
		if (w == second)
			combat->animFrame = 4;
		else if (w == 0xe || w == 0x22)
			combat->animFrame = 3;
		else
			combat->animFrame = 2;
	}

	combat->f20 = 0x30;
	combat->weapon = &combat->weapons[combat->weaponId];
	combat->state = 0;
	combat->timer = 0;
	combat->nextShotTime = 0;
	combat->toggle = 0;
	combat->bonusFlag = combat->weaponId == 6;
	combat->shotsLeft = combat->weapons[combat->weaponId].numShots;
	if (combat->weaponId == 0x1d && !(Random_nextInt(&g_app->random) & 1))
		combat->shotsLeft = 1;
	if (combat->attacker && combat->attacker->def->eSubType == 5 && (combat->attacker->info & 0x800000))
		combat->shotsLeft = 1;

	if (g_app->canvas->state != 4)
		combat->savedAngle = g_app->canvas->viewAngle;
	Canvas_setState(g_app->canvas, 4);
}

/* 0x00035780: one combat tick; returns 1 while the attack is still running */
int Combat_runFrame(Combat *combat)
{
	int done = combat->attacker == NULL ? Combat_playerAttackFrame(combat) : Combat_monsterAttackFrame(combat);
	return 1 - done;
}

/* 0x00009518 */
void Combat_calcTargetDist(Combat *combat)
{
	combat->targetDistSq = Entity_distSqFrom(combat->target, g_app->game->viewX, g_app->game->viewY);
	combat->targetTileDist = Combat_WorldDistToTileDist(combat, combat->targetDistSq);
}

/* 0x0000914c: can the player's weapon reach a non-monster target (breakables)? */
int Combat_targetInWeaponRange(Combat *combat, Entity *entity)
{
	Player *player = g_app->player;
	WeaponInfo *wp = &combat->weapons[player->ce.weapon];
	int dist = Combat_WorldDistToTileDist(combat, Entity_distSqFrom(entity, player->ce.x, player->ce.y));
	int maxRange = wp->rangeMax;

	if (maxRange > 1)
		maxRange += 2;
	if (wp->rangeMin > dist || dist > maxRange)
		return 0;
	if (entity == NULL || !(entity->info & 0x40000))
		return 0;
	if (entity->def->eType != 10)
		return 1;
	return (entity->def->parm & (int)arm_bit(player->ce.weapon)) != 0;
}

/* ------------------------------------------------------------------ player attack */

static void playerAmmoWarning(Combat *combat)
{
	int ammoType = combat->weapon->ammoType;
	if (ammoType == 0)
		return;
	int ammo = g_app->player->ammo[ammoType];
	int use = combat->weapon->ammoUsage;
	int msg;

	Canvas_resetEvents(g_app->canvas);
	if (use > ammo) {
		msg = 0x50;
	} else if (ammo < use * 2) {
		Canvas_addTextArgInt(g_app->canvas, 1);
		msg = 0x51;
	} else {
		if (use * 3 > ammo)
			Canvas_addTextArgInt(g_app->canvas, 2);
		else if (ammo < use * 4)
			Canvas_addTextArgInt(g_app->canvas, 3);
		else
			return;
		msg = 0x52;
	}
	Hud_addMessage(g_app->hud, msg);
}

/* Fire one shot of the player's weapon: roll, stats, damage bookkeeping. */
static void playerShot(Combat *combat)
{
	Applet *app = g_app;
	Player *player = app->player;

	combat->shotStart = app->time;
	combat->shotEnd = app->time + combat->weapon->shotDelay * 10;
	combat->shotDamage = 0;

	if (combat->targetType == 2) {
		Entity *target = combat->target;
		int res[6];

		if (target->info & 0x1000000)
			combat->hitFlags |= 0x100;
		if (target->monster->flags & 8)
			combat->hitFlags |= 0x400;
		if (target->monster->flags & 0x7000)
			combat->hitFlags |= 0x800;
		resLoad(combat, res);
		CombatEntity_calcCombat(&player->ce, &target->monster->ce, res);
		resStore(combat, res);

		int counted = 0;
		if (combat->hitFlags & 3) {
			player->critCounter = 0;
			if (combat->hitFlags & 2) {
				combat->crit = 1;
				combat->hitType = 2;
			} else {
				combat->hitType = 1;
			}
			combat->hit = 1;
			combat->shotDamage = combat->rolledDamage;
			combat->targetHpAfter = CombatEntity_getHealth(&combat->target->monster->ce) - combat->shotDamage;
			if (combat->shotDamage > 0 && combat->hit) {
				app->statDamageBySubtype[combat->target->def->eSubType] += combat->shotDamage;
				app->statDamageDealt += combat->shotDamage;
				app->statDamageByWeapon[combat->weaponId] += combat->shotDamage;
				app->statHitsByWeapon[combat->weaponId]++;
				if (combat->shotDamage > app->statBiggestHit)
					app->statBiggestHit = combat->shotDamage;
				if (player->equippedRing == 5 && (Random_nextInt(&app->random) & 0xff) < 0x80)
					combat->bonusFlag = 1;
				counted = 1;
			}
		}
		if (!counted)
			app->statMissesByWeapon[combat->weaponId]++;
	} else {
		combat->hitType = Combat_targetInWeaponRange(combat, combat->target);
		int lo = Combat_getWeaponDamage(&combat->weapon->dmgMin, player->ce.weapon);
		int lo2 = Combat_getWeaponDamage(&combat->weapon->dmgMin, player->ce.weapon);
		combat->shotDamage = lo + (combat->weapon->dmgMax - lo2) / 2;
	}

	combat->totalDamage += combat->shotDamage;
	combat->accum += combat->perShotAdd;
	combat->shotsLeft--;
}

/* Swing / impact sound of the player's weapon when the shot leaves. */
static void playerShotSound(Combat *combat)
{
	Applet *app = g_app;
	int w = combat->weaponId;
	uint32_t bit = arm_bit(w);
	int snd;

	if (w == 0) {
		snd = 0x72;
	} else if (bit & 0x8e) { /* weapons 1, 2, 3, 7 */
		int kind;
		if (combat->hitType == 0) {
			int dx = app->game->viewX - combat->targetX;
			int dy = app->game->viewY - combat->targetY;
			int d = mul32(dy, dy) + mul32(dx, dx);
			if (d > 0x1000 || (bit & 0xc))
				kind = 0;
			else if (combat->targetType == 2)
				kind = combat->targetSubtype == 2 ? 1 : 2;
			else if (combat->targetType == 10)
				kind = 1;
			else
				kind = 3;
		} else if (w == 2 && app->player->fc0 == 0) {
			kind = -1; /* goes straight to the generic sound list with kind 0 */
		} else if (combat->targetType == 2 || combat->targetType == 9) {
			int sub = combat->targetSubtype;
			kind = (sub == 2 || sub == 3 || sub == 15) ? 1 : 2;
		} else {
			kind = combat->targetSubtype == 16 ? 2 : 1;
		}

		if (kind >= 0 && w == 1) {
			static const int16_t axe[4] = { 0x70, 0x6e, 0x6d, 0x6e };
			snd = axe[kind];
		} else if (kind >= 0 && w == 7) {
			static const int16_t hammer[4] = { 0x68, 0x69, 0x66, 0x67 };
			snd = hammer[kind];
		} else {
			static const int16_t blade[4] = { 0x63, 0x71, 0x62, 0x6f };
			snd = blade[kind < 0 ? 0 : kind];
		}
	} else if (bit & 0x60) {
		snd = 0x61;
	} else if (w == 10) {
		snd = 0x6a;
	} else if (w == 8) {
		snd = 0x6c;
	} else if (w == 9) {
		snd = 0x6b;
	} else {
		return;
	}
	Sound_play(app->sndSys, snd, 0, 4, -1);
}

/* State 2 of the player attack: report the result. Returns the frame result. */
static int playerReport(Combat *combat)
{
	Applet *app = g_app;
	Player *player = app->player;
	Hud *hud = app->hud;

	Game_wakeNearbyMonsters(app->game, app->canvas->destX, app->canvas->destY);

	if (combat->totalDamage == 0) {
		if (player->statusEffects[9].turns != 0)
			goto done;
		if (combat->targetType == 2) {
			Entity *target = combat->target;
			int mult = target->def->parm == 2 ? 400 : target->def->parm == 1 ? 0x140 : 0x100;
			int pAcc = CombatEntity_getAccuracy(&player->ce);
			int tAcc = CombatEntity_getAccuracy(&target->monster->ce);
			int tDef = CombatEntity_getDefense(&target->monster->ce);
			int margin = pAcc - (mul32((tAcc + tDef / 4) << 8, mult) >> 16);
			int dodged = 0;

			if (!((target->monster->flags & 8) &&
			      (combat->targetSubtype == 14 || combat->targetSubtype == 13))) {
				int roll = Random_nextInt(&app->random) & 0xff;
				if (margin < 0)
					dodged = roll >= 12;
				else
					dodged = roll < 12;
			}

			uint32_t flags = combat->hitFlags;
			int msg;
			if (flags & 0x80) {
				msg = 0x4c;
			} else if (flags & 0x200) {
				msg = 0x53;
			} else if (flags & 1) {
				goto done;
			} else if (dodged && !(target->monster->monsterEffects & 2) && !(flags & 0x800)) {
				Canvas_resetEvents(app->canvas);
				Canvas_addTextArgStr(app->canvas, &target->name);
				msg = 0x54;
			} else {
				msg = 0x56;
			}
			Hud_addMessage(hud, msg);
		} else if (combat->weaponId != 10) {
			Hud_addMessage(hud, 0x58);
		}
		goto done;
	}

	if (combat->targetType != 2) {
		if (combat->hitType != 0)
			Entity_diedDefault(combat->target);
		goto done;
	}

	String *out = Hud_getMessageBufferDefault(hud);
	if (combat->totalDamage > 0) {
		if (combat->hitFlags & 0x800)
			Text_getString(app->canvas, 0, 0x59, out);
		else if (combat->crit)
			Text_getString(app->canvas, 0, 0x5b, out);
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgInt(app->canvas, combat->totalDamage);
		Text_getString(app->canvas, 0, 0x5c, out);
	} else if (combat->totalDamage < 0) {
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgInt(app->canvas, -combat->totalDamage);
		Text_getString(app->canvas, 0, 0x5d, out);
	}

	if (CombatEntity_getHealth(&combat->target->monster->ce) <= 0) {
		Hud_finishMessageBuffer(hud);
		if (combat->targetTileDist == 1)
			Hud_randomBark(hud, 1);
		else if (combat->targetTileDist <= 3)
			Hud_randomBark(hud, 2);
		Entity_diedDefault(combat->target);
	} else {
		Hud_finishMessageBuffer(hud);
		EntityMonster *mon = combat->target->monster;
		if (!(mon->monsterEffects & 0x1f)) {
			int snd = EntityMonster_getRandomSound(mon, 6);
			if (snd != -1)
				Sound_play(app->sndSys, snd, 0, 2, -1);
		}
		if (combat->crit)
			Hud_randomBark(hud, 0);
	}
	if (player->ce.weapon == 0 && combat->targetType == 2)
		Hud_randomBark(hud, 0xb);

done:
	combat->timer = app->time;
	if (combat->targetType == 2 && CombatEntity_getHealth(&combat->target->monster->ce) > 0)
		return 0;
	return 1;
}

/* 0x0002f1c8: returns 1 when the attack is over */
int Combat_playerAttackFrame(Combat *combat)
{
	Applet *app = g_app;

	switch (combat->state) {
	case 0:
		combat->toggle = 0;
		combat->effectMode = 0;
		combat->state = 1;
		combat->nextShotTime = app->time;
		combat->shotStart = 0;
		combat->totalDamage = 0;
		combat->accum = 0;
		combat->hitType = 0;
		combat->targetHpAfter = 0;
		combat->done = 0;
		combat->crit = 0;
		combat->hit = 0;
		combat->numActiveMissiles = 0;
		combat->shotEnd = 0;
		combat->hitFlags = 0;
		Combat_calcTargetDist(combat);
		if (combat->targetType == 2) {
			combat->bloodColor = Combat_getBloodColor(combat, combat->target);
			return 0;
		}
		playerAmmoWarning(combat);
		return 0;

	case 1:
		if (combat->timer != 0) {
			if (app->time <= combat->nextShotTime)
				return 0;
			if (combat->toggle)
				combat->toggle = 0;
			if (app->time > combat->timer) {
				combat->state = 2;
				combat->timer = 0;
			}
			return 0;
		}

		if (combat->shotStart == 0) {
			if (app->time <= combat->nextShotTime)
				goto resolve;
			if (combat->shotsLeft > 0) {
				combat->toggle = combat->toggle == 0;
			} else {
				if (combat->toggle == 0)
					goto resolve;
				combat->toggle = 0;
			}
			if (combat->toggle)
				playerShot(combat);

			int delay = combat->weapon->shotDelay;
			if (app->player->statusEffects[3].turns > 0) /* haste */
				combat->nextShotTime = app->time + (combat->toggle ? delay * 5 : delay * 2);
			else
				combat->nextShotTime = app->time + (combat->toggle ? delay * 8 : delay * 3);
			if (combat->shotStart == 0)
				goto resolve;
		}

		if (app->time > combat->shotStart) {
			playerShotSound(combat);
			Combat_launchProjectile(combat);
			combat->shotStart = 0;
		}
	resolve:
		Combat_resolveProjectiles(combat);
		return 0;

	case 2:
		combat->toggle = 0;
		if (combat->timer != 0) {
			if (app->time > combat->timer) {
				combat->state = 3;
				combat->timer = 0;
				return 1;
			}
			return 0;
		}
		return playerReport(combat);

	case 3:
		return 1;
	default:
		return 0;
	}
}

/* ------------------------------------------------------------------ monster attack */

/* State 2 of a monster attack: apply the accumulated result to the player. */
static void monsterReport(Combat *combat)
{
	Applet *app = g_app;
	Player *player = app->player;
	Entity *attacker = combat->attacker;
	int msg;

	combat->toggle = 0;
	attacker->info &= ~0x10000u;

	if (combat->weaponId == 0x13) {
		Combat_raiseDead(attacker, attacker->monster->goal.param);
		return;
	}
	if (combat->target != NULL) {
		Entity_diedDefault(combat->target);
		return;
	}

	Sprite *spr = entSprite(attacker);
	Game *game = app->game;
	/* Game.viewZ holds the current view angle (Game_updatePlayerVars) */
	app->hud->damageDir = Dir_relativeQuadrant(0, game->viewX, game->viewY, game->viewZ, spr->x, spr->y);
	app->hud->damageDirUntil = app->time + 500;

	if (combat->hit == 0) {
		app->hud->damageFlash = 0;
		int chance = player->ce.level * 25 - 25;
		if ((Random_nextInt(&app->random) & 0xff) < chance) {
			msg = 0x55;
		} else {
			Canvas_resetEvents(app->canvas);
			Canvas_addTextArgStr(app->canvas, &attacker->name);
			msg = 0x57;
		}
		Hud_addMessage(app->hud, msg);
		return;
	}

	CombatEntity_addArmor(&player->ce, -combat->accum);
	if (combat->totalDamage <= 0) {
		app->hud->damageFlash = 0;
		combat->totalDamage = 0;
	} else {
		int diff = game->difficulty;
		if (diff != 1) {
			int bonus = diff == 2 ? app->canvas->mapId : app->canvas->mapId * 2;
			combat->totalDamage += bonus + (combat->totalDamage >> 1);
		}
		Player_takeDamage(player, combat->totalDamage, attacker);
	}

	int effect = combat->weapon->effectType;
	if (effect == 0x1b && (Random_nextInt(&app->random) & 1)) { /* poison */
		if (player->statusEffects[2].turns > 0) {
			msg = 0x5e;
		} else {
			if (!Player_addStatusEffect(player, 10, -6, 5))
				return;
			Player_updateStats(player);
			Player_hurtFeedback(player, attacker != NULL);
			msg = 0x5f;
		}
	} else if (combat->weapon->effectType == 0x1d) { /* confusion ("Turning!") */
		if (player->god || player->statusEffects[2].turns > 0) {
			msg = 0x61;
		} else {
			Game_executeTile0(game, 0, 2, 0xf000);
			if (!Player_addStatusEffect(player, 4, 5, 10))
				return;
			Render *render = app->render;
			if (render->drunkLevel == 0 && render->drunkTarget == 0)
				render->oscTime = (int32_t)GETUPTIMEMS();
			render->drunkTarget += 10;
			Player_updateStats(player);
			CombatEntity_setHealth(&attacker->monster->ce,
			                       CombatEntity_getHealth(&attacker->monster->ce) + combat->totalDamage * 2);
			msg = 0x62;
		}
	} else if (combat->weapon->effectType == 0x17) { /* drain */
		if (player->god || player->statusEffects[2].turns > 0) {
			msg = 99;
		} else {
			CombatEntity *mce = &attacker->monster->ce;
			int amount = (attacker->monster->monsterEffects & 4) ? 3 : 5;
			Player_addStatusEffect(player, 0xd, -amount, amount);
			Player_updateStats(player);
			int gain = amount - 2;
			int cap = CombatEntity_getDefense(&combat->monsterStats[attacker->def->parm + 0x1b]) + 0x2d;
			if (CombatEntity_getDefense(mce) < cap) {
				CombatEntity_setStrength(mce, CombatEntity_getStrength(mce) + gain);
				CombatEntity_setDefense(mce, CombatEntity_getDefense(mce) + gain);
				CombatEntity_setAccuracy(mce, CombatEntity_getAccuracy(mce) + gain);
			}
			CombatEntity_setHealth(mce, CombatEntity_getHealth(mce) + combat->totalDamage);
			msg = 100;
		}
	} else {
		if (combat->totalDamage > 0)
			return;
		msg = 0x65;
	}
	Hud_addMessage(app->hud, msg);
}

/* Start one monster shot: animation frame, timing and the hit roll. */
static void monsterShot(Combat *combat)
{
	Applet *app = g_app;
	Entity *attacker = combat->attacker;
	Sprite *spr = entSprite(attacker);
	int sub = attacker->def->eSubType;

	if (sub == 5) {
		if (attacker->info & 0x800000) {
			combat->animFrame = 4;
		} else {
			combat->animFrame = combat->shotsLeft == 1 ? 2 : 3;
			spr->info ^= 0x1000000;
		}
	} else if (sub == 11) {
		spr->info ^= 0x1000000;
	} else if (sub == 7 || !(Random_nextInt(&app->random) & 1)) {
		spr->info &= ~0x1000000u;
	} else {
		spr->info |= 0x1000000;
	}

	if (combat->weaponId == 0x17)
		Canvas_startShake(app->canvas, 1000, 3, 0);
	if (attacker->monster->goal.type != 6)
		setSpriteFrame(spr, combat->animFrame);

	combat->shotStart = app->time;
	attacker->monster->frameTime = app->time;
	sub = attacker->def->eSubType;
	if (sub == 7 || sub == 12) {
		attacker->monster->frameTime += 150;
		combat->shotStart += 300;
	} else if (sub == 14 || sub == 13) {
		attacker->monster->frameTime += 350;
		combat->shotStart += 350;
	} else if (combat->weaponId == 0xe || combat->weaponId == 0x22) {
		attacker->monster->frameTime -= 1;
		combat->shotStart -= 1;
	} else {
		attacker->monster->frameTime += 150;
		combat->shotStart += 150;
	}

	if (combat->weapon->dmgMax > 0) {
		if (combat->target != NULL) {
			combat->hitType = 1;
			combat->shotDamage = 1;
		} else {
			Player *player = app->player;
			int res[6];
			resLoad(combat, res);
			CombatEntity_calcCombat(&attacker->monster->ce, &player->ce, res);
			resStore(combat, res);
			if (player->statusEffects[1].amount > 0) { /* blocked by a shield charge */
				combat->hitFlags = 0;
				if (--player->statusEffects[1].amount == 0)
					Player_removeBuff(player, 1);
				Player_updateStats(player);
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
		}
		combat->totalDamage += combat->shotDamage;
		combat->accum += combat->perShotAdd;
	}
	combat->shotsLeft--;
}

/* 0x0002be94: returns 1 when the attack is over */
int Combat_monsterAttackFrame(Combat *combat)
{
	Applet *app = g_app;

	switch (combat->state) {
	case 0:
		combat->state = 1;
		combat->toggle = 0;
		combat->nextShotTime = app->time;
		combat->shotStart = 0;
		combat->totalDamage = 0;
		combat->accum = 0;
		combat->hitType = 0;
		combat->hitFlags = 0;
		combat->done = 0;
		combat->crit = 0;
		combat->hit = 0;
		return 0;
	case 1:
		break;
	case 2:
		monsterReport(combat);
		combat->state = 3;
		return 1;
	case 3:
		return 1;
	default:
		return 0;
	}

	Entity *attacker = combat->attacker;

	/* walk-in melee: step next to the player first */
	if (combat->weaponId == 0xe || combat->weaponId == 0x22) {
		Entity *door = app->game->watchEntity;
		if (door)
			Game_useDoor(app->game, 0x10, Render_getLineIndex(entLine(door)), 1);

		Sprite *spr = entSprite(attacker);
		int sx = spr->x >> 6, sy = spr->y >> 6;
		int px = app->player->ce.x >> 6, py = app->player->ce.y >> 6;
		if (sx == px)
			sy = sy < py ? py - 1 : py + 1;
		else
			sx = sx < px ? px - 1 : px + 1;
		int destY = sy * 64 + 32;
		attacker->monster->destX = sx * 64 + 32;
		attacker->monster->destY = destY;
		if (spr->x != attacker->monster->destX || spr->y != destY) {
			setSpriteFrame(spr, 2);
			app->game->monstersMoving = 1;
			attacker->monster->goal.flags |= 2;
			return 0;
		}
		Game_unlinkEntity_43710(app->game, attacker);
		Game_linkEntity(app->game, attacker, spr->x >> 6, spr->y >> 6);
		attacker->monster->goal.flags &= ~2;
	}

	if (combat->timer != 0 && combat->numActiveMissiles == 0) {
		if (app->time <= combat->nextShotTime)
			return 0;
		setSpriteFrame(entSprite(attacker), 0);
		attacker->monster->frameTime = 0;
		if (app->time > combat->timer && app->time > 0) {
			combat->state = 2;
			combat->timer = 0;
			attacker->monster->frameTime = 0;
		}
		return 0;
	}

	if (combat->shotStart == 0) {
		if (app->time <= combat->nextShotTime)
			goto resolve;
		int nextShot = combat->nextShotTime;
		if (combat->shotsLeft > 0) {
			combat->toggle = combat->toggle == 0;
		} else {
			if (combat->toggle == 0)
				goto resolve;
			combat->toggle = 0;
		}
		if (combat->toggle)
			monsterShot(combat);
		else if (nextShot > app->time)
			setSpriteFrame(entSprite(attacker), 0);

		combat->nextShotTime = app->time + combat->weapon->shotDelay * 10;
		if (combat->shotStart == 0)
			goto resolve;
	}

	if (app->time > combat->shotStart) {
		int second = Combat_getMonsterAttackField(combat, attacker->def, 1);
		int snd = EntityMonster_getSound(attacker->monster, combat->weaponId == second ? 5 : 4);
		if (snd != -1)
			Sound_play(app->sndSys, snd, 0, 4, -1);
		if (combat->weaponId == 0x17) {
			GameSprite *gs = Game_gspriteAllocAnim(app->game, 0xdf, attacker->monster->destX,
			                                       attacker->monster->destY, 0x20);
			gs->sprite->renderMode = 4;
		}
		Combat_launchProjectile(combat);
		combat->shotStart = 0;
	}
resolve:
	Combat_resolveProjectiles(combat);
	return 0;
}

/* ------------------------------------------------------------------ missiles */

/* 0x0000736c: flying projectile sprite from (sx,sy) to (dx,dy) at z 0x20 */
GameSprite *Combat_launchMissile(Combat *combat, int sx, int sy, int dx, int dy, int vertical, int flags)
{
	Applet *app = g_app;

	if (combat->numActiveMissiles == MAX_ACTIVE_MISSILES) {
		App_fatalError(app, "MAX_ACTIVE_MISSILES");
		return NULL;
	}
	GameSprite *gs = Game_gspriteAlloc(app->game, 0, combat->projSprite, 0, 4);
	combat->missiles[combat->numActiveMissiles++] = gs;
	gs->f10 = 0;
	gs->sprite->renderMode = flags;
	gs->start[0] = sx;
	gs->sprite->x = sx;
	gs->start[1] = sy;
	gs->sprite->y = sy;
	gs->start[2] = 0x20;
	gs->sprite->z = 0x20;
	gs->dest[2] = 0x20;
	gs->dest[1] = dy;
	gs->dest[0] = dx;

	int proj = combat->projSprite;
	int speed;
	if (proj == 0xe0 || proj == 0xee)
		speed = 0x100;
	else if (proj == 0xdc || proj == 0xdd || proj == 0xde || proj == 0xe4)
		speed = 0x180;
	else
		speed = 0x140;

	if (proj == 0) {
		gs->sprite->info |= 0x10000; /* invisible hit (melee) */
		return gs;
	}

	gs->vel[2] = 0;
	gs->vel[1] = 0;
	gs->vel[0] = 0;
	int len = 1;
	/* the flight path aims at the player's line; the side component only
	 * moves when the shot misses */
	int sideMoves = combat->hit == 0 && (combat->attacker != NULL || combat->targetType == 2);
	if (!vertical) {
		gs->vel[0] = speed;
		gs->dest[0] += app->canvas->viewStepX > 0 ? -1 : 1;
		int end = gs->dest[0], start = gs->start[0];
		if (end < start)
			len = start - end;
		else if (end > start)
			len = end - start;
		gs->travelTime = len * 1000 / speed;
		if (sideMoves) {
			gs->vel[1] = (gs->dest[1] - gs->start[1]) * 1000 / gs->travelTime;
		} else {
			gs->vel[1] = 0;
			gs->dest[1] = gs->start[1];
		}
		if (end < start)
			gs->vel[0] = -gs->vel[0];
	} else {
		gs->vel[1] = speed;
		gs->dest[0] += app->canvas->viewStepY > 0 ? -1 : 1;
		int end = gs->dest[1], start = gs->start[1];
		if (end < start)
			len = start - end;
		else if (end > start)
			len = end - start;
		gs->travelTime = len * 1000 / speed;
		if (sideMoves) {
			gs->vel[0] = (gs->dest[0] - gs->start[0]) * 1000 / gs->travelTime;
		} else {
			gs->vel[0] = 0;
			gs->dest[0] = gs->start[0];
		}
		if (end < start)
			gs->vel[1] = -gs->vel[1];
	}
	Render_relinkSprite(app->render, gs->sprite);
	return gs;
}

/* 0x00024ecc: pick the projectile sprite for the weapon and launch it */
void Combat_launchProjectile(Combat *combat)
{
	Applet *app = g_app;
	int mode = 0;

	switch (combat->weapon->effectType) {
	case 1: combat->projSprite = 0xe1; mode = 4; break;
	case 5: combat->projSprite = 0xe3; mode = 4; break;
	case 6:
		if (app->player->fc0 == 0) {
			combat->projSprite = 0xe2;
		} else {
			combat->dirty = 1;
			combat->projSprite = 0;
		}
		break;
	case 0xf: combat->projSprite = 0xe4; break;
	case 0x10:
	case 0x11:
	case 0x12:
	case 0x13: combat->projSprite = 0; mode = 1; break;
	case 0x14: combat->projSprite = 0xe6; mode = 4; break;
	case 0x16: combat->projSprite = 0xe0; mode = 4; break;
	case 0x18: combat->projSprite = 0xde; break;
	case 0x19: combat->projSprite = 0xdd; break;
	case 0x1a: combat->projSprite = 0xdc; break;
	case 0x1b: combat->projSprite = 0xe7; break;
	case 0x1c: combat->projSprite = 0xee; mode = 4; break;
	case 0x1d: combat->projSprite = 0xce; mode = 4; break;
	case 0x1e: combat->projSprite = 0xe5; break;
	default:
		combat->dirty = 1; /* nothing flies: resolve right away */
		return;
	}

	int sx, sy, tx, ty, vertical;
	if (combat->attacker == NULL) {
		int tt = combat->targetType;
		if (tt == 0 || tt == 12) {
			tx = combat->targetX;
			ty = combat->targetY;
		} else if (combat->target->info & 0x200000) {
			Line *ln = entLine(combat->target);
			tx = ln->x1 + (ln->x2 - ln->x1) / 2;
			ty = ln->y1 + (ln->y2 - ln->y1) / 2;
		} else {
			Sprite *spr = entSprite(combat->target);
			tx = spr->x;
			ty = spr->y;
		}
		Game *game = app->game;
		sx = game->viewX + game->viewStepX / 2;
		sy = game->viewY + game->viewStepY / 2;
		int dx = tx - sx, dy = ty - sy;

		if (dx != 0 && combat->hitType == 0 && tt == 2)
			ty += (Random_nextInt(&app->random) & 1) ? 25 : -25;
		if (dy != 0) {
			if (combat->hitType == 0 && combat->targetType == 2)
				tx += (Random_nextInt(&app->random) & 1) ? 25 : -25;
			vertical = 1;
		} else {
			vertical = 0;
		}
	} else {
		EntityMonster *mon = combat->attacker->monster;
		if (mon->target == NULL) {
			tx = app->game->viewX;
			ty = app->game->viewY;
		} else {
			Sprite *spr = entSprite(mon->target);
			tx = spr->x;
			ty = spr->y;
		}
		sx = mon->destX;
		sy = mon->destY;
		int effect = combat->weapon->effectType;
		int beam = effect >= 0x10 && effect <= 0x13;
		int dx = tx - sx, dy = ty - sy;

		if (dx < 0) {
			if (beam)
				sx = combat->f20 + tx - 1;
			if (combat->hitType != 0)
				tx = combat->f20 + tx;
			else
				ty += (Random_nextInt(&app->random) & 1) ? 25 : -25;
		} else if (dx > 0) {
			if (beam)
				sx = tx - combat->f20 - 1;
			if (combat->hitType != 0)
				tx -= combat->f20;
			else
				ty += (Random_nextInt(&app->random) & 1) ? 25 : -25;
		}
		if (dy < 0) {
			if (beam)
				sy = combat->f20 + ty - 1;
			if (combat->hitType != 0)
				ty = combat->f20 + ty;
			else
				tx += (Random_nextInt(&app->random) & 1) ? 25 : -25;
			vertical = 1;
		} else if (dy > 0) {
			if (beam)
				sy = ty - combat->f20 - 1;
			if (combat->hitType != 0)
				ty -= combat->f20;
			else
				tx += (Random_nextInt(&app->random) & 1) ? 25 : -25;
			vertical = 1;
		} else {
			vertical = 0;
		}
	}

	GameSprite *missile = Combat_launchMissile(combat, sx, sy, tx, ty, vertical, mode);
	if (missile && combat->bonusFlag && missile->travelTime > 0) {
		/* flaming bonus shot: a second flame sprite follows the missile low */
		GameSprite *fx = Game_gspriteAlloc(app->game, 0, 0xe0, 0, 4);
		Vec3_copy(fx->dest, missile->dest);
		Vec3_copy(fx->start, missile->start);
		int z = app->render->effFloor - 0x12;
		fx->start[2] = z;
		fx->dest[2] = z;
		fx->travelTime = missile->travelTime;
		fx->f10 = missile->travelTime;
		Vec3_copy(fx->vel, missile->vel);
		fx->sprite->x = fx->start[0];
		fx->sprite->y = fx->start[1];
		fx->sprite->z = fx->start[2];
		fx->sprite->renderMode = 4;
		fx->sprite->info |= 0x20000000;
		Render_relinkSprite(app->render, fx->sprite);
	}
	combat->dirty = 0;
}

/* 0x00044508: retire landed missiles (impact animation) and apply the result */
void Combat_resolveProjectiles(Combat *combat)
{
	Applet *app = g_app;

	for (int i = 0; i < combat->numActiveMissiles; i++) {
		GameSprite *gs = combat->missiles[i];
		if (gs->flags & 4) /* still flying */
			continue;

		int mode = 0;
		switch (combat->weapon->effectType) {
		case 1:
		case 0x18: combat->projSprite = 0xf3; mode = 4; break;
		case 5:
		case 0x14:
		case 0x1a: combat->projSprite = 0xf2; mode = 4; break;
		case 0xf: combat->projSprite = 0xf8; mode = 9; break;
		case 0x10: combat->projSprite = 0xf6; mode = 2; break;
		case 0x11: combat->projSprite = 0xf7; mode = 2; break;
		case 0x12:
		case 0x1e: combat->projSprite = 0xf8; mode = 2; break;
		case 0x13: combat->projSprite = 0xf9; mode = 2; break;
		case 0x15: combat->projSprite = 0xf5; mode = 2; break;
		case 0x16:
			if (Entity_distSqFrom(combat->attacker, app->game->destX, app->game->destY) <= 0x1000) {
				combat->projSprite = 0xf8;
				mode = 1;
			} else {
				combat->projSprite = 0;
			}
			break;
		case 0x19: combat->projSprite = 0xf5; mode = 6; break;
		case 0x1b: combat->projSprite = 0xfa; break;
		case 0x1c: combat->projSprite = 0xfc; mode = 4; break;
		case 0x1d: combat->projSprite = 0xfd; mode = 2; break;
		default: break;
		}

		int x = gs->dest[0], y = gs->dest[1], z = gs->dest[2];
		Game_gspriteDestroy(app->game, gs);
		if (combat->projSprite != 0 &&
		    (combat->attacker == NULL || (combat->target == NULL && combat->totalDamage != 0))) {
			GameSprite *impact = Game_gspriteAllocAnim(app->game, combat->projSprite, x, y, z);
			impact->sprite->renderMode = mode;
			if (combat->attacker != NULL) {
				Sprite *spr = entSprite(combat->attacker);
				if (spr->scale < 0x40)
					impact->sprite->scale = spr->scale;
				if (spr->info & 0x1000000)
					impact->sprite->info |= 0x1000000;
			}
		}

		for (int j = i + 1; j < combat->numActiveMissiles; j++)
			combat->missiles[j - 1] = combat->missiles[j];
		combat->numActiveMissiles--;
		combat->dirty = 1;
	}

	if (combat->dirty) {
		if (combat->target == NULL)
			Combat_finishMonsterAttack(combat);
		else
			Combat_finishPlayerAttack(combat);
		combat->done = 1;
		combat->dirty = 0;
	}

	if (combat->numActiveMissiles == 0) {
		if (combat->hitType == 0)
			combat->done = 1;
		else if (combat->done == 0)
			return;
		if (combat->shotsLeft == 0 && combat->toggle == 0)
			combat->timer = app->time;
	}
}

/* ------------------------------------------------------------------ results */

/* Heal a monster hit by a negative-damage attack. */
static void healTarget(Combat *combat)
{
	if (combat->totalDamage >= 0)
		return;
	CombatEntity *ce = &combat->target->monster->ce;
	if (CombatEntity_getHealth(ce) - combat->totalDamage > CombatEntity_getMaxHealth(ce))
		combat->totalDamage = -(CombatEntity_getMaxHealth(ce) - CombatEntity_getHealth(ce));
	CombatEntity_setHealth(ce, CombatEntity_getHealth(ce) - combat->totalDamage);
	combat->target->monster->flags &= 0x8fff;
}

/* Player hit a monster: shield-spell reflection, weapon effects, damage,
 * fear ring, splash and knockback. */
static void playerHitMonster(Combat *combat)
{
	Applet *app = g_app;
	Player *player = app->player;
	Entity *target = combat->target;

	if (combat->totalDamage <= 0) {
		healTarget(combat);
		return;
	}

	if ((combat->targetSubtype == 14 || combat->targetSubtype == 13) && combat->weaponId != 0 &&
	    (target->monster->flags & 8)) {
		/* the shielded boss reflects magic */
		combat->totalDamage = 0;
		if (player->statusEffects[1].amount == 0) {
			int changed = 0;
			int reflected = 1;
			if (combat->weaponId == 8) {
				if (player->statusEffects[2].turns > 0)
					Hud_addMessage(app->hud, 0x66);
				else
					changed = Player_addStatusEffect(player, 0xb, -6, 3);
			} else if (combat->weaponId == 9) {
				if (player->statusEffects[2].turns > 0)
					Hud_addMessage(app->hud, 0x60);
				else
					changed = Player_addStatusEffect(player, 10, -6, 5);
			} else if (combat->weaponId == 10) {
				changed = Player_addStatusEffect(player, 9, 0, 3);
			} else {
				Hud_addMessageFlags(app->hud, 0x67, 3);
				reflected = 0;
			}
			if (reflected) {
				Hud_addMessageFlags(app->hud, 0x68, 3);
				if (changed)
					Player_updateStats(player);
			}
			entSprite(target)->renderMode = 10;
			Particles_spawnBurst(combat, combat->targetTileDist, (int)0xff009fef, 0x20);
		} else {
			if (--player->statusEffects[1].amount == 0)
				Player_removeBuff(player, 1);
			Player_updateStats(player);
		}
		if (combat->totalDamage <= 0) {
			healTarget(combat);
			return;
		}
	}

	Combat_applyWeaponEffect(combat);
	if ((combat->weaponId == 10 || combat->weaponId == 8) &&
	    !(combat->targetSubtype == 12 && (target->monster->flags & 0x100))) {
		Entity_takeDamage(target, 0);
		combat->totalDamage = 0;
	} else {
		Entity_takeDamage(target, combat->totalDamage);
		Particles_spawnBloodDefault(combat, combat->targetTileDist, combat->bloodColor, -1);
	}

	if (CombatEntity_getHealth(&target->monster->ce) <= 0)
		return;

	int knock = 0;
	if (combat->bonusFlag) {
		knock = 2;
	} else if (player->equippedRing == 7 && !(target->monster->monsterEffects & 2) &&
	           target->monster->goal.type != 4 && !Entity_isBoss(target)) {
		/* ring of fear */
		if ((Random_nextInt(&app->random) & 0xff) < 0x20) {
			MonsterGoal_reset(&target->monster->goal);
			target->monster->goal.type = 4;
			target->monster->goal.param = Random_nextInt(&app->random) % 3 + 3;
			Canvas_resetEvents(app->canvas);
			Canvas_addTextArgStr(app->canvas, &target->name);
			Hud_addMessage(app->hud, 0x69);
		}
	}

	if (combat->weaponId == 6) { /* explosive: splash the 8 surrounding tiles */
		int ox = combat->targetX, oy = combat->targetY;
		int tx = ox >> 6, ty = oy >> 6;
		knock++;
		if (combat->targetType != 2) {
			tx = (ox + (app->canvas->viewStepX >> 2)) >> 6;
			ty = (oy + (app->canvas->viewStepY >> 2)) >> 6;
			Combat_damageTile(combat, tx, ty, 0x10000, ox, oy, 1);
		}
		Combat_damageTile(combat, tx - 1, ty, 0x4000, combat->targetX, combat->targetY, 1);
		Combat_damageTile(combat, tx + 1, ty, 0x4000, combat->targetX, combat->targetY, 1);
		Combat_damageTile(combat, tx, ty - 1, 0x4000, combat->targetX, combat->targetY, 1);
		Combat_damageTile(combat, tx, ty + 1, 0x4000, combat->targetX, combat->targetY, 1);
		Combat_damageTile(combat, tx - 1, ty + 1, 0x4000, combat->targetX, combat->targetY, 1);
		Combat_damageTile(combat, tx + 1, ty + 1, 0x4000, combat->targetX, combat->targetY, 1);
		Combat_damageTile(combat, tx + 1, ty - 1, 0x4000, combat->targetX, combat->targetY, 1);
		Combat_damageTile(combat, tx - 1, ty - 1, 0x4000, combat->targetX, combat->targetY, 1);
	}

	if (!(combat->target->monster->monsterEffects & 2) && knock > 0) {
		Game_unlinkEntityById(app->player);
		Entity_knockback(combat->target, app->game->viewX, app->game->viewY, knock);
		Game_relinkAtViewDest(app->player);
	}
}

/* 0x00018194: the player's shot has landed */
void Combat_finishPlayerAttack(Combat *combat)
{
	Applet *app = g_app;
	Player *player = app->player;

	if (combat->targetType == 3)
		setSpriteFrame(entSprite(combat->target), 3);

	if (combat->targetType == 7 && combat->targetSubtype == 4 && combat->weaponId == 1 &&
	    !(entSprite(combat->target)->info & 0x40000)) {
		/* chopping at a lever/rope with the axe triggers the tile ahead */
		Canvas *cv = app->canvas;
		int flags = Canvas_flagForFacingDir(cv, 0xf000);
		Game_executeTile(app->game, (cv->destX + cv->viewStepX) >> 6, (cv->destY + cv->viewStepY) >> 6,
		                 flags | 0x400, 1);
		return;
	}

	Entity *target = combat->target;
	if (target && target->monster && target->def->eType == 2 && !(target->info & 0x80000))
		Game_activateEx(app->game, target, 1, 0);

	if (combat->hitType == 0) {
		if (combat->target && combat->targetType == 2)
			combat->target->monster->flags &= 0x8fff;
		return;
	}

	switch (combat->targetType) {
	case 2:
		playerHitMonster(combat);
		break;
	case 10: /* breakable */
		if (combat->targetSubtype == 1) {
			Particles_spawnImage1(combat, (int)0xff845d39);
			entSprite(combat->target)->info |= 0x10000;
			Game_unlinkEntity_43710(app->game, combat->target);
		} else if (combat->targetSubtype == 3) {
			Game_unlinkEntity_43710(app->game, combat->target);
		} else {
			entSprite(combat->target)->info |= 0x10000;
			combat->targetTileDist = Combat_WorldDistToTileDist(
				combat, Entity_distSqFrom(combat->target, player->ce.x, player->ce.y));
			Particles_spawnPreset1(combat, combat->targetTileDist);
			Entity_diedDefault(combat->target);
		}
		break;
	case 8:
		if (combat->target->def->parm == 0) {
			Particles_spawnPreset3(combat, combat->targetTileDist, (int)0xffcccc00);
			Game_unlinkEntity_31bb8(app->game, combat->target);
			Hud_addMessage(app->hud, 0x6a);
			Player_addXP(player, 2);
		}
		break;
	case 9: { /* corpse */
		Sprite *spr = entSprite(combat->target);
		spr->info |= 0x10000;
		combat->effectMode = 1;
		combat->bloodColor = Combat_getBloodColor(combat, combat->target);
		Particles_spawnBlood(combat, combat->targetTileDist, combat->bloodColor, 0xf, 0x10, -1);
		Entity_spawnBloodFX(spr->ent);
		if (combat->target->def->eSubType == 16)
			Game_executeTile(app->game, spr->x >> 6, spr->y >> 6, 0xf000, 1);
		break;
	}
	default:
		break;
	}

	if (player->ce.weapon == 1 && combat->targetType == 2) /* alternate axe swing side */
		player->f264 = (uint32_t)player->f264 <= 1 ? 1 - player->f264 : 0;
}

/* 0x00018a64: a monster's shot has landed on the player */
void Combat_finishMonsterAttack(Combat *combat)
{
	Applet *app = g_app;

	if (combat->target != NULL)
		return;

	Sprite *spr = entSprite(combat->attacker);
	Game *game = app->game;
	int ax = spr->x, ay = spr->y;
	app->hud->damageDir = Dir_relativeQuadrant(0, game->viewX, game->viewY, game->viewZ, ax, ay);
	app->hud->damageDirUntil = app->time + 500;
	if (combat->hit == 0 || combat->totalDamage <= 0) {
		app->hud->damageFlash = -1;
		return;
	}
	Player_hurtFeedback(app->player, combat->attacker != NULL);
	if (combat->attacker == NULL)
		return;

	Entity *playerEnt = &game->entities[1];
	int ppos[2];
	Entity_calcPosition(playerEnt, ppos);
	Game_unlinkEntity_43710(game, combat->attacker);

	int w = combat->weaponId;
	if (w == 0x17 || w == 0x22) {
		/* charge / grab: the player is pushed away from the monster */
		int dist = combat->targetTileDist != 1 ? 1 : 2;
		Entity_knockback(playerEnt, combat->attacker->monster->destX, combat->attacker->monster->destY, dist);
	} else if (w == 0x21) {
		/* sweep: the player is knocked sideways, the side depends on the
		 * monster's facing (sprite flip) */
		int apos[2];
		Entity_calcPosition(combat->attacker, apos);
		int px = ppos[0], py = ppos[1];
		int dx = px - apos[0], dy = py - apos[1];
		int sideX = 0, sideY = 0; /* +1/-1 picks the tile, axis by which one is set */
		int flipped = (spr->info & 0x1000000) != 0;
		if (dx != 0 && dy != 0)
			sideX = flipped ? 1 : -1;
		else if (dx < 0)
			sideY = flipped ? 1 : -1;
		else if (dx > 0)
			sideY = flipped ? -1 : 1;
		else if (flipped)
			sideX = dy >= 0 ? 1 : -1;
		else
			sideX = dy < 0 ? 1 : -1;
		int srcX = px, srcY = py;
		if (sideX)
			srcX = ((px >> 6) + sideX) << 6;
		else
			srcY = ((py >> 6) + sideY) << 6;
		Entity_knockback(playerEnt, srcX, srcY, 1);
	}

	int li = combat->attacker->linkIndex;
	Game_linkEntity(game, combat->attacker, li % 32, li / 32);
}

/* 0x0000a6b0: splash damage on one tile (player tile or a monster/object) */
void Combat_damageTile(Combat *combat, int tx, int ty, int scale, int a, int b, int c)
{
	(void)combat;
	Applet *app = g_app;
	int res[6] = { 8, 0, 0, 0, 0, 0 }; /* flag 8: ignore weapon range */

	if (tx == app->canvas->viewX >> 6 && ty == app->canvas->viewY >> 6) {
		CombatEntity *pce = &app->player->ce;
		CombatEntity_calcCombat(pce, pce, res);
		res[1] = mul32(scale, res[1]) >> 16;
		Player_hurtFeedback(app->player, 0);
		if (res[1] > 0) {
			Player_takeDamage(app->player, res[1], NULL);
			CombatEntity_addArmor(pce, -res[2]);
		}
		return;
	}

	Entity *ent = Game_findEntityAt(app->game, tx << 6, ty << 6, 0x76bd);
	if (ent == NULL || ent->def->eType == 9 || !(ent->info & 0x40000))
		return;

	if (ent->monster == NULL) {
		if (ent->def->eType == 8 || ent->def->eType == 10)
			return;
	} else {
		if (ent->def->eSubType == 14 && (ent->monster->flags & 8))
			return;
		uint32_t info = ent->info;
		ent->info = info | 0x10000000;
		if (!(info & 0x80000))
			Game_activateEx(app->game, ent, 1, 0);
		CombatEntity_calcCombat(&app->player->ce, &ent->monster->ce, res);
		res[1] = mul32(scale, res[1]) >> 16;
		if (res[1] < 1)
			return;
		if (!(ent->def->eSubType == 4 || (ent->info & 0x1000000)))
			ent->monster->monsterEffects = (ent->monster->monsterEffects & 0xe7f7) | 8; /* set on fire */
		Entity_takeDamage(ent, res[1]);
		Entity_knockback(ent, a, b, c);
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgStr(app->canvas, &ent->name);
		Canvas_addTextArgInt(app->canvas, res[1]);
		if (CombatEntity_getHealth(&ent->monster->ce) > 0) {
			Hud_addMessage(app->hud, 0x6c);
			int snd = EntityMonster_getRandomSound(ent->monster, 6);
			if (snd != -1)
				Sound_play(app->sndSys, snd, 0, 2, -1);
			return;
		}
		Hud_addMessage(app->hud, 0x6b);
	}
	Entity_diedDefault(ent);
}

/* 0x000096b8: status effect a player weapon leaves on the monster.
 * monsterEffects: bits 0..4 effects, bits 5..14 a 2-bit timer per effect. */
void Combat_applyWeaponEffect(Combat *combat)
{
	Applet *app = g_app;
	int sub = combat->targetSubtype;
	int immuneUndead = sub == 4 || (combat->target->info & 0x1000000);
	int immuneMagic = immuneUndead || sub == 8 || sub == 9;
	EntityMonster *mon = combat->target->monster;

	switch (combat->weaponId) {
	case 0: {
		int tsub = combat->target->def->eSubType;
		if ((tsub == 14 || tsub == 13) && (mon->flags & 8)) {
			mon->flags &= ~8;
			mon->monsterEffects |= 0x10;
			mon->monsterEffects |= 0x6000;
		}
		break;
	}
	case 3:
	case 9:
		if (immuneMagic) {
			Hud_addMessage(app->hud, 0x4c);
			break;
		}
		mon->monsterEffects |= 0x60;
		mon->monsterEffects |= 1;
		combat->bloodColor = (int)0xff66ff66;
		Hud_addMessage(app->hud, 0x4e);
		break;
	case 5:
	case 6:
	case 7:
		if (!immuneUndead) {
			uint16_t e = mon->monsterEffects | 0x1800;
			e |= 8;
			mon->monsterEffects = e & 0xfe7d;
			combat->bloodColor = (int)0xffffff66;
		}
		break;
	case 8:
		mon->monsterEffects |= 0x600;
		mon->monsterEffects |= 4;
		CombatEntity_scaleStats(&mon->ce, 1);
		Hud_addMessage(app->hud, 0x4f);
		break;
	case 10:
		if (immuneUndead) {
			Hud_addMessage(app->hud, 0x4c);
			break;
		}
		mon->monsterEffects |= sub == 7 ? 0x100 : 0x180;
		mon->monsterEffects |= 2;
		mon->monsterEffects &= ~(0x1800 | 8);
		combat->bloodColor = (int)0xff6666ff;
		Hud_addMessage(app->hud, 0x4d);
		MonsterGoal_reset(&mon->goal);
		break;
	default:
		break;
	}
}

/* 0x0000838c: particle scale for the hit; may gib the target on a kill */
int Combat_calcGibScale(Combat *combat, int n)
{
	Applet *app = g_app;

	if (combat->shotDamage == 0)
		return 0;
	int scale = (combat->shotDamage << 16) / (CombatEntity_getMaxHealth(&combat->target->monster->ce) << 8);
	scale = mul32(scale, 0x3000) >> 8;

	if (combat->targetHpAfter <= 0) {
		scale = (int32_t)((uint32_t)scale << 9) >> 8;
		Entity *ent = combat->target;
		int16_t flags = (int16_t)ent->monster->flags;
		if (!(flags & 0x800)) {
			int gib;
			if ((ent->monster->monsterEffects & 2) || (flags & 0x10))
				gib = 1;
			else
				gib = combat->crit != 0 && combat->targetSubtype != 9 && combat->targetSubtype != 4 &&
				      combat->targetSubtype != 2 && !(ent->info & 0x1000000) && !Entity_isBoss(ent);
			if (gib) {
				combat->effectMode = n <= 1;
				if (n > 1)
					scale *= 3;
				combat->target->info |= 0x40020000;
				app->statGibs++;
				entSprite(combat->target)->info |= 0x10000;
				Hud_randomBark(app->hud, 3);
			}
		}
	}
	scale += 0x80;
	if (scale < 0x100)
		scale = 0x100;
	return scale >> 8;
}

/* 0x0003147c: caster raises the corpse entity corpseIdx */
void Combat_raiseDead(Entity *caster, int corpseIdx)
{
	Applet *app = g_app;
	Entity *corpse = &app->game->entities[corpseIdx];
	Sprite *spr = entSprite(corpse);

	Canvas_resetEvents(app->canvas);
	Canvas_addTextArgStr(app->canvas, &caster->name);
	if (Game_findEntityAt(app->game, spr->x, spr->y, 0x7daf) != NULL) {
		Hud_addMessage(app->hud, 0xa1); /* "%1's raise failed!" */
		corpse->info &= ~0x2000000u;
		return;
	}

	Entity_resurrect(corpse, spr->x, spr->y, spr->z);
	Game_activateEx(app->game, corpse, 0, 0);
	if (caster->def->eSubType == 4) { /* necromancer: raised as undead */
		corpse->info |= 0x1000000;
		String_setLength(&corpse->name, 0);
		Text_getString(app->canvas, 0,
		               ModData_s16(&g_mod, MOD_CORPSE_NAME_IDS + corpse->def->parm * 2 + corpse->def->eSubType * 6),
		               &corpse->name);
		Game_gspriteAllocAnim(app->game, 0xc6, spr->x, spr->y, spr->z);
	}
	Canvas_addTextArgStr(app->canvas, &corpse->name);
	if (caster->def->eType == 1)
		Game_gspriteAllocAnim(app->game, 0xf4, spr->x, spr->y, spr->z);
	else
		Hud_addMessage(app->hud, 0xa2); /* "%1 rais-es %2!" */
	Sound_play(app->sndSys, 0x12, 0, 2, -1);
}

/* 0x0004481c: per-turn monster effect timers, damage over time, boss regen */
void Monster_updateEffects(Entity *ent)
{
	Applet *app = g_app;
	EntityMonster *mon = ent->monster;

	if (mon == NULL)
		return;

	if (ent->def->eSubType == 13 && CombatEntity_getHealth(&mon->ce) < CombatEntity_getMaxHealth(&mon->ce)) {
		CombatEntity_setHealth(&mon->ce, CombatEntity_getHealth(&mon->ce) + 20);
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgStr(app->canvas, &ent->def->name);
		Canvas_addTextArgInt(app->canvas, 20);
		Hud_addMessage(app->hud, 0xa3);
	}

	for (int i = 0; i < 5; i++) {
		int bit = 1 << i;
		int16_t effects = (int16_t)ent->monster->monsterEffects;

		if (!(effects & bit)) {
			/* boss shield returns once effect 0x10 has run out */
			if (bit == 0x10 && ent->def->eType == 2 &&
			    (ent->def->eSubType == 14 || ent->def->eSubType == 13)) {
				if (!(ent->monster->flags & 8)) {
					int pos[2], view[3];
					Entity_calcPosition(ent, pos);
					Player_getViewPos(app->player, view);
					int dx = pos[0] - view[0];
					int dy = pos[1] - view[1];
					int dist = Combat_WorldDistToTileDist(app->combat, Entity_distSqFrom(ent, view[0], view[1]));
					if (dx > 0)
						dx = 0x40;
					else if (dx < 0)
						dx = -0x40;
					if (dy > 0)
						dy = 0x40;
					else if (dy < 0)
						dy = -0x40;
					if (app->canvas->viewStepX == dx && app->canvas->viewStepY == dy)
						Particles_spawnToTarget(app->combat, (int)0xff009fef, dist);
					ent->monster->flags |= 8;
				}
				EntityMonster_clearEffects(ent->monster);
			}
			continue;
		}

		int shift = 5 + i * 2;
		int timer = (effects >> shift) & 3;

		if (ent->def->eType != 9 && (ent->info & 0x40000)) {
			int sub = ent->def->eSubType;
			int dmg;
			String_setLength(&app->canvas->scratch, 0);
			Canvas_resetEvents(app->canvas);
			if (bit != 1 && bit != 8)
				goto expire;
			if (bit == 1) { /* poison */
				if (sub == 7 || sub == 3 || sub == 15)
					dmg = 6;
				else if (sub == 1 || sub == 5)
					dmg = 10;
				else
					dmg = 8;
				Canvas_addTextArgInt(app->canvas, dmg);
				Text_getString(app->canvas, 0, 0xa4, &app->canvas->scratch);
			} else { /* burning */
				if (sub == 6)
					dmg = 8;
				else if (sub == 2 || sub == 9 || sub == 5)
					dmg = 6;
				else
					dmg = 4;
				Canvas_addTextArgInt(app->canvas, dmg);
				Text_getString(app->canvas, 0, 0xa5, &app->canvas->scratch);
			}

			Entity_takeDamage(ent, dmg);
			int snd = EntityMonster_getRandomSound(ent->monster, 6);
			if (snd != -1)
				Sound_play(app->sndSys, snd, 0, 2, -1);
			Text_getString(app->canvas, 0, 0xa6, &app->canvas->scratch);
			Hud_addMessageStr(app->hud, &app->canvas->scratch);
			if (CombatEntity_getHealth(&ent->monster->ce) <= 0) {
				Entity_died(ent, 1);
				timer = 1;
				ent->monster->monsterEffects = (ent->monster->monsterEffects & 0x801f) | 0x2aa0;
				goto countdown;
			}
		}

	expire:
		if (timer == 0) {
			ent->monster->monsterEffects &= ~bit;
			if (bit == 4)
				CombatEntity_scaleStats(&ent->monster->ce, 0);
			continue;
		}
	countdown:
		ent->monster->monsterEffects =
			(uint16_t)((ent->monster->monsterEffects & ~(3 << shift)) | ((timer - 1) << shift));
	}
}
