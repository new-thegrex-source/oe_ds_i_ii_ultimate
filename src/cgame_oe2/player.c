/*
 * Player: stats, level-up/XP, health, status effects ("buffs"), per-turn
 * updates, damage/death, score, notebook/quest log and help bitmasks.
 *
 * Original class: Player (Applet+0x124, 0x274 bytes).
 *
 * Status effects: 14 timed effects in Player.statusEffects[] {turns, amount,
 * stacks}. Player_updateStats folds them into the 12 per-stat modifiers in
 * Player.buffs[] {turns, amount}; Player_calcStats applies the modifiers and the
 * equipped ring to the base stats. Modifier slots used here: 5 regeneration,
 * 6 accuracy, 7 strength, 8 defense, 10 poison (damage per turn), 11 crouch
 * (lower eye height), 0 familiar energy display.
 */
#include "game.h"

/* ---------------------------------------------------------------- accessors */

/* 0x000023f0 */
Entity *Player_getPlayerEnt(Player *player)
{
	/* entities[1] is the player's body; entities[2] is the familiar. */
	if (player->isFamiliar == 0)
		return &g_app->game->entities[1];
	return &g_app->game->entities[2];
}

/* 0x00002438 */
void Player_getViewPos(Player *player, int *out3)
{
	Canvas *cv = g_app->canvas;

	if (player->isFamiliar == 0) {
		out3[0] = cv->destX;
		out3[1] = cv->destY;
		out3[2] = cv->destAngle;
	} else {
		/* while possessing the familiar, the body stays where it was left */
		out3[0] = cv->savedViewX;
		out3[1] = cv->savedViewY;
		out3[2] = cv->savedViewAngle;
	}
}

/* ---------------------------------------------------------------- health */

/* 0x000044c4 */
int Player_addHealth(Player *player, int amount)
{
	return Player_addHealthEx(player, amount, 1);
}

/* 0x000044cc */
int Player_addHealthEx(Player *player, int amount, int showMsg)
{
	Applet *app = g_app;
	CombatEntity *ce = &player->ce;
	int before, newHealth;

	if (amount > 0) {
		if (CombatEntity_getHealth(ce) == CombatEntity_getMaxHealth(ce))
			return 0;
		Hud_startStatFlash();
	} else if (player->god != 0) {
		return 0;
	}

	before = CombatEntity_getHealth(ce);
	app->hud->lastHealth = before;
	newHealth = before + amount;
	if (newHealth > CombatEntity_getMaxHealth(ce))
		newHealth = CombatEntity_getMaxHealth(ce);
	CombatEntity_setHealth(ce, newHealth);

	if (showMsg != 0 && before < CombatEntity_getHealth(ce)) {
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgInt(app->canvas, CombatEntity_getHealth(ce) - before);
		Hud_addMessage(app->hud, 0xc4); /* "+%1 health" */
	}

	if (amount > 0)
		app->statHealthGained += amount;
	else
		app->statHealthLost += amount;
	return 1;
}

/* ---------------------------------------------------------------- XP / levels */

/* 0x00009214 */
int Player_xpForLevel(Player *player, int lvl)
{
	int n = lvl - 1;
	return n * n * n * 20 + lvl * 180 - 20;
}

/* Stat gain on level-up, capped so the stat never exceeds 99. Returns the
 * amount actually added (0 = nothing changed). */
static int levelUpGain(int current, int gain)
{
	if (current + gain > 99)
		gain = 99 - current;
	return gain;
}

