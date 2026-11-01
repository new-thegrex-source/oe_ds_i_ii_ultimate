/*
 * Combat.cpp (part 1): combat stat blocks (CombatEntity), weapon table,
 * hit / crit / damage formulas, item description texts and the startup that
 * fills the weapon and monster tables.
 *
 * All tables the original kept in its executable are read from the user's
 * orcsandelves2.mod (see the MOD_* offsets below).
 */
#include "game.h"

/* ---- data inside orcsandelves2.mod ---- */
#define MOD_ARMOR_TABLE        0x45dccu /* 5 x {s16 deflect, s16 durability} */
#define MOD_TILE_DIST_TABLE    0x45ebcu /* 15 x s32: squared world distance of k tiles, (64k)^2 */
#define MOD_MONSTER_ATTACKS    0x45efcu /* s32 [subtype*9 + parm*3 + field]: field 0/1 weapon, 2 chance */

/* Combat_startup does not use a data table: the weapon and monster stat
 * blocks are immediates in its code (42 WeaponInfo_setup + 37 CombatEntity_set
 * calls). The port replays that code from the .mod (combat_replayStartup). */
#define MOD_CODE_COMBAT_STARTUP     0x3edf4u
#define MOD_CODE_COMBAT_STARTUP_END 0x3fd70u
#define MOD_CODE_WEAPONINFO_SETUP   0x3b774u
#define MOD_CODE_COMBATENTITY_SET   0x3b70cu
/* original offsets inside Combat, used to map the replayed stores */
#define ORIG_COMBAT_WEAPONS       0xb4
#define ORIG_COMBAT_MONSTERSTATS  0x870
#define ORIG_COMBAT_CRIT          0x80
#define ORIG_COMBAT_HIT           0x84
#define ORIG_WEAPONINFO_SIZE      0x2c
#define ORIG_COMBATENTITY_SIZE    0x34

#define NUM_WEAPONS        45
#define NUM_MONSTER_STATS  48

/* weapon ids that matter to the formulas */
#define WP_PLAYER_LAST     10 /* player weapons are 0..10 */

static int armorDeflect(int armorType) { return ModData_s16(&g_mod, MOD_ARMOR_TABLE + armorType * 4); }
static int armorDurability(int armorType) { return ModData_s16(&g_mod, MOD_ARMOR_TABLE + armorType * 4 + 2); }

/* 32-bit wrapping multiply, as the ARM MUL did. */
static int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

/* 0x0000391c */
int Combat_WorldDistToTileDist(Combat *combat, int distSq)
{
	(void)combat;
	for (int k = 0; k < 15; k++) {
		if (distSq < ModData_s32(&g_mod, MOD_TILE_DIST_TABLE + k * 4))
			return k;
	}
	return 15;
}

/* ------------------------------------------------------------------ CombatEntity */

/* 0x00004440 */
void CombatEntity_addArmor(CombatEntity *ce, int amount)
{
	if (amount >= 0)
		g_app->statArmorGained += amount;
	else
		g_app->statArmorLost += amount;

	int armor = ce->armor + amount;
	if (armor < 0)
		armor = 0;
	else if (armor > armorDurability(ce->armorType))
		armor = armorDurability(ce->armorType);
	ce->armor = armor;
}

/* 0x00009e10 */
void CombatEntity_copy(CombatEntity *dst, CombatEntity *src)
{
	*dst = *src;
}

/* 0x0001baac */
int CombatEntity_getAccuracy(CombatEntity *ce) { return ce->accuracy; }
/* 0x0001bbc0 */
int CombatEntity_getDefense(CombatEntity *ce) { return ce->defense; }
/* 0x0001bbc8 */
int CombatEntity_getArmorPoints(CombatEntity *ce) { return ce->armor; }
/* 0x0001bc20 */
int CombatEntity_getHealth(CombatEntity *ce) { return ce->health; }
/* 0x0001be94 */
int CombatEntity_getMaxHealth(CombatEntity *ce) { return ce->maxHealth; }
/* 0x0001c300 */
int CombatEntity_getStrength(CombatEntity *ce) { return ce->strength; }

