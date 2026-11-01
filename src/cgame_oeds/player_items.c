/*
 * Player inventory: giving and using items, buying, requirement checks,
 * weapon selection and firing, the level-select starting kits and the
 * "first time you see X" help popups.
 *
 * Give/require types: 0 item (Player.items[13], cap 99), 1 weapon bit
 * (Player.weapons), 2 ring bit (Player.rings), 3 ammo (Player.ammo[5], cap 99),
 * 4 gold, 7 armour, 8 quest item bit (Player.questItems).
 */
#include "game.h"
#include "ds_local.h"

/* Player-visible strings in the module's data area. */
#define MOD_STR_OUT_OF_AMMO 0x2da70u

/* Help texts (Player_showHelp), one per help id. */
#define MOD_HELP_TALK            0x2da90u /* 0 */
#define MOD_HELP_RUNE_LOCKS      0x2db18u /* 3 */
#define MOD_HELP_BARRICADES      0x2dc50u /* 4 */
#define MOD_HELP_WIELD_WAND_ME   0x2dd40u /* 5 */
#define MOD_HELP_WIELD_WAND      0x2deb0u /* 6, 7 */
#define MOD_HELP_FOOD            0x2df50u /* 8 */
#define MOD_HELP_GOLD            0x2dfb8u /* 9 */
#define MOD_HELP_NOTE            0x2e074u /* 10 */
#define MOD_HELP_DESTRUCTIBLE    0x2e0f8u /* 11 */
#define MOD_HELP_FIRE_PORTAL     0x2e188u /* 13 */
#define MOD_HELP_FIRE_PORTAL_OFF 0x2e254u /* 14 */
#define MOD_HELP_WAND_STOLEN     0x2e300u /* 16 */
#define MOD_HELP_DOORS           0x2e358u /* 18 */
#define MOD_HELP_CRYSTALS        0x2e40cu /* 19 */
#define MOD_HELP_ARMOR_LOW       0x2e500u /* 20 */

/* Sound resource ids */
#define SND_USE_ITEM 0x140e
#define SND_DRINK    0x142b
#define SND_BUY      0x1411

/* EntityDef types/subtypes for item help */
#define ETYPE_ITEM 6