/* 0x00004df4 */
void Player_levelUp(Player *player)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	CombatEntity *base = &player->baseCe;
	String *msg;
	int gain;

	player->level++;
	player->ce.level = player->level;
	player->nextLevelXP = Player_xpForLevel(player, player->level);

	/* The level-up text is built in one of the two rotating message strings
	 * and handed to the help queue. */
	msg = &cv->msgRing[cv->msgRingIdx];
	cv->msgRingIdx = (cv->msgRingIdx + 1) % 2;
	String_setLength(msg, 0);

	Canvas_resetEvents(app->canvas);
	Canvas_addTextArgInt(app->canvas, player->level);
	Text_getString(app->canvas, 0, 0xbe, msg);

	/* max health +10, capped at 999 */
	gain = 10;
	if (CombatEntity_getMaxHealth(base) + 10 > 999)
		gain = 999 - CombatEntity_getMaxHealth(base);
	if (gain != 0) {
		CombatEntity_setMaxHealth(base, CombatEntity_getMaxHealth(base) + gain);
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgInt(app->canvas, gain);
		Text_getString(app->canvas, 0, 0xbf, msg);
	}

	/* defense +1 */
	gain = levelUpGain(CombatEntity_getDefense(base), 1);
	if (gain != 0) {
		CombatEntity_setDefense(base, CombatEntity_getDefense(base) + gain);
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgInt(app->canvas, gain);
		Text_getString(app->canvas, 0, 0xc0, msg);
	}

	/* strength +2 */
	gain = levelUpGain(CombatEntity_getStrength(base), 2);
	if (gain != 0) {
		CombatEntity_setStrength(base, CombatEntity_getStrength(base) + gain);
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgInt(app->canvas, gain);
		Text_getString(app->canvas, 0, 0xc1, msg);
	}

	/* accuracy +2 */
	gain = levelUpGain(CombatEntity_getAccuracy(base), 2);
	if (gain != 0) {
		CombatEntity_setAccuracy(base, CombatEntity_getAccuracy(base) + gain);
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgInt(app->canvas, gain);
		Text_getString(app->canvas, 0, 0xc2, msg);
	}

	Player_calcStats(player);
	Text_getString(app->canvas, 0, 0xc3, msg);
	CombatEntity_setHealth(&player->ce, CombatEntity_getMaxHealth(&player->ce));

	if (app->canvas->state != 2) {
		/* help type 2: show this ready-made String */
		Canvas_addHelpMessage(app->canvas, msg, 2, NULL);
		Sound_play(app->sndSys, 0x54, 2, 5, -1);
	}
	app->statLevelUps++;
}

/* 0x000058cc */
void Player_addXP(Player *player, int xp)
{
	Applet *app = g_app;

	Canvas_resetEvents(app->canvas);
	Canvas_addTextArgInt(app->canvas, xp);
	Hud_addMessage(app->hud, xp < 0 ? 0xbc : 0xbd);

	player->currentXP += xp;
	player->xpGained += xp;
	while (player->currentXP >= player->nextLevelXP)
		Player_levelUp(player);
	app->statXpGained += xp;
}

/* ---------------------------------------------------------------- stats */

/* 0x00044d44 */
void Player_calcStats(Player *pl)
{
	TextUtil *tu = g_app->textUtil;
	int accuracy = pl->baseCe.accuracy;
	int strength = pl->baseCe.strength;
	int defense = pl->baseCe.defense;
	int maxHealth = pl->baseCe.maxHealth;

	pl->ce.accuracy = accuracy;
	pl->ce.strength = strength;
	pl->ce.defense = defense;
	pl->ce.maxHealth = maxHealth;

	/* equipped ring bonus (only if the ring is owned) */
	if (pl->rings & (arm_bit(pl->equippedRing))) {
		switch (pl->equippedRing) {
		case 1: strength += strength / 4; break;
		case 2: accuracy += accuracy / 4; break;
		case 3: defense += defense / 4; break;
		case 6: pl->ce.maxHealth = maxHealth + maxHealth / 2; break;
		case 8: /* drunkard's ring */
			strength += 10;
			defense += 10;
			accuracy -= 15;
			break;
		default: break;
		}
	}

	if (pl->ce.health > pl->ce.maxHealth)
		pl->ce.health = pl->ce.maxHealth;

	pl->ce.strength = TextUtil_clamp(tu, pl->buffs[7].amount + strength, 0, 255);
	pl->ce.accuracy = TextUtil_clamp(tu, pl->buffs[6].amount + accuracy, 0, 255);
	pl->ce.defense = TextUtil_clamp(tu, pl->buffs[8].amount + defense, 0, 255);
}

/* Add an effect's amount to one stat modifier; the modifier keeps the shortest
 * remaining duration of the effects feeding it. */
