/*
 * Player: inventory, weapons/ammo, rings, gold, give/require/buy, using items
 * (potions/food/ale), firing, familiar (possession) mode and the "equip for map"
 * debug cheat.
 *
 * Item kinds used by give/require (script and pickup "type"):
 *   0 inventory item (count byte, max 99)   1 weapon bit      2 ring bit
 *   3 ammo (count byte, max 99)             4 gold            5 health
 *   7 armor level                           9 lock-pick skill
 * Weapon indices 0..10 (Combat.weapons[]); weapon 4 is the familiar wand,
 * ammo slot 6 is familiar energy, ammo slot 1 the wand charges.
 */
#include "game.h"

/* Squared world distance (4096 = one tile, 64 units) used both for "close
 * enough to melee" and for the familiar's leash. A constant in the .mod's data. */
#define MOD_ONE_TILE_DIST_SQ 0x45ebcu /* int32 */

/* ---------------------------------------------------------------- give / require */

/* 0x0001c5e4 */
int Player_give(Player *player, int type, int idx, int amount, int silent)
{
	Applet *app = g_app;
	uint32_t bit = arm_bit(idx);
	uint32_t had;
	int v;

	if (amount == 0)
		return 0;

	switch (type) {
	case 0: /* inventory item */
		v = player->inventory[idx] + amount;
		if (v > 99)
			v = 99;
		player->inventory[idx] = (uint8_t)v;
		if (silent == 0)
			Player_showInvHelp(player, idx, 0);
		app->statItemsGained[idx] += amount;
		break;

	case 1: /* weapon */
		had = player->weapons;
		if (amount < 0) {
			player->weapons = had & ~bit;
			if (player->ce.weapon == idx)
				Player_selectNextWeapon(player);
			/* losing the familiar wand on map 3 swaps weapon 0's definition */
			if (idx == 4 && player->highestMap == 3)
				app->combat->weapons[0] = app->combat->weapons[42];
			return 1;
		}
		if (idx == 3) {
			player->weapons = had & ~4u;  /* weapon 3 replaces weapon 2 */
		} else {
			if (idx == 2 && (had & 8) != 0)
				return 1;              /* already have the upgrade */
			if (bit & 0x60) {
				player->weapons = had & ~0x60u; /* weapons 5 and 6 exclude each other */
			} else if (idx == 0) {
				player->f260 = 1;
			} else if (idx == 4) {
				Player_give(player, 3, 6, 99, 1); /* full familiar energy */
			}
		}
		player->weapons |= bit;
		if (silent == 0)
			Player_showWeaponHelp(player, idx, 0);
		if ((had & bit) == 0)
			Player_selectWeapon(player, idx);
		break;

	case 2: /* ring */
		if (amount < 0) {
			player->rings &= ~bit;
		} else {
			player->rings |= bit;
			if (silent == 0)
				Player_showSpellHelp(player, idx, 0);
		}
		break;

	case 3: /* ammo */
		v = player->ammo[idx] + amount;
		if (v > 99)
			v = 99;
		player->ammo[idx] = (uint8_t)v;
		/* some ammo types come with the weapon that fires them */
		if (idx == 1) {
			Player_capStat24(player);
		} else if (idx == 6) {
			Player_capStat29(player);
		} else if (idx == 3) {
			if ((player->weapons & 0x400) == 0)
				Player_addWeapon(player, 10, silent);
		} else if (idx == 4) {
			if ((player->weapons & 0x200) == 0)
				Player_addWeapon(player, 9, silent);
		} else if (idx == 5) {
			if ((player->weapons & 0x100) == 0)
				Player_addWeapon(player, 8, silent);
		}
		app->statAmmoGained[idx] += amount;
		break;

	case 4: /* gold */
		v = player->gold + amount;
		if (v < 0) {
			v = 0;
			amount = -player->gold;
		}
		player->gold = v;
		if (amount > 0)
			app->statGoldGained += amount;
		else
			app->statGoldSpent -= amount;
		break;

	case 5: /* health */
		Player_addHealthEx(app->player, amount, 1);
		break;

	case 7: /* armor */
		player->armorLevel = idx & 0xff;
		CombatEntity_setArmor(&player->ce, idx & 0xff);
		if (silent == 0)
			Player_showArmorHelp(player, player->armorLevel, 0);
		break;

	case 9: /* lock-pick skill */
		if (player->lockLevel < idx)
			player->lockLevel = idx & 0xff;
		break;

	default: /* 6, 8 and unknown kinds give nothing */
		return 0;
	}
	return 1;
}

