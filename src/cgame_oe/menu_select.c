/*
 * Menus (Applet+0x1b3ab4, pointer at Applet+0x12c): what happens when a menu
 * item is chosen (Menus_select returns the next menu id, 0 to close, -1 for
 * none), remembered selections per menu, item detail pages, the Gaya store's
 * barter logic and starting a game.
 */

#include "game.h"
#include "oe_vm.h"

/* ---- data in the .mod ---- */
#define MOD_STORE_TABLE        0x2e7a8u /* 29 x int32 {type, index, price, qty}; Menu.storeRecord = record*4 */
#define MOD_S_LOADING_GAME     0x2f9fcu
#define MOD_S_GAME_LOADED      0x2fa0cu
#define MOD_S_SAVING_GAME      0x2fa18u
#define MOD_S_RESTARTING       0x2fa28u
#define MOD_S_GIVE_ALL         0x2fa38u
#define MOD_S_GIVE_MONEY       0x2fa44u
#define MOD_S_GIVE_AMMO        0x2fa50u
#define MOD_S_DRAGON_WARNING   0x2fa5cu
#define MOD_S_VIBRATE          0x2f290u
#define MOD_S_NONE             0x2ef4cu
#define MOD_S_SOUND            0x2f288u
#define MOD_S_ON               0x2f29cu
#define MOD_S_OFF              0x2f2a0u
#define MOD_S_VOLUME           0x2f2c0u
#define MOD_S_FMT_PERCENT      0x2f2b8u
#define MOD_S_EMPTY            0x2eaf4u

enum { SND_MENU_MUSIC = 5500 };
enum { MAP_FIRST = 6000, MAP_GAYA = 0x1778 };
enum {
	CANVAS_AUTOMAP = 5, CANVAS_CREDITS = 0x11, CANVAS_PORTAL = 0x15
};
enum { ITEM_SLIDER = 0x80 };

/* Menu.indexes slots (12 x {selected, scroll}, saved in Config). */
enum {
	SLOT_RINGS = 0, SLOT_WEAPONS = 1, SLOT_POTIONS = 2, SLOT_OTHER = 3, SLOT_ITEMS = 4, SLOT_STORE = 5,
	SLOT_STORE_WEAPONS = 6, SLOT_STORE_ARMOR = 7, SLOT_STORE_RINGS = 8, SLOT_STORE_POTIONS = 9, SLOT_VARS = 10,
	SLOT_CHEATS = 11
};