static void addToModifier(Player *pl, int slot, int amount, int turns)
{
	Buff *b = &pl->buffs[slot];

	b->amount = TextUtil_clamp(g_app->textUtil, b->amount + amount, -99, 99);
	if (turns < b->turns || b->turns == 0)
		b->turns = turns;
}

/* 0x00042f20 */
void Player_updateStats(Player *pl)
{
	TextUtil *tu = g_app->textUtil;
	int i;

	for (i = 0; i < 12; i++) {
		pl->buffs[i].turns = 0;
		pl->buffs[i].amount = 0;
	}
	pl->numBuffs = 0;

	for (i = 0; i < 14; i++) {
		StatusEffect *se = &pl->statusEffects[i];

		if (se->turns <= 0)
			continue;

		switch (i) {
		case 11:
			/* own modifier, then also strength/accuracy/defense like effect 13 */
			pl->buffs[11].amount = TextUtil_clamp(tu, pl->buffs[11].amount + se->amount, -99, 99);
			pl->buffs[11].turns = se->turns;
			/* fall through */
		case 13:
			addToModifier(pl, 7, se->amount, se->turns);
			addToModifier(pl, 6, se->amount, se->turns);
			addToModifier(pl, 8, se->amount, se->turns);
			break;
		case 4:
		case 12:
			/* strength and defense up, accuracy down by 1.5x */
			addToModifier(pl, 7, se->amount, se->turns);
			addToModifier(pl, 8, se->amount, se->turns);
			addToModifier(pl, 6, -(se->amount + se->amount / 2), se->turns);
			break;
		default:
			pl->buffs[i].amount = TextUtil_clamp(tu, pl->buffs[i].amount + se->amount, -99, 99);
			pl->buffs[i].turns = se->turns;
			break;
		}
	}

	for (i = 0; i < 12; i++) {
		if (pl->buffs[i].turns > 0)
			pl->numBuffs++;
	}
	Player_calcStats(pl);
}

/* 0x00009a40 */
void Player_capStat29(Player *p)
{
	/* familiar energy (ammo slot 6) capped by progress */
	int cap = p->highestMap * 2 + 11;
	if (cap > 99)
		cap = 99;
	if (p->ammo[6] > cap)
		p->ammo[6] = (uint8_t)cap;
}

/* 0x00009a64 */
void Player_capStat24(Player *p)
{
	/* ammo slot 1 (wand charges) capped by progress */
	int cap = p->highestMap * 13 - 3;
	if (cap > 99)
		cap = 99;
	if (p->ammo[1] > cap)
		p->ammo[1] = (uint8_t)cap;
}

/* 0x0002ba40  script/console stat change */
void Player_modifyStat(Player *player, int stat, int amount)
{
	Render *render = g_app->render;

	switch (stat) {
	case 0:
		Player_addHealthEx(player, amount, 1);
		if (amount < 0)
			Player_hurtFeedback(player, 0);
		break;
	case 2:
		player->baseCe.defense += amount;
		Player_calcStats(player);
		break;
	case 3:
		player->baseCe.strength += amount;
		Player_calcStats(player);
		break;
	case 4:
		player->baseCe.accuracy += amount;
		Player_calcStats(player);
		break;
	case 5:
		Player_give(player, 4, 0, amount, 0);
		break;
	case 6:
		Player_addXP(player, amount);
		break;
	case 7:
		if (render->drunkTarget + amount < 0)
			render->drunkTarget = 0;
		else
			render->drunkTarget += amount;
		break;
	case 8:
		/* enough XP for the next level */
		if (amount > 0)
			Player_addXP(player, player->nextLevelXP - player->currentXP);
		break;
	default:
		break;
	}
}

/* 0x00034d20 */
int Player_requireStat(Player *player, int stat, int value)
{
	CombatEntity *ce = &player->ce;
	int v;

	switch (stat) {
	case 0: v = CombatEntity_getHealth(ce); break;
	case 2: v = CombatEntity_getDefense(ce); break;
	case 3: v = CombatEntity_getStrength(ce); break;
	case 4: v = CombatEntity_getAccuracy(ce); break;
	case 5: v = player->gold; break;
	case 6: v = player->currentXP; break;
	case 7: v = g_app->render->drunkTarget; break;
	default: return 1;
	}
	return v >= value;
}