/* 0x00034c10  script condition; for bit kinds (1, 2) max != 0 means "has it",
 * max == 0 means "doesn't have it" */
int Player_requireItem(Player *player, int type, int idx, int min, int max)
{
	int count;
	uint32_t has;

	switch (type) {
	case 0: count = player->inventory[idx]; break;
	case 3: count = player->ammo[idx]; break;
	case 4: count = player->gold; break;
	case 1:
	case 2:
		has = (type == 1 ? player->weapons : player->rings) & (arm_bit(idx));
		if (max != 0)
			return has != 0;
		return has == 0;
	case 7: return idx <= player->armorLevel;
	case 9: return idx <= player->lockLevel;
	default: return 0;
	}
	return min <= count && count <= max;
}

/* 0x00030d9c  store purchase (first argument unused) */
void Player_buyItem(void *a, int type, int sub, int cost, int amount)
{
	Applet *app = g_app;
	Player *player = app->player;

	(void)a;
	Sound_play(app->sndSys, 0xd, 0, 3, -1);
	Player_give(app->player, 4, 0, -cost, 0);
	Player_give(app->player, type, sub, amount, 0);

	if (type == 1) {
		app->player->boughtWeapons |= arm_bit(sub);
		/* weapons 5 and 6 come with 20 rounds of ammo 2 */
		if (sub == 5 || sub == 6)
			Player_give(player, 3, 2, 20, 0);
	} else if (type == 3) {
		/* Buying ammo for the ranged weapons also gives the weapon. The masks
		 * tested here (0xa, 9, 8) are what the original tests; Player_give's
		 * ammo branch does the equivalent check with the real weapon bits. */
		if (sub == 3) {
			if ((player->weapons & 0xa) == 0)
				Player_give(player, 1, 10, 1, 0);
		} else if (sub == 4) {
			if ((player->weapons & 9) == 0)
				Player_give(player, 1, 9, 1, 0);
		} else if (sub == 5) {
			if ((player->weapons & 8) == 0)
				Player_give(player, 1, 8, 1, 0);
		}
	}
}

/* ---------------------------------------------------------------- using items */

/* Drinking ale (items 12 and 13). Returns 0 if nothing happened. */
static int drinkAle(Player *pl, int idx)
{
	Applet *app = g_app;
	Render *render;
	int healed = Player_addHealthEx(pl, 20, 1) != 0;

	if (Player_addStatusEffect(pl, 12, 10, 30) != 0) {
		if (idx == 13) /* the strong one counts twice */
			Player_addStatusEffect(pl, 12, 10, 30);
	} else if (!healed) {
		return 0;
	}

	render = app->render;
	if (render->drunkLevel == 0 && render->drunkTarget == 0)
		app->render->oscTime = (int32_t)GETUPTIMEMS();
	pl->f138 = pl->totalMoves + pl->statusEffects[12].turns;
	app->render->drunkTarget += 30;
	Player_updateStats(pl);
	Hud_randomBark(app->hud, 8);
	return 1;
}