static char *modStr(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

/* 1 << n as the ARM did it (shift by the low byte; 32 and more give 0). */
static uint32_t bitOf(int n)
{
	n &= 0xff;
	return n < 32 ? 1u << n : 0;
}

/* ---------------------------------------------------------------- giving */

/* 0x00012f98  returns 1 if something was given/taken */
int Player_give(Player *player, int type, int idx, int amount, int silent)
{
	Applet *app = player->app;
	uint32_t bit = bitOf(idx);
	int v;

	if (amount == 0)
		return 0;

	switch (type) {
	case 0: /* item */
		v = player->items[idx] + amount;
		if (v > 99)
			v = 99;
		player->items[idx] = (uint8_t)v;
		if (silent == 0)
			Player_showInvHelp(player, idx, 0);
		app->statItemsGained[idx] += amount;
		break;

	case 1: /* weapon */
		if (amount < 0) {
			player->weapons &= ~bit;
			return 1;
		}
		/* upgrades replace the weapon they improve on */
		if (idx == 2 || idx == 3)
			player->weapons &= ~2u;
		else if ((bit & 0x7f) >> 4 != 0)
			player->weapons &= ~0x70u;
		else if (bit & 0x80)
			player->weapons &= ~0x80u;
		player->weapons |= bit;
		if (silent == 0)
			Player_showWeaponHelp(player, idx, 0);
		break;

	case 2: /* ring */
		if (amount < 0) {
			player->rings &= (uint16_t)~bit;
		} else {
			player->rings |= (uint16_t)bit;
			if (silent == 0)
				Player_showRingHelp(player, idx, 0);
		}
		break;

	case 3: /* ammo */
		v = player->ammo[idx] + amount;
		if (v > 99)
			v = 99;
		player->ammo[idx] = (uint8_t)v;
		if (idx == 1) {
			Player_clampStat30(player);
		} else if (idx == 3 && (player->weapons & 0x80) == 0) {
			/* the first ammo of type 3 comes with weapon 7 */
			player->weapons |= 0x80;
			Player_selectWeapon(player, 7);
			if (silent == 0)
				Player_showWeaponHelp(player, 7, 0);
		}
		app->statAmmoGained[idx] += amount;
		break;

	case 4: /* gold */
		v = player->gold + amount;
		if (v < 0) {
			amount = -player->gold;
			v = 0;
		}
		player->gold = v;
		if (amount > 0)
			app->statGoldGained += amount;
		else
			app->statGoldSpent -= amount;
		break;

	case 7: /* armour */
		player->armorOwned = (uint8_t)idx;
		CombatEntity_setArmor(&player->ce, (int8_t)player->armorOwned);
		if (silent == 0)
			Player_showArmorHelp(player, (int8_t)player->armorOwned, 0);
		break;

	case 8: /* quest item */
		if (amount < 0)
			player->questItems &= (uint16_t)~bit;
		else
			player->questItems |= (uint16_t)bit;
		break;

	case 11: /* DS Player_give 0x0203dabc: a wand spell (0 force, 1 fire, 2 lightning, 3 stun) */
		if (amount < 0) {
			player->dsSpells &= (uint16_t)~bit;
		} else if (amount != 2) {
			/* (amount 2 is a DS menu call, not a gift) */
			player->dsSpells |= (uint16_t)bit;
			Player_give(player, 3, 1, 100, 1);		/* and the wand's energy refilled */
			Hud_addMessage(app->hud, (char *)DsStrings_get("CodeStrings", 218), 0);	/* "New Spell!" */
		}
		break;

	default:
		return 0;
	}
	return 1;
}

/* 0x00022144  bit types (1, 2, 8): max == 0 means "must NOT own" */
int Player_requireItem(Player *player, int type, int idx, int min, int max)
{
	uint32_t bit = bitOf(idx);
	int v;

	switch (type) {
	case 0:
		v = player->items[idx];
		break;
	case 1:
		if (max == 0)
			return (player->weapons & bit) == 0;
		return (player->weapons & bit) != 0;
	case 2:
		if (max == 0)
			return (player->rings & bit) == 0;
		return (player->rings & bit) != 0;
	case 3:
		v = player->ammo[idx];
		break;
	case 4:
		v = player->gold;
		break;
	case 7:
		return (int8_t)player->armorOwned >= idx;
	case 8:
		if (max == 0)
			return (player->questItems & bit) == 0;
		return (player->questItems & bit) != 0;
	default:
		return 0;
	}
	return min <= v && v <= max;
}

/* 0x0001fcf4  store purchase (called by Menus_select with the Menu) */
void Player_buyItem(Menu *menu, int type, int sub, int cost, int amount)
{
	Sound_play(menu->app->sound, SND_BUY, 0, 3, -1);
	Player_give(menu->player, 4, 0, -cost, 0);
	Player_give(menu->player, type, sub, amount, 0);
	if (type == 3) {
		/* ammo 3 without weapon 7: the weapon comes with it */
		if (sub == 3 && (menu->player->weapons & 0x80) == 0)
			Player_give(menu->player, 1, 7, 1, 0);
	} else if (type == 1 && (sub == 4 || sub == 5 || sub == 6)) {
		Player_give(menu->player, 3, 2, 0x14, 0);
	}
}

/* ---------------------------------------------------------------- using items */

/* 0x0002c00c  returns 1 if the item was used up */
int Player_useItem(Player *pl, int idx, int arg)
{
	Applet *app = pl->app;
	Render *render = pl->render;
	int advanceTurn = 1;
	int recalc = 0;
	int sound = SND_USE_ITEM;
	int any;

	/* Callers only pass 0..12 (item menus, quick potion); the original's default path would touch items[idx]
	 * for any index, so reject impossible ones instead of writing out of bounds. */
	if ((unsigned)idx >= sizeof(pl->items) || pl->items[idx] == 0)
		return 0;
	app->statItemUse[idx]++;

	switch (idx) {
	case 0: /* small health potion */
		if (Player_addHealth(pl, 40) == 0)
			return 0;
		break;
	case 1: /* large health potion */
		if (Player_addHealth(pl, 80) == 0)
			return 0;
		break;
	case 2:
		if (Player_addStatusEffect(pl, 8, 10, 30) == 0)
			return 0;
		recalc = 1;
		break;
	case 3:
		if (Player_addStatusEffect(pl, 7, 10, 30) == 0)
			return 0;
		recalc = 1;
		break;
	case 4:
		if (Player_addStatusEffect(pl, 9, 10, 30) == 0)
			return 0;
		recalc = 1;
		break;
	case 5: /* haste: does not cost a turn */
		if (pl->effects[4].turns != 0)
			return 0;
		advanceTurn = 0;
		Player_addStatusEffect(pl, 4, 0, 20);
		recalc = 1;
		break;
	case 6: /* remedy: cures drain, poison and drunkenness */
		if (pl->effects[3].turns != 0)
			return 0;
		Player_removeBuff(pl, 11);
		Player_removeBuff(pl, 1);
		Player_removeBuff(pl, 10);
		render->drunkTarget = 0;
		render->drunkLevel = 0;
		Player_addStatusEffect(pl, 3, 0, 5);
		recalc = 1;
		break;
	case 7:
		if (Player_addStatusEffect(pl, 6, 5, 30) == 0)
			return 0;
		recalc = 1;
		break;
	case 8: /* every part is tried; any success uses the item */
		any = Player_addHealth(pl, 30) != 0;
		if (Player_addStatusEffect(pl, 7, 15, 30) != 0)
			any = 1;
		if (Player_addStatusEffect(pl, 8, 15, 30) != 0)
			any = 1;
		if (Player_addStatusEffect(pl, 9, 15, 30) != 0)
			any = 1;
		if (!any)
			return 0;
		recalc = 1;
		break;
	case 9:
		if (Player_addStatusEffect(pl, 2, 4, 30) == 0)
			return 0;
		recalc = 1;
		break;
	case 10: /* invisibility */
		if (pl->effects[5].turns != 0)
			return 0;
		Player_addStatusEffect(pl, 5, 0, 10);
		recalc = 1;
		break;
	case 11: /* drink */
		any = Player_addHealth(pl, 20) != 0;
		if (Player_addStatusEffect(pl, 10, 10, 40) == 0 && !any)
			return 0;
		if (render->drunkLevel == 0 && render->drunkTarget == 0)
			render->drunkStart = (int32_t)GETUPTIMEMS();
		pl->f110 = pl->totalTurns + pl->effects[10].turns;
		render->drunkTarget += 40;
		Player_updateStats(pl);
		Hud_randomBark(app->hud, 9, 0);
		sound = SND_DRINK;
		break;
	case 12: /* armour kit */
		if (CombatEntity_getArmorPoints(&pl->ce) == CombatEntity_getMaxArmorPoints(&pl->ce))
			return 0;
		CombatEntity_addArmor(&pl->ce, 75);
		break;
	default:
		sound = 0; /* unreachable in the game: consumed silently */
		break;
	}

	if (recalc)
		Player_updateStats(pl);
	if (sound != 0)
		Sound_play(app->sound, sound, 0, 3, -1);
	pl->items[idx]--;
	if (advanceTurn)
		Game_advanceTurn(pl->game);
	return 1;
}

/* ---------------------------------------------------------------- weapons */

/* 0x0001267c  returns 0 if the weapon can't be used */
int Player_fireWeapon(Player *player, void *target, int x, int y)
{
	Applet *app = player->app;
	Entity *ent = (Entity *)target;
	Weapon *w;
	int ammo;

	if (player->weaponsPending != 0 && (player->weapons & bitOf(player->ce.weapon)) == 0)
		return 0;

	if (ent->monster != NULL)
		ent->monster->flags &= ~1u;

	w = &app->combat->weapons[player->ce.weapon];
	if (w->ammoType != 0) {
		ammo = (int8_t)player->ammo[w->ammoType];
		if (w->ammoUsage > 0 && ammo - w->ammoUsage < 0) {
			Hud_addMessage(app->hud, modStr(MOD_STR_OUT_OF_AMMO), 3);
			return 0;
		}
		ammo = (int8_t)(ammo - w->ammoUsage);
		player->ammo[w->ammoType] = (uint8_t)ammo;
		/* the original adds the ammo left, not the ammo used */
		app->statAmmoUsed[w->ammoType] += ammo;
	}
	app->statWeaponUse[player->ce.weapon]++;

	if (player->lastTarget != ent) {
		player->lastTarget = ent;
		player->missStreak = 0;
	}
	Combat_start(app->combat, NULL, ent, x, y);
	if (player->ce.weapon == 0)
		app->hud->numSubtitles = 0;
	return 1;
}

/* 0x000245fc */
void Player_selectWeapon(Player *player, int w)
{
	if (player->render->mapFlagBits & 2)
		w = 0; /* map without weapons: wand only */
	if (w == 0 && (player->weapons & 1) == 0)
		w = 1;

	if (player->ce.weapon != w) {
		Entity *facing = player->facing;

		/* weapon 8 against the shielded boss: shield flash at its position */
		if (facing != NULL && w == 8 && facing->def->eType == 2 && facing->def->eSubType == 0xd) {
			EntityMonster *mon = facing->monster;

			if ((mon->flags & 8) != 0 && mon->petrifyTurns == 0) {
				GameSprite *gs = Game_gspriteAllocAnim(player->game, 0xff, mon->destX, mon->destY, 0x34);
				gs->sprite->scale = Render_spriteAt(player->render, (facing->info & 0xffff))->scale;
			}
		}
		player->prevWeapon = player->ce.weapon;
	}
	player->ce.weapon = (uint8_t)w;
	Canvas_drawPlayingSoftKeys(player->canvas);
	if (w == 10)
		Combat_checkSurrounded(player->app->combat);
}

/* 0x00024530  next owned weapon after the current one, wrapping */
void Player_selectNextWeapon(Player *player)
{
	int cur = player->ce.weapon;
	int w;

	for (w = cur + 1; w < 11; w++) {
		if (player->weapons & bitOf(w)) {
			Player_selectWeapon(player, w);
			return;
		}
	}
	for (w = 0; w < cur; w++) {
		if (player->weapons & bitOf(w)) {
			Player_selectWeapon(player, w);
			return;
		}
	}
}

/* 0x00024578 */
void Player_selectPrevWeapon(Player *player)
{
	int cur = player->ce.weapon;
	int w;

	for (w = cur - 1; w >= 0; w--) {
		if (player->weapons & bitOf(w)) {
			Player_selectWeapon(player, w);
			return;
		}
	}
	for (w = 10; w > cur; w--) {
		if (player->weapons & bitOf(w)) {
			Player_selectWeapon(player, w);
			return;
		}
	}
}

/* 0x000245c0  equip ring "spell"; taking off ring 8 sobers up */
void Player_selectSpell(Player *player, int spell)
{
	if (player->ring == 8 && player->render->drunkLevel != 0 && player->f110 <= player->totalTurns)
		player->render->drunkTarget = 0;
	player->ring = (uint8_t)spell;
	Player_calcStats(player);
}

/* 0x00008788  take every weapon away (kept in weaponsPending) */
void Player_holsterWeapon(Player *player)
{
	player->weaponsPending = player->weapons;
	player->weapons = 0;
	player->ce.weapon = 0;
}

/* 0x0000dacc  give the holstered weapons back */
void Player_refreshWeapons(Player *player)
{
	player->weapons |= player->weaponsPending;
	player->weaponsPending = 0;
	if ((player->weapons & bitOf(player->ce.weapon)) == 0)
		Player_selectNextWeapon(player);
}

/* ---------------------------------------------------------------- level-select kits */

static void giveSilently(Player *p, int type, int idx, int amount)
{
	Player_give(p, type, idx, amount, 1);
}

/* 0x0000dcd0  debug / level-select: the kit a player would have on map mapId */
void Player_equipForMap(Player *player, int mapId)
{
	CombatEntity *ce = &player->ce;
	uint8_t savedHelp;
	int level = 1;

	Player_reset(player);
	player->maxMapReached = (uint16_t)mapId;
	savedHelp = player->helpEnabled;
	player->helpEnabled = 0;

	if (mapId == 0x1771) {
		level = 2;
		giveSilently(player, 4, 0, 58);
		CombatEntity_setArmor(ce, 1);
		giveSilently(player, 2, 2, 1);
		giveSilently(player, 0, 12, 1);
		giveSilently(player, 0, 0, 10);
		giveSilently(player, 0, 2, 9);
		giveSilently(player, 0, 3, 3);
		giveSilently(player, 0, 4, 1);
		giveSilently(player, 0, 6, 1);
	} else if (mapId == 0x1772) {
		level = 4;
		giveSilently(player, 4, 0, 23);
		CombatEntity_setArmor(ce, 1);
		giveSilently(player, 2, 2, 1);
		giveSilently(player, 2, 5, 1);
		giveSilently(player, 0, 12, 1);
		giveSilently(player, 0, 0, 16);
		giveSilently(player, 0, 1, 3);
		giveSilently(player, 0, 2, 4);
		giveSilently(player, 0, 3, 5);
		giveSilently(player, 0, 4, 4);
		giveSilently(player, 0, 6, 2);
		giveSilently(player, 3, 3, 25);
		giveSilently(player, 3, 2, 25);
		player->weapons = (player->weapons & ~0x70u) | 0x90;
	} else if (mapId == 0x1773) {
		level = 5;
		giveSilently(player, 4, 0, 94);
		CombatEntity_setArmor(ce, 1);
		giveSilently(player, 2, 2, 1);
		giveSilently(player, 2, 5, 1);
		giveSilently(player, 2, 7, 1);
		giveSilently(player, 0, 12, 2);
		giveSilently(player, 0, 11, 4);
		giveSilently(player, 0, 0, 23);
		giveSilently(player, 0, 1, 20);
		giveSilently(player, 0, 2, 9);
		giveSilently(player, 0, 3, 9);
		giveSilently(player, 0, 4, 9);
		giveSilently(player, 0, 6, 4);
		giveSilently(player, 0, 5, 5);
		giveSilently(player, 0, 7, 4);
		giveSilently(player, 3, 3, 20);
		giveSilently(player, 3, 2, 40);
		player->weapons = (player->weapons & ~0x7eu) | 0x94;
	} else if (mapId == 0x1774) {
		level = 6;
		giveSilently(player, 4, 0, 296);
		CombatEntity_setArmor(ce, 2);
		giveSilently(player, 2, 2, 1);
		giveSilently(player, 2, 5, 1);
		giveSilently(player, 2, 7, 1);
		giveSilently(player, 0, 12, 2);
		giveSilently(player, 0, 11, 5);
		giveSilently(player, 0, 0, 32);
		giveSilently(player, 0, 1, 30);
		giveSilently(player, 0, 2, 13);
		giveSilently(player, 0, 3, 13);
		giveSilently(player, 0, 4, 13);
		giveSilently(player, 0, 6, 10);
		giveSilently(player, 0, 5, 10);
		giveSilently(player, 0, 7, 5);
		giveSilently(player, 0, 9, 2);
		giveSilently(player, 3, 3, 16);
		giveSilently(player, 3, 2, 45);
		giveSilently(player, 3, 4, 25);
		player->weapons = (player->weapons & ~0x7eu) | 0x294;
	} else if (mapId == 0x1775) {
		level = 7;
		giveSilently(player, 4, 0, 3240);
		CombatEntity_setArmor(ce, 2);
		giveSilently(player, 2, 2, 1);
		giveSilently(player, 2, 5, 1);
		giveSilently(player, 2, 7, 1);
		giveSilently(player, 2, 8, 1);
		giveSilently(player, 0, 12, 3);
		giveSilently(player, 0, 11, 8);
		giveSilently(player, 0, 0, 36);
		giveSilently(player, 0, 1, 38);
		giveSilently(player, 0, 2, 18);
		giveSilently(player, 0, 3, 13);
		giveSilently(player, 0, 4, 16);
		giveSilently(player, 0, 6, 12);
		giveSilently(player, 0, 5, 11);
		giveSilently(player, 0, 7, 6);
		giveSilently(player, 0, 9, 5);
		giveSilently(player, 0, 10, 6);
		giveSilently(player, 3, 3, 14);
		giveSilently(player, 3, 2, 54);
		giveSilently(player, 3, 4, 21);
		player->weapons = (player->weapons & ~0x7eu) | 0x394;
	}

	if (mapId == 0x1776) {
		level = 7;
		giveSilently(player, 4, 0, 1592);
		CombatEntity_setArmor(ce, 2);
		giveSilently(player, 2, 2, 1);
		giveSilently(player, 2, 5, 1);
		giveSilently(player, 2, 7, 1);
		giveSilently(player, 2, 8, 1);
		giveSilently(player, 2, 6, 1);
		giveSilently(player, 0, 12, 3);
		giveSilently(player, 0, 11, 11);
		giveSilently(player, 0, 0, 32);
		giveSilently(player, 0, 1, 38);
		giveSilently(player, 0, 2, 20);
		giveSilently(player, 0, 3, 15);
		giveSilently(player, 0, 4, 20);
		giveSilently(player, 0, 6, 12);
		giveSilently(player, 0, 5, 12);
		giveSilently(player, 0, 7, 10);
		giveSilently(player, 0, 9, 5);
		giveSilently(player, 0, 10, 6);
		giveSilently(player, 0, 8, 3);
		giveSilently(player, 3, 3, 14);
		giveSilently(player, 3, 2, 90);
		giveSilently(player, 3, 4, 18);
		player->weapons = (player->weapons & ~0x7eu) | 0x7cc;
	}
	if (mapId == 0x1777) {
		level = 8;
		giveSilently(player, 4, 0, 5977);
		CombatEntity_setArmor(ce, 3);
		giveSilently(player, 2, 2, 1);
		giveSilently(player, 2, 5, 1);
		giveSilently(player, 2, 7, 1);
		giveSilently(player, 2, 8, 1);
		giveSilently(player, 2, 6, 1);
		giveSilently(player, 2, 4, 1);
		giveSilently(player, 0, 12, 7);
		giveSilently(player, 0, 11, 11);
		giveSilently(player, 0, 0, 28);
		giveSilently(player, 0, 1, 45);
		giveSilently(player, 0, 2, 24);
		giveSilently(player, 0, 3, 17);
		giveSilently(player, 0, 4, 20);
		giveSilently(player, 0, 6, 15);
		giveSilently(player, 0, 5, 14);
		giveSilently(player, 0, 7, 14);
		giveSilently(player, 0, 9, 11);
		giveSilently(player, 0, 10, 7);
		giveSilently(player, 0, 8, 8);
		giveSilently(player, 3, 3, 10);
		giveSilently(player, 3, 2, 93);
		giveSilently(player, 3, 4, 18);
		player->weapons = (player->weapons & ~0x7eu) | 0x7cc;
	}

	while (player->level != level)
		Player_levelUp(player);
	Player_give(player, 3, 1, 99, 0);
	player->helpEnabled = savedHelp;
	Player_selectNextWeapon(player);
}

/* ---------------------------------------------------------------- help popups */

static uint32_t helpTextOffset(int id)
{
	switch (id) {
	case 0: return MOD_HELP_TALK;
	case 3: return MOD_HELP_RUNE_LOCKS;
	case 4: return MOD_HELP_BARRICADES;
	case 5: return MOD_HELP_WIELD_WAND_ME;
	case 6:
	case 7: return MOD_HELP_WIELD_WAND;
	case 8: return MOD_HELP_FOOD;
	case 9: return MOD_HELP_GOLD;
	case 10: return MOD_HELP_NOTE;
	case 11: return MOD_HELP_DESTRUCTIBLE;
	case 13: return MOD_HELP_FIRE_PORTAL;
	case 14: return MOD_HELP_FIRE_PORTAL_OFF;
	case 16: return MOD_HELP_WAND_STOLEN;
	case 18: return MOD_HELP_DOORS;
	case 19: return MOD_HELP_CRYSTALS;
	case 20: return MOD_HELP_ARMOR_LOW;
	default: return 0;
	}
}

/* 0x000268ec  each help text is shown once (Player.helpShown) */
void Player_showHelp(Player *player, int id, int force)
{
	uint32_t bit, off;
	char *text;

	/* not while a skippable camera sequence plays */
	if (player->game->skippable != 0 && player->game->activeCamera != NULL)
		return;
	if (player->helpEnabled == 0 && force == 0)
		return;

	bit = bitOf(id);
	if (player->helpShown & bit)
		return;
	player->helpShown |= bit;

	off = helpTextOffset(id);
	text = off != 0 ? (char *)ModData_str(&g_mod, off) : NULL;
	Canvas_addHelpMessage(player->canvas, text, 0);
	if (id != 10)
		Canvas_dequeueHelpDialog(player->canvas, 0);
}

/* Common body of the "first time you get X" popups: each kind keeps its own
 * bitmask and queues the item's EntityDef (type 6, subtype kind). */
static void showItemHelp(Player *player, uint32_t *mask, int subType, int n, int force)
{
	uint32_t bit;
	void *def;

	if (player->helpEnabled == 0 && force == 0)
		return;
	bit = bitOf(n);
	if (*mask & bit)
		return;
	*mask |= bit;
	def = EntityDefs_find(player->app->entityDefs, ETYPE_ITEM, subType, n);
	Canvas_addHelpText(player->canvas, (char *)def);
}

/* 0x000269fc */
void Player_showInvHelp(Player *player, int n, int force)
{
	showItemHelp(player, &player->itemHelp, 0, n, force);
}

/* 0x00026b24  (never for weapon 0, the wand) */
void Player_showWeaponHelp(Player *pl, int n, int force)
{
	if (pl->helpEnabled == 0 && force == 0)
		return;
	if (n == 0)
		return;
	showItemHelp(pl, &pl->weaponHelp, 1, n, force);
}

/* 0x00026ae4 */
void Player_showRingHelp(Player *pl, int n, int force)
{
	showItemHelp(pl, &pl->ringHelp, 2, n, force);
}

/* 0x00026710 */
void Player_showArmorHelp(Player *player, int n, int force)
{
	showItemHelp(player, &player->armorHelp, 7, n, force);
}