/* ---------------------------------------------------------------- status effects */

/* 0x0000544c */
int Player_addStatusEffect(Player *player, int effect, int amount, int turns)
{
	StatusEffect *se = &player->statusEffects[effect];
	int stacks, wasActive, delta;

	/* poison already running: just restart it */
	if (effect == 10 && player->statusEffects[10].turns > 0) {
		player->statusEffects[10].turns = 5;
		player->statusEffects[10].stacks = 1;
		return 0;
	}

	stacks = se->stacks;
	wasActive = stacks != 0;

	if (effect == 1) {
		/* fixed +4 per stack, at most two stacks, 31 turns */
		if (se->stacks == 2)
			return 0;
		se->stacks++;
		se->amount += 4;
		se->turns = 31;
		if (!wasActive)
			player->numStatusEffects++;
		return 1;
	}

	if (stacks >= 4)
		return 0;
	if (effect != 0 && effect != 3 && effect != 13)
		turns++;

	delta = amount;
	if (amount != 0 && stacks > 0) {
		/* diminishing returns for each extra stack (8.8 fixed point) */
		int mag = amount < 0 ? -amount : amount;
		int step = (mag << 16) / 0x500;
		int scaled = (((stacks + 1) << 8) * step) >> 8;
		int reduced = mag - ((scaled + 0x80) >> 8);

		if (reduced <= 0)
			return 0;
		if (amount > 0) {
			if (se->amount == 99)
				return 0;
			delta = reduced;
		} else {
			if (se->amount == -99)
				return 0;
			delta = -reduced;
		}
	}

	se->amount = TextUtil_clamp(g_app->textUtil, se->amount + delta, -99, 99);
	if (se->turns < turns)
		se->turns = turns;
	se->stacks++;
	if (!wasActive)
		player->numStatusEffects++;
	return 1;
}

/* 0x000055d8 */
void Player_setStatusEffect(Player *player, int effect, int amount, int count, int turns)
{
	player->statusEffects[effect].amount = amount;
	player->statusEffects[effect].stacks = count;
	player->statusEffects[effect].turns = turns;
	player->numStatusEffects++;
}

/* 0x00031b84  clears one status effect (name kept from the original symbol map) */
void Player_removeBuff(Player *player, int idx)
{
	StatusEffect *se = &player->statusEffects[idx];

	if (se->turns == 0)
		return;
	se->amount = 0;
	se->stacks = 0;
	se->turns = 0;
	player->numStatusEffects--;
}

/* 0x00044e90 */
void Player_tickStatusEffects(Player *pl)
{
	int warn = 0;
	int i;

	if (pl->numStatusEffects == 0)
		return;

	for (i = 0; i < 14; i++) {
		int turns = pl->statusEffects[i].turns;

		if (turns == 0)
			continue;
		/* effect 0 (familiar) only counts down while possessing */
		if (i == 0 && pl->isFamiliar == 0)
			continue;
		if (turns <= 5)
			warn = 1; /* about to run out */
		if (turns == 1)
			Player_removeBuff(pl, i);
		else
			pl->statusEffects[i].turns = turns - 1;
	}
	if (warn)
		Sound_play(g_app->sndSys, 0xb, 0, 1, -1);
	Player_updateStats(pl);
}

/* ---------------------------------------------------------------- turns */