/* 0x00045394  use one inventory item; a and b are unused */
int Player_useItem(Player *pl, int idx, int a, int b)
{
	Applet *app = g_app;
	int endTurn = 1;
	int ok;
	int sound = 0x60;

	(void)a;
	(void)b;
	if (pl->inventory[idx] == 0)
		return 0;
	app->statItemsUsed[idx]++;

	switch (idx) {
	case 0: /* small health potion */
		if (Player_addHealthEx(pl, 40, 1) == 0)
			return 0;
		break;
	case 1: /* large health potion */
		if (Player_addHealthEx(pl, 80, 1) == 0)
			return 0;
		break;
	case 2:
		if (Player_addStatusEffect(pl, 7, 10, 30) == 0)
			return 0;
		Player_updateStats(pl);
		break;
	case 3:
		if (Player_addStatusEffect(pl, 6, 10, 30) == 0)
			return 0;
		Player_updateStats(pl);
		break;
	case 4:
		if (Player_addStatusEffect(pl, 8, 10, 30) == 0)
			return 0;
		Player_updateStats(pl);
		break;
	case 5:
		/* free action: does not use up the turn */
		if (pl->statusEffects[3].turns != 0)
			return 0;
		endTurn = 0;
		Player_addStatusEffect(pl, 3, 0, 20);
		Player_updateStats(pl);
		break;
	case 6: /* cure: clears drunkenness, poison and the like */
		if (pl->statusEffects[2].turns != 0)
			return 0;
		Player_removeBuff(pl, 13);
		Player_removeBuff(pl, 12);
		Player_removeBuff(pl, 10);
		Player_removeBuff(pl, 11);
		Player_removeBuff(pl, 4);
		app->render->drunkTarget = 0;
		app->render->drunkLevel = 0;
		Player_addStatusEffect(pl, 2, 0, 5);
		Player_updateStats(pl);
		break;
	case 7:
		if (Player_addStatusEffect(pl, 5, 5, 30) == 0)
			return 0;
		Player_updateStats(pl);
		break;
	case 8:
		ok = Player_addHealthEx(pl, 30, 1) != 0;
		ok |= Player_addStatusEffect(pl, 6, 15, 30) != 0;
		ok |= Player_addStatusEffect(pl, 7, 15, 30) != 0;
		ok |= Player_addStatusEffect(pl, 8, 15, 30) != 0;
		if (!ok)
			return 0;
		Player_updateStats(pl);
		break;
	case 9:
		if (Player_addStatusEffect(pl, 1, 4, 30) == 0)
			return 0;
		Player_updateStats(pl);
		break;
	case 10: /* armor repair kit */
		if (CombatEntity_getArmorPoints(&pl->ce) == CombatEntity_getMaxArmorPoints(&pl->ce))
			return 0;
		CombatEntity_addArmor(&pl->ce, 75);
		break;
	case 11: /* familiar energy (needs the wand) */
		if ((pl->weapons & 0x10) == 0)
			return 0;
		if (pl->statusEffects[0].turns > 0)
			return 0;
		if (Player_addStatusEffect(pl, 0, 0, 10) == 0)
			return 0;
		Player_give(pl, 3, 6, 99, 1);
		Player_updateStats(pl);
		break;
	case 12:
	case 13: /* ale */
		if (!drinkAle(pl, idx))
			return 0;
		sound = 0x5f;
		break;
	default:
		/* no effect; only items 10..14 play the use sound */
		if ((unsigned)(idx - 10) >= 5)
			sound = 0;
		break;
	}

	if (sound != 0)
		Sound_play(app->sndSys, sound, 0, 3, -1);
	pl->inventory[idx]--;
	if (endTurn)
		Game_advanceTurn(app->game);
	return 1;
}

/* ---------------------------------------------------------------- weapons */

/* A weapon is selectable if owned and it either needs no ammo or has some. */
static int weaponUsable(Player *player, int w)
{
	WeaponInfo *wi = &g_app->combat->weapons[w];

	if ((player->weapons & (arm_bit(w))) == 0)
		return 0;
	return wi->ammoUsage == 0 || player->ammo[wi->ammoType] != 0;
}

