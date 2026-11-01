/*
 * Combat rules: weapon and monster stat tables, CombatEntity stats, the
 * hit / damage / armour formulas, entity definitions (entities.bin) and the
 * small direction helpers that live next to them in the original.
 *
 * The weapon and monster stat tables are not data in the .mod: the original
 * Combat_startup (0x288d8) builds them with 46 Weapon_init and 40
 * CombatEntity_init calls whose arguments are instruction immediates. We never
 * copy those numbers; Combat_startup below replays that Thumb code from the
 * user's .mod with a tiny interpreter (see combat_replayStartup).
 */

#include "game.h"

#include <stdio.h>

/* ---- .mod data tables ---- */
#define MOD_ARMOR_TABLE          0x2c788u /* 4 x {u16 deflect, u16 durability} */
#define MOD_TILE_DIST_TABLE      0x2ca70u /* 15 x s32 squared world distance thresholds */
#define MOD_MONSTER_ATTACKS      0x2cbfdu /* u8 [eSubType*9 + parm*3 + field] */
#define MOD_ADJACENT_DIRS        0x2ccbcu /* 4 x {s32 dx, s32 dy} */

/* ---- .mod strings ---- */
#define MOD_STR_BAD_ANGLE        0x30528u
#define MOD_STR_BAD_DIRECTION    0x30514u
#define MOD_STR_DEFLECT          0x2d060u
#define MOD_STR_DURABILITY       0x2d04cu
#define MOD_STR_ARMOR_HELP       0x2d070u
#define MOD_STR_DAMAGE_NA        0x2ce60u
#define MOD_STR_BASE_DAMAGE      0x2ce70u
#define MOD_STR_BASE_ACCURACY    0x2ce84u
#define MOD_STR_RANGE_ONE        0x2ceb4u
#define MOD_STR_RANGE_SPAN       0x2ce9cu
#define MOD_STR_SPECIAL_FIRE     0x2cec8u /* weapon 2 */
#define MOD_STR_SPECIAL_EXPLODE  0x2cee8u /* weapon 7 */
#define MOD_STR_SPECIAL_AREA     0x2cf30u /* weapon 9 */
#define MOD_STR_SPECIAL_MAGIC    0x2cf64u /* weapons 0 and 3 */
#define MOD_STR_SPECIAL_PETRIFY  0x2cf88u /* weapon 8 */
#define MOD_STR_SPECIAL_SPIN     0x2cfacu /* weapon 10 */
#define MOD_STR_SPECIAL_KNOCK    0x2d014u /* weapon 6 */
#define MOD_STR_EMPTY            0x30510u /* (EntityDef with no name) */

/* ---- code in the .mod replayed by Combat_startup ---- */
#define MOD_CODE_COMBAT_STARTUP     0x288d8u
#define MOD_CODE_COMBAT_STARTUP_END 0x2959cu
#define MOD_CODE_COMBATENTITY_INIT  0x17984u
#define MOD_CODE_WEAPON_INIT        0x179c4u

/* Original layout of the Combat object, as addressed by the startup code. */
#define ORIG_COMBAT_CHARGE_LEVEL  0x80
#define ORIG_COMBAT_CRIT          0x78
#define ORIG_COMBAT_HIT           0x79
#define ORIG_COMBAT_FUMBLE        0x7a
#define ORIG_COMBAT_WEAPONS       0xa4
#define ORIG_WEAPON_SIZE          0x24
#define ORIG_COMBAT_MONSTERSTATS  0x788
#define ORIG_COMBATENTITY_SIZE    0x18
#define ORIG_COMBAT_NAMES         0xc08
#define ORIG_COMBAT_BACKPTRS      0xcd0 /* app, canvas, game, render, player */
#define ORIG_COMBAT_END           0xce4

#define NUM_WEAPONS       ((int)(sizeof(((Combat *)0)->weapons) / sizeof(Weapon)))
#define NUM_MONSTER_STATS ((int)(sizeof(((Combat *)0)->monsterStats) / sizeof(CombatEntity)))
#define NUM_MONSTER_NAMES ((int)(sizeof(((Combat *)0)->monsterNames) / sizeof(char *)))

/* CombatEntity.type of the player */
#define CE_TYPE_PLAYER (-1)

/* Combat result flags (res[0]) */
enum {
	HIT_HIT = 0x1,
	HIT_CRIT = 0x2,
	HIT_AVERAGE = 0x4,       /* in: damage = middle of the range */
	HIT_IGNORE_RANGE = 0x8,  /* in */
	HIT_NO_STR_BONUS = 0x10, /* in (petrify) */
	HIT_STRICT_RANGE = 0x20, /* in */
	HIT_PLAYER_BONUS = 0x40, /* in: accuracy relative to the base stat */
	HIT_SUMMONED = 0x100,    /* in: treat the defender as class 5 */
	HIT_BACKSTAB = 0x200,    /* in */
	HIT_OUT_OF_RANGE = 0x400,
	HIT_SHIELD = 0x800,      /* in */
};

static int armorDeflect(int armorType) { return ModData_u16(&g_mod, MOD_ARMOR_TABLE + armorType * 4); }
static int armorDurability(int armorType) { return ModData_u16(&g_mod, MOD_ARMOR_TABLE + armorType * 4 + 2); }

/* 1 << n as the ARM core computes it (0 for n >= 32). */
static uint32_t bit32(int n) { return (n >= 0 && n < 32) ? (1u << n) : 0u; }

static char *modStr(uint32_t off) { return (char *)ModData_str(&g_mod, off); }

/* ------------------------------------------------------------------ directions */

/* 0x00000fe4: angle (multiple of 0x80) -> direction code */
int Dir_fromAngle(int angle)
{
	if ((angle & 0x7f) == 0) {
		switch (angle >> 7) {
		case 0: return 1;
		case 1: return 4;
		case 2: return 2;
		case 3: return 5;
		case 4: return 0;
		case 5: return 7;
		case 6: return 3;
		case 7: return 6;
		default: break;
		}
	}
	App_setErrorCode(g_app, 1);
	App_setErrorStr(g_app, modStr(MOD_STR_BAD_ANGLE), angle);
	return 0;
}