/* 0x00005c00 */
void Player_advanceTurn(Player *player)
{
	Applet *app = g_app;

	if (player->isFamiliar == 0) {
		/* familiar energy recharges while in your own body */
		Player_give(player, 3, 6, 4, 1);
	} else if (app->canvas->state != 0x11) {
		int left = player->buffs[0].turns;

		if (left <= 0) {
			player->ammo[6]--;
			left = player->ammo[6];
		}
		if (left == 0) {
			/* out of energy: snap back to the body */
			Player_showHelp(app->player, 0xb, 1);
			Hud_addMessageFlags(app->hud, 0x1b, 2);
			Sound_play(app->sndSys, 0x65, 0, 4, -1);
			Player_setFamiliar(player, 0, 1, 0);
		}
	}

	player->moves++;
	player->totalMoves++;
	Player_tickStatusEffects(player);

	/* regeneration */
	if (player->buffs[5].turns > 0)
		Player_addHealthEx(player, player->buffs[5].amount, 0);

	/* poison */
	if (player->buffs[10].turns > 0) {
		String *buf;

		Player_addHealthEx(player, player->buffs[10].amount, 1);
		buf = Hud_getMessageBuffer(app->hud, 1);
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgInt(app->canvas, -player->buffs[10].amount);
		Text_getString(app->canvas, 0, 0xb5, buf);
		Hud_finishMessageBuffer(app->hud);
		Game_gspriteAllocAnim(app->game, 0xfb, 0, 0, 0x20);
		app->canvas->waitTime = app->time + 500;
	}

	/* modifier 11 lowers the eye height (or shrinks the left-behind body) */
	if (player->isFamiliar == 0)
		app->canvas->destZ = player->buffs[11].turns > 0 ? 0x1b : 0x24;
	else
		player->familiarSprite->sprite->scale = player->buffs[11].turns > 0 ? 0x20 : 0x40;

	/* combat ends 4 turns after the last attack */
	if (player->inCombat != 0 && player->totalMoves - player->lastCombatTurn >= 4) {
		player->f130 = player->totalMoves;
		player->inCombat = 0;
	}

	/* wand charge (ammo 1) regeneration */
	if (player->ce.weapon != 0) {
		if (player->totalMoves - player->f130 >= 2) {
			Player_give(player, 3, 1, (app->canvas->mapId - 1) / 2 + 1, 0);
			player->f130 = player->totalMoves;
		}
	} else if (player->inCombat == 0 && player->totalMoves - player->f130 > 6) {
		Player_give(player, 3, 1, (app->canvas->mapId - 1) / 2 + 2, 0);
		player->f130 = player->totalMoves;
	}

	/* ring 4: regeneration */
	if (player->equippedRing == 4)
		Player_addHealthEx(player, 2, 0);

	/* low armor and an armor kit in the pack: companion hint */
	if (CombatEntity_getArmorPoints(&player->ce) < 20 &&
	    Player_requireItem(player, 0, 10, 1, 99) != 0)
		Hud_randomBark(app->hud, 9);

	if (player->statusEffects[12].turns > 0 || app->player->equippedRing == 8)
		app->stat5c8++; /* turns spent drunk */

	if (app->render->drunkLevel != 0 && player->f138 <= player->totalMoves)
		app->render->drunkTarget = 0;

	player->turnTime = app->time;
}

/* 0x00043ae4  empty in the original; called with (player, elapsedMs) */
void Player_stub(void)
{
}

/* ---------------------------------------------------------------- damage / death */

/* 0x0002cddc */
void Player_hurtFeedback(Player *player, int keepMsg)
{
	Applet *app = g_app;

	if (player->god != 0)
		return;
	if (keepMsg == 0)
		app->hud->damageDir = 0;
	app->hud->damageFlash = 1;
	if (player->ce.weapon == 4)
		Sound_play(app->sndSys, 0x65, 0, 5, -1);
	else
		Sound_play(app->sndSys, 0x5d, 0, 3, -1);
	Canvas_startShake(app->canvas, 500, 2, 150);
}