/* 0x0001be78 */
int CombatEntity_getMaxArmorPoints(CombatEntity *ce)
{
	return armorDurability(ce->armorType);
}

static int clampStat(int v)
{
	if (v < 0)
		return 0;
	if (v > 255)
		return 255;
	return v;
}

/* 0x00039c18 */
void CombatEntity_setAccuracy(CombatEntity *ce, int v) { ce->accuracy = clampStat(v); }
/* 0x00039e1c */
void CombatEntity_setDefense(CombatEntity *ce, int v) { ce->defense = clampStat(v); }
/* 0x0003b554 */
void CombatEntity_setStrength(CombatEntity *ce, int v) { ce->strength = clampStat(v); }

/* 0x00039c6c: new armor starts at full durability */
void CombatEntity_setArmor(CombatEntity *ce, int type)
{
	ce->armorType = type;
	ce->armor = armorDurability(type);
}

/* 0x0003a038 */
void CombatEntity_setHealth(CombatEntity *ce, int v)
{
	if (v < 0)
		v = 0;
	else if (v > ce->maxHealth)
		v = ce->maxHealth;
	ce->health = v;
}

/* 0x0003a4e0 */
void CombatEntity_setMaxHealth(CombatEntity *ce, int v)
{
	if (v < 0)
		v = 0;
	else if (v > 999)
		v = 999;
	ce->maxHealth = v;
}

/* 0x0003b70c */
void CombatEntity_set(CombatEntity *ce, int type, int health, int defense, int strength, int accuracy)
{
	CombatEntity_setMaxHealth(ce, health);
	CombatEntity_setHealth(ce, health);
	CombatEntity_setDefense(ce, defense);
	CombatEntity_setStrength(ce, strength);
	CombatEntity_setAccuracy(ce, accuracy);
	ce->level = 1;
	ce->type = type;
}

/* 0x00044d0c: weakness effect halves (or restores) strength, accuracy, defense */
void CombatEntity_scaleStats(CombatEntity *ce, int halve)
{
	if (halve) {
		ce->defense >>= 1;
		ce->strength >>= 1;
		ce->accuracy >>= 1;
	} else {
		ce->defense <<= 1;
		ce->strength <<= 1;
		ce->accuracy <<= 1;
	}
}

/* 0x000095bc */
int CombatEntity_calcXP(CombatEntity *ce)
{
	return ((ce->defense + ce->strength) * 5 + ce->accuracy * 6 + ce->maxHealth * 5 + 49) / 50;
}

/* 0x00036b54: the player's block is saved in full, monsters only save health */
void CombatEntity_saveState(CombatEntity *ce, OutputStream *os)
{
	if (ce->type == -1) {
		OutStream_writeBool(os, (int8_t)ce->level);
		OutStream_writeBool(os, (int8_t)ce->strength);
		OutStream_writeBool(os, (int8_t)ce->accuracy);
		OutStream_writeBool(os, (int8_t)ce->defense);
		OutStream_writeBool(os, (int8_t)ce->weapon);
		OutStream_writeBool(os, (int8_t)ce->armorType);
		OutStream_writeShort(os, (int16_t)ce->armor);
		OutStream_writeShort(os, (int16_t)ce->maxHealth);
	}
	OutStream_writeShort(os, (int16_t)ce->health);
}

/* 0x00028c58 */
void CombatEntity_loadState(CombatEntity *ce, InputStream *stream)
{
	if (ce->type == -1) {
		ce->level = Stream_readUByte(stream);
		ce->strength = Stream_readUByte(stream);
		ce->accuracy = Stream_readUByte(stream);
		ce->defense = Stream_readUByte(stream);
		ce->weapon = Stream_readUByte(stream);
		ce->armorType = Stream_readUByte(stream);
		ce->armor = Stream_readShort(stream);
		ce->maxHealth = Stream_readShort(stream);
	}
	ce->health = Stream_readShort(stream);
}

/* ------------------------------------------------------------------ formulas */

/* 0x0001bf10: on difficulty 4 the player's weapons lose a quarter of their minimum damage */
int Combat_getWeaponDamage(int *wpnDamage, int weapon)
{
	if (g_app->game->difficulty == 4 && weapon <= WP_PLAYER_LAST)
		return *wpnDamage - (*wpnDamage >> 2);
	return *wpnDamage;
}