/* 0x00001474: direction code -> angle */
int Dir_toAngle(int dir)
{
	switch (dir) {
	case 0: return 0x200;
	case 1: return 0;
	case 2: return 0x100;
	case 3: return 0x300;
	case 4: return 0x80;
	case 5: return 0x180;
	case 6: return 0x380;
	case 7: return 0x280;
	default:
		App_setErrorCode(g_app, 1);
		App_setErrorStr(g_app, modStr(MOD_STR_BAD_DIRECTION), dir);
		return 0;
	}
}

/* 0x00005ed4: side of a viewer at (ax, ay) facing `angle` on which (bx, by)
 * lies: 0 front (or same tile), 1 and 3 the sides, 2 behind. The first
 * argument is unused (it was the object pointer in the original). */
int Dir_relativeQuadrant(int x, int ax, int ay, int angle, int bx, int by)
{
	switch (angle & 0x3ff) {
	case 0:
		if (ay < by)
			return 1;
		if (by < ay)
			return 3;
		if (bx < ax)
			return 2;
		break;
	case 0x100:
		if (ay < by)
			return 2;
		if (ay == by) {
			if (bx < ax)
				return 3;
			if (ax < bx)
				return 1;
		}
		break;
	case 0x200:
		if (ay < by)
			return 3;
		if (by < ay)
			return 1;
		if (ax < bx)
			return 2;
		break;
	case 0x300:
		if (by <= ay) {
			if (by < ay)
				return 2;
			if (bx < ax)
				return 1;
			if (ax < bx)
				return 3;
		}
		break;
	}
	return 0;
}

/* ------------------------------------------------------------------ distances */

/* 0x00002a60: squared world distance -> tile distance (0..15) */
int Combat_worldDistToTileDist(int distSq)
{
	for (int k = 0; k < 15; k++) {
		if (distSq < ModData_s32(&g_mod, MOD_TILE_DIST_TABLE + k * 4))
			return k;
	}
	return 15;
}

/* 0x00006a50 */
void Combat_calcTargetDist(Combat *combat)
{
	combat->targetDistSq = Entity_distSqFrom(combat->target, combat->canvas->viewX, combat->canvas->viewY);
	combat->targetTileDist = Combat_worldDistToTileDist(combat->targetDistSq);
}

/* 0x0000662c: can the player's current weapon reach this (live) entity? */
int Combat_targetInWeaponRange(Combat *combat, void *target)
{
	Entity *ent = target;
	Player *player = combat->player;
	Weapon *w = &combat->weapons[player->ce.weapon];

	/* The original measured the distance before testing for NULL; the port
	 * checks first so that Entity_distSqFrom is never handed a null entity. */
	if (ent == NULL)
		return 0;
	int dist = Combat_worldDistToTileDist(Entity_distSqFrom(ent, player->ce.x, player->ce.y));
	if (dist < w->rangeMin || dist > w->rangeMax)
		return 0;
	if (!(ent->info & 0x40000))
		return 0;
	if (ent->def->eType == 10 && !((uint32_t)ent->def->parm & bit32(combat->player->ce.weapon)))
		return 0;
	return 1;
}

/* 0x00006ecc: the spinning weapon (10) hits everything around the player;
 * flag the attack when two or more monsters stand on the 4 adjacent tiles. */
void Combat_checkSurrounded(Combat *combat)
{
	uint8_t surrounded = 0;

	if (combat->player->ce.weapon == 10 && combat->weapons[10].ammoUsage <= combat->player->ammo[1]) {
		int tx = combat->canvas->destX >> 6;
		int ty = combat->canvas->destY >> 6;
		int count = 0;
		for (int i = 0; i < 4; i++) {
			int dx = ModData_s32(&g_mod, MOD_ADJACENT_DIRS + i * 8);
			int dy = ModData_s32(&g_mod, MOD_ADJACENT_DIRS + i * 8 + 4);
			if (Game_findEntityAt(combat->game, (dx + tx) * 64, (dy + ty) * 64, 4) != NULL)
				count++;
		}
		if (count > 1)
			surrounded = 1;
	}
	combat->surrounded = surrounded;
}

/* ------------------------------------------------------------------ CombatEntity stats */

/* 0x00012994 */
int CombatEntity_getAccuracy(CombatEntity *ce) { return ce->accuracy; }
/* 0x00012a04 */
int CombatEntity_getDefense(CombatEntity *ce) { return ce->defense; }
/* 0x00012a08 */
int CombatEntity_getArmorPoints(CombatEntity *ce) { return (uint16_t)ce->armor; }
/* 0x00012a44 */
int CombatEntity_getHealth(CombatEntity *ce) { return (uint16_t)ce->health; }
/* 0x00012af8 */
int CombatEntity_getMaxHealth(CombatEntity *ce) { return (uint16_t)ce->maxHealth; }
/* 0x00012e70 */
int CombatEntity_getStrength(CombatEntity *ce) { return ce->strength; }

/* 0x00012ae4: durability of a fresh piece of the worn armour type */
int CombatEntity_getMaxArmorPoints(CombatEntity *ce)
{
	return armorDurability(ce->armorType);
}

static int clampByte(int v)
{
	if (v < 0)
		return 0;
	if (v > 0xff)
		return 0xff;
	return v;
}

/* 0x0002581c */
void CombatEntity_setAccuracy(CombatEntity *ce, int v) { ce->accuracy = (uint8_t)clampByte(v); }
/* 0x0002589c */
void CombatEntity_setDefense(CombatEntity *ce, int v) { ce->defense = (uint8_t)clampByte(v); }
/* 0x00026444 */
void CombatEntity_setStrength(CombatEntity *ce, int v) { ce->strength = (uint8_t)clampByte(v); }