static char *S(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

/* Field `field` (0 type, 1 index, 2 price, 3 qty) of the store record at int offset rec. */
static int32_t storeField(int rec, int field)
{
	return ModData_s32(&g_mod, MOD_STORE_TABLE + (uint32_t)(rec + field) * 4);
}

/* 0x000234c4 Remember the current selection/scroll of a menu. */
void Menus_saveIndexes(void *menus, int slot)
{
	Menu *menu = menus;
	MenuSystem *ms = menu->app->menuSystem;

	menu->indexes[slot * 2] = ms->selected;
	menu->indexes[slot * 2 + 1] = ms->scroll;
}

/* 0x00027840 Start a new game (newGame) or load the saved one. */
void Menu_startGame(Menu *menu, int newGame)
{
	menu->app->menuSystem->background = NULL;
	if (newGame == 0) {
		Canvas_loadState(menu->canvas, 1, S(MOD_S_LOADING_GAME));
		return;
	}
	menu->game->activeLoadType = 0;
	menu->canvas->loadType = 0;
	Player_reset(menu->player);
	menu->player->deaths = 0;
	menu->player->helpShown = 0;
	menu->player->itemHelp = 0;
	menu->player->ringHelp = 0;
	Canvas *cv = menu->canvas;
	if (cv->skipIntro == 0)
		Canvas_setState(cv, 9);
	else
		Canvas_loadMap(cv, (uint16_t)cv->startMapId, 0);
}

/* 0x00026750 "Details" on an inventory or store item: remember the item's
 * texts and open the details page (menu 400). */
void Menus_showDetails(void *menus, int menuId, int item)
{
	Menu *menu = menus;
	MenuSystem *ms = menu->app->menuSystem;
	EntityDefManager *defs = menu->app->entityDefs;
	int subType, index;

	menu->detailText1 = NULL;
	menu->detailText2 = NULL;
	menu->detailDef = NULL;

	switch (menuId) {
	case 500: /* the armour row of the items menu */
		if (ms->items[item].action != 500)
			return;
		Menus_saveIndexes(menus, SLOT_ITEMS);
		subType = 7;
		index = menu->player->ce.armorType;
		break;
	case 0x12f: { /* store: weapons and ammo */
		if (item < 2)
			return;
		Menus_saveIndexes(menus, SLOT_STORE_WEAPONS);
		int rec = ms->items[item].action;
		subType = storeField(rec, 0);
		index = storeField(rec, 1);
		if (subType == 3 && index == 3) {
			/* this ammo record is shown as weapon 7 */
			subType = 1;
			index = 7;
		} else {
			subType &= 0xff;
		}
		break;
	}
	case 0x130:
	case 0x131:
	case 0x132: {
		if (item < 2)
			return;
		Menus_saveIndexes(menus, menuId == 0x130 ? SLOT_STORE_ARMOR
					 : menuId == 0x131 ? SLOT_STORE_RINGS : SLOT_STORE_POTIONS);
		int rec = ms->items[item].action;
		subType = storeField(rec, 0) & 0xff;
		index = storeField(rec, 1);
		break;
	}
	case 0x1f5:
		if (item < 2)
			return;
		Menus_saveIndexes(menus, SLOT_WEAPONS);
		subType = 1;
		index = ms->items[item].action;
		break;
	case 0x1f6:
		if (item < 2)
			return;
		Menus_saveIndexes(menus, SLOT_RINGS);
		subType = 2;
		index = ms->items[item].action;
		break;
	case 0x1f7:
		if (item < 2)
			return;
		Menus_saveIndexes(menus, SLOT_POTIONS);
		subType = 0;
		index = ms->items[item].action;
		break;
	case 0x1f8:
		if (item < 2)
			return;
		Menus_saveIndexes(menus, SLOT_OTHER);
		index = ms->items[item].action;
		if (index > 0xff) {
			/* quest items are listed as 0x100 + index */
			subType = 8;
			index -= 0x100;
		} else {
			subType = 0;
		}
		break;
	default:
		return;
	}

	EntityDef *def = EntityDefs_find(defs, 6, subType, index);
	menu->detailDef = def;
	if (def != NULL) {
		menu->detailText2 = def->longName;
		menu->detailText1 = def->description;
		ms->backMenu = menuId;
		MenuSystem_setMenu(ms, 400);
	}
}

/* Options (menu 9 from the title screen, 0x66 in game). Two layouts: with
 * .bar option 7 set there is one "FX:" row (Sound / Vibrate / None),
 * otherwise separate "Vibrate:" and "Sound:" rows. */
static int selectOptions(Menu *menu, MenuSystem *ms, int item)
{
	Applet *app = menu->app;
	Canvas *cv = menu->canvas;
	Sound *snd = app->sound;
	int volumeRow;

	if (item == 0)
		return ms->backMenu;

	if (cv->barOpt7) {
		volumeRow = 2;
		if (item == 1) {
			if (snd->enabled) {
				snd->enabled = 0;
				menu->canvas->vibrate = 1;
				strcpy(ms->items[1].text2, S(MOD_S_VIBRATE));
				Canvas_startShake(menu->canvas, 0, 0, 500);
				Sound_clearChannelFlags(app->sound);
				Sound_freeAllChannels(app->sound);
			} else if (cv->vibrate) {
				cv->vibrate = 0;
				strcpy(ms->items[1].text2, S(MOD_S_NONE));
			} else {
				snd->enabled = 1;
				menu->canvas->vibrate = 0;
				strcpy(ms->items[1].text2, S(MOD_S_SOUND));
				if (ms->backMenu == 100)
					MenuSystem_playOpenSound(ms);
				else
					Sound_play(app->sound, SND_MENU_MUSIC, 3, 5, -1);
			}
			/* the volume row exists only while sound is on */
			if (app->sound->enabled) {
				MenuItem_setFull(&ms->items[volumeRow], S(MOD_S_VOLUME), S(MOD_S_EMPTY), ITEM_SLIDER, 0, 0, NULL);
				App_snprintf(ms->items[volumeRow].text2, sizeof ms->items[volumeRow].text2,
					     S(MOD_S_FMT_PERCENT), app->sound->volume * 100 / 100);
			} else {
				MenuItem_setFull(&ms->items[volumeRow], NULL, NULL, 0, 0, 0, NULL);
			}
		} else if (item == 2) {
			goto volumeStep;
		}
	} else {
		volumeRow = 3;
		if (item == 1) {
			cv->vibrate = !cv->vibrate;
			if (cv->vibrate) {
				Canvas_startShake(menu->canvas, 0, 0, 500);
				strcpy(ms->items[1].text2, S(MOD_S_ON));
			} else {
				strcpy(ms->items[1].text2, S(MOD_S_OFF));
			}
		} else if (item == 2) {
			snd->enabled = !snd->enabled;
			if (snd->enabled) {
				if (ms->backMenu == 100)
					MenuSystem_playOpenSound(ms);
				else
					Sound_play(app->sound, SND_MENU_MUSIC, 3, 5, -1);
				strcpy(ms->items[2].text2, S(MOD_S_ON));
				MenuItem_setFull(&ms->items[volumeRow], S(MOD_S_VOLUME), S(MOD_S_EMPTY), ITEM_SLIDER, 0, 0, NULL);
				App_snprintf(ms->items[volumeRow].text2, sizeof ms->items[volumeRow].text2,
					     S(MOD_S_FMT_PERCENT), app->sound->volume * 100 / 100);
			} else {
				strcpy(ms->items[2].text2, S(MOD_S_OFF));
				Sound_clearChannelFlags(app->sound);
				Sound_freeAllChannels(app->sound);
				MenuItem_setFull(&ms->items[volumeRow], NULL, NULL, 0, 0, 0, NULL);
			}
		} else if (item == 3) {
			goto volumeStep;
		}
	}
	if (item > volumeRow) {
		/* port: the engine's settings after the game's rows (cgame_common/oe_options.c) */
		int row = item - volumeRow - 1;
		if (Options_select(row)) {
			char label[sizeof ms->items[0].text];
			Options_row(row, label, ms->items[item].text2);
		}
		ms->dirty = 1;
		return ms->menuId;
	}
	goto done;

volumeStep:
	/* selecting the volume row steps it by 10, wrapping past 100 to 0 */
	app->sound->volume += 10;
	if (app->sound->volume > 100)
		app->sound->volume = 0;
	Sound_applyVolume(app->sound);
	App_snprintf(ms->items[volumeRow].text2, sizeof ms->items[volumeRow].text2, S(MOD_S_FMT_PERCENT),
		     app->sound->volume * 100 / 100);
done:
	Game_saveConfig(menu->game);
	ms->dirty = 1;
	return ms->menuId; /* same menu: stays open */
}

/* Developer cheats (menu 0xca). Returns the menu to (re)open. */
static int selectCheats(Menu *menu, MenuSystem *ms, int item)
{
	Player *pl = menu->player;
	int i;

	Menus_saveIndexes(menu, SLOT_CHEATS);
	switch (item) {
	case 0:
		return ms->backMenu;
	case 1:
		pl->noclip = !pl->noclip;
		break;
	case 2:
		menu->game->disableAI = menu->game->disableAI == 0;
		break;
	case 3:
		pl->helpEnabled = !pl->helpEnabled;
		ms->menuId = 0;
		Game_saveConfig(menu->game);
		return 0xca;
	case 4:
		Player_equipForMap(pl, menu->canvas->mapId);
		break;
	case 5:
		Player_addXP(pl, pl->nextLevelXP - pl->xp);
		break;
	case 6:
		pl->god = !pl->god;
		break;
	case 7: /* give all */
		pl->weapons = 0x7ff;
		pl->rings = 0x1ff;
		for (i = 0; i < 5; i++)
			Player_give(menu->player, 3, i, 10, 0);
		for (i = 0; i < 13; i++) {
			Player_give(menu->player, 0, i & 0xff, 5, 0);
			menu->canvas->helpCount = 0;
		}
		Player_give(menu->player, 4, 0, 1000, 0);
		Hud_addMessage(menu->app->hud, S(MOD_S_GIVE_ALL), 0);
		return 0xca;
	case 8:
		Game_revealAutomapRect(menu->game, 0, 0, 0x20, 0x20);
		return 0xca;
	case 9:
		Player_give(menu->player, 4, 0, 1000, 0);
		Hud_addMessage(menu->app->hud, S(MOD_S_GIVE_MONEY), 0);
		return 0xca;
	case 10:
		for (i = 0; i < 5; i++)
			Player_give(menu->player, 3, i, 10, 0);
		Hud_addMessage(menu->app->hud, S(MOD_S_GIVE_AMMO), 0);
		return 0xca;
	case 11: /* cycle the quest gems */
		if (pl->questItems == 0)
			pl->questItems = 1;
		else if (pl->questItems & 1)
			pl->questItems = 4;
		else if (pl->questItems & 4)
			pl->questItems = 2;
		else
			pl->questItems = 0;
		break;
	case 12:
		Game_giveAllNotes(menu->game);
		return 0xca;
	case 13:
		menu->canvas->showCodes = !menu->canvas->showCodes;
		break;
	case 14:
		menu->canvas->showMonsters = !menu->canvas->showMonsters;
		break;
	case 15:
		pl->invisible = !pl->invisible;
		break;
	case 16:
		for (i = 0; i < 12; i++)
			Player_addStatusEffect(menu->player, i & 0xff, 10, 5);
		Player_updateStats(menu->player);
		break;
	case 17:
		for (i = 0; i < 12; i++) {
			menu->player->effects[i].turns = 0;
			menu->player->effects[i].stacks = 0;
			menu->player->effects[i].amount = 0;
		}
		menu->player->numEffects = 0;
		Player_updateStats(menu->player);
		break;
	case 18: { /* kill every live, active non-boss monster */
		Game *game = menu->game;
		for (i = 0; i < game->numEntities; i++) {
			Entity *ent = &game->entities[i];
			EntityDef *def = ent->def;
			if (def != NULL && def->eType == 2 && def->eSubType < 0xc && (ent->info & 0x40000) != 0 &&
			    (ent->monster->flags & 0x80) == 0)
				Entity_died(ent, 0);
			game = menu->game;
		}
		break;
	}
	default:
		return 0;
	}
	/* rebuild the menu so the on/off values refresh */
	ms->menuId = 0;
	return 0xca;
}

/* Developer "Vars" (menu 0xcd): renderer and debug toggles. */
static int selectVars(Menu *menu, MenuSystem *ms, int item)
{
	Canvas *cv = menu->canvas;
	Render *r = menu->render;

	Menus_saveIndexes(menu, SLOT_VARS);
	switch (item) {
	case 0:
		return menu->app->menuSystem->backMenu;
	case 1:
		cv->showLocation = !cv->showLocation;
		break;
	case 2: {
		int frames = menu->canvas->animFrames + 1;
		if (frames > 15)
			frames = 2;
		Canvas_setAnimFrames(menu->canvas, frames);
		Game_saveConfig(menu->game);
		break;
	}
	case 3:
		cv->showSpeeds = !cv->showSpeeds;
		break;
	case 4:
		r->rSkipCull = !r->rSkipCull;
		break;
	case 5:
		r->rNoTexture = !r->rNoTexture;
		break;
	case 6:
		r->rSkipBSP = !r->rSkipBSP;
		break;
	case 7:
		r->rSkipLines = !r->rSkipLines;
		break;
	case 8:
		r->rSkipDynLines = !r->rSkipDynLines;
		break;
	case 9:
		r->rSkipSprites = !r->rSkipSprites;
		break;
	case 10:
		cv->cameraMode = !cv->cameraMode;
		break;
	case 11:
		menu->app->fileDebug = !menu->app->fileDebug;
		Game_saveConfig(menu->game);
		break;
	case 12:
		cv->showNodes = !cv->showNodes;
		break;
	case 13:
		r->rShowLines = !r->rShowLines;
		break;
	case 14:
		menu->game->autoShowUntouched = !menu->game->autoShowUntouched;
		break;
	case 15:
		cv->showSaveSize = !cv->showSaveSize;
		break;
	default:
		return 0;
	}
	ms->menuId = 0;
	return 0xcd;
}

/* Gaya's tolerance for low offers, from the player's mood (Player.mood). */
static int moodTolerance(int mood)
{
	if (mood == 0)
		return 0;
	if (mood <= 0x20)
		return 0x4000;
	if (mood <= 0x60)
		return 0xc000;
	if (mood <= 0xa0)
		return 0x10000;
	if (mood <= 0xe0)
		return 0x18000;
	return 0x20000;
}

/* Barter: the player made an offer for the store record Menu.storeRecord
 * (menu 0x133). Decides accept (result 1..5 by how much above the price) or
 * reject (0), adjusts Gaya's mood and runs her reaction script (tile event
 * (result, mood tier) of map events). */
static int selectOffer(Menu *menu, MenuSystem *ms, int action)
{
	Applet *app = menu->app;
	Player *pl = menu->player;

	if (action == 0)
		return ms->backMenu;

	menu->offerIndex = app->menu->storeRecord;
	int offer = ms->items[2].action;
	if (pl->gold < offer)
		return 0x134;

	int rec = menu->storeRecord;
	int type = storeField(rec, 0);
	int index = storeField(rec, 1);
	if (type == 3) {
		if (pl->ammo[index] == 99)
			return 0x137;
	} else if (type == 0) {
		if (pl->items[index] == 99)
			return 0x137;
	}

	int next = -1;
	int result = 0;  /* 0 rejected, 1 accepted (at price) .. 5 (far above) */
	int moodDelta = 0;
	int price = storeField(rec, 2);
	int qty = storeField(rec, 3);
	int oldMood = pl->mood;
	int tol = moodTolerance(oldMood);
	int diff = offer - price;
	int pct = (diff * 100 / price) << 16; /* percent difference, 16.16 */

	if (offer == 0) {
		moodDelta = -0x40;
	} else if (diff >= 0) {
		result = 1;
		if (diff > 1000) {
			result = 5;
			moodDelta = 0x40;
		} else if (diff > 100) {
			result = 4;
			moodDelta = 0x20;
		} else if (diff > 10) {
			result = 3;
			moodDelta = 0x10;
		} else if (diff > 0) {
			result = 2;
			moodDelta = 8;
		}
	} else if (price <= 10) {
		if (-40 * tol >= pct)
			moodDelta = -8;
		else if (pct < 0)
			result = 1;
	} else if (price <= 100) {
		if (-25 * tol >= pct)
			moodDelta = -0x10;
		else if (-15 * tol >= pct)
			moodDelta = -8;
		else if (pct < 0)
			result = 1;
	} else if (price <= 1000) {
		if (-25 * tol >= pct) {
			moodDelta = -0x20;
		} else if (-15 * tol >= pct) {
			moodDelta = -0x10;
		} else if (-5 * tol >= pct) {
			result = 1;
			moodDelta = -8;
		} else if (pct < 0) {
			result = 1;
		}
	} else {
		if (-50 * tol >= pct) {
			moodDelta = -0x40;
		} else if (-15 * tol >= pct) {
			moodDelta = -0x20;
		} else if (-5 * tol >= pct) {
			result = 1;
			moodDelta = -8;
		} else if (pct < 0) {
			result = 1;
		}
	}

	ScriptThread *thread = Game_allocScriptThread(menu->game);
	if (pl->mood < 0x20 && moodDelta < 0)
		ScriptThread_initTileEvent(thread, 1, 6, 0xf000, 1);
	else if (moodDelta != 0)
		ScriptThread_initTileEvent(thread, 0, 6, 0xf000, 1);
	Player_addMood(menu->player, moodDelta);
	if (oldMood <= 0xe0 && menu->player->mood > 0xe0)
		menu->player->mood = 0xe0;

	if (result > 0) {
		app->statStoreBuys[menu->storeRecord / 4]++;
		Player_buyItem(menu, storeField(menu->storeRecord, 0), storeField(menu->storeRecord, 1),
			       ms->items[2].action, qty);
		MenuSystem_setMenu(ms, 0x135);
	} else {
		MenuSystem_setMenu(ms, 0x136);
	}

	if (moodDelta == 0) {
		Canvas *cv = menu->canvas;
		if (cv->helpCount > 0) {
			Canvas_dequeueHelpDialog(cv, 0);
			menu->canvas->fec8 = 1;
		} else {
			next = 0x135; /* also after a rejection with no mood change (as in the original) */
		}
		return next;
	}

	int tier;
	int mood = menu->player->mood;
	if (mood < 0x20)
		tier = 1;
	else if (mood < 0x60)
		tier = 2;
	else if (mood < 0xa0)
		tier = 3;
	else if (mood <= 0xe0)
		tier = 4;
	else
		tier = 5;
	ScriptThread_executeTile(thread, result, tier, 0xf000, 1);
	return -1;
}

/* 0x000246ac Run the action of item `item` of menu `menuId`; returns the menu
 * to open next (the current one to stay, 0 to close the menu, -1 none). */
int Menus_select(void *menus, int menuId, int item)
{
	Menu *menu = menus;
	Applet *app = menu->app;
	MenuSystem *ms = app->menuSystem;
	Canvas *cv = menu->canvas;
	/* the item's action (0 for an empty row) */
	int action = ms->items[item].text[0] != '\0' ? ms->items[item].action : 0;

	switch (menuId) {
	/* ---- front end ---- */
	case 1: /* main menu */
		switch (item) {
		case 0:
			if (Game_hasSaveData(menu->game))
				return 8;
			Menu_startGame(menu, 1);
			return 0;
		case 1:
			return 9;
		case 2:
			return 2;
		case 3:
			return 5;
		default:
			return 0;
		}
	case 2: /* help/about */
		switch (item) {
		case 0:
			return ms->backMenu;
		case 1:
			return 4;
		case 2:
			return 3;
		case 3:
			Canvas_setState(menu->canvas, CANVAS_CREDITS);
			return -1;
		default:
			return 0;
		}
	case 3:
	case 4:
		return 2;
	case 5: /* exit? */
		if (action != 1)
			return 1;
		Applet_shutdown(app);
		return 5;
	case 6: /* erase saved game? */
		if (action != 1)
			return 1;
		Game_deleteSaveFiles(menu->game);
		Menu_startGame(menu, 1);
		return 0;
	case 8: /* continue / new game */
		if (item == 0) {
			Menu_startGame(menu, 0);
			return 0;
		}
		if (item == 1) {
			if (Game_hasSaveData(menu->game))
				return 6;
			Menu_startGame(menu, 1);
			return 0;
		}
		return ms->backMenu;
	case 9:
	case 0x66:
		return selectOptions(menu, ms, item);
	case 10: /* level statistics */
		if (menu->canvas->creditsRunning != 0)
			return 0xc;
		Canvas_loadMap(menu->canvas, (uint16_t)menu->f20, 1);
		return 0;
	case 0xc:
		return 0xd;
	case 0xd:
		Canvas_setState(menu->canvas, CANVAS_CREDITS);
		return 0;
	case 0xe: /* enable sounds? */
		if (action == 1) {
			app->sound->enabled = 1;
			if (menu->canvas->barOpt7)
				menu->canvas->vibrate = 0;
		}
		return 1;
	case 0xf: /* continue playing? */
		if (action == 0) {
			cv->viewX = cv->destX;
			cv->viewY = menu->canvas->destY;
			menu->canvas->viewAngle = menu->canvas->destAngle;
			Canvas_finishRotation(menu->canvas);
			menu->canvas->fadeDir = 1;
			menu->canvas->fadeStart = (int32_t)GETUPTIMEMS();
			Canvas_setState(menu->canvas, CANVAS_PORTAL);
			return 0;
		}
		if (action == 1) {
			ms->backMenu = 0xf;
			return 0x10;
		}
		return 0;
	case 0x10: /* go to main menu? */
		if (action == 0) {
			Canvas_saveState(menu->canvas, 7, S(MOD_S_SAVING_GAME));
			return 0;
		}
		if (action == 2)
			return ms->backMenu;
		Canvas_backToMain(menu->canvas, 0);
		return 1;

	/* ---- in game ---- */
	case 100:
		ms->backMenu = 0;
		switch (item) {
		case 3:
			Canvas_saveState(menu->canvas, 3, S(MOD_S_SAVING_GAME));
			return 0;
		case 4:
			return Game_hasSaveData(menu->game) ? 0x70 : 0x71;
		case 5:
			Canvas_setState(menu->canvas, CANVAS_AUTOMAP);
			return action;
		case 10:
			ms->backMenu = 100;
			return action;
		default:
			return action;
		}
	case 0x65:
		return item == 0 ? 100 : 0x65;
	case 0x67: /* help menu */
		switch (item) {
		case 0:
			return ms->backMenu;
		case 2:
			menu->canvas->swallowKey = 1; /* show the on-screen help overlay */
			return 0;
		case 3:
			return 0x69;
		case 4:
			return 0x68;
		case 5:
			return 0x6b;
		case 6:
			return 0x6d;
		case 7:
			return 0x6c;
		case 8:
			Canvas_setState(menu->canvas, CANVAS_CREDITS);
			return -1;
		default:
			return 0;
		}
	case 0x68:
	case 0x69:
	case 0x6b:
	case 0x6c:
	case 0x6d:
		return 0x67;
	case 0x6a: { /* quit to main menu */
		int back = ms->backMenu;
		if (back == 100) {
			if (action == 0) {
				Canvas_saveState(menu->canvas, 7, S(MOD_S_SAVING_GAME));
				return 0;
			}
			if (action == 2)
				return back;
		} else if (action != 1) {
			return back;
		}
		Canvas_backToMain(menu->canvas, 0);
		return 1;
	}
	case 0x6e:
		return 100;
	case 0x70: /* load game? */
		if (action != 1)
			return 100;
		Canvas_loadState(menu->canvas, 1, S(MOD_S_LOADING_GAME));
		Hud_addMessage(menu->app->hud, S(MOD_S_GAME_LOADED), 0);
		return 0;
	case 0x71:
		return ms->backMenu;
	case 0x72: { /* died: try again? (reads the action even of an empty row) */
		int choice = ms->items[item].action;
		if (choice == 0) {
			if (!Game_hasSaveData(menu->game))
				return 0x71;
			Canvas_loadState(menu->canvas, 1, S(MOD_S_LOADING_GAME));
			return 0;
		}
		if (choice == 1) {
			Canvas_loadState(menu->canvas, 2, S(MOD_S_RESTARTING));
			return 0;
		}
		if (choice == 2)
			return 0x6a;
		return 0;
	}
	case 0x73: /* restart level? */
		if (action != 1)
			return 100;
		Canvas_loadState(menu->canvas, 2, S(MOD_S_LOADING_GAME));
		Hud_addMessage(menu->app->hud, S(MOD_S_GAME_LOADED), 0);
		return 0;

	/* ---- developer ---- */
	case 200:
		switch (item) {
		case 0:
			return ms->backMenu;
		case 1:
			Struct_clearPair(menu, SLOT_CHEATS);
			return 0xca;
		case 2:
			return 0xc9;
		case 3:
			return 0xcb;
		case 4:
			return 0xcc;
		default:
			return 0;
		}
	case 0xc9: /* change map */
		if (item == 0)
			return ms->backMenu;
		if (item >= 1 && item <= 11) {
			Canvas_loadMap(menu->canvas, MAP_FIRST + item - 1, 0);
			return 0;
		}
		if (item == 12) { /* reset level: reload without the saved world */
			menu->game->skipLoadWorld = 1;
			Canvas_loadMap(menu->canvas, menu->canvas->mapId, 0);
			return 0;
		}
		return 0;
	case 0xca:
		return selectCheats(menu, ms, item);
	case 0xcb:
		return 200;
	case 0xcc: /* developer tools */
		switch (item) {
		case 0:
			return ms->backMenu;
		case 1:
			Struct_clearPair(menu, SLOT_VARS);
			return 0xcd;
		case 2:
			Canvas_enterState10(menu->canvas, 0);
			return 0;
		case 3:
			return 0x12e;
		case 6:
		case 10:
			return 0xcc;
		case 7:
			app->errorSkip = 1;
			return 0xcc;
		case 8:
			/* "Error Loop": the original hangs here on purpose */
			for (;;) {
				volatile int spin = 0;
				(void)spin;
			}
		case 9:
			menu->canvas->state = -1;
			return 0xcc;
		case 11:
			Game_setRespawn(menu->game);
			return 0xcc;
		default:
			return 0;
		}
	case 0xcd:
		return selectVars(menu, ms, item);

	/* ---- Gaya's store ---- */
	case 0x12e:
		Menus_saveIndexes(menus, SLOT_STORE);
		return action;
	case 0x12f:
	case 0x130:
	case 0x131:
	case 0x132: {
		int slot = menuId == 0x12f ? SLOT_STORE_WEAPONS
			 : menuId == 0x130 ? SLOT_STORE_ARMOR
			 : menuId == 0x131 ? SLOT_STORE_RINGS : SLOT_STORE_POTIONS;
		Menus_saveIndexes(menus, slot);
		if (item == 0)
			return ms->backMenu;
		ms->backMenu = menuId;
		menu->storeRecord = action;
		return 0x133;
	}
	case 0x133:
		return selectOffer(menu, ms, action);
	case 0x134:
	case 0x136:
		return 0x133;
	case 0x135:
	case 0x137:
	case 400:
		return ms->backMenu;

	/* ---- inventory ---- */
	case 500:
		Menus_saveIndexes(menus, SLOT_ITEMS);
		return action;
	case 0x1f5: /* weapons */
		Menus_saveIndexes(menus, SLOT_WEAPONS);
		if (item <= 1)
			return action;
		if (action != 0 && menu->canvas->mapId == MAP_GAYA) {
			/* no weapons drawn in front of the dragon */
			Canvas_showDialog(menu->canvas, NULL, S(MOD_S_DRAGON_WARNING), 1, 0, 0);
			return 0;
		}
		Player_selectWeapon(menu->player, action & 0xff);
		return 0;
	case 0x1f6: /* rings */
		Menus_saveIndexes(menus, SLOT_RINGS);
		if (item <= 1)
			return action;
		Player_selectSpell(menu->player, action & 0xff);
		ms->dirty = 1;
		ms->menuId = 0;
		return 0x1f6;
	case 0x1f7: /* potions */
		Menus_saveIndexes(menus, SLOT_POTIONS);
		if (item <= 1)
			return action;
		menu->useItem = (uint8_t)action;
		ms->backMenu = 0x1f7;
		return 0x1f9;
	case 0x1f8: /* other items */
		Menus_saveIndexes(menus, SLOT_OTHER);
		if (item <= 1)
			return action;
		if (action >= 0x100)
			return 0x1f8; /* quest items can't be used */
		menu->useItem = (uint8_t)action;
		ms->backMenu = 0x1f8;
		return 0x1f9;
	case 0x1f9: { /* use item? */
		if (action != 1)
			return ms->backMenu;
		int used = Player_useItem(menu->player, menu->useItem, 0);
		Game_snapAllMovers(menu->game, 1);
		if (menu->canvas->state != 2)
			return 0;
		if (used)
			return ms->backMenu;
		int8_t idx = (int8_t)menu->useItem;
		if (idx == 1 || idx == 0)
			return 0x1fa; /* health potion at full health */
		if (idx == 0xc)
			return 0x1fc; /* armour kit at full armour */
		return 0x1fb;
	}
	case 0x1fa:
	case 0x1fb:
	case 0x1fc:
		return ms->backMenu;

	default:
		return 0;
	}
}
