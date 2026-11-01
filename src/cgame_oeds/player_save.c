/*
 * Player persistence: the player part of a SaveData record, and the debug
 * dump written when the Player's memory canaries are found broken.
 *
 * Record layout (Player_saveState / Player_loadState, after Game's header):
 *   CombatEntity base, CombatEntity current (CombatEntity_saveState),
 *   int weapons, int gold, int level, int xp, byte armorOwned, long playTime,
 *   int totalTurns, int visitedMaps, int killsDoneMaps, int secretsDoneMaps,
 *   int weaponsPending, byte prevWeapon, byte missStreak, short lastTarget
 *   (entity index), short rings, byte ring, short questItems,
 *   int wandChatTime-now, int wandChatMode, int wandChatStart-now,
 *   int wandChatStr, int weaponHelp, int armorHelp, int attackTurn,
 *   ubyte attacked, int timestamp, short maxMapReached, byte mood,
 *   int lastAmmoTurn, int f110, int drunkTarget, 5 x byte ammo,
 *   13 x byte items, int firePortals, byte numEffects
 *   [12 x {byte stacks, byte turns, byte amount} if numEffects != 0],
 *   int goldGained, int goldSpent, int maxHit, int damage, short drunkTurns,
 *   short gibs, 11 x short weaponUse, 13 x short itemUse, 9 x int ringTurns,
 *   ubyte gameWon;
 * then the port's DS extras (records saved before them end here and load with them 0):
 *   int DS_SAVE_MARKER, int dsFlags, int dsCollectibles, short dsSpells, byte dsSpellHelp.
 */
#include "game.h"

/* Entity index saved for "no last target". The original stored the raw
 * pointer difference (garbage for NULL); any value outside 0..399 reads back
 * as no target here. */
#define NO_TARGET_INDEX (-1)

#define DS_SAVE_MARKER  0x44535831	/* "DSX1" */