/* 0x00025864: new armour starts at full durability */
void CombatEntity_setArmor(CombatEntity *ce, int type)
{
	ce->armorType = (int8_t)type;
	ce->armor = (int16_t)armorDurability(type);
}

/* 0x000258b0 */
void CombatEntity_setHealth(CombatEntity *ce, int v)
{
	if (v < 0)
		v = 0;
	else if (v > (uint16_t)ce->maxHealth)
		v = (uint16_t)ce->maxHealth;
	ce->health = (int16_t)v;
}

/* 0x00025a80 */
void CombatEntity_setMaxHealth(CombatEntity *ce, int v)
{
	if (v < 0)
		v = 0;
	else if (v > 999)
		v = 999;
	ce->maxHealth = (int16_t)v;
}

/* 0x00003070: armour durability +/- amount, clamped to the armour type's
 * maximum; also feeds the armour lost / gained statistics. */
void CombatEntity_addArmor(CombatEntity *ce, int amount)
{
	if (amount < 0)
		g_app->statArmorLost += amount;
	else
		g_app->statArmorGained += amount;

	int v = (uint16_t)ce->armor + amount;
	if (v < 0) {
		v = 0;
	} else if (v > armorDurability(ce->armorType)) {
		ce->armor = (int16_t)armorDurability(ce->armorType);
		return;
	}
	ce->armor = (int16_t)v;
}

/* 0x00017984: one monster stat block (also the player's base block) */
void CombatEntity_init(CombatEntity *ce, int type, int maxHealth, int defense, int strength, int accuracy)
{
	CombatEntity_setMaxHealth(ce, maxHealth);
	CombatEntity_setHealth(ce, maxHealth);
	CombatEntity_setDefense(ce, defense);
	CombatEntity_setStrength(ce, strength);
	CombatEntity_setAccuracy(ce, accuracy);
	ce->type = (int8_t)type;
	ce->level = 1;
}

/* 0x00006abc: experience for killing a monster with these stats */
int CombatEntity_calcXP(CombatEntity *ce)
{
	return ((ce->defense + ce->strength) * 5 + ce->accuracy * 6 + (uint16_t)ce->maxHealth * 5 + 49) / 50;
}

/* 0x000237d8: the player's block is saved in full; monsters save what their
 * class can change at run time, then health. */
void CombatEntity_saveState(CombatEntity *ce, void *os)
{
	switch (ce->type) {
	case CE_TYPE_PLAYER:
		OutStream_writeBool(os, (int8_t)ce->level);
		OutStream_writeBool(os, (int8_t)ce->strength);
		OutStream_writeBool(os, (int8_t)ce->accuracy);
		OutStream_writeBool(os, (int8_t)ce->defense);
		OutStream_writeBool(os, (int8_t)ce->weapon);
		OutStream_writeBool(os, ce->armorType);
		OutStream_writeShort(os, ce->armor);
		/* fall through: the player's maxHealth follows (0x2382a b 0x23830) */
	case 0xf:
		OutStream_writeShort(os, ce->maxHealth);
		break;
	case 0xd:
		OutStream_writeShort(os, ce->maxHealth);
		OutStream_writeBool(os, (int8_t)ce->strength);
		OutStream_writeBool(os, (int8_t)ce->accuracy);
		OutStream_writeBool(os, (int8_t)ce->defense);
		break;
	case 4:
		OutStream_writeShort(os, ce->maxHealth);
		OutStream_writeBool(os, (int8_t)ce->strength);
		break;
	default:
		break;
	}
	OutStream_writeShort(os, ce->health);
}

/* 0x0001a640: mirror of CombatEntity_saveState */
void CombatEntity_loadState(CombatEntity *ce, void *file)
{
	File *in = file;

	switch (ce->type) {
	case CE_TYPE_PLAYER:
		ce->level = (uint8_t)Stream_readByte(in);
		ce->strength = (uint8_t)Stream_readByte(in);
		ce->accuracy = (uint8_t)Stream_readByte(in);
		ce->defense = (uint8_t)Stream_readByte(in);
		ce->weapon = (uint8_t)Stream_readByte(in);
		ce->armorType = (int8_t)Stream_readByte(in);
		ce->armor = (int16_t)Stream_readShort(in);
		/* fall through: the player's maxHealth follows */
	case 0xf:
		ce->maxHealth = (int16_t)Stream_readShort(in);
		break;
	case 0xd:
		ce->maxHealth = (int16_t)Stream_readShort(in);
		ce->strength = (uint8_t)Stream_readByte(in);
		ce->accuracy = (uint8_t)Stream_readByte(in);
		ce->defense = (uint8_t)Stream_readByte(in);
		break;
	case 4:
		ce->maxHealth = (int16_t)Stream_readShort(in);
		ce->strength = (uint8_t)Stream_readByte(in);
		break;
	default:
		break;
	}
	ce->health = (int16_t)Stream_readShort(in);
}

/* ------------------------------------------------------------------ weapons */

/* 0x000179c4: fill one weapon record (+0 is never written) */
void Weapon_init(Weapon *w, int dmgMin, int dmgMax, int rangeMin, int rangeMax, int accuracy, int ammoType,
                 int ammoUsage, int damageScale, int effect, int numShots, int shotDelay)
{
	w->dmgMin = dmgMin;
	w->dmgMax = dmgMax;
	w->rangeMin = (uint8_t)rangeMin;
	w->rangeMax = (uint8_t)rangeMax;
	w->accuracy = accuracy;
	w->ammoType = ammoType;
	w->ammoUsage = ammoUsage;
	w->damageScale = damageScale;
	w->effect = (uint8_t)effect;
	w->numShots = (uint8_t)numShots;
	w->shotDelay = (uint8_t)shotDelay;
}

/* 0x00012b60: one of the 3 attack bytes of a monster definition (by class
 * and tier): 0 and 1 are weapon ids, 2 the chance of picking the second one */
int Combat_getMonsterAttackField(Combat *combat, void *defPtr, int field)
{
	EntityDef *def = defPtr;
	return ModData_u8(&g_mod, MOD_MONSTER_ATTACKS + def->eSubType * 9 + field + def->parm * 3);
}