/* 0x0001bf68 */
int Combat_getMonsterAttackField(Combat *self, EntityDef *def, int field)
{
	(void)self;
	return ModData_s32(&g_mod, MOD_MONSTER_ATTACKS + (def->eSubType * 9 + field) * 4 + def->parm * 12);
}

/* 0x000085e0: percent damage modifier of a player weapon against a monster
 * class, returned as 8.8 fixed point (0 = immune). res[0] & 0x100 makes the
 * defender count as class 4. */
int Combat_weaponVsTypeMod(WeaponInfo *weapon, int *defender, int *res)
{
	int w = (int)(weapon - g_app->combat->weapons);
	int cls = *defender;
	uint32_t bit = arm_bit(w);
	uint32_t playerBit = arm_bit(g_app->player->ce.weapon);
	int pct = 100;

	if (res[0] & 0x100)
		cls = 4;

	switch (cls) {
	case 0:
		if (bit & 0x60)
			pct = 130;
		break;
	case 1:
		if (w == 9 || w == 3)
			pct = 150;
		break;
	case 2:
		if (w == 7)
			pct = 125;
		else if (w == 0 || (bit & 0x60))
			pct = 110;
		else if (w == 1 || w == 2)
			pct = 75;
		break;
	case 3:
	case 15:
		if (w == 7 || w == 0)
			pct = 125;
		else if (bit & 0x60)
			pct = 110;
		else if (w == 1)
			pct = 50;
		break;
	case 4:
		if (w == 0)
			pct = 120;
		else if (bit & 0xc)
			pct = 50;
		else if (w == 1)
			pct = 30;
		else if (bit & 0x60)
			pct = 80;
		else if (w == 10 || w == 9)
			return 0;
		break;
	case 5:
		if (w == 7 || w == 9)
			pct = 125;
		else if (w == 0)
			pct = 60;
		break;
	case 6:
		if (w == 7)
			pct = 150;
		else if (bit & 0x60)
			pct = 125;
		else if (playerBit & 0xc)
			pct = 70;
		else if (w == 1)
			pct = 50;
		break;
	case 7:
	case 12:
		if (w == 0 || (bit & 0x60))
			pct = 150;
		else if (w == 3 || w == 10 || w == 9)
			pct = 75;
		break;
	case 8:
		if (w == 9)
			return 0;
		if (bit & 0x60)
			pct = 50;
		else if (playerBit & 0xc)
			pct = 75;
		else if (w == 1)
			pct = 150;
		break;
	case 9:
		if (w == 7)
			pct = 130;
		else if (bit & 0x60)
			pct = 120;
		else if (w == 9)
			return 0;
		else if (w == 1 || (playerBit & 0xc))
			pct = 50;
		else if (w == 0)
			pct = 25;
		break;
	default:
		break;
	}

	if (g_app->game->difficulty == 4 && pct > 100)
		pct = 100;
	return (pct << 16) / 0x6400;
}

/* 0x00008eac: range check and hit/crit roll. res[] = {flags, damage, armorHit,
 * tileDist, critChance, hitChance}. Returns the new flags (1 hit, 2 crit). */