/* 0x00023b28  returns 1 if the stream had no error */
int Player_saveState(Player *player, void *os)
{
	OutputStream *out = (OutputStream *)os;
	Applet *app = player->app;
	int i, target;

	CombatEntity_saveState(&player->baseCe, os);
	CombatEntity_saveState(&player->ce, os);
	OutStream_writeInt(os, (int)player->weapons);
	OutStream_writeInt(os, player->gold);
	OutStream_writeInt(os, player->level);
	OutStream_writeInt(os, player->xp);
	OutStream_writeBool(os, (int8_t)player->armorOwned);

	/* bank the play time of this session */
	player->playTime = (app->gameTime - player->timeStart) + player->playTime;
	player->timeStart = app->gameTime;
	OutStream_writeLong(os, player->playTime);

	OutStream_writeInt(os, player->totalTurns);
	OutStream_writeInt(os, (int)player->visitedMaps);
	OutStream_writeInt(os, (int)player->killsDoneMaps);
	OutStream_writeInt(os, (int)player->secretsDoneMaps);
	OutStream_writeInt(os, (int)player->weaponsPending);
	OutStream_writeBool(os, (int8_t)player->prevWeapon);
	OutStream_writeBool(os, (int8_t)player->missStreak);

	target = NO_TARGET_INDEX;
	if (player->lastTarget != NULL)
		target = (int)(player->lastTarget - player->game->entities);
	OutStream_writeShort(os, (int16_t)target);

	OutStream_writeShort(os, (int16_t)player->rings);
	OutStream_writeBool(os, (int8_t)player->ring);
	OutStream_writeShort(os, (int16_t)player->questItems);
	OutStream_writeInt(os, player->wandChatTime - app->tickTime);
	OutStream_writeInt(os, player->wandChatMode);
	OutStream_writeInt(os, player->wandChatStart - app->tickTime);
	OutStream_writeInt(os, player->wandChatStr);
	OutStream_writeInt(os, (int)player->weaponHelp);
	OutStream_writeInt(os, (int)player->armorHelp);
	OutStream_writeInt(os, player->attackTurn);
	OutStream_writeByte(os, player->attacked);
	OutStream_writeInt(os, player->timestamp);
	OutStream_writeShort(os, (int16_t)player->maxMapReached);
	OutStream_writeBool(os, (int8_t)player->mood);
	OutStream_writeInt(os, player->lastAmmoTurn);
	OutStream_writeInt(os, player->f110);
	OutStream_writeInt(os, player->render->drunkTarget);
	for (i = 0; i < 5; i++)
		OutStream_writeBool(os, (int8_t)player->ammo[i]);
	for (i = 0; i < 13; i++)
		OutStream_writeBool(os, (int8_t)player->items[i]);
	OutStream_writeInt(os, (int)player->firePortals);
	OutStream_writeBool(os, (int8_t)player->numEffects);
	if (player->numEffects != 0) {
		for (i = 0; i < 12; i++) {
			OutStream_writeBool(os, (int8_t)player->effects[i].stacks);
			OutStream_writeBool(os, (int8_t)player->effects[i].turns);
			OutStream_writeBool(os, (int8_t)player->effects[i].amount);
		}
	}

	OutStream_writeInt(os, app->statGoldGained);
	OutStream_writeInt(os, app->statGoldSpent);
	OutStream_writeInt(os, app->statMaxHit);
	OutStream_writeInt(os, app->statDamage);
	OutStream_writeShort(os, (int16_t)app->statDrunkTurns);
	OutStream_writeShort(os, (int16_t)app->statGibs);
	for (i = 0; i < 11; i++)
		OutStream_writeShort(os, (int16_t)app->statWeaponUse[i]);
	for (i = 0; i < 13; i++)
		OutStream_writeShort(os, (int16_t)app->statItemUse[i]);
	for (i = 0; i < 9; i++)
		OutStream_writeInt(os, app->statRingTurns[i]);
	OutStream_writeByte(os, player->gameWon);
	OutStream_writeInt(os, DS_SAVE_MARKER);
	OutStream_writeInt(os, (int)player->dsFlags);
	OutStream_writeInt(os, (int)player->dsCollectibles);
	OutStream_writeShort(os, (int16_t)player->dsSpells);
	OutStream_writeByte(os, player->dsSpellHelp);

	if (out->error != 0) {
		App_setErrorCode(g_app, 1);
		return 0;
	}
	return 1;
}