/* 0x000129f4: {u16 deflect, u16 durability} of an armour type, inside the
 * module image (read-only; little-endian like the phone). */
void *Combat_getArmorStats(int armorType)
{
	return (void *)ModData_ptr(&g_mod, MOD_ARMOR_TABLE + armorType * 4, 4);
}

/* 0x00012998: armour description shown in the inventory (armorType -1 = only
 * the help pointer) */
void Combat_getArmorDescription(Combat *combat, int armorType, char *buf)
{
	if (armorType != -1) {
		buf += sprintf(buf, modStr(MOD_STR_DEFLECT), armorDeflect(armorType));
		buf += sprintf(buf, modStr(MOD_STR_DURABILITY), armorDurability(armorType));
	}
	sprintf(buf, "%s", modStr(MOD_STR_ARMOR_HELP));
}

/* 0x00012e74: weapon description shown in the inventory */
void Combat_getWeaponDescription(Combat *combat, int weapon, char *buf)
{
	Weapon *w = &combat->weapons[weapon];

	if (weapon == 8)
		buf += sprintf(buf, "%s", modStr(MOD_STR_DAMAGE_NA));
	else
		buf += sprintf(buf, modStr(MOD_STR_BASE_DAMAGE), w->dmgMin);
	buf += sprintf(buf, modStr(MOD_STR_BASE_ACCURACY), (w->accuracy * 100) >> 8);
	if (w->rangeMin == w->rangeMax)
		buf += sprintf(buf, modStr(MOD_STR_RANGE_ONE), w->rangeMin);
	else
		buf += sprintf(buf, modStr(MOD_STR_RANGE_SPAN), w->rangeMin, w->rangeMax);

	uint32_t special = 0;
	switch (weapon) {
	case 2: special = MOD_STR_SPECIAL_FIRE; break;
	case 7: special = MOD_STR_SPECIAL_EXPLODE; break;
	case 9: special = MOD_STR_SPECIAL_AREA; break;
	case 0:
	case 3: special = MOD_STR_SPECIAL_MAGIC; break;
	case 8: special = MOD_STR_SPECIAL_PETRIFY; break;
	case 10: special = MOD_STR_SPECIAL_SPIN; break;
	case 6: special = MOD_STR_SPECIAL_KNOCK; break;
	default: break;
	}
	if (special)
		sprintf(buf, "%s", modStr(special));
}

/* ------------------------------------------------------------------ hit and damage */

/* 0x00005f4c: damage multiplier (/256) of a weapon against a defender class.
 * Weapons 4..6 and 1..3 form two groups the classes treat alike. */
int Combat_weaponVsTypeMod(void *weaponPtr, void *defender, short *res)
{
	Weapon *weapon = weaponPtr;
	int w = (int)(weapon - g_app->combat->weapons);
	uint32_t bit = bit32(w & 0xff);
	int group456 = ((bit & 0x7f) >> 4) != 0;
	int group123 = ((bit & 0xf) >> 1) != 0;
	int type = ((CombatEntity *)defender)->type;

	if (res[0] & HIT_SUMMONED)
		type = 5;

	switch (type) {
	case 0:
		return group456 ? 0x180 : 0x100;
	case 2:
		if (w == 9)
			return 0x200;
		if (w == 2 || w == 7)
			return 0x180;
		return (w != 0 && !group456) ? 0x100 : 0xc0;
	case 4:
		if (w == 0 || w == 3 || w == 9)
			return 0x180;
		if (w == 7)
			return 0x140;
		if (w == 1)
			return 0x80;
		return group456 ? 0x80 : 0x100;
	case 5:
		if (w == 0 || w == 3)
			return 0x180;
		if (w == 8)
			return 0;
		if (w == 10)
			return 0x100;
		return 0x80;
	case 6:
		return group123 ? 0x140 : 0x100;
	case 7:
		if (w == 9)
			return 0x180;
		if (w == 0 || w == 1 || w == 3)
			return 0x80;
		return group456 ? 0x80 : 0x100;
	case 8:
		return (w == 0 || w == 3) ? 0x180 : 0x100;
	case 9:
		if (w == 2)
			return 0xc0;
		if (w == 7 || w == 9)
			return 0x80;
		if (w == 8)
			return 0x180;
		return 0x100;
	case 10:
		if (group123)
			return 0x180;
		return (w != 0 && !group456) ? 0x100 : 0xc0;
	case 0xb:
		if (w == 9 || w == 2 || w == 7)
			return 0x140;
		return (w == 0 || w == 3) ? 0x40 : 0x100;
	case 0xd:
		if (!(res[0] & HIT_SHIELD))
			return 0x100;
		if (w == 10)
			return 0x200;
		if (w == 0 || w == 8)
			return 0x100;
		return 0x4c;
	case 0xf:
		return w == 10 ? 0x240 : 0;
	default:
		return 0x100;
	}
}

/* 0x00006480: range check, then the hit and crit rolls.
 * res[0] holds the input flags and receives HIT_HIT / HIT_CRIT /
 * HIT_OUT_OF_RANGE. The original also left the tile distance and the two
 * chances in the bytes after res[2] (Combat+0x42/+0x44/+0x48); nothing reads
 * them back, so the port keeps them local and never writes past res[2]. */