int CombatEntity_calcHit(CombatEntity *atk, CombatEntity *def, int *res)
{
	Combat *combat = g_app->combat;
	Player *player = g_app->player;
	WeaponInfo *wp = &combat->weapons[atk->weapon];
	int dx = atk->x - def->x;
	int dy = atk->y - def->y;
	int distSq = mul32(dy, dy) + mul32(dx, dx);
	int tileDist = Combat_WorldDistToTileDist(combat, distSq);
	int outOfRange = 0;

	if (tileDist < wp->rangeMin)
		outOfRange = wp->rangeMin - tileDist;
	else if (tileDist > wp->rangeMax)
		outOfRange = tileDist - wp->rangeMax;
	res[3] = tileDist;

	if (res[0] & 8) {
		outOfRange = 0;
	} else if ((wp->rangeMin == wp->rangeMax || (res[0] & 0x20)) && outOfRange > 0) {
		res[0] |= 0x200;
		return res[0];
	}

	/* hit chance */
	res[5] = wp->accuracy;
	if ((arm_bit(atk->weapon) & 0xc) && distSq > ModData_s32(&g_mod, MOD_TILE_DIST_TABLE + 4))
		res[5] -= 0x26;

	int accMul = 6;
	int accPenalty = 0;
	if (atk->type == -1 && player->baseCe.accuracy > atk->accuracy) {
		accPenalty = (player->baseCe.accuracy - atk->accuracy) * -5;
		accMul = player->statusEffects[12].turns > 0 ? 1 : 2;
	}
	res[5] += (atk->accuracy - 10) * accMul + accPenalty;
	if (def->type != -1 || player->statusEffects[12].turns <= 0)
		res[5] -= def->defense * 4 - 0x30;
	res[5] -= outOfRange * 64;
	if (res[5] < 1)
		res[5] = 1;

	/* crit chance: the player always, monsters only on difficulty 4 */
	if ((atk->type == -1 || g_app->game->difficulty == 4) && atk->weapon != 1) {
		res[4] = (atk->accuracy - 20) * accMul;
		res[4] -= def->defense * 3 - 30;
		if (res[4] < 1)
			res[4] = 1;
	} else {
		res[4] = 0;
	}

	int roll = Random_nextInt(&g_app->random) & 0xff;
	int autoHit = (res[0] & 0x800) && !(atk->type == -1 && player->statusEffects[12].turns > 0) &&
	              tileDist <= 1;
	if (!autoHit && res[5] < roll && atk->weapon != 0x22) {
		if (tileDist == 1)
			player->critCounter++;
		return res[0];
	}

	roll = Random_nextInt(&g_app->random) & 0xff;
	res[0] |= (res[4] > roll) ? 2 : 1;
	return res[0];
}

/* 0x00008870: res[1] = damage after weapon roll, strength, class modifier and armor */
int CombatEntity_calcDamage(CombatEntity *atk, CombatEntity *def, int *res)
{
	Combat *combat = g_app->combat;
	int w = atk->weapon;
	WeaponInfo *wp = &combat->weapons[w];
	int dmg = Combat_getWeaponDamage(&wp->dmgMin, w);
	int dmgMax = wp->dmgMax;

	if (res[0] & 2) {
		dmg = dmgMax * 2;
	} else if (res[0] & 4) {
		dmg = Combat_getWeaponDamage(&wp->dmgMin, atk->weapon);
		dmg += (dmgMax - Combat_getWeaponDamage(&wp->dmgMin, atk->weapon)) / 2;
	} else if (Combat_getWeaponDamage(&wp->dmgMin, atk->weapon) != dmgMax) {
		int roll = Random_nextInt(&g_app->random) & 0xff;
		dmg += roll % (dmgMax - Combat_getWeaponDamage(&wp->dmgMin, atk->weapon));
	}

	if (!(res[0] & 0x10) && atk->strength > 20)
		dmg += (atk->strength - 20) / 2;

	if (res[0] & 0x800) { /* sneak attack */
		if ((arm_bit(atk->weapon) & 0xc) && !(Random_nextByte(&g_app->random) & 1))
			dmg += dmgMax * 2;
		else
			dmg += dmgMax;
	}

	int mod = Combat_weaponVsTypeMod(wp, &def->type, res);
	if (mod == 0)
		return 0;
	if ((res[0] & 2) && atk->accuracy > 25)
		mod += (atk->accuracy * 12 - 300) / 5;

	dmg = mul32(dmg << 8, mod) >> 16;
	if (dmg < 0) {
		res[1] = dmg;
		return dmg;
	}
	if (dmg < 1)
		dmg = 1;

	int drunkPlayer = def->type == -1 && g_app->render->drunkLevel != 0;
	int absorb = drunkPlayer ? (def->defense - 20) / 3 : (def->defense - 20) / 4;
	if (absorb < 0)
		absorb = 0;
	if (def->armor > 0) {
		int deflect = armorDeflect(def->armorType);
		if (def->armorType == 2 &&
		    (atk->weapon == 0x12 || atk->weapon == 0x18 || atk->weapon == 0x1b || atk->weapon == 0x1c ||
		     atk->weapon == 0x1e || (res[0] & 0x100)))
			deflect >>= 1;
		absorb += deflect;
		res[2] = 1;
	} else {
		res[2] = 0;
	}

	absorb = mul32(absorb, wp->damageScale) >> 8;
	if (absorb > dmg)
		absorb = dmg;
	dmg -= absorb;
	if (dmg < 0)
		dmg = 0;
	else if (dmg > 999)
		dmg = 999;
	if (dmg == 0 && drunkPlayer)
		dmg = 1;
	res[1] = dmg;
	return dmg;
}