/* 0x0002d2d4 */
void Player_takeDamage(Player *player, int dmg, Entity *attacker)
{
	Applet *app = g_app;
	CombatEntity *ce = &player->ce;
	int health, maxHp, before, after;

	/* a hit ends possession */
	if (player->isFamiliar != 0) {
		Canvas *cv = app->canvas;
		Canvas_resetPlayerPos(cv, cv->savedViewX, cv->savedViewY);
		Player_setFamiliar(player, 0, 0, 0);
	}
	if (player->god != 0)
		return;

	Canvas_resetEvents(app->canvas);
	if (attacker == NULL) {
		Canvas_addTextArgInt(app->canvas, dmg);
		Hud_addMessage(app->hud, 0x5c);
	} else {
		Canvas_addTextArgStr(app->canvas, &attacker->name);
		Canvas_addTextArgInt(app->canvas, dmg);
		Hud_addMessage(app->hud, 0xc5);
	}
	if (dmg == 0)
		return;

	if (attacker != NULL && attacker->def->eType == 2)
		app->statDamageTakenBySubtype[attacker->def->eSubType] += dmg;

	health = CombatEntity_getHealth(ce);
	if (dmg >= health) {
		if (player->noDeathFlag == 0) {
			/* fatal: run the map's death event first */
			Game_executeTile0(app->game, 6, 0, 0xf000);
			if (attacker != NULL && attacker->def->eType == 2 && attacker->def->eSubType == 13)
				return;
		} else {
			dmg = health - 1;
		}
	}

	/* health fraction before/after, in 1/256 units */
	maxHp = CombatEntity_getMaxHealth(ce);
	before = (health << 16) / (maxHp << 8);
	maxHp = CombatEntity_getMaxHealth(ce);
	after = ((health - dmg) << 16) / (maxHp << 8);
	if (after > 0) {
		if (before > 26 && after <= 26)
			Hud_addMessageFlags(app->hud, 0xc6, 3); /* "Near Death!" */
		else if (before > 78 && after <= 78)
			Hud_addMessageFlags(app->hud, 0xc7, 3); /* "Low Health!" */
	}

	Player_addHealthEx(player, -dmg, 1);
	if (app->canvas->state == 5)
		Canvas_setState(app->canvas, 3);

	if (CombatEntity_getHealth(ce) <= 0) {
		Player_died(player);
		return;
	}
	if (CombatEntity_getHealth(ce) < CombatEntity_getMaxHealth(ce) / 10)
		Hud_randomBarkEx(app->hud, 5, 1);
}

/* 0x0000bf98 */
void Player_died(Player *player)
{
	Applet *app = g_app;

	if (app->canvas->state == 0xc)
		return;
	player->totalDeaths++;
	Sound_play(app->sndSys, 0x5c, 0, 5, -1);
	Canvas_startShake(app->canvas, 350, 5, 350);
	CombatEntity_setHealth(&player->ce, 0);
	Canvas_setState(app->canvas, 0xc);
	app->game->combatMonsters = NULL;
	app->statDeaths++;
}

/* ---------------------------------------------------------------- score */

/* 0x00008dd4 */
int Player_calcScore(Player *player)
{
	Applet *app = g_app;
	int score = 0;
	int all = 1;
	int i, minutes, deaths;

	/* 1000 per level cleared of monsters, +1000 for all seven */
	for (i = 0; i < 7; i++) {
		if (player->killedMonstersLevels & (1u << i))
			score += 1000;
		else
			all = 0;
	}
	if (all)
		score += 1000;

	deaths = player->totalDeaths;
	if (deaths == 0)
		score += 1000;
	else if (deaths < 10)
		score += (5 - deaths) * 50;
	else
		score -= 250;

	minutes = ((app->playTime - player->playTime) + player->totalTime) / 60000;
	if (minutes < 120)
		score += (120 - minutes) * 15;

	if (player->totalMoves < 5000)
		score += (5000 - player->totalMoves) / 2;
	return score;
}

/* 0x00009474 */
int Player_calcBonusScore(Player *player)
{
	Applet *app = g_app;
	int score = 0;
	int all = 1;
	int i, rating = 0;
	int32_t lock;

	/* 1000 per level with every secret found, +1000 for all seven */
	for (i = 0; i < 7; i++) {
		if (player->foundSecretsLevels & (1u << i))
			score += 1000;
		else
			all = 0;
	}
	if (all)
		score += 1000;

	/* lock-picking accuracy: lockpickTries = (attempts << 16) | correct */
	lock = app->player->lockpickTries;
	if (lock != 0) {
		int32_t correct = (int32_t)(((uint32_t)lock & 0xff00ffffu) << 8);
		rating = (correct / (lock >> 16) >> 6) + 1;
	}
	score += rating * 2000;

	if (app->stat5d0 < 5)
		score += 2000;
	return score;
}

/* ---------------------------------------------------------------- notebook / quests */

/* 0x0000524c */
void Player_addNotebookIndex(Player *player, int strId)
{
	if (player->numNotebookIndexes == 8) {
		App_fatalError(g_app, "MAX_NOTEBOOK_INDEXES");
		return;
	}
	player->notebookIndexes[player->numNotebookIndexes++] = (int16_t)strId;
}