/* 0x000381b8 */
void Player_selectNextWeapon(Player *player)
{
	int cur = player->ce.weapon;
	int w;

	for (w = cur + 1; w < 11; w++) {
		if (weaponUsable(player, w)) {
			Player_selectWeapon(player, w);
			return;
		}
	}
	/* wrap around */
	if (cur == 0)
		return;
	for (w = 0; w < cur; w++) {
		if (weaponUsable(player, w)) {
			Player_selectWeapon(player, w);
			return;
		}
	}
}

/* 0x000382ac */
void Player_selectPrevWeapon(Player *player)
{
	int cur = player->ce.weapon;
	int w;

	for (w = cur - 1; w >= 0; w--) {
		if (weaponUsable(player, w)) {
			Player_selectWeapon(player, w);
			return;
		}
	}
	/* wrap around */
	if (cur == 10)
		return;
	for (w = 10; w > cur; w--) {
		if (weaponUsable(player, w)) {
			Player_selectWeapon(player, w);
			return;
		}
	}
}

/* 0x0003840c */
void Player_selectWeapon(Player *player, int w)
{
	Applet *app = g_app;

	for (;;) {
		if ((app->player->weapons & (arm_bit(w))) == 0) {
			/* not owned: move on from it (the original still selects w below) */
			player->ce.weapon = w;
			Player_selectNextWeapon(player);
		}
		/* the familiar can only use its wand */
		if (app->player->isFamiliar == 0 || w == 4)
			break;
		w = 4;
	}

	if (player->ce.weapon != w)
		player->prevWeapon = player->ce.weapon;
	player->ce.weapon = w;
	if (app->canvas->state == 3)
		Canvas_drawPlayingSoftKeys(app->canvas);
	if (w == 2 && app->game->numScriptThreads == 0)
		Player_showWeaponHelp(player, w, 0);
}

/* 0x0001c598 */
void Player_addWeapon(Player *player, int weapon, int silent)
{
	player->weapons |= arm_bit(weapon);
	Player_selectWeapon(player, weapon);
	if (silent == 0)
		Player_showWeaponHelp(player, weapon, 0);
}

/* 0x0000c04c  take everything but the starting weapons (arrest) */
void Player_resetWeapons(Player *player)
{
	Applet *app = g_app;

	player->disabledWeapons |= player->weapons;
	player->ce.weapon = 0;
	player->weapons = 0x11;
	app->stat5d0++;
	if ((player->weapons & (arm_bit(player->ce.weapon))) == 0)
		Player_selectNextWeapon(player);
}

/* 0x00013ffc  give back the weapons taken by Player_resetWeapons */
void Player_refreshWeapons(Player *player)
{
	player->weapons |= player->disabledWeapons;
	player->disabledWeapons = 0;
	if ((player->weapons & (arm_bit(player->ce.weapon))) == 0)
		Player_selectNextWeapon(player);
}

/* 0x000383a0  equip ring idx */
void Player_selectSpell(Player *player, int idx)
{
	Render *render = g_app->render;

	/* taking off the drunkard's ring sobers you up unless you are really drunk */
	if (player->equippedRing == 8 && render->drunkLevel != 0 && player->f138 <= player->totalMoves)
		render->drunkTarget = 0;
	player->equippedRing = idx;
	Player_calcStats(player);
}