/* 0x000084e0 */
void CombatEntity_calcCombat(CombatEntity *atk, CombatEntity *def, int *res)
{
	res[1] = 0;
	int flags = CombatEntity_calcHit(atk, def, res);
	if (flags & 3) {
		int dmg = CombatEntity_calcDamage(atk, def, res);
		if (dmg == 0)
			flags |= 0x80;
		res[1] = dmg;
		res[0] = flags;
	}
}

/* ------------------------------------------------------------------ weapons and armor */

/* 0x0003b774 */
void WeaponInfo_setup(WeaponInfo *w, int dmgMin, int dmgMax, int rangeMin, int rangeMax, int accuracy,
                      int ammoType, int ammoUsage, int damageScale, int effectType, int numShots,
                      int shotDelay)
{
	w->dmgMin = dmgMin;
	w->dmgMax = dmgMax;
	w->rangeMin = rangeMin;
	w->rangeMax = rangeMax;
	w->accuracy = accuracy;
	w->ammoType = ammoType;
	w->ammoUsage = ammoUsage;
	w->damageScale = damageScale;
	w->effectType = effectType;
	w->numShots = numShots;
	w->shotDelay = shotDelay;
}

/* 0x0001bb78 */
void Combat_getArmorStats(int armorType, int *out2)
{
	out2[0] = armorDeflect(armorType);
	out2[1] = armorDurability(armorType);
}

/* 0x0001bab4: "Deflect: %1" / "Durability: %1" + help line */
String *Combat_getArmorDescription(Combat *combat, int armorType)
{
	Canvas *canvas = g_app->canvas;
	String *s = &combat->description;

	String_setLength(s, 0);
	if (armorType != -1) {
		int stats[2];
		Combat_getArmorStats(armorType, stats);
		Canvas_resetEvents(canvas);
		Canvas_addTextArgInt(canvas, stats[0]);
		Text_getString(canvas, 0, 0x77, s);
		Canvas_resetEvents(canvas);
		Canvas_addTextArgInt(canvas, stats[1]);
		Text_getString(canvas, 0, 0x78, s);
	}
	Text_getString(canvas, 0, 0x79, s);
	return s;
}

/* 0x0001c40c: "Base Damage / Accuracy / Ideal Range / Special" */
String *Combat_getWeaponDescription(Combat *combat, int weapon)
{
	Canvas *canvas = g_app->canvas;
	WeaponInfo *wp = &combat->weapons[weapon];
	String *s = &combat->description;

	String_setLength(s, 0);
	Canvas_resetEvents(canvas);
	Canvas_addTextArgInt(canvas, Combat_getWeaponDamage(&wp->dmgMin, weapon));
	Text_getString(canvas, 0, 0x6d, s);

	Canvas_resetEvents(canvas);
	Canvas_addTextArgInt(canvas, wp->accuracy * 100 >> 8);
	Text_getString(canvas, 0, 0x6e, s);

	Canvas_resetEvents(canvas);
	Canvas_addTextArgInt(canvas, wp->rangeMin);
	if (wp->rangeMin == wp->rangeMax) {
		Text_getString(canvas, 0, 0x70, s);
	} else {
		Canvas_addTextArgInt(canvas, wp->rangeMax);
		Text_getString(canvas, 0, 0x6f, s);
	}

	int special;
	switch (weapon) {
	case 7: special = 0x71; break;
	case 3: special = 0x72; break;
	case 0: special = 0x73; break;
	case 10: special = 0x74; break;
	case 9: special = 0x75; break;
	case 8: special = 0x76; break;
	default: return s;
	}
	Text_getString(canvas, 0, special, s);
	return s;
}