int CombatEntity_calcHit(CombatEntity *atk, void *defPtr, short *res)
{
	CombatEntity *def = defPtr;
	Combat *combat = g_app->combat;
	Weapon *w = &combat->weapons[atk->weapon];
	int flags = res[0];
	int outOfRange = 0;

	int dx = atk->x - def->x;
	int dy = atk->y - def->y;
	int dist = Combat_worldDistToTileDist(dx * dx + dy * dy);
	if (dist < w->rangeMin)
		outOfRange = w->rangeMin - dist;
	else if (dist > w->rangeMax)
		outOfRange = dist - w->rangeMax;

	if (flags & HIT_IGNORE_RANGE) {
		outOfRange = 0;
	} else if (atk->weapon != 8 && (w->rangeMin == w->rangeMax || (flags & HIT_STRICT_RANGE)) && outOfRange > 0) {
		res[0] = (short)(flags | HIT_OUT_OF_RANGE);
		return res[0];
	}

	int hitChance;
	if (atk->type == CE_TYPE_PLAYER && (flags & HIT_PLAYER_BONUS))
		hitChance = (atk->accuracy - g_app->player->baseCe.accuracy) * 6 + w->accuracy;
	else
		hitChance = atk->accuracy * 6 + w->accuracy - 60;
	hitChance -= def->defense * 4 - 48;
	if (atk->weapon == 8 && dist == 1)
		hitChance += g_app->player->missStreak * 32;
	else
		hitChance -= outOfRange * 64;
	if (hitChance < 1)
		hitChance = 1;

	int critChance;
	if (atk->type == CE_TYPE_PLAYER) {
		critChance = atk->accuracy * 6 - 120;
		critChance -= def->defense * 3 - 30;
		if (critChance < 1)
			critChance = 1;
	} else {
		critChance = 0;
	}

	if (Random_nextByte(&g_app->random) > hitChance) {
		if (dist == 1)
			g_app->player->missStreak++;
		return res[0];
	}
	int result = (Random_nextByte(&g_app->random) < critChance ? HIT_CRIT : HIT_HIT) | res[0];
	res[0] = (short)result;
	return (short)result;
}

/* 0x000060a0: damage of a hit (res[1]); res[2] = 1 when armour absorbed part */
int CombatEntity_calcDamage(CombatEntity *atk, void *defPtr, short *res)
{
	CombatEntity *def = defPtr;
	Combat *combat = g_app->combat;
	Weapon *w = &combat->weapons[atk->weapon];
	int dmgMin = w->dmgMin;
	int dmgMax = w->dmgMax;
	int base = dmgMin;

	if (res[0] & HIT_CRIT)
		base = dmgMax * 2;
	else if (res[0] & HIT_AVERAGE)
		base = (dmgMax - dmgMin) / 2 + dmgMin;
	else if (dmgMin != dmgMax)
		base = dmgMin + Random_nextByte(&g_app->random) % (dmgMax - dmgMin);

	if (!((uint16_t)res[0] & HIT_NO_STR_BONUS) && atk->strength > 20)
		base += (atk->strength - 20) / 2;

	int mod = Combat_weaponVsTypeMod(w, def, res);
	int flags = res[0];
	if (flags & HIT_BACKSTAB)
		mod = (mod * 0xc0) >> 8;
	if (mod == 0)
		return 0;

	if ((flags & HIT_CRIT) && atk->accuracy > 25)
		mod += (atk->accuracy * 12 - 300) / 5;

	int dmg = (mod * base * 256) >> 16;
	if (dmg >= 0) {
		if (dmg < 1)
			dmg = 1;
		int absorb = (def->defense - 20) / 4;
		if (absorb < 0)
			absorb = 0;
		short armorHit = 0;
		if (def->armor != 0) {
			int deflect = armorDeflect(def->armorType);
			int pierce = atk->weapon == 0x14 || atk->weapon == 0x1a || atk->weapon == 0x1d ||
			             atk->weapon == 0x1e || atk->weapon == 0x24 || ((flags & 0x3ff) >> 8) != 0;
			if (def->armorType == 3 && pierce)
				deflect >>= 1;
			absorb += deflect;
			armorHit = 1;
		}
		res[2] = armorHit;
		int scaled = (absorb * w->damageScale) >> 8;
		int taken = scaled < dmg ? scaled : dmg;
		dmg -= taken & 0xff; /* only the low byte is subtracted: the original does this too */
		if (dmg < 0)
			dmg = 0;
		if (dmg > 999)
			dmg = 999;
	}
	res[1] = (short)dmg;
	return (short)dmg;
}

/* 0x00005e90: hit roll then damage. The petrifying weapon (8) rolls its
 * damage without crit and strength bonus; the flags of the hit roll are
 * restored afterwards. */
void CombatEntity_calcCombat(CombatEntity *atk, void *def, short *res)
{
	res[1] = 0;
	int flags = CombatEntity_calcHit(atk, def, res);
	if (!(flags & (HIT_HIT | HIT_CRIT)))
		return;
	if (atk->weapon == 8)
		res[0] = (short)((res[0] & 0xfffd) | 0x11);
	int dmg = CombatEntity_calcDamage(atk, def, res);
	if (dmg != 0) {
		res[0] = (short)flags;
		res[1] = (short)dmg;
	}
}

/* 0x00005da8: particle scale for the hit (8.8 -> integer, at least 1) and,
 * on an overkill, the decision to gib the target. */
int Combat_calcGibScale(Combat *combat, int kill)
{
	if (combat->shotDamage == 0)
		return 0;

	int maxHp = CombatEntity_getMaxHealth(&combat->target->monster->ce);
	int ratio = (combat->shotDamage << 16) / (maxHp << 8);
	int scale = (ratio * 0x3000) >> 8;

	if (combat->targetHpAfter < 1) {
		scale = (scale << 9) >> 8;
		if (kill == 1) {
			Entity *ent = combat->target;
			int sub = combat->targetSubtype;
			int gib = 0;
			if (!(ent->monster->flags & 0x800)) {
				if (ent->monster->petrifyTurns != 0)
					gib = 1;
				else if (combat->crit && sub != 0xb && sub != 5 && sub != 4 && sub != 2 &&
				         !(ent->info & 0x11000000) && !Entity_isBoss(ent))
					gib = 1;
			}
			if (gib) {
				combat->f7c = 1;
				combat->target->info |= 0x20000;
				combat->app->statGibs++;
				Render_spriteAt(combat->render, (combat->target->info & 0xffff))->info |= 0x10000;
				Hud_randomBark(combat->app->hud, 4, 0);
			}
		}
	}

	int v = scale + 0x80;
	if (v < 0x100)
		v = 0x100;
	return v >> 8;
}