/* 0x0001b458  returns 1 if an attack (or familiar toggle) happened */
int Player_fireWeapon(Player *player, Entity *target, int x, int y)
{
	Applet *app = g_app;
	int weapon;
	WeaponInfo *wi;

	if (player->disabledWeapons != 0 &&
	    (player->weapons & (arm_bit(player->ce.weapon))) == 0)
		return 0;

	weapon = player->ce.weapon;
	if (weapon == 4) {
		/* familiar wand: toggle possession */
		int wasFamiliar = player->isFamiliar;

		if (wasFamiliar == 0) {
			Player_setFamiliar(player, 1, 0, 1);
		} else {
			Canvas *cv = app->canvas;
			int dx = cv->viewX - cv->savedViewX;
			int dy = cv->viewY - cv->savedViewY;

			if (dx * dx + dy * dy > ModData_s32(&g_mod, MOD_ONE_TILE_DIST_SQ))
				Player_setFamiliar(player, 0, 1, 1);
			else
				Canvas_resetPlayerPos(cv, cv->savedViewX, cv->savedViewY);
		}
		if (player->isFamiliar == wasFamiliar)
			return 0;
		Sound_play(app->sndSys, 100, 0, 4, -1);
		return 1;
	}

	if (weapon == 0) {
		if (app->player->f260 == 0) {
			/* bare hands */
			player->prevWeapon = 0;
			if ((app->player->helpBitmask & 0x20000) == 0) {
				Canvas_showDialogText(app->canvas, 0, 0, 0x11, 1, 1, 0);
				app->player->helpBitmask |= 0x20000;
			} else {
				Hud_addTimedMsg(app->hud, 0x1d);
			}
			return 0;
		}
		if (target->def->eType == 3) {
			player->prevWeapon = 0;
			Hud_addTimedMsg(app->hud, 0x1e);
			return 0;
		}
	} else if ((arm_bit(weapon)) & 0xc) {
		/* weapons 2 and 3 behave differently at point-blank range */
		int distSq = Entity_distSqFrom(Player_getPlayerEnt(player), x, y);
		player->fc0 = distSq <= ModData_s32(&g_mod, MOD_ONE_TILE_DIST_SQ) ? 1 : 0;
	}

	if (target->monster != NULL) {
		uint16_t flags = target->monster->flags;
		target->monster->flags = flags & ~1u;
		if (target->monster->flags & 0x200)
			target->monster->flags = flags & ~0x201u;
	}

	weapon = player->ce.weapon;
	wi = &app->combat->weapons[weapon];
	if (wi->ammoType != 0) {
		int have = player->ammo[wi->ammoType];
		int left;

		if (wi->ammoUsage > 0 && have - wi->ammoUsage < 0) {
			Hud_addMessageFlags(app->hud, 200, 3); /* "Out of Ammo!" */
			return 0;
		}
		left = have - wi->ammoUsage;
		player->ammo[wi->ammoType] = (uint8_t)left;
		/* the original adds the remaining count, not the amount used */
		app->statAmmoUsed[wi->ammoType] += left;
	}
	app->statShotsByWeapon[player->ce.weapon]++;

	if (player->lastTarget != target) {
		player->lastTarget = target;
		player->critCounter = 0;
	}
	Combat_start(app->combat, NULL, target, x, y);
	if (player->ce.weapon == 0)
		app->hud->numLines = 0;
	return 1;
}

/* ---------------------------------------------------------------- familiar */

/* 0x00039e38  on: start possessing the familiar (p4: step it one tile forward);
 * off: p3 != 0 plays the return transition (state 0x12), else ends at once */
