/*
 * Player: save/load of the player record (part of the "HPLR".."TPLR" block
 * written by Game_savePlayerState) and play-time formatting.
 *
 * Record layout (all little-endian, in this order):
 *   CombatEntity baseCe, CombatEntity ce        (CombatEntity_saveState)
 *   int boughtWeapons, weapons, gold, level, currentXP
 *   byte armorLevel, lockLevel
 *   int totalTime (ms), totalMoves, completedLevels, killedMonstersLevels,
 *       foundSecretsLevels, disabledWeapons, lockpickTries
 *   byte prevWeapon, critCounter
 *   short lastTarget entity index (-1 none), short rings
 *   byte equippedRing, short f38
 *   int f104, weaponHelpBitmask, armorHelpBitmask, lastCombatTurn
 *   byte inCombat, short highestMap
 *   int f130, f138, Render.drunkTarget
 *   byte ammo[7], byte inventory[15]
 *   byte numStatusEffects; if non-zero 14 x {byte stacks, byte turns, sbyte amount}
 *   int statGoldGained, statGoldSpent, statBiggestHit, statDamageDealt
 *   short stat5c8, statGibs, stat5d0, statSteals
 *   short statShotsByWeapon[11], short statItemsUsed[15], int statTurnsByRing[9]
 *   byte gameCompleted
 */
#include "game.h"

/* 0x00037150 */
int Player_saveState(Player *player, OutputStream *os)
{
	Applet *app = g_app;
	int i;

	CombatEntity_saveState(&player->baseCe, os);
	CombatEntity_saveState(&player->ce, os);
	OutStream_writeInt(os, (int)player->boughtWeapons);
	OutStream_writeInt(os, (int)player->weapons);
	OutStream_writeInt(os, player->gold);
	OutStream_writeInt(os, player->level);
	OutStream_writeInt(os, player->currentXP);
	OutStream_writeBool(os, (int8_t)player->armorLevel);
	OutStream_writeBool(os, (int8_t)player->lockLevel);

	/* bank the time played since the last save/level start */
	player->totalTime = (app->playTime - player->playTime) + player->totalTime;
	player->playTime = app->playTime;
	OutStream_writeInt(os, player->totalTime);

	OutStream_writeInt(os, player->totalMoves);
	OutStream_writeInt(os, (int)player->completedLevels);
	OutStream_writeInt(os, (int)player->killedMonstersLevels);
	OutStream_writeInt(os, (int)player->foundSecretsLevels);
	OutStream_writeInt(os, (int)player->disabledWeapons);
	OutStream_writeInt(os, player->lockpickTries);
	OutStream_writeBool(os, (int8_t)player->prevWeapon);
	OutStream_writeBool(os, (int8_t)player->critCounter);
	if (player->lastTarget == NULL)
		OutStream_writeShort(os, -1);
	else
		OutStream_writeShort(os, (int16_t)Game_getEntityIndex(player->lastTarget));
	OutStream_writeShort(os, (int16_t)player->rings);
	OutStream_writeBool(os, (int8_t)player->equippedRing);
	OutStream_writeShort(os, (int16_t)player->f38);
	OutStream_writeInt(os, player->f104);
	OutStream_writeInt(os, (int)player->weaponHelpBitmask);
	OutStream_writeInt(os, (int)player->armorHelpBitmask);
	OutStream_writeInt(os, player->lastCombatTurn);
	OutStream_writeByte(os, player->inCombat);
	OutStream_writeShort(os, (int16_t)player->highestMap);
	OutStream_writeInt(os, player->f130);
	OutStream_writeInt(os, player->f138);
	OutStream_writeInt(os, app->render->drunkTarget);

	for (i = 0; i < 7; i++)
		OutStream_writeBool(os, (int8_t)player->ammo[i]);
	for (i = 0; i < 15; i++)
		OutStream_writeBool(os, (int8_t)player->inventory[i]);

	OutStream_writeBool(os, (int8_t)player->numStatusEffects);
	if (player->numStatusEffects != 0) {
		for (i = 0; i < 14; i++) {
			OutStream_writeBool(os, (int8_t)player->statusEffects[i].stacks);
			OutStream_writeBool(os, (int8_t)player->statusEffects[i].turns);
			OutStream_writeBool(os, (int8_t)player->statusEffects[i].amount);
		}
	}

	OutStream_writeInt(os, app->statGoldGained);
	OutStream_writeInt(os, app->statGoldSpent);
	OutStream_writeInt(os, app->statBiggestHit);
	OutStream_writeInt(os, app->statDamageDealt);
	OutStream_writeShort(os, (int16_t)app->stat5c8);
	OutStream_writeShort(os, (int16_t)app->statGibs);
	OutStream_writeShort(os, (int16_t)app->stat5d0);
	OutStream_writeShort(os, (int16_t)app->statSteals);
	for (i = 0; i < 11; i++)
		OutStream_writeShort(os, (int16_t)app->statShotsByWeapon[i]);
	for (i = 0; i < 15; i++)
		OutStream_writeShort(os, (int16_t)app->statItemsUsed[i]);
	for (i = 0; i < 9; i++)
		OutStream_writeInt(os, app->statTurnsByRing[i]);
	OutStream_writeByte(os, player->gameCompleted);
	return 1;
}