/* 0x0001abdc  returns 1 if the file had no error */
int Player_loadState(Player *player, void *file)
{
	File *f = (File *)file;
	Applet *app = player->app;
	int i, target;

	CombatEntity_loadState(&player->baseCe, file);
	CombatEntity_loadState(&player->ce, file);
	player->weapons = (uint32_t)Stream_readInt(f);
	player->gold = Stream_readInt(f);
	player->level = Stream_readInt(f);
	player->xp = Stream_readInt(f);
	player->armorOwned = (uint8_t)Stream_readByte(f);
	player->playTime = Stream_readInt2(f);
	player->timeStart = app->gameTime;
	player->totalTurns = Stream_readInt(f);
	player->visitedMaps = (uint32_t)Stream_readInt(f);
	player->killsDoneMaps = (uint32_t)Stream_readInt(f);
	player->secretsDoneMaps = (uint32_t)Stream_readInt(f);
	player->weaponsPending = (uint32_t)Stream_readInt(f);
	player->prevWeapon = (uint8_t)Stream_readByte(f);
	player->missStreak = (int8_t)Stream_readByte(f);

	target = Stream_readShort(f);
	if (target >= 0 && target < 400)
		player->lastTarget = &player->game->entities[target];
	else
		player->lastTarget = NULL;

	player->rings = (uint16_t)Stream_readShort(f);
	player->ring = (uint8_t)Stream_readByte(f);
	player->questItems = (uint16_t)Stream_readShort(f);
	player->wandChatTime = Stream_readInt(f) + app->tickTime;
	player->wandChatMode = Stream_readInt(f);
	player->wandChatStart = Stream_readInt(f) + app->tickTime;
	player->wandChatStr = Stream_readInt(f);
	player->weaponHelp = (uint32_t)Stream_readInt(f);
	player->armorHelp = (uint32_t)Stream_readInt(f);
	player->attackTurn = Stream_readInt(f);
	player->attacked = (uint8_t)Stream_readUByte(f);
	player->timestamp = Stream_readInt(f);
	player->maxMapReached = (uint16_t)Stream_readShort(f);
	player->mood = (uint8_t)Stream_readByte(f);
	player->lastAmmoTurn = Stream_readInt(f);
	player->f110 = Stream_readInt(f);
	player->render->drunkTarget = Stream_readInt(f);
	player->render->drunkLevel = player->render->drunkTarget;
	for (i = 0; i < 5; i++)
		player->ammo[i] = (uint8_t)Stream_readByte(f);
	for (i = 0; i < 13; i++)
		player->items[i] = (uint8_t)Stream_readByte(f);
	player->firePortals = (uint32_t)Stream_readInt(f);
	player->numEffects = (uint8_t)Stream_readByte(f);
	for (i = 0; i < 12; i++) {
		if (player->numEffects == 0) {
			player->effects[i].stacks = 0;
			player->effects[i].turns = 0;
			player->effects[i].amount = 0;
		} else {
			player->effects[i].stacks = (uint8_t)Stream_readByte(f);
			player->effects[i].turns = (uint8_t)Stream_readByte(f);
			player->effects[i].amount = (int8_t)Stream_readByte(f);
		}
	}

	app->statGoldGained = Stream_readInt(f);
	app->statGoldSpent = Stream_readInt(f);
	app->statMaxHit = Stream_readInt(f);
	app->statDamage = Stream_readInt(f);
	app->statDrunkTurns = Stream_readShort(f);
	app->statGibs = Stream_readShort(f);
	for (i = 0; i < 11; i++)
		app->statWeaponUse[i] = Stream_readShort(f);
	for (i = 0; i < 13; i++)
		app->statItemUse[i] = Stream_readShort(f);
	for (i = 0; i < 9; i++)
		app->statRingTurns[i] = Stream_readInt(f);
	player->gameWon = (uint8_t)Stream_readUByte(f);
	if (f->error == 0) {
		/* the DS extras; a record from before them ends here: that is not an error */
		if (Stream_readInt(f) == DS_SAVE_MARKER && f->error == 0) {
			player->dsFlags = (uint32_t)Stream_readInt(f);
			player->dsCollectibles = (uint32_t)Stream_readInt(f);
			player->dsSpells = (uint16_t)Stream_readShort(f);
			player->dsSpellHelp = (uint8_t)Stream_readUByte(f);
		} else {
			player->dsFlags = player->dsCollectibles = 0;
			player->dsSpells = 0;
			player->dsSpellHelp = 0;
			f->error = 0;
		}
	}

	Player_updateStats(player);
	Player_calcStats(player);
	app->hud->moodTarget = player->mood;
	app->hud->moodShown = player->mood;
	app->hud->moodTime = 0;

	if (f->error != 0) {
		App_setErrorCode(g_app, 1);
		return 0;
	}
	return 1;
}

/* Write a raw memory dump to a debug file. */
static void dumpToFile(Applet *app, const char *name, const void *data, int size)
{
	OutputStream os;

	OutputStream_ctor(&os, app);
	File_open(&os, (char *)name, _OFM_CREATE);
	OutStream_writeDirect(&os, (void *)data, size);
	OutputStream_flush(&os);
	OutputStream_dtor(&os, 0);
}

/* 0x0002c29c  memory-corruption check: dump Player and Game if a canary broke */
void Player_checkCanaries(Player *pl)
{
	if (pl->canary[0] == 0xdeadbeefu && pl->canary[1] == 0xfeedf00du &&
	    pl->canary[2] == 0xfefeeeeeu && pl->canary[3] == 0xbaadbeefu)
		return;

	pl->app->fileDebug = 1;
	/* the original wrote its 0x160 / 0x9434-byte objects; the port writes its own structs */
	dumpToFile(pl->app, "pdump", pl, (int)sizeof(Player));
	dumpToFile(pl->app, "gdump", pl->app->game, (int)sizeof(Game));
}