void Player_setFamiliar(Player *player, int on, int p3, int p4)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	GameSprite *gs;

	if (cv->state == 5)
		return;

	if (on == 0) {
		if (p3 == 0) {
			player->isFamiliar = 0;
		} else {
			Canvas_requestState12(cv, 1000, 8);
			Canvas_setState(cv, 0x12);
		}
		Canvas_drawPlayingSoftKeys(cv);
		return;
	}

	if (cv->state != 6 && player->ammo[6] == 0) {
		/* no energy */
		Hud_addMessageFlags(app->hud, 0x1b, 2);
		Sound_play(app->sndSys, 0x65, 0, 4, -1);
		player->isFamiliar = 0;
		return;
	}

	/* remember where the body stays */
	cv->savedViewX = cv->viewX;
	cv->savedViewY = cv->viewY;
	cv->savedViewAngle = cv->viewAngle;
	cv->savedViewZ = app->render->ceilHeight;
	player->isFamiliar = 1;
	Game_relinkAtViewDest(player);

	if (p4 != 0) {
		Canvas_finishMovement_77f4(cv, cv->viewX + cv->viewStepX, cv->viewY + cv->viewStepY);
		if (cv->destX == cv->savedViewX && cv->destY == cv->savedViewY) {
			/* blocked: no room for the familiar */
			Player_showHelp(player, 0xc, 1);
			Hud_addMessageFlags(app->hud, 0x1c, 3);
			Game_unlinkEntityById(player);
			player->isFamiliar = 0;
			return;
		}
	}

	/* leave the body behind as a sprite */
	cv->destZ = 0xf;
	gs = Game_gspriteAlloc(app->game, 0, 0x46, 0, 0);
	app->player->familiarSprite = gs;
	gs->sprite->info |= 0x80000000u;
	gs->start[0] = cv->savedViewX;
	gs->sprite->x = cv->savedViewX;
	gs->start[1] = cv->savedViewY;
	gs->sprite->y = cv->savedViewY;
	Vec3_copy(gs->dest, gs->start);
	gs->travelTime = 0;
	gs->f10 = 0;
	Render_relinkSprite(app->render, gs->sprite);
	Canvas_drawPlayingSoftKeys(cv);
}

/* 0x0002b934  can the player walk through / interact past this entity? */
int Player_canUseEntity(Player *player, Entity *ent)
{
	Applet *app = g_app;
	EntityDef *def;

	if (ent == NULL)
		return 0;

	def = ent->def;
	if (player->isFamiliar != 0) {
		if (def->eType == 6) {
			if (def->eSubType != 0)
				return 1;
			return def->parm != 13 && def->parm != 12;
		}
		if (def->eType == 7) {
			if (def->tileIndex != 0xa4 && def->tileIndex != 0xac && def->tileIndex != 0xaf)
				return 0;
			return app->canvas->viewZ == 0xf;
		}
		if (def->eType == 13)
			return 1;
	}

	def = ent->def;
	if (def->eType == 12)
		return (app->render->sprites[(ent->info & 0xffff) - 1].info & 0xff) == 0x83;
	return def->eType == 3 && def->eSubType == 1;
}

/* ---------------------------------------------------------------- debug */

/* 0x000144b8  "equip" cheat / level select: everything a player would have
 * collected by the start of map N, applied cumulatively from map N down. */