/* 0x00029568 */
int Player_loadState(Player *player, InputStream *stream)
{
	Applet *app = g_app;
	int i, idx, n;

	CombatEntity_loadState(&player->baseCe, stream);
	CombatEntity_loadState(&player->ce, stream);
	player->boughtWeapons = (uint32_t)Stream_readInt(stream);
	player->weapons = (uint32_t)Stream_readInt(stream);
	player->gold = Stream_readInt(stream);
	player->level = Stream_readInt(stream);
	player->currentXP = Stream_readInt(stream);
	player->armorLevel = Stream_readUByte(stream);
	player->lockLevel = Stream_readUByte(stream);
	player->totalTime = Stream_readInt(stream);
	player->playTime = app->playTime;
	player->totalMoves = Stream_readInt(stream);
	player->completedLevels = (uint32_t)Stream_readInt(stream);
	player->killedMonstersLevels = (uint32_t)Stream_readInt(stream);
	player->foundSecretsLevels = (uint32_t)Stream_readInt(stream);
	player->disabledWeapons = (uint32_t)Stream_readInt(stream);
	player->lockpickTries = Stream_readInt(stream);
	player->prevWeapon = Stream_readUByte(stream);
	player->critCounter = Stream_readUByte(stream);

	idx = Stream_readShort(stream);
	player->lastTarget = idx == -1 ? NULL : &app->game->entities[idx];

	player->rings = (uint32_t)Stream_readShort(stream);
	player->equippedRing = Stream_readUByte(stream);
	player->f38 = Stream_readShort(stream);
	player->f104 = Stream_readInt(stream);
	player->weaponHelpBitmask = (uint32_t)Stream_readInt(stream);
	player->armorHelpBitmask = (uint32_t)Stream_readInt(stream);
	player->lastCombatTurn = Stream_readInt(stream);
	player->inCombat = Stream_readBool(stream);
	player->highestMap = Stream_readShort(stream);
	player->f130 = Stream_readInt(stream);
	player->f138 = Stream_readInt(stream);
	n = Stream_readInt(stream);
	app->render->drunkTarget = n;
	app->render->drunkLevel = n;

	for (i = 0; i < 7; i++)
		player->ammo[i] = (uint8_t)Stream_readUByte(stream);
	for (i = 0; i < 15; i++)
		player->inventory[i] = (uint8_t)Stream_readUByte(stream);

	player->numStatusEffects = Stream_readUByte(stream);
	if (player->numStatusEffects != 0) {
		for (i = 0; i < 14; i++) {
			player->statusEffects[i].stacks = Stream_readUByte(stream);
			player->statusEffects[i].turns = Stream_readUByte(stream);
			player->statusEffects[i].amount = Stream_readByte(stream);
		}
	} else {
		for (i = 0; i < 14; i++) {
			player->statusEffects[i].stacks = 0;
			player->statusEffects[i].turns = 0;
			player->statusEffects[i].amount = 0;
		}
	}

	app->statGoldGained = Stream_readInt(stream);
	app->statGoldSpent = Stream_readInt(stream);
	app->statBiggestHit = Stream_readInt(stream);
	app->statDamageDealt = Stream_readInt(stream);
	app->stat5c8 = Stream_readShort(stream);
	app->statGibs = Stream_readShort(stream);
	app->stat5d0 = Stream_readShort(stream);
	app->statSteals = Stream_readShort(stream);
	for (i = 0; i < 11; i++)
		app->statShotsByWeapon[i] = Stream_readShort(stream);
	for (i = 0; i < 15; i++)
		app->statItemsUsed[i] = Stream_readShort(stream);
	for (i = 0; i < 9; i++)
		app->statTurnsByRing[i] = Stream_readInt(stream);
	player->gameCompleted = Stream_readBool(stream);

	Player_updateStats(player);
	Player_calcStats(player);
	return 1;
}

/* 0x0001b81c  "H:MM:SS"; returns outStr */
void *Player_formatTime(Player *self, int ms, String *outStr)
{
	int seconds = ms / 1000;
	int minutes = seconds / 60;
	int hours = minutes / 60;

	(void)self;
	String_setLength(outStr, 0);
	String_appendInt(outStr, hours);
	String_appendCStr_76f0(outStr, ":");
	if (minutes % 60 < 10)
		String_appendCStr_76f0(outStr, "0");
	String_appendInt(outStr, minutes % 60);
	String_appendCStr_76f0(outStr, ":");
	seconds -= minutes * 60;
	if (seconds < 10)
		String_appendCStr_76f0(outStr, "0");
	return String_appendInt(outStr, seconds);
}