/* 0x0001ca78  rebuild the quest list from the map's script byte code */
void Player_initNotebookFromMap(void)
{
	Applet *app = g_app;
	int i;

	app->player->numNotebookIndexes = 0;
	app->player->questComplete = 0;
	app->player->questFailed = 0;
	for (i = 0; i < app->render->numByteCodes; i++) {
		int op = app->render->byteCodes[i * 3];
		/* script ops that reference a notebook entry */
		if (op == 0x28 || op == 0x57)
			Player_addNotebookIndex(app->player, (int16_t)app->render->byteCodes[i * 3 + 1]);
	}
}

/* 0x00024898 */
int Player_isQuestDone(Player *player, int q)
{
	return (player->questComplete & (arm_bit(q))) != 0;
}

/* 0x000248ac */
int Player_isQuestFailed(Player *player, int q)
{
	return (player->questFailed & (arm_bit(q))) != 0;
}

/* 0x00044c94  mode 1 = done, 2 = failed, anything else = reset */
void Player_setQuestFlag(Player *pl, int id, int mode)
{
	int i;
	uint8_t bit;

	for (i = 0; i < pl->numNotebookIndexes; i++) {
		if (pl->notebookIndexes[i] == id)
			break;
	}
	if (i >= pl->numNotebookIndexes)
		return;

	bit = (uint8_t)(arm_bit(i));
	if (mode == 1) {
		pl->questComplete |= bit;
	} else if (mode == 2) {
		pl->questFailed |= bit;
	} else {
		pl->questComplete &= (uint8_t)~bit;
		pl->questFailed &= (uint8_t)~bit;
	}
}

/* ---------------------------------------------------------------- help */

/* 0x0003be48  returns 1 if the help text was queued */
int Player_showHelp(Player *player, int id, int force)
{
	Applet *app = g_app;
	uint32_t bit;

	/* while possessing the familiar only its own hints (0xb, 0xc) are shown */
	if (player->isFamiliar != 0 && id != 0xb && id != 0xc)
		return 0;
	if (Game_isCinematicActive(app->game) != 0)
		return 0;
	if (player->enableHelp == 0 && force == 0)
		return 0;

	bit = arm_bit(id);
	if (player->helpBitmask & bit)
		return 0;
	player->helpBitmask |= bit;
	Canvas_addHelpTextId(app->canvas, id);
	if (id != 6)
		Canvas_dequeueHelpDialog0(app->canvas);
	return 1;
}

/* Common body of the four "first time you get X" help popups: each kind keeps
 * its own bitmask and looks up the item's EntityDef (eType 6, subtype kind). */
static void showItemHelp(Player *player, uint32_t *mask, int subType, int n, int force)
{
	uint32_t bit;

	if (player->enableHelp == 0 && force == 0)
		return;
	bit = arm_bit(n);
	if (*mask & bit)
		return;
	*mask |= bit;
	Canvas_addHelpEntity(g_app->canvas, EntityDefs_find(g_app->entityDefs, 6, subType, n));
}

/* 0x0003bee8 */
void Player_showInvHelp(Player *player, int n, int force)
{
	showItemHelp(player, &player->invHelpBitmask, 0, n, force);
}

/* 0x0003bfe8 */
void Player_showWeaponHelp(Player *player, int n, int force)
{
	showItemHelp(player, &player->weaponHelpBitmask, 1, n, force);
}

/* 0x0003bf68 */
void Player_showSpellHelp(Player *player, int n, int force)
{
	showItemHelp(player, &player->spellHelpBitmask, 2, n, force);
}

/* 0x0003bb0c */
void Player_showArmorHelp(Player *player, int n, int force)
{
	showItemHelp(player, &player->armorHelpBitmask, 7, n, force);
}

/* ---------------------------------------------------------------- lifecycle */

