/*
 * Player: health, XP and level-ups, status effects ("buffs"), per-turn
 * updates, damage and death, score, quest-log entries and the wand's
 * "I want to talk" state.
 *
 * Original class: Player (Applet+0x118 -> object at Applet+0x1a7b68, 0x160 bytes).
 *
 * Status effects: 12 timed effects in Player.effects[] {turns, amount, stacks}
 * (1 poison, 2 protection, 3 remedy, 4 haste, 5 invisibility, 6 regeneration,
 * 7 accuracy, 8 strength, 9 defense, 10 drunk, 11 drain ...).
 * Player_updateStats folds them into the 10 display/stat modifiers in
 * Player.buffs[] {turns, amount}; Player_calcStats applies modifiers 7
 * (accuracy), 8 (strength) and 9 (defense) plus the equipped ring to the base
 * stats. Modifier 1 is poison damage per turn, 6 regeneration per turn.
 */
#include "game.h"

#include <stdio.h>

/* Player-visible strings in the module's data area. */
#define MOD_STR_GAINED_HEALTH   0x2d904u
#define MOD_STR_POISON_DAMAGE   0x2d918u
#define MOD_STR_LEVEL_UP        0x2d930u
#define MOD_STR_LEVEL_MAXHEALTH 0x2d948u
#define MOD_STR_LEVEL_DEFENSE   0x2d95cu
#define MOD_STR_LEVEL_STRENGTH  0x2d96cu
#define MOD_STR_LEVEL_ACCURACY  0x2d97cu
#define MOD_STR_LEVEL_TRAILER   0x2d98cu
#define MOD_STR_GAINED_XP       0x2d9f0u
#define MOD_STR_INSUFF_HEALTH   0x2da00u
#define MOD_STR_INSUFF_FUNDS    0x2da18u
#define MOD_STR_INSUFF_XP       0x2da2cu
#define MOD_STR_DID_DAMAGE      0x2da40u
#define MOD_STR_DAMAGE_SUFFIX   0x2da48u
#define MOD_STR_NEAR_DEATH      0x2da58u
#define MOD_STR_LOW_HEALTH      0x2da64u
#define MOD_STR_TIME_FORMAT     0x2da80u
#define MOD_STR_NOTEBOOK_ENTRY  0x2e6acu
#define MOD_STR_CRUSHED         0x2e6d8u

/* Tables */
#define MOD_CRUSH_DIRS          0x2d8d4u /* 4 x {int dx, int dy, int unused}: E, N, S, W */

/* Sound resource ids */
#define SND_LEVEL_UP      0x141c
#define SND_PLAYER_DIED   0x13c2
#define SND_PLAYER_HURT   0x13d9
#define SND_EFFECT_ENDING 0x13c0

/* Canvas states used here */
#define CANVAS_STATE_MENU      2
#define CANVAS_STATE_PLAYING   3
#define CANVAS_STATE_AUTOMAP   5
#define CANVAS_STATE_DYING     0xd

#define LEVEL_UP_TEXT_SIZE 600 /* one half of Canvas.helpBuf */