/* 0x0001db24: ARGB colour of a monster's blood / debris */
int Combat_getBloodColor(void *entPtr)
{
	Entity *ent = entPtr;

	if (ent->monster->petrifyTurns != 0)
		return (int)0xffaaaaaa;
	if (ent->info & 0x1000000)
		return (int)0xffdadada;
	switch (ent->def->eSubType) {
	case 5:
		return (int)0xffdadada;
	case 3:
	case 0xe:
		return (int)0xff00ff00;
	case 4:
		return ent->def->parm == 1 ? (int)0xff2536a5 : (int)0xff916a10;
	case 2:
		return (int)0xff867459;
	default:
		return (int)0xffbb0000;
	}
}

/* 0x00022778: one combat animation step; 1 while the attack is still running */
int Combat_runFrame(Combat *combat)
{
	int done = combat->attacker == NULL ? Combat_playerAttackFrame(combat) : Combat_monsterAttackFrame(combat);
	return done == 0;
}

/* ------------------------------------------------------------------ entity definitions */

/* 0x00011778: definition by type / subtype / parameter (parm -1 = any) */
void *EntityDefs_find(EntityDefManager *defs, int eType, int eSubType, int parm)
{
	for (uint8_t i = 0; i < defs->numDefs; i++) {
		EntityDef *def = &defs->defs[i];
		if (def->eType == eType && def->eSubType == eSubType && (parm == -1 || def->parm == parm))
			return def;
	}
	return NULL;
}

/* 0x0001bdc8: definition by map tile number */
void *EntityDef_lookup(EntityDefManager *defs, int tile)
{
	for (int i = 0; i < defs->numDefs; i++) {
		if (defs->defs[i].tileIndex == tile)
			return &defs->defs[i];
	}
	return NULL;
}

static char *dupString(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);
	if (d)
		memcpy(d, s, n);
	return d;
}

/* 0x00027b28: entities.bin (resource 5001): u16 count, then per definition
 * {u16 tile, u8 icon, u8 nameLen, name, u8 longNameLen, longName, description
 * NUL-terminated, u8 eType, u8 eSubType, s32 parm}. */
int EntityDef_startup(EntityDefManager *edm)
{
	InputStream is;
	char tmp[256];
	int ok = 0;

	edm->numDefs = 0;
	InputStream_ctor(&is, edm->app, 1);
	if (!Resource_open(&is, 5001, 1)) {
		App_setErrorCode(g_app, 0x16);
		goto done;
	}

	uint8_t *buf = InputStream_getBuffer(&is);
	int pos = 0;
	int count = Resource_shiftShort(buf, &pos);
	edm->numDefs = (int16_t)count;
	if (count > 200) {
		App_setErrorCode(g_app, 2);
		goto done;
	}

	for (int i = 0; i < edm->numDefs; i++) {
		EntityDef *def = &edm->defs[i];
		def->tileIndex = (int16_t)Resource_shiftShort(buf, &pos);
		def->icon = buf[pos];
		int len = buf[pos + 1];
		pos += 2;
		if (len == 0) {
			def->name = dupString(ModData_str(&g_mod, MOD_STR_EMPTY));
		} else {
			memcpy(tmp, buf + pos, (size_t)len);
			tmp[len] = '\0';
			def->name = dupString(tmp);
			pos += len;
		}

		len = buf[pos++];
		if (len == 0) {
			def->longName = dupString(def->name);
		} else {
			memcpy(tmp, buf + pos, (size_t)len);
			tmp[len] = '\0';
			def->longName = dupString(tmp);
			pos += len;
		}

		def->description = dupString((const char *)buf + pos);
		pos += (int)strlen(def->description);
		def->eType = buf[pos + 1];
		def->eSubType = buf[pos + 2];
		pos += 3;
		def->parm = Resource_shiftInt(buf, &pos);
	}
	ok = 1;

done:
	InputStream_dtor(&is, 0);
	return ok;
}

/* 0x00026ba0 */
void EntityDefManager_shutdown(EntityDefManager *edm)
{
	/* numDefs can exceed the array after a failed startup (count > 200) */
	int n = edm->numDefs < 200 ? edm->numDefs : 200;
	for (int i = 0; i < n; i++) {
		free(edm->defs[i].name);
		free(edm->defs[i].longName);
		free(edm->defs[i].description);
	}
}

/* ------------------------------------------------------------------ Combat_startup */

/*
 * The original Combat_startup is straight-line Thumb code: it loads each
 * argument with movs/lsls/adds or a literal-pool ldr, spills the stacked ones
 * to [sp], and calls Weapon_init / CombatEntity_init on Combat-relative
 * addresses; at the end it stores pointers to the shadow-monster names (with
 * the PIC idiom `ldr rN, =ofs; add rN, pc`) and clears a few state bytes.
 * We replay exactly that: a value is an immediate, a Combat-relative address,
 * a stack address or unknown; only the instruction forms that function uses
 * are understood, anything else aborts the replay.
 */
enum { TV_UNKNOWN, TV_CONST, TV_COMBAT, TV_STACK };
typedef struct ThumbValue {
	int kind;
	int32_t v;
} ThumbValue;

#define REPLAY_STACK_WORDS 64 /* sp offsets -0x80 .. +0x7c relative to entry */

typedef struct StartupReplay {
	Combat *combat;
	ThumbValue r[16];
	ThumbValue stack[REPLAY_STACK_WORDS];
	int weapons, stats;
} StartupReplay;

static ThumbValue tvConst(int32_t v)
{
	ThumbValue t = { TV_CONST, v };
	return t;
}

static ThumbValue tvUnknown(void)
{
	ThumbValue t = { TV_UNKNOWN, 0 };
	return t;
}

static ThumbValue *replayStackSlot(StartupReplay *st, int32_t spOffset)
{
	int idx = (spOffset + REPLAY_STACK_WORDS * 2) / 4;
	if ((spOffset & 3) || idx < 0 || idx >= REPLAY_STACK_WORDS)
		return NULL;
	return &st->stack[idx];
}