/* 0x0001bb9c */
int Combat_getTargetType(Combat *combat)
{
	if (combat->target == NULL)
		return 0;
	return combat->target->def->eType;
}

/* 0x0002e7e0: ARGB colour of the blood/debris particles of a monster */
int Combat_getBloodColor(Combat *combat, Entity *entity)
{
	(void)combat;
	EntityDef *def = entity->def;
	int sub = def->eSubType;

	if (sub == 16)
		return (int)0xffb4805f;
	uint16_t effects = entity->monster->monsterEffects;
	if (effects & 2)
		return (int)0xffaaaaff;
	if (effects & 8)
		return (int)0xffffaaaa;
	if ((entity->info & 0x1000000) || sub == 4)
		return (int)0xffdadada;
	switch (sub) {
	case 3:
		if (def->parm == 0)
			return (int)0xff689c20;
		if (def->parm == 1)
			return (int)0xff69892e;
		return (int)0xff663ece;
	case 15:
		return (int)0xff663ece;
	case 8:
		if (def->parm == 0)
			return (int)0xffb4805f;
		if (def->parm == 1)
			return (int)0xff849943;
		return (int)0xffafa59b;
	case 2:
		return (int)0xff867459;
	default:
		return (int)0xffbb0000;
	}
}

/* ------------------------------------------------------------------ startup */

/* A value tracked while replaying the startup code: an immediate, or an
 * address relative to the Combat object / the stack pointer at entry. */
enum { AV_UNKNOWN, AV_CONST, AV_COMBAT, AV_STACK };
typedef struct ArmValue {
	int kind;
	int32_t v;
} ArmValue;

#define REPLAY_STACK_WORDS 64 /* covers sp-0x80 .. sp+0x80 */

typedef struct StartupReplay {
	Combat *combat;
	ArmValue r[16];
	ArmValue stack[REPLAY_STACK_WORDS];
	int weapons, stats;
} StartupReplay;

static ArmValue *replayStackSlot(StartupReplay *st, int32_t spOffset)
{
	int idx = (spOffset + REPLAY_STACK_WORDS * 2) / 4;
	if ((spOffset & 3) || idx < 0 || idx >= REPLAY_STACK_WORDS)
		return NULL;
	return &st->stack[idx];
}

/* Fetch the n-th stacked call argument (n = 0 is [sp]). */
static int replayStackArg(StartupReplay *st, int n, int32_t *out)
{
	ArmValue *slot = replayStackSlot(st, st->r[13].v + n * 4);
	if (!slot || slot->kind != AV_CONST)
		return 0;
	*out = slot->v;
	return 1;
}

static int replayCall(StartupReplay *st, uint32_t target)
{
	ArmValue *r = st->r;
	if (r[0].kind != AV_COMBAT || r[1].kind != AV_CONST || r[2].kind != AV_CONST || r[3].kind != AV_CONST)
		return 0;
	int32_t off = r[0].v;

	if (target == MOD_CODE_WEAPONINFO_SETUP) {
		int32_t a[8];
		int idx = (off - ORIG_COMBAT_WEAPONS) / ORIG_WEAPONINFO_SIZE;
		if ((off - ORIG_COMBAT_WEAPONS) % ORIG_WEAPONINFO_SIZE || idx < 0 || idx >= NUM_WEAPONS)
			return 0;
		for (int i = 0; i < 8; i++) {
			if (!replayStackArg(st, i, &a[i]))
				return 0;
		}
		WeaponInfo_setup(&st->combat->weapons[idx], r[1].v, r[2].v, r[3].v, a[0], a[1], a[2], a[3], a[4], a[5],
		                 a[6], a[7]);
		st->weapons++;
	} else if (target == MOD_CODE_COMBATENTITY_SET) {
		int32_t a[2];
		int idx = (off - ORIG_COMBAT_MONSTERSTATS) / ORIG_COMBATENTITY_SIZE;
		if ((off - ORIG_COMBAT_MONSTERSTATS) % ORIG_COMBATENTITY_SIZE || idx < 0 || idx >= NUM_MONSTER_STATS)
			return 0;
		if (!replayStackArg(st, 0, &a[0]) || !replayStackArg(st, 1, &a[1]))
			return 0;
		CombatEntity_set(&st->combat->monsterStats[idx], r[1].v, r[2].v, r[3].v, a[0], a[1]);
		st->stats++;
	} else {
		return 0;
	}
	/* caller-saved registers are dead after a call */
	r[0].kind = r[1].kind = r[2].kind = r[3].kind = r[12].kind = r[14].kind = AV_UNKNOWN;
	return 1;
}