/* 0x000350b0  new-game reset */
void Player_reset(Player *player, int resetView)
{
	Applet *app = g_app;
	int i;

	String_setLength(&app->hud->str1228, 0);
	app->hud->numMessages = 0;
	player->numNotebookIndexes = 0;
	player->questComplete = 0;
	player->questFailed = 0;
	player->boughtWeapons = 0;
	player->lockpickTries = 0;
	Applet_resetCounters(app, 1);
	player->facingEntity = NULL;
	player->f260 = 0;
	player->isFamiliar = 0;
	player->familiarSprite = NULL;

	if (resetView != 0) {
		Canvas *cv = app->canvas;
		cv->prevX = 0;
		cv->savedViewX = 0;
		cv->destX = 0;
		cv->viewX = 0;
		cv->prevY = 0;
		cv->savedViewY = 0;
		cv->destY = 0;
		cv->viewY = 0;
		cv->destZ = 0x24;
		cv->viewZ = 0x24;
		cv->savedViewAngle = 0;
		cv->destAngle = 0;
		cv->viewAngle = 0;
	}

	player->noclip = 0;
	player->fc0 = 1;
	player->inCombat = 0;
	player->level = 1;
	player->currentXP = 0;
	player->nextLevelXP = Player_xpForLevel(player, 1);
	player->gold = 0;
	player->rings = 0;
	player->equippedRing = 0;
	player->f38 = 0;
	for (i = 0; i < 7; i++)
		player->ammo[i] = 0;
	for (i = 0; i < 15; i++)
		player->inventory[i] = 0;
	player->numBuffs = 0;
	for (i = 0; i < 12; i++) {
		player->buffs[i].turns = 0;
		player->buffs[i].amount = 0;
	}
	player->numStatusEffects = 0;
	for (i = 0; i < 14; i++) {
		player->statusEffects[i].turns = 0;
		player->statusEffects[i].stacks = 0;
		player->statusEffects[i].amount = 0;
	}

	player->ammo[1] = 25;
	player->weapons = 7;
	player->disabledWeapons = 0;
	player->foundSecretsLevels = 0;
	player->killedMonstersLevels = 0;

	CombatEntity_setMaxHealth(&player->baseCe, 100);
	CombatEntity_setDefense(&player->baseCe, 20);
	CombatEntity_setStrength(&player->baseCe, 20);
	CombatEntity_setAccuracy(&player->baseCe, 25);
	Player_calcStats(player);
	CombatEntity_setHealth(&player->ce, 100);
	player->ce.weapon = 0;
	player->ce.level = 1;
	player->armorLevel = 0;
	player->lockLevel = 0;
	CombatEntity_setArmor(&player->ce, 0);
	player->ce.type = -1;
	player->baseCe.type = -1;

	player->totalTime = 0;
	player->totalMoves = 0;
	player->completedLevels = 0;
	player->highestMap = 1;
	player->f138 = 0;
	app->render->drunkLevel = 0;
	app->render->drunkTarget = 0;
	player->gameCompleted = 0;
}

/* 0x0003fd70 */
int Player_startup(Player *pl)
{
	pl->isFamiliar = 0;
	pl->f260 = 0;
	pl->noclip = 0;
	pl->god = 0;
	pl->equippedRing = 0;
	pl->helpBitmask = 0;
	pl->invHelpBitmask = 0;
	pl->spellHelpBitmask = 0;
	pl->weaponHelpBitmask = 0;
	pl->armorHelpBitmask = 0;
	pl->enableHelp = 1;
	pl->critCounter = 0;
	pl->lastTarget = NULL;
	pl->totalDeaths = 0;
	Player_reset(pl, 1);
	return 1;
}

/* 0x0002542c  called when a map starts */
void Player_initForLevel(Player *player)
{
	Applet *app = g_app;

	player->playTime = app->playTime;
	player->moves = 0;
	player->xpGained = 0;
	player->numNotebookIndexes = 0;
	player->questComplete = 0;
	player->questFailed = 0;
	player->f130 = 0;
	player->turnTime = app->time;
	player->inCombat = 0;
	Player_capStat24(player);

	/* On map 3 without the familiar wand, weapon 0 uses the alternate
	 * definition stored in Combat weapon slot 42. */
	if (player->highestMap == 3 && (player->weapons & 0x10) == 0)
		app->combat->weapons[0] = app->combat->weapons[42];
}