static char *modStr(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

static int clampInt(int v, int lo, int hi)
{
	if (v > hi)
		return hi;
	if (v < lo)
		return lo;
	return v;
}

/* ---------------------------------------------------------------- health */

/* 0x000030d0 */
int Player_addHealth(Player *p, int amount)
{
	Applet *app = p->app;
	CombatEntity *ce = &p->ce;
	char msg[64];
	int before, health;

	if (amount > 0) {
		if (CombatEntity_getHealth(ce) == CombatEntity_getMaxHealth(ce))
			return 0;
		Hud_startStatFlash(p);
	} else if (p->god != 0) {
		return 0;
	}

	before = CombatEntity_getHealth(ce);
	app->hud->fromHp = before;
	health = before + amount;
	if (health > CombatEntity_getMaxHealth(ce))
		health = CombatEntity_getMaxHealth(ce);
	CombatEntity_setHealth(ce, health);

	if (before < CombatEntity_getHealth(ce)) {
		App_snprintf(msg, sizeof msg, modStr(MOD_STR_GAINED_HEALTH), CombatEntity_getHealth(ce) - before);
		Hud_addMessage(app->hud, msg, 0);
	}

	if (amount > 0)
		app->statHpHealed += amount;
	else
		app->statHpLost += amount;
	return 1;
}

/* ---------------------------------------------------------------- XP / levels */

/* 0x000067f8 */
int Player_xpForLevel(Player *player, int lvl)
{
	int n = lvl - 1;
	return n * n * n * 20 + lvl * 180 - 20;
}

/* Append one formatted "+N" line of the level-up text. */
static char *appendLine(char *end, char *limit, uint32_t fmtOff, int value)
{
	int n;

	if (end >= limit)
		return end;
	n = snprintf(end, (size_t)(limit - end), modStr(fmtOff), value);
	if (n < 0)
		return end;
	return n < limit - end ? end + n : limit - 1;
}

/* One of defense/strength/accuracy: +gain, capped so the stat stays <= 99. */
static char *levelUpStat(CombatEntity *base, int (*get)(CombatEntity *), void (*set)(CombatEntity *, int),
                         int gain, uint32_t fmtOff, char *end, char *limit)
{
	if (get(base) + gain > 99) {
		gain = 99 - get(base);
		if (gain == 0)
			return end;
	}
	set(base, get(base) + gain);
	return appendLine(end, limit, fmtOff, gain);
}

/* 0x000031dc */
void Player_levelUp(Player *p)
{
	Applet *app = p->app;
	Canvas *cv = p->canvas;
	CombatEntity *base = &p->baseCe;
	char *msg, *end, *limit;
	int maxHealth, gain;

	p->level++;
	p->ce.level = (uint8_t)p->level;
	p->nextLevelXP = Player_xpForLevel(p, p->level);

	/* The text is built in one of the two rotating halves of Canvas.helpBuf
	 * and handed to the help queue. */
	msg = &cv->helpBuf[cv->helpBufIdx * LEVEL_UP_TEXT_SIZE];
	cv->helpBufIdx = (cv->helpBufIdx + 1) % 2;
	limit = msg + LEVEL_UP_TEXT_SIZE;
	msg[0] = '\0';
	end = appendLine(msg, limit, MOD_STR_LEVEL_UP, p->level);

	/* max health +10, capped at 999 */
	maxHealth = CombatEntity_getMaxHealth(base);
	gain = 10;
	if (maxHealth + 10 >= 1000)
		gain = 999 - maxHealth;
	if (gain != 0) {
		CombatEntity_setMaxHealth(base, maxHealth + gain);
		end = appendLine(end, limit, MOD_STR_LEVEL_MAXHEALTH, gain);
	}

	end = levelUpStat(base, CombatEntity_getDefense, CombatEntity_setDefense, 1, MOD_STR_LEVEL_DEFENSE, end, limit);
	end = levelUpStat(base, CombatEntity_getStrength, CombatEntity_setStrength, 2, MOD_STR_LEVEL_STRENGTH, end, limit);
	end = levelUpStat(base, CombatEntity_getAccuracy, CombatEntity_setAccuracy, 2, MOD_STR_LEVEL_ACCURACY, end, limit);

	Player_calcStats(p);
	if (end < limit)
		snprintf(end, (size_t)(limit - end), "%s", modStr(MOD_STR_LEVEL_TRAILER));
	CombatEntity_setHealth(&p->ce, CombatEntity_getMaxHealth(&p->ce));

	if (cv->state != CANVAS_STATE_MENU) {
		Canvas_addHelpMessage(cv, msg, 2);
		Sound_play(app->sound, SND_LEVEL_UP, 2, 5, -1);
	}
	app->statLevelUps++;
}

/* 0x000039a0 */
void Player_addXP(Player *p, int xp)
{
	Applet *app = p->app;
	char msg[64];

	App_snprintf(msg, sizeof msg, modStr(MOD_STR_GAINED_XP), xp);
	Hud_addMessage(app->hud, msg, 0);
	p->xp += xp;
	p->levelXP += xp;
	while (p->xp >= p->nextLevelXP)
		Player_levelUp(p);
	app->statXP += xp;
}

/* ---------------------------------------------------------------- stats */

/* 0x0002bab4 */
void Player_calcStats(Player *pl)
{
	int accuracy = pl->baseCe.accuracy;
	int strength = pl->baseCe.strength;
	int defense = pl->baseCe.defense;
	uint16_t maxHealth = (uint16_t)pl->baseCe.maxHealth;

	pl->ce.accuracy = (uint8_t)accuracy;
	pl->ce.strength = (uint8_t)strength;
	pl->ce.defense = (uint8_t)defense;
	pl->ce.maxHealth = (int16_t)maxHealth;

	/* equipped ring bonus, only if the ring is owned */
	if (pl->ring < 16 && (pl->rings & (1u << pl->ring)) != 0) {
		switch (pl->ring) {
		case 1: strength += strength >> 2; break;
		case 2: accuracy += accuracy >> 2; break;
		case 3: defense += defense >> 2; break;
		case 6: pl->ce.maxHealth = (int16_t)(maxHealth + (maxHealth >> 1)); break;
		case 8: /* drunkard's ring */
			strength += 10;
			defense += 10;
			accuracy -= 15;
			break;
		default:
			break;
		}
	}

	if ((uint16_t)pl->ce.health > (uint16_t)pl->ce.maxHealth)
		pl->ce.health = pl->ce.maxHealth;

	pl->ce.strength = (uint8_t)clampInt((int8_t)pl->buffs[8].amount + strength, 0, 255);
	pl->ce.accuracy = (uint8_t)clampInt((int8_t)pl->buffs[7].amount + accuracy, 0, 255);
	pl->ce.defense = (uint8_t)clampInt((int8_t)pl->buffs[9].amount + defense, 0, 255);
}

/* Add an effect's amount to one stat modifier; the modifier keeps the shortest
 * remaining duration of the effects feeding it. */
static void addToModifier(Player *pl, int slot, int amount, uint8_t turns)
{
	Buff *b = &pl->buffs[slot];

	b->amount = (int8_t)clampInt((int8_t)b->amount + amount, -99, 99);
	if (turns < b->turns || b->turns == 0)
		b->turns = turns;
}

/* 0x0002aaa0 */
void Player_updateStats(Player *pl)
{
	int i;

	for (i = 0; i < 10; i++) {
		pl->buffs[i].turns = 0;
		pl->buffs[i].amount = 0;
	}
	pl->numBuffs = 0;

	for (i = 0; i < 12; i++) {
		StatusEffect *se = &pl->effects[i];
		int amount = (int8_t)se->amount;

		if (se->turns == 0)
			continue;
		if (i == 10) {
			/* drunk: strength and defense up, accuracy down by 1.5x */
			addToModifier(pl, 8, amount, se->turns);
			addToModifier(pl, 9, amount, se->turns);
			addToModifier(pl, 7, -(amount / 2 + amount), se->turns);
		} else if (i == 11) {
			addToModifier(pl, 8, amount, se->turns);
			addToModifier(pl, 7, amount, se->turns);
			addToModifier(pl, 9, amount, se->turns);
		} else {
			pl->buffs[i].amount = (int8_t)clampInt((int8_t)pl->buffs[i].amount + amount, -99, 99);
			pl->buffs[i].turns = se->turns;
		}
	}

	for (i = 0; i < 10; i++) {
		if (pl->buffs[i].turns != 0)
			pl->numBuffs++;
	}
	Player_calcStats(pl);
}

/* 0x00006f5c  cap ammo slot 1 (wand charges) by progress; topped up on the hub */
void Player_clampStat30(Player *player)
{
	int cap = player->maxMapReached * 13 - 77990;

	if (cap > 99)
		cap = 99;
	if (player->canvas->mapId == 6011)	/* DS Dragon's Lair (BREW 6008) */
		player->ammo[1] = (uint8_t)cap;
	if (player->ammo[1] > cap)
		player->ammo[1] = (uint8_t)cap;
}

/* 0x0001bf24  script/console stat change */
void Player_modifyStat(Player *player, int stat, int amount, int arg)
{
	Render *render = player->render;

	if (stat == 0) {
		Player_addHealth(player, amount);
		if (amount < 0)
			Player_hurtFeedback(player, 0);
	} else if (stat == 5) {
		Player_give(player, 4, 0, amount, 0);
	} else if (stat == 6) {
		Player_addXP(player, amount);
	} else if (stat == 7) {
		if (render->drunkTarget + amount < 0)
			render->drunkTarget = 0;
		else
			render->drunkTarget += amount;
	}
}

/* 0x000221f8 */
int Player_requireStat(Player *player, int stat, int value)
{
	uint32_t msg;

	if (stat == 0) {
		if (CombatEntity_getHealth(&player->ce) >= value)
			return 1;
		msg = MOD_STR_INSUFF_HEALTH;
	} else if (stat == 5) {
		if (player->gold >= value)
			return 1;
		msg = MOD_STR_INSUFF_FUNDS;
	} else if (stat == 6) {
		if (player->xp >= value)
			return 1;
		msg = MOD_STR_INSUFF_XP;
	} else {
		if (stat == 7 && player->render->drunkTarget < value)
			return 0;
		return 1;
	}
	Hud_addMessage(player->app->hud, modStr(msg), 0);
	return 0;
}

/* 0x00017dc4  monsters lose track of an invisible player */
int Player_isVisible(Player *player)
{
	return player->invisible == 0 && player->effects[5].turns == 0;
}

/* 0x00003a04 */
void Player_addMood(Player *p, int delta)
{
	p->mood = (uint8_t)clampInt(p->mood + delta, 0, 255);
}

/* ---------------------------------------------------------------- status effects */

/* 0x000037d4 */
int Player_addStatusEffect(Player *p, int effect, int amount, int turns)
{
	StatusEffect *se = &p->effects[effect];
	uint8_t oldStacks = se->stacks;
	int stacks = (int8_t)se->stacks;
	int delta = amount;

	if (stacks >= 4)
		return 0;
	if (effect != 4 && effect != 1 && effect != 11)
		turns++;

	if (amount != 0 && stacks > 0) {
		/* diminishing returns for each extra stack (8.8 fixed point):
		 * |a| - round((stacks+1) * |a| / 5) */
		int mag = amount < 0 ? -amount : amount;
		int step = (mag << 16) / 0x500;
		int reduced = mag - (((((stacks + 1) << 8) * step >> 8) + 0x80) >> 8);

		if (reduced <= 0)
			return 0;
		if (amount > 0) {
			if ((int8_t)se->amount == 99)
				return 0;
			delta = reduced;
		} else {
			if ((int8_t)se->amount == -99)
				return 0;
			delta = -reduced;
		}
	}

	se->amount = (int8_t)clampInt((int8_t)se->amount + delta, -99, 99);
	if (se->turns < turns)
		se->turns = (uint8_t)turns;
	se->stacks++;
	if (oldStacks == 0)
		p->numEffects++;
	return 1;
}

/* 0x000201b4  clears one status effect */
void Player_removeBuff(Player *player, int idx)
{
	StatusEffect *se = &player->effects[idx];

	if (se->turns == 0)
		return;
	se->amount = 0;
	se->stacks = 0;
	se->turns = 0;
	player->numEffects--;
}

/* 0x0002bb8c */
void Player_tickStatusEffects(Player *pl)
{
	int warn = 0;
	int i;

	if (pl->numEffects == 0)
		return;

	for (i = 0; i < 12; i++) {
		uint8_t turns = pl->effects[i].turns;

		if (turns == 0)
			continue;
		if (turns < 6) {
			warn = 1; /* about to run out */
			if (turns == 1) {
				Player_removeBuff(pl, i);
				continue;
			}
		}
		pl->effects[i].turns = turns - 1;
	}
	if (warn)
		Sound_play(g_app->sound, SND_EFFECT_ENDING, 0, 3, -1);
	Player_updateStats(pl);
}

/* ---------------------------------------------------------------- turns */

/* 0x00003b34 */
void Player_advanceTurn(Player *p)
{
	Applet *app = p->app;
	int amount;

	p->levelTurns++;
	p->totalTurns++;
	Player_tickStatusEffects(p);

	/* regeneration */
	if (p->buffs[6].turns != 0)
		Player_addHealth(p, (int8_t)p->buffs[6].amount);

	/* poison */
	if (p->buffs[1].turns != 0) {
		char *buf;
		int dmg = (int8_t)p->buffs[1].amount;

		Player_addHealth(p, dmg);
		buf = Hud_getMessageBuffer(app->hud, 1);
		snprintf(buf, 0x40, modStr(MOD_STR_POISON_DAMAGE), -dmg);
		Hud_finishMessageBuffer(app->hud);
		Game_gspriteAllocAnim(p->game, 0xf4, 0, 0, 0x20);
		p->canvas->ignoreKeysUntil = app->tickTime + 500;
	}

	/* combat ends 4 turns after the last attack */
	if (p->attacked != 0 && p->totalTurns - p->attackTurn > 3) {
		if (p->wandChatMode == 1)
			Player_setWandState(p, 1);
		p->attacked = 0;
		p->lastAmmoTurn = p->totalTurns;
	}

	/* wand charge (ammo 1) regeneration */
	amount = 0;
	if (p->ce.weapon == 0 || p->ce.weapon == 10) {
		if (p->attacked == 0 && p->totalTurns - p->lastAmmoTurn >= 7)
			amount = (p->canvas->mapId - 6000) / 2 + 2;
	} else if (p->totalTurns - p->lastAmmoTurn >= 2) {
		amount = (p->canvas->mapId - 6000) / 2 + 1;
	}
	if (amount != 0) {
		Player_give(p, 3, 1, amount, 0);
		p->lastAmmoTurn = p->totalTurns;
	}

	/* ring 4: regeneration */
	if (p->ring == 4)
		Player_addHealth(p, 2);

	/* low armour and an armour kit in the pack: the wand mentions it */
	if (CombatEntity_getArmorPoints(&p->ce) < 20 && Player_requireItem(p, 0, 12, 1, 99) != 0)
		Hud_randomBark(app->hud, 0xd, 0);

	if (p->effects[10].turns != 0 || app->player->ring == 8)
		app->statDrunkTurns++;

	if (p->render->drunkLevel != 0 && p->f110 <= p->totalTurns)
		p->render->drunkTarget = 0;

	p->buffChangeTime = app->tickTime;
	app->hud->nextBarkTime = app->gameTime;
}

/* ---------------------------------------------------------------- damage / death */

/* 0x0001cc60 */
void Player_hurtFeedback(Player *player, int keepDir)
{
	Applet *app = player->app;

	if (player->god != 0)
		return;
	if (keepDir == 0)
		app->hud->arrowSide = 0;
	app->hud->arrowDir = 1;
	Sound_play(app->sound, SND_PLAYER_HURT, 0, 3, -1);
	Canvas_startShake(player->canvas, 500, 2, 150);
}

/* 0x0001cf30 */
void Player_takeDamage(Player *player, int dmg, void *attacker)
{
	Applet *app = player->app;
	Entity *ent = (Entity *)attacker;
	CombatEntity *ce = &player->ce;
	char msg[128];
	char num[64];
	int before, after;

	if (player->god != 0)
		return;

	/* "[<name> did ]<n> damage!" */
	memset(msg, 0, sizeof msg);
	if (ent != NULL)
		App_snprintf(msg, sizeof msg, modStr(MOD_STR_DID_DAMAGE), ent->name);
	App_snprintf(num, sizeof num, "%d", dmg);
	strncat(msg, num, sizeof msg - strlen(msg) - 1);
	strncat(msg, modStr(MOD_STR_DAMAGE_SUFFIX), sizeof msg - strlen(msg) - 1);
	Hud_addMessage(app->hud, msg, 0);
	if (dmg == 0)
		return;

	if (ent != NULL && ent->def->eType == 2)
		app->statDamageTakenFrom[ent->def->eSubType] += dmg;

	/* health fraction before/after, in 1/256 units */
	before = CombatEntity_getHealth(ce) * 0x10000 / (CombatEntity_getMaxHealth(ce) * 0x100);
	after = (CombatEntity_getHealth(ce) - dmg) * 0x10000 / (CombatEntity_getMaxHealth(ce) * 0x100);
	if (after > 0) {
		if (before > 26 && after <= 26)
			Hud_addMessage(app->hud, modStr(MOD_STR_NEAR_DEATH), 3);
		else if (before > 78 && after <= 78)
			Hud_addMessage(app->hud, modStr(MOD_STR_LOW_HEALTH), 3);
	}

	Player_addHealth(player, -dmg);
	if (player->canvas->state == CANVAS_STATE_AUTOMAP)
		Canvas_setState(player->canvas, CANVAS_STATE_PLAYING);

	if (CombatEntity_getHealth(ce) <= 0)
		Player_died(player);
	else if (CombatEntity_getHealth(ce) < CombatEntity_getMaxHealth(ce) / 10)
		Hud_randomBark(app->hud, 6, 1);
}

/* 0x000086fc */
void Player_died(Player *player)
{
	Applet *app = player->app;

	if (player->canvas->state == CANVAS_STATE_DYING)
		return;
	player->deaths++;
	Sound_play(app->sound, SND_PLAYER_DIED, 0, 5, -1);
	Canvas_startShake(player->canvas, 350, 5, 350);
	CombatEntity_setHealth(&player->ce, 0);
	Canvas_setState(player->canvas, CANVAS_STATE_DYING);
	player->game->combatMonsters = NULL;
	Hud_randomBark(app->hud, 1, 1);
	app->statDeaths++;
}

/* 0x0000d848  a door or moving block closed on (tx, ty): if the player stands
 * there, push them to the first free neighbouring tile (the block's own
 * direction costs 25 HP, any other 50), or kill them if there is none. */
void Player_crushed(Player *player, int tx, int ty, int dx, int dy)
{
	struct { int dx, dy, dmg; } cand[7];
	Canvas *cv = player->canvas;
	int n = 0, i, damage = 0;
	int x = 0, y = 0;

	if (player->noclip != 0)
		return;
	if ((cv->viewX >> 6) != tx || (cv->viewY >> 6) != ty)
		return;
	if ((player->render->mapFlags[ty * 32 + tx] & 1) == 0)
		return;

	dx = clampInt(dx, -1, 1);
	dy = clampInt(dy, -1, 1);
	memset(cand, 0, sizeof cand);

	/* 1. along the block's movement */
	if (dx != 0 || dy != 0) {
		cand[n].dx = dx;
		cand[n].dy = dy;
		cand[n].dmg = 25;
		n++;
	}
	/* 2. back where the player came from */
	if (cv->prevX != cv->destX || cv->prevY != cv->destY) {
		cand[n].dx = clampInt(cv->prevX - cv->destX, -1, 1);
		cand[n].dy = clampInt(cv->prevY - cv->destY, -1, 1);
		cand[n].dmg = 50;
		n++;
	}
	/* 3. backwards from the view direction */
	cand[n].dx = clampInt(-cv->stepX, -1, 1);
	cand[n].dy = clampInt(-cv->stepY, -1, 1);
	cand[n].dmg = 50;
	n++;
	/* 4. the four directions */
	for (i = 0; i < 4; i++) {
		cand[n].dx = ModData_s32(&g_mod, MOD_CRUSH_DIRS + i * 12);
		cand[n].dy = ModData_s32(&g_mod, MOD_CRUSH_DIRS + i * 12 + 4);
		cand[n].dmg = 50;
		n++;
	}

	for (i = 0; i < n; i++) {
		if (cand[i].dx == -dx && cand[i].dy == -dy)
			continue; /* never into the block itself */
		x = cv->destX + cand[i].dx * 64;
		y = cv->destY + cand[i].dy * 64;
		if (Game_findEntityAt(player->game, x, y, 0x74b5) == NULL) {
			damage = cand[i].dmg;
			break;
		}
	}

	Hud_addMessage(player->app->hud, modStr(MOD_STR_CRUSHED), 0);
	if (i == n) {
		Player_died(player);
		return;
	}
	Player_hurtFeedback(player, 0);
	Player_takeDamage(player, damage, NULL);
	Particles_spawnBlood(player->app->combat, 0, (int)0xffff0000u, 0x28, 0);
	cv->prevX = cv->destX;
	cv->prevY = cv->destY;
	cv->destX = x;
	cv->destY = y;
	cv->pushDx = -cand[i].dx;
	cv->pushDy = -cand[i].dy;
	cv->pushSteps = 1;
	cv->f230 = 1;
}

/* ---------------------------------------------------------------- score / time */

/* 0x000069a0 */
int Player_calcScore(Player *player)
{
	int score = 0;
	int allKills = 1, allSecrets = 1;
	int i, minutes;

	/* 1000 per level cleared of monsters and per level with every secret
	 * found, +1000 for each complete set of eight */
	for (i = 0; i < 8; i++) {
		uint32_t bit = 1u << i;

		if (player->killsDoneMaps & bit)
			score += 1000;
		else
			allKills = 0;
		if (player->secretsDoneMaps & bit)
			score += 1000;
		else
			allSecrets = 0;
	}
	if (allKills)
		score += 1000;
	if (allSecrets)
		score += 1000;

	if (player->deaths == 0)
		score += 1000;
	else if (player->deaths < 10)
		score += (5 - player->deaths) * 50;
	else
		score -= 250;

	minutes = ((player->app->gameTime - player->timeStart) + player->playTime) / 60000;
	if (minutes < 120)
		score += (120 - minutes) * 15;

	if (player->totalTurns < 5000)
		score += (5000 - player->totalTurns) / 2;
	return score;
}

/* 0x00012898  "hh:mm:ss" (the player argument is unused) */
void Player_formatTime(Player *player, char *buf, int size, int ms)
{
	int secs = ms / 1000;
	int mins = secs / 60;

	App_snprintf(buf, size, modStr(MOD_STR_TIME_FORMAT), mins / 60, mins % 60, secs - mins * 60);
}

/* 0x0002445c  counts the map's op-0x53 tile-event commands: all of them, and
 * those whose arg2 is 0 */
void Player_countSecrets(Player *player, int *found, int *total)
{
	Render *render = player->render;
	int i, all = 0, zero = 0;

	if (render == NULL) {
		*found = 0;
		*total = 0;
		return;
	}
	for (i = 0; i < render->numCommands; i++) {
		if (render->commands[i].op == 0x53) {
			all++;
			if (render->commands[i].arg2 == 0)
				zero++;
		}
	}
	*found = zero;
	*total = all;
}

/* ---------------------------------------------------------------- quest log / wand chat */

/* 0x0000369c */
void Player_addNotebookIndex(Player *p, int strId)
{
	if (p->notebookCount == 8)
		return;
	p->notebook[p->notebookCount++] = (int16_t)strId;
	Canvas_showDialog(p->canvas, NULL, modStr(MOD_STR_NOTEBOOK_ENTRY), 3, 0, 0);
}

/* 0x000264f8  state 1 = the wand wants to talk */
void Player_setWandState(Player *player, int state)
{
	player->wandChatMode = state;
	player->wandChatStart = player->app->tickTime;
	if (state == 1)
		player->wandChatTime = player->app->tickTime - 1;
}

/* 0x0002b200  shift the wand-chat timers past a pause */
void Player_addPausedTime(Player *pl, int ms)
{
	if (ms > 0) {
		pl->wandChatStart += ms;
		pl->wandChatTime += ms;
	}
}

/* 0x0002beb8 */
void Player_updateWandChat(Player *pl)
{
	Applet *app = pl->app;
	Random *rnd = &app->random;
	int elapsed, period;
	uint32_t r;

	if ((pl->weapons & 1) == 0 || pl->wandChatMode != 1 || pl->attacked != 0)
		return;

	if (pl->ce.weapon == 0) {
		/* the wand is wielded: deliver the message */
		Canvas_showDialog(pl->canvas, NULL, pl->render->mapStrings[pl->wandChatStr], 1, 1, 0);
		Player_setWandState(pl, 0);
		app->hud->numSubtitles = 0;
		return;
	}

	if (pl->wandChatTime >= app->tickTime)
		return;

	/* nag, getting ruder after 15 s */
	elapsed = app->tickTime - pl->wandChatStart;
	Hud_randomBark(app->hud, elapsed < 15000 ? 0xb : 0xc, 1);

	period = elapsed;
	if (elapsed >= 30000) {
		Player_showHelp(pl, (pl->helpShown & 0x40) ? 7 : 6, 0);
		period = 30000;
	} else if (elapsed < 6000) {
		if (elapsed > 2000)
			Player_showHelp(pl, 5, 0);
		period = 1000;
	}

	/* inlined Random_nextInt (the refill only rewrites the used bytes) */
	if ((uint32_t)rnd->index + 4 >= 0x80) {
		GETRAND(rnd->pool, rnd->index);
		rnd->index = 0;
	}
	r = Random_wordAt(rnd, rnd->index); /* a possibly unaligned word load at pool + index (0x2bfba) */
	rnd->index += 4;
	pl->wandChatTime = period / 4 + app->tickTime + (int)(r & 0x7fffffffu) % period;
}

/* ---------------------------------------------------------------- lifecycle */

/* 0x00022354  new game */
void Player_reset(Player *player)
{
	Applet *app = player->app;
	int i;

	app->hud->msg2[0] = '\0';
	app->hud->numMessages = 0;
	player->notebookCount = 0;
	Applet_resetCounters(app, 1);
	player->facing = NULL;
	player->noclip = 0;
	player->invisible = 0;
	player->weaponHelp = 0;
	player->armorHelp = 0;
	player->firePortals = 3;
	player->attacked = 0;
	player->timestamp = 0;
	player->level = 1;
	player->xp = 0;
	player->nextLevelXP = Player_xpForLevel(player, 1);
	player->gold = 0;
	player->rings = 0;
	player->ring = 0;
	player->questItems = 0;
	for (i = 0; i < 5; i++)
		player->ammo[i] = 0;
	for (i = 0; i < 13; i++)
		player->items[i] = 0;
	player->numBuffs = 0;
	for (i = 0; i < 10; i++) {
		player->buffs[i].turns = 0;
		player->buffs[i].amount = 0;
	}
	player->numEffects = 0;
	for (i = 0; i < 12; i++) {
		player->effects[i].turns = 0;
		player->effects[i].stacks = 0;
		player->effects[i].amount = 0;
	}

	player->ammo[1] = 25;
	player->weapons = 3;
	player->weaponsPending = 0;
	player->secretsDoneMaps = 0;
	player->killsDoneMaps = 0;

	CombatEntity_setMaxHealth(&player->baseCe, 100);
	CombatEntity_setDefense(&player->baseCe, 20);
	CombatEntity_setStrength(&player->baseCe, 20);
	CombatEntity_setAccuracy(&player->baseCe, 25);
	Player_calcStats(player);
	CombatEntity_setHealth(&player->ce, 100);
	player->ce.weapon = 1;
	player->ce.level = 1;
	player->armorOwned = 0;
	CombatEntity_setArmor(&player->ce, 0);
	player->ce.type = -1;
	player->baseCe.type = -1;

	player->playTime = 0;
	player->totalTurns = 0;
	player->visitedMaps = 0;
	player->maxMapReached = 6000;
	player->f110 = 0;
	player->render->drunkLevel = 0;
	player->render->drunkTarget = 0;
	player->gameWon = 0;
}

/* 0x0002959c */
int Player_startup(Player *pl)
{
	Applet *app;

	if (pl->app == NULL) /* back-pointer normally set by App_initSubsystems */
		pl->app = g_app;
	app = pl->app;
	pl->canvas = app->canvas;
	pl->game = app->game;
	pl->render = app->render;
	pl->canary[0] = 0xdeadbeefu;
	pl->canary[1] = 0xfeedf00du;
	pl->canary[2] = 0xfefeeeeeu;
	pl->canary[3] = 0xbaadbeefu;
	pl->noclip = 0;
	pl->god = 0;
	pl->ring = 0;
	pl->helpShown = 0;
	pl->itemHelp = 0;
	pl->ringHelp = 0;
	pl->weaponHelp = 0;
	pl->armorHelp = 0;
	pl->helpEnabled = 1;
	pl->missStreak = 0;
	pl->lastTarget = NULL;
	pl->deaths = 0;
	Player_reset(pl);
	return 1;
}

/* 0x00018718  called when a map starts */
void Player_initForLevel(Player *player)
{
	Applet *app = player->app;

	player->timeStart = app->gameTime;
	player->levelTurns = 0;
	player->levelXP = 0;
	player->notebookCount = 0;
	player->wandChatMode = 0;
	player->wandChatTime = 0;
	player->lastAmmoTurn = 0;
	player->buffChangeTime = app->tickTime;
	player->attacked = 0;
	if (player->weaponsPending != 0)
		Player_refreshWeapons(player);
	Player_clampStat30(player);
	/* maps flagged "no weapons" force the wand */
	if (player->render->mapFlagBits & 2)
		Player_selectWeapon(player, 0);
}