static int replayAdd(ArmValue *dst, ArmValue a, ArmValue b, int subtract)
{
	if (b.kind != AV_CONST)
		return 0;
	if (a.kind == AV_UNKNOWN)
		return 0;
	dst->kind = a.kind;
	dst->v = subtract ? a.v - b.v : a.v + b.v;
	return 1;
}

/* Interpret the straight-line ARM code of the original Combat_startup and
 * perform its WeaponInfo_setup / CombatEntity_set calls. Only the handful of
 * instruction forms that function uses are understood. Returns 1 on success. */
static int combat_replayStartup(Combat *combat)
{
	StartupReplay st;
	memset(&st, 0, sizeof st);
	st.combat = combat;
	st.r[0].kind = AV_COMBAT;
	st.r[13].kind = AV_STACK;

	for (uint32_t pc = MOD_CODE_COMBAT_STARTUP; pc < MOD_CODE_COMBAT_STARTUP_END; pc += 4) {
		uint32_t w = ModData_u32(&g_mod, pc);
		ArmValue *r = st.r;

		if ((w >> 28) != 0xe)
			goto unsupported;

		if ((w & 0x0ffffff0u) == 0x012fff10u) /* bx: end of function */
			return st.weapons > 0 && st.stats > 0;

		uint32_t cls = (w >> 25) & 7;
		if (cls == 5) { /* b / bl */
			if (!(w & 0x01000000u))
				goto unsupported;
			int32_t off = (int32_t)(w << 8) >> 6;
			if (!replayCall(&st, pc + 8 + (uint32_t)off))
				goto unsupported;
			continue;
		}
		if (cls == 4) { /* ldm / stm */
			int pre = (w >> 24) & 1, up = (w >> 23) & 1, wb = (w >> 21) & 1, load = (w >> 20) & 1;
			int rn = (w >> 16) & 15;
			uint32_t list = w & 0xffff;
			int count = 0;
			for (int i = 0; i < 16; i++)
				count += (list >> i) & 1;
			if (r[rn].kind != AV_STACK || (w & 0x00400000u))
				goto unsupported;
			int32_t addr = up ? r[rn].v + (pre ? 4 : 0) : r[rn].v - count * 4 + (pre ? 0 : 4);
			for (int i = 0; i < 16; i++) {
				if (!((list >> i) & 1))
					continue;
				ArmValue *slot = replayStackSlot(&st, addr);
				if (!slot)
					goto unsupported;
				if (load)
					r[i] = *slot;
				else
					*slot = r[i];
				addr += 4;
			}
			if (wb)
				r[rn].v += up ? count * 4 : -count * 4;
			continue;
		}
		if (((w >> 26) & 3) == 1) { /* ldr / str with immediate offset */
			int pre = (w >> 24) & 1, up = (w >> 23) & 1, byte = (w >> 22) & 1, wb = (w >> 21) & 1;
			int load = (w >> 20) & 1;
			int rn = (w >> 16) & 15, rd = (w >> 12) & 15;
			int32_t off = (int32_t)(w & 0xfff);
			if ((w & 0x02000000u) || !pre || byte || wb)
				goto unsupported;
			int32_t addr = r[rn].v + (up ? off : -off);
			if (r[rn].kind == AV_STACK) {
				ArmValue *slot = replayStackSlot(&st, addr);
				if (!slot)
					goto unsupported;
				if (load)
					r[rd] = *slot;
				else
					*slot = r[rd];
			} else if (r[rn].kind == AV_COMBAT && !load && r[rd].kind == AV_CONST) {
				if (addr == ORIG_COMBAT_CRIT)
					combat->crit = r[rd].v;
				else if (addr == ORIG_COMBAT_HIT)
					combat->hit = r[rd].v;
				else
					goto unsupported;
			} else {
				goto unsupported;
			}
			continue;
		}
		if (((w >> 26) & 3) == 0) { /* data processing */
			int imm = (w >> 25) & 1;
			int op = (w >> 21) & 15;
			int rn = (w >> 16) & 15, rd = (w >> 12) & 15;
			ArmValue op2;
			if (!imm && (w & 0x90) == 0x90)
				goto unsupported; /* multiply / extra load-store */
			if (imm) {
				uint32_t rot = ((w >> 8) & 15) * 2, v8 = w & 0xff;
				op2.kind = AV_CONST;
				op2.v = (int32_t)(rot ? (v8 >> rot) | (v8 << (32 - rot)) : v8);
			} else {
				if (w & 0xff0)
					goto unsupported; /* shifted register operand */
				op2 = r[w & 15];
			}
			if (rd == 15)
				goto unsupported;
			switch (op) {
			case 0xd: /* mov */
				r[rd] = op2;
				break;
			case 0xf: /* mvn */
				if (op2.kind != AV_CONST)
					goto unsupported;
				r[rd].kind = AV_CONST;
				r[rd].v = ~op2.v;
				break;
			case 0x4: /* add */
				if (!replayAdd(&r[rd], r[rn], op2, 0))
					goto unsupported;
				break;
			case 0x2: /* sub */
				if (!replayAdd(&r[rd], r[rn], op2, 1))
					goto unsupported;
				break;
			default:
				goto unsupported;
			}
			continue;
		}
	unsupported:
		Oe2_warn("Combat_startup: cannot replay instruction %08x at 0x%05x", w, pc);
		return 0;
	}
	Oe2_warn("Combat_startup: no return found");
	return 0;
}