/* n-th stacked call argument ([sp + 4n]); must be an immediate */
static int replayStackArg(StartupReplay *st, int n, int32_t *out)
{
	ThumbValue *slot = replayStackSlot(st, st->r[13].v + n * 4);
	if (!slot || slot->kind != TV_CONST)
		return 0;
	*out = slot->v;
	return 1;
}

/* a + b keeping track of what kind of address the result is */
static ThumbValue replayAdd(ThumbValue a, ThumbValue b)
{
	ThumbValue t = tvUnknown();
	if (a.kind == TV_CONST && b.kind == TV_CONST) {
		t = tvConst(a.v + b.v);
	} else if (a.kind == TV_CONST && (b.kind == TV_COMBAT || b.kind == TV_STACK)) {
		t.kind = b.kind;
		t.v = a.v + b.v;
	} else if (b.kind == TV_CONST && (a.kind == TV_COMBAT || a.kind == TV_STACK)) {
		t.kind = a.kind;
		t.v = a.v + b.v;
	}
	return t;
}

static int replayCall(StartupReplay *st, uint32_t target)
{
	ThumbValue *r = st->r;
	if (r[0].kind != TV_COMBAT || r[1].kind != TV_CONST || r[2].kind != TV_CONST || r[3].kind != TV_CONST)
		return 0;
	int32_t off = r[0].v;

	if (target == MOD_CODE_WEAPON_INIT) {
		int32_t a[8];
		int rel = off - ORIG_COMBAT_WEAPONS;
		int idx = rel / ORIG_WEAPON_SIZE;
		if (rel < 0 || rel % ORIG_WEAPON_SIZE || idx >= NUM_WEAPONS)
			return 0;
		for (int i = 0; i < 8; i++) {
			if (!replayStackArg(st, i, &a[i]))
				return 0;
		}
		Weapon_init(&st->combat->weapons[idx], r[1].v, r[2].v, r[3].v, a[0], a[1], a[2], a[3], a[4], a[5], a[6],
		            a[7]);
		st->weapons++;
	} else if (target == MOD_CODE_COMBATENTITY_INIT) {
		int32_t a[2];
		int rel = off - ORIG_COMBAT_MONSTERSTATS;
		int idx = rel / ORIG_COMBATENTITY_SIZE;
		if (rel < 0 || rel % ORIG_COMBATENTITY_SIZE || idx >= NUM_MONSTER_STATS)
			return 0;
		if (!replayStackArg(st, 0, &a[0]) || !replayStackArg(st, 1, &a[1]))
			return 0;
		CombatEntity_init(&st->combat->monsterStats[idx], r[1].v, r[2].v, r[3].v, a[0], a[1]);
		st->stats++;
	} else {
		return 0;
	}
	/* caller-saved registers are dead after a call */
	r[0] = r[1] = r[2] = r[3] = r[12] = r[14] = tvUnknown();
	return 1;
}

/* A store into the Combat object at original offset `off`. */
static int replayStoreCombat(StartupReplay *st, int32_t off, ThumbValue v, int isByte)
{
	Combat *combat = st->combat;

	/* the back-pointers are copied from the applet; Combat_startup sets them itself */
	if (!isByte && off >= ORIG_COMBAT_BACKPTRS && off < ORIG_COMBAT_END && !(off & 3))
		return 1;
	if (v.kind != TV_CONST)
		return 0;
	if (isByte) {
		switch (off) {
		case ORIG_COMBAT_CRIT: combat->crit = (uint8_t)v.v; return 1;
		case ORIG_COMBAT_HIT: combat->hit = (uint8_t)v.v; return 1;
		case ORIG_COMBAT_FUMBLE: combat->fumble = (uint8_t)v.v; return 1;
		default: return 0;
		}
	}
	if (off == ORIG_COMBAT_CHARGE_LEVEL) {
		combat->chargeLevel = v.v;
		return 1;
	}
	int rel = off - ORIG_COMBAT_NAMES;
	if (rel >= 0 && !(rel & 3) && rel / 4 < NUM_MONSTER_NAMES) {
		/* a module address of a NUL-terminated name, or 0 */
		combat->monsterNames[rel / 4] = v.v ? (char *)ModData_str(&g_mod, (uint32_t)v.v) : NULL;
		return 1;
	}
	return 0;
}

static int replayLoadStore(StartupReplay *st, int load, int isByte, int rn, int rd, int32_t imm)
{
	ThumbValue *r = st->r;
	ThumbValue base = r[rn];

	if (base.kind == TV_STACK) {
		ThumbValue *slot = replayStackSlot(st, base.v + imm);
		if (!slot || isByte)
			return 0;
		if (load)
			r[rd] = *slot;
		else
			*slot = r[rd];
		return 1;
	}
	if (load) {
		r[rd] = tvUnknown(); /* memory contents are not modelled */
		return 1;
	}
	if (base.kind == TV_COMBAT)
		return replayStoreCombat(st, base.v + imm, r[rd], isByte);
	return 0;
}