void Player_equipForMap(Player *player, int map)
{
	int savedHelp;
	int level = 1;

	Player_reset(player, 0);
	player->highestMap = map;
	savedHelp = player->enableHelp;
	player->enableHelp = 0;
	player->weapons = 0;

	switch (map) {
	case 1:
		goto map1;
	case 2:
		goto map2;
	case 3:
		goto map3;
	case 4:
		goto map4;
	case 5:
		goto map5;
	case 6:
		goto map6;
	default: /* 0, 7 and above: everything */
		break;
	}

	level = 3;
	Player_give(player, 4, 0, 0x1081, 1);
	if (player->ce.armorType != 2)
		CombatEntity_setArmor(&player->ce, 2);
	Player_give(player, 0, 1, 3, 1);
	Player_give(player, 0, 2, 11, 1);
	Player_give(player, 0, 3, 4, 1);
	Player_give(player, 0, 4, 5, 1);
	Player_give(player, 0, 5, 5, 1);
	Player_give(player, 0, 6, 4, 1);
	Player_give(player, 0, 8, 7, 1);
	Player_give(player, 0, 7, 4, 1);
	Player_give(player, 0, 9, 4, 1);
	Player_give(player, 0, 11, 1, 1);
	Player_give(player, 0, 10, 1, 1);
	Player_give(player, 2, 5, 1, 1);
	Player_give(player, 3, 2, 23, 1);
	Player_give(player, 3, 3, 13, 1);
	Player_give(player, 3, 5, 14, 1);
	player->weapons |= 200;
map6:
	level += 1;
	Player_give(player, 4, 0, 0xb74, 1);
	if (player->ce.armorType < 1)
		CombatEntity_setArmor(&player->ce, 1);
	Player_give(player, 0, 1, 7, 1);
	Player_give(player, 0, 2, 3, 1);
	Player_give(player, 0, 3, 4, 1);
	Player_give(player, 0, 4, 2, 1);
	Player_give(player, 0, 5, 7, 1);
	Player_give(player, 0, 7, 7, 1);
	Player_give(player, 0, 9, 6, 1);
	Player_give(player, 0, 11, 2, 1);
	Player_give(player, 0, 10, 1, 1);
	Player_give(player, 2, 4, 1, 1);
	Player_give(player, 3, 2, 23, 1);
	Player_give(player, 3, 4, 21, 1);
	Player_give(player, 9, 3, 1, 1);
map5:
	level += 2;
	Player_give(player, 4, 0, 0x6c5, 1);
	if (player->ce.armorType < 1)
		CombatEntity_setArmor(&player->ce, 1);
	Player_give(player, 0, 1, 4, 1);
	Player_give(player, 0, 2, 2, 1);
	Player_give(player, 0, 3, 2, 1);
	Player_give(player, 0, 5, 3, 1);
	Player_give(player, 0, 6, 2, 1);
	Player_give(player, 0, 7, 2, 1);
	Player_give(player, 0, 11, 1, 1);
	Player_give(player, 0, 10, 2, 1);
	Player_give(player, 2, 8, 1, 1);
	Player_give(player, 3, 2, 23, 1);
	Player_give(player, 3, 5, 3, 1);
	Player_give(player, 9, 3, 1, 1);
map4:
	level += 1;
	Player_give(player, 4, 0, 0x415, 1);
	Player_give(player, 0, 0, 8, 1);
	Player_give(player, 0, 1, 6, 1);
	Player_give(player, 0, 2, 8, 1);
	Player_give(player, 0, 3, 4, 1);
	Player_give(player, 0, 4, 8, 1);
	Player_give(player, 0, 7, 2, 1);
	Player_give(player, 0, 10, 2, 1);
	Player_give(player, 2, 2, 1, 1);
	Player_give(player, 3, 2, 34, 1);
	Player_give(player, 3, 5, 15, 1);
	Player_give(player, 9, 2, 1, 1);
map3:
	level += 2;
	Player_give(player, 4, 0, 0x1c2, 1);
	Player_give(player, 0, 0, 2, 1);
	Player_give(player, 0, 1, 2, 1);
	Player_give(player, 0, 2, 3, 1);
	Player_give(player, 0, 3, 2, 1);
	Player_give(player, 0, 4, 2, 1);
	Player_give(player, 0, 11, 2, 1);
	Player_give(player, 0, 10, 1, 1);
	Player_give(player, 3, 2, 10, 1);
	Player_give(player, 9, 1, 1, 1);
	player->weapons |= 0x20;
map2:
	level += 1;
	Player_give(player, 4, 0, 0x104, 1);
	Player_give(player, 2, 3, 1, 1);
	Player_give(player, 0, 10, 1, 1);
	Player_give(player, 0, 0, 3, 1);
	Player_give(player, 0, 2, 2, 1);
	Player_give(player, 0, 3, 1, 1);
	player->f260 = 1;
	player->weapons |= 0x10;
map1:
	player->weapons |= 7;
	if (player->weapons & 0x40)
		player->weapons &= ~0x20u;
	if (player->weapons & 8)
		player->weapons &= ~4u;

	while (player->level != level)
		Player_levelUp(player);
	Player_give(player, 3, 1, 99, 1);
	player->enableHelp = savedHelp;
	Player_selectNextWeapon(player);
	Canvas_updateFacing(g_app->canvas);
}