/* 0x0003edf4: weapon table (player weapons 0..10, monster attacks 11..42) and
 * monster stat blocks (class*3 + tier). Also clears crit/hit. */
int Combat_startup(Combat *combat)
{
	if (!combat_replayStartup(combat)) {
		Oe2_warn("Combat_startup: weapon/monster tables could not be read from the .mod");
		return 0;
	}
	return 1;
}

/* 0x0003c5d8 */
void Combat_shutdown(Combat *combat)
{
	String_release(&combat->description);
}

/* ------------------------------------------------------------------ Hud barks */

/* 0x0002a750: parse "chance,text,text,..." line idx of string group 19 */
void Combat_parseQuipLine(Hud *hud, BarkGroup *out, int idx)
{
	(void)hud;
	Canvas *canvas = g_app->canvas;
	const char *data = canvas->strData[0x13];
	int16_t i = canvas->strOffsets[0x13][idx];

	String_setLength(&canvas->scratch, 0);
	for (; data[i] != ','; i = (int16_t)(i + 1))
		String_appendChar(&canvas->scratch, (uint8_t)data[i]);
	out->chance = String_toInt(&canvas->scratch);
	out->count = 0;

	String *cur = &out->strings[0];
	for (int j = (int16_t)(i + 1); data[j] != '\0'; j++) {
		if (data[j] == ',') {
			out->count++;
			cur = &out->strings[out->count < 20 ? out->count : 19];
		} else {
			String_appendChar(cur, (uint8_t)data[j]);
		}
	}
	out->count++;
}

/* 0x0002a838: the 12 companion bark groups (called from Hud_startup with the Hud) */
int Combat_loadQuips(void *hudPtr)
{
	Hud *hud = hudPtr;
	int ok = Text_loadStringGroup(g_app->canvas, 0x13);
	if (ok) {
		for (int i = 0; i < 12; i++)
			Combat_parseQuipLine(hud, &hud->barks[i], i);
		Canvas_unloadStringGroup(g_app->canvas, 0x13);
	}
	return ok != 0;
}