static int combat_replayStartup(Combat *combat)
{
	StartupReplay st;
	memset(&st, 0, sizeof st);
	st.combat = combat;
	st.r[0].kind = TV_COMBAT;
	st.r[13].kind = TV_STACK;
	ThumbValue *r = st.r;

	uint32_t pc = MOD_CODE_COMBAT_STARTUP;
	while (pc >= MOD_CODE_COMBAT_STARTUP && pc < MOD_CODE_COMBAT_STARTUP_END) {
		uint32_t w = ModData_u16(&g_mod, pc);
		uint32_t next = pc + 2;

		if (w < 0x1800) { /* lsls / lsrs / asrs rd, rm, #imm5 */
			int op = (w >> 11) & 3, imm = (w >> 6) & 31, rm = (w >> 3) & 7, rd = w & 7;
			if (r[rm].kind != TV_CONST) {
				r[rd] = tvUnknown();
			} else {
				uint32_t v = (uint32_t)r[rm].v;
				if (op == 0)
					v = v << imm;
				else if (op == 1)
					v = imm ? v >> imm : 0;
				else
					v = (uint32_t)(imm ? (int32_t)v >> imm : (int32_t)v >> 31);
				r[rd] = tvConst((int32_t)v);
			}
		} else if (w < 0x2000) { /* adds / subs rd, rn, rm|#imm3 */
			int isImm = (w >> 10) & 1, sub = (w >> 9) & 1, x = (w >> 6) & 7, rn = (w >> 3) & 7, rd = w & 7;
			ThumbValue b = isImm ? tvConst(x) : r[x];
			if (sub) {
				if (b.kind != TV_CONST)
					goto unsupported;
				b.v = -b.v;
			}
			r[rd] = replayAdd(r[rn], b);
		} else if (w < 0x4000) { /* movs / cmp / adds / subs rd, #imm8 */
			int op = (w >> 11) & 3, rd = (w >> 8) & 7, imm = w & 0xff;
			if (op == 0)
				r[rd] = tvConst(imm);
			else if (op == 2)
				r[rd] = replayAdd(r[rd], tvConst(imm));
			else if (op == 3)
				r[rd] = replayAdd(r[rd], tvConst(-imm));
			else
				goto unsupported; /* cmp: no conditional code expected */
		} else if (w >= 0x4400 && w < 0x4800) { /* hi-register add / mov / bx */
			int op = (w >> 8) & 3, rm = (w >> 3) & 15, rd = (w & 7) | ((w >> 4) & 8);
			ThumbValue src = rm == 15 ? tvConst((int32_t)(pc + 4)) : r[rm];
			if (op == 3) /* bx: end of the function */
				return st.weapons > 0 && st.stats > 0;
			if (rd == 15 || op == 1)
				goto unsupported;
			if (op == 0)
				r[rd] = replayAdd(r[rd], src); /* `add rN, pc` turns a literal into a module address */
			else
				r[rd] = src;
		} else if (w >= 0x4800 && w < 0x5000) { /* ldr rd, [pc, #imm8*4] */
			uint32_t addr = ((pc + 4) & ~3u) + (w & 0xff) * 4;
			r[(w >> 8) & 7] = tvConst(ModData_s32(&g_mod, addr));
		} else if (w >= 0x6000 && w < 0x8000) { /* ldr/str(b) rd, [rn, #imm5] */
			int isByte = (w >> 12) & 1, load = (w >> 11) & 1;
			int32_t imm = (int32_t)((w >> 6) & 31) * (isByte ? 1 : 4);
			if (!replayLoadStore(&st, load, isByte, (w >> 3) & 7, w & 7, imm))
				goto unsupported;
		} else if (w >= 0x9000 && w < 0xa000) { /* ldr/str rd, [sp, #imm8*4] */
			if (!replayLoadStore(&st, (w >> 11) & 1, 0, 13, (w >> 8) & 7, (int32_t)(w & 0xff) * 4))
				goto unsupported;
		} else if ((w & 0xff00) == 0xb000) { /* add / sub sp, #imm7*4 */
			int32_t imm = (int32_t)(w & 0x7f) * 4;
			r[13].v += (w & 0x80) ? -imm : imm;
		} else if ((w & 0xfe00) == 0xb400) { /* push {list[, lr]} */
			uint32_t list = (w & 0xff) | ((w & 0x100) ? 0x4000 : 0);
			int count = 0;
			for (int i = 0; i < 16; i++)
				count += (list >> i) & 1;
			int32_t addr = r[13].v - count * 4;
			r[13].v = addr;
			for (int i = 0; i < 16; i++) {
				if (!((list >> i) & 1))
					continue;
				ThumbValue *slot = replayStackSlot(&st, addr);
				if (!slot)
					goto unsupported;
				*slot = r[i];
				addr += 4;
			}
		} else if ((w & 0xfe00) == 0xbc00) { /* pop {list[, pc]} */
			if (w & 0x100)
				return st.weapons > 0 && st.stats > 0;
			int32_t addr = r[13].v;
			for (int i = 0; i < 8; i++) {
				if (!((w >> i) & 1))
					continue;
				ThumbValue *slot = replayStackSlot(&st, addr);
				if (!slot)
					goto unsupported;
				r[i] = *slot;
				addr += 4;
			}
			r[13].v = addr;
		} else if ((w & 0xf800) == 0xe000) { /* b: skips a literal pool */
			int32_t off = (int32_t)((w & 0x7ff) << 21) >> 20;
			next = pc + 4 + (uint32_t)off;
		} else if ((w & 0xf800) == 0xf000) { /* bl (two halves) */
			uint32_t lo = ModData_u16(&g_mod, pc + 2);
			if ((lo & 0xf800) != 0xf800)
				goto unsupported;
			int32_t off = ((int32_t)((w & 0x7ff) << 21) >> 9) | (int32_t)((lo & 0x7ff) << 1);
			if (!replayCall(&st, pc + 4 + (uint32_t)off))
				goto unsupported;
			next = pc + 4;
		} else {
			goto unsupported;
		}
		pc = next;
		continue;

	unsupported:
		Oe_warn("Combat_startup: cannot replay Thumb instruction %04x at 0x%05x", (unsigned)w, (unsigned)pc);
		return 0;
	}
	Oe_warn("Combat_startup: replay left the function at 0x%05x", (unsigned)pc);
	return 0;
}

/* 0x000288d8: back-pointers, then the weapon table (weapons[1..]), the
 * monster stat blocks [class*3 + tier], the shadow-monster names, and the
 * attack state bytes. The tables are recovered from the module's code. */
int Combat_startup(Combat *combat)
{
	Applet *app = combat->app;
	combat->canvas = app->canvas;
	combat->game = app->game;
	combat->render = app->render;
	combat->player = app->player;

	if (!combat_replayStartup(combat)) {
		Oe_warn("Combat_startup: weapon/monster tables could not be read from the .mod");
		return 0;
	}
	return 1;
}
