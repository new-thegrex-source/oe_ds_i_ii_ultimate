/*
 * Menus_select (0x384c8): what happens when an item is chosen. Returns the menu
 * to open next (0 closes the menu and resumes play, -1 or the current id keeps
 * the menu). Also the remembered list positions, item details, starting a game
 * and the Menu builder's startup/shutdown.
 */

#include "game.h"
#include "oe_vm.h"

#define MOD_STORE_TABLE 0x464a0u /* 37 x {u32 itemType, u32 itemIndex, u32 price, u32 quantity} */
#define MOD_STR_BAD_INDEX 0x4675cu /* fatal error texts */
#define MOD_STR_FORCED    0x46774u

static char *mod_cstr(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

enum { TEXT_GROUP_MENU = 3 };

/* Word w of the store table (an item's action is entry*4). */
static int32_t storeWord(int w)
{
	return ModData_s32(&g_mod, MOD_STORE_TABLE + (uint32_t)w * 4);
}

/* Debug switches flip 0 <-> 1 (any other value becomes 0). */
static int toggle(int v)
{
	return ((uint32_t)v > 1) ? 0 : 1 - v;
}

/* 0x0003e368 */
int Menu_startup(Menu *menu)
{
	menu->storeItem = 0;
	menu->storeItem2 = -1;
	return 1;
}

/* 0x0003c3a8 */
void Menus_shutdown(Menu *menus)
{
	String_release(&menus->scratch0);
	String_release(&menus->scratch1);
	String_release(&menus->scratch2);
	String_release(&menus->detailsText);
	String_release(&menus->detailsText2);
	Menu_freeContacts(menus);
}

/* 0x00036660 Remember the list position of a menu (13 slots, saved in Config). */
void Menus_saveIndexes(Menu *menus, int slot)
{
	Applet *app = g_app;
	MenuSystem *ms = app->menuSystem;

	menus->savedIndexes[slot * 2] = ms->selectedIndex;
	menus->savedIndexes[slot * 2 + 1] = ms->scrollIndex;
	if (ms->selectedIndex > ms->numItems)
		App_fatalError(app, mod_cstr(MOD_STR_BAD_INDEX));
}

/* 0x00026508 */
void Menu_loadIndexes(Menu *menu, int slot)
{
	MenuSystem *ms = g_app->menuSystem;

	ms->selectedIndex = menu->savedIndexes[slot * 2];
	ms->scrollIndex = menu->savedIndexes[slot * 2 + 1];
	if (ms->selectedIndex > ms->numItems) {
		ms->scrollIndex = 0;
		ms->selectedIndex = 0;
	}
}

/* 0x0003d80c Leave the front end: load the saved game, or start a new one
 * (intro story unless skipped). */
void Menu_startGame(Menu *menu, int newGame)
{
	Applet *app = g_app;
	MenuSystem *ms = app->menuSystem;
	Player *pl = app->player;
	Canvas *cv = app->canvas;

	if (ms->imgLogo.bitmap != NULL)
		App_freeImage(app, &ms->imgLogo);
	ms->imgBG = NULL;
	App_freeImage(app, &ms->imgMenuMap);

	if (newGame == 0) {
		Canvas_startLoading(cv, 1, TEXT_GROUP_MENU, 0x11c);
		return;
	}

	app->game->loadType = 0;
	cv->loadType = 0;
	for (int i = 0; i < 13; i++) {
		menu->savedIndexes[i * 2 + 1] = 0;
		menu->savedIndexes[i * 2] = 0;
	}
	Player_reset(pl, 1);
	pl->totalDeaths = 0;
	pl->helpBitmask = 0;
	pl->invHelpBitmask = 0;
	pl->spellHelpBitmask = 0;
	pl->weaponHelpBitmask = 0;
	pl->armorHelpBitmask = 0;
	if (cv->skipIntro != 0)
		Canvas_loadMapEx(cv, cv->startupMap, 0);
	else
		Canvas_setState(cv, 8);
}

/* 0x0003bb8c Select on an item with details (flag 0x20): show its entity
 * description in menu 400. */
void Menus_showDetails(Menu *menus, int menuId, int item)
{
	Applet *app = g_app;
	MenuSystem *ms = app->menuSystem;
	int sub, index, slot, w;

	String_setLength(&menus->detailsText, 0);
	String_setLength(&menus->detailsText2, 0);
	menus->detailsDef = NULL;

	switch (menuId) {
	case 0x12f: /* armoury: ammunition is described by its weapon's entity */
		if (item < 2)
			return;
		Menus_saveIndexes(menus, 6);
		w = ms->items[item].action;
		sub = storeWord(w) & 0xff;
		index = storeWord(w + 1);
		if (storeWord(w) == 3 && index >= 3 && index <= 5) {
			sub = 1;
			index = (index == 3) ? 10 : (index == 4) ? 9 : 8;
		}
		break;
	case 0x130:
	case 0x131:
	case 0x132:
	case 0x134:
		if (item < 2)
			return;
		slot = (menuId == 0x130) ? 7 : (menuId == 0x131) ? 8 : (menuId == 0x132) ? 9 : 10;
		Menus_saveIndexes(menus, slot);
		w = ms->items[item].action;
		index = storeWord(w + 1);
		sub = storeWord(w) & 0xff;
		break;
	case 0x1f5: /* weapons */
		if (item < 2)
			return;
		Menus_saveIndexes(menus, 1);
		index = ms->items[item].action;
		sub = 1;
		break;
	case 0x1f6: /* rings */
		if (item < 2)
			return;
		Menus_saveIndexes(menus, 0);
		index = ms->items[item].action;
		sub = 2;
		break;
	case 0x1f7: /* potions */
		if (item < 2)
			return;
		Menus_saveIndexes(menus, 2);
		index = ms->items[item].action;
		sub = 0;
		break;
	case 0x1f8: /* other: lock picks, armour, misc */
		if (item == 3) {
			Menus_saveIndexes(menus, 3);
			index = ms->items[3].action;
			sub = 9;
		} else if (item == 4) {
			Menus_saveIndexes(menus, 3);
			sub = 7;
			index = app->player->ce.armorType;
		} else {
			if (item < 4)
				return;
			Menus_saveIndexes(menus, 3);
			index = ms->items[item].action;
			sub = 0;
		}
		break;
	default:
		return;
	}

	EntityDef *def = EntityDefs_find(app->entityDefs, 6, sub, index);
	menus->detailsDef = def;
	if (def != NULL) {
		String_appendString(&menus->detailsText2, &def->longName);
		String_appendString(&menus->detailsText, &menus->detailsDef->description);
		ms->backMenu = menuId;
		MenuSystem_setMenu(ms, 400);
	}
}

/* Options (0x11 front end, 0x66 in game). Rows: back, vibration, sound, volume;
 * or back, combined sound/vibration, volume on exclusive-feedback phones. */
static int selectOptions(Menu *menus, int item)
{
	Applet *app = g_app;
	MenuSystem *ms = app->menuSystem;
	Canvas *cv = app->canvas;
	Sound *snd = app->sndSys;

	if (item == 0)
		return ms->backMenu;

	/* port: the engine's settings after the game's rows (cgame_common/oe_options.c) */
	if (item >= (cv->exclusiveFeedback == 0 ? 4 : 3)) {
		int row = item - (cv->exclusiveFeedback == 0 ? 4 : 3);
		if (Options_select(row)) {
			char label[32], value[16];
			Options_row(row, label, value);
			String_setLength(&ms->items[item].text2, 0);
			String_appendCStr_76f0(&ms->items[item].text2, value);
		}
		return ms->menu;
	}

	if (cv->exclusiveFeedback == 0) {
		if (item == 1) {
			cv->vibrateEnabled = toggle(cv->vibrateEnabled);
			String_setLength(&ms->items[1].text2, 0);
			if (cv->vibrateEnabled == 0) {
				Text_getString(app->canvas, TEXT_GROUP_MENU, 0x85, &ms->items[1].text2);
			} else {
				Canvas_startShake(cv, 0, 0, 500);
				Text_getString(app->canvas, TEXT_GROUP_MENU, 0x84, &ms->items[1].text2);
			}
		} else if (item == 2) {
			snd->enabled = toggle(snd->enabled);
			if (snd->enabled == 0) {
				String_setLength(&ms->items[2].text2, 0);
				Text_getString(cv, TEXT_GROUP_MENU, 0x85, &ms->items[2].text2);
				String_setLength(&ms->items[3].text2, 0);
				Text_getString(cv, TEXT_GROUP_MENU, 0x11f, &ms->items[3].text2);
				Sound_requestStopAll(snd);
				String a, b;
				String_ctor(&a);
				String_ctor(&b);
				MenuItem_set(&ms->items[3], &a, &b, 0);
				String_dtor(&b, 0);
				String_dtor(&a, 0);
			} else {
				if (ms->backMenu == 100)
					MenuSystem_playOpenSound(ms);
				else
					Sound_play(snd, 0x56, 3, 5, -1);
				String_setLength(&ms->items[2].text2, 0);
				Text_getString(cv, TEXT_GROUP_MENU, 0x84, &ms->items[2].text2);
				String_setLength(&menus->scratch0, 0);
				Text_getString(cv, TEXT_GROUP_MENU, 0x8c, &menus->scratch0);
				TextUtil_dehyphenate(app->textUtil, &menus->scratch0);
				String_setLength(&menus->scratch1, 0);
				String_appendChar(String_appendInt(&menus->scratch1, snd->volume), '%');
				MenuItem_set(&ms->items[3], &menus->scratch0, &menus->scratch1, 0x80);
			}
		} else if (item == 3) {
			snd->volume += 10;
			if (snd->volume > 100)
				snd->volume = 0;
			Sound_applyVolume(snd);
			String_setLength(&ms->items[3].text2, 0);
			String_appendChar(String_appendInt(&ms->items[3].text2, snd->volume), '%');
		}
	} else {
		int volumeItem;
		if (item == 1) {
			/* cycle sound -> vibrate -> silent -> sound */
			if (snd->enabled != 0) {
				snd->enabled = 0;
				app->canvas->vibrateEnabled = 1;
				String_setLength(&ms->items[1].text2, 0);
				Text_getString(app->canvas, TEXT_GROUP_MENU, 0x88, &ms->items[1].text2);
				TextUtil_dehyphenate(app->textUtil, &ms->items[1].text2);
				Canvas_startShake(app->canvas, 0, 0, 500);
				Sound_requestStopAll(app->sndSys);
			} else if (cv->vibrateEnabled != 0) {
				cv->vibrateEnabled = 0;
				String_setLength(&ms->items[1].text2, 0);
				Text_getString(app->canvas, TEXT_GROUP_MENU, 0x41, &ms->items[1].text2);
			} else {
				snd->enabled = 1;
				app->canvas->vibrateEnabled = 0;
				String_setLength(&ms->items[1].text2, 0);
				Text_getString(app->canvas, TEXT_GROUP_MENU, 0x86, &ms->items[1].text2);
				if (ms->backMenu == 100)
					MenuSystem_playOpenSound(ms);
				else
					Sound_play(app->sndSys, 0x56, 3, 5, -1);
			}
			volumeItem = 2;
		} else if (item == 2) {
			snd->volume += 10;
			if (snd->volume > 100)
				snd->volume = 0;
			Sound_applyVolume(snd);
			menus->volume = snd->volume;
			volumeItem = menus->soundItem;
		} else {
			Game_saveConfig(app->game);
			return ms->menu;
		}
		Menus_setVolumeItem(menus, volumeItem);
	}
	Game_saveConfig(app->game);
	return ms->menu;
}

/* Main menu */
static int selectMainMenu(Menu *menus, int item)
{
	MenuSystem *ms = g_app->menuSystem;

	switch (item) {
	case 0: /* continue */
		Menu_startGame(menus, 0);
		return 0;
	case 1: /* new game: confirm erasing an existing save first */
		return App_hasSaveData(g_app) == 0 ? 0x10 : 0xe;
	case 2:
		return 0x11;
	case 3:
		return 4;
	case 4:
		ms->moreGamesPage = 0;
		return 0x17;
	default:
		return 0;
	}
}

/* Help / About */
static int selectHelpMenu(int item)
{
	static const int16_t pages[] = { -1, 9, 10, 0xb, 0xc, 5, 7, 6, -1, 8 };

	if (item == 0)
		return g_app->menuSystem->backMenu;
	if (item == 8) { /* credits */
		Canvas_setState(g_app->canvas, 0xe);
		return -1;
	}
	if (item >= 1 && item <= 9)
		return pages[item];
	return 4;
}

/* In-game help topics */
static int selectGameHelp(int item)
{
	Applet *app = g_app;

	switch (item) {
	case 0:
		return app->menuSystem->backMenu;
	case 1:
		return 0;
	case 2: /* controls overlay */
		app->canvas->hideHud = 1;
		app->menuSystem->menu = 0x67;
		return 0;
	case 3: return 0x68;
	case 4: return 0x69;
	case 5: return 0x6a;
	case 6: return 0x6b;
	case 7: return 0x6d;
	case 8: return 0x6f;
	case 9: return 0x6e;
	case 10: /* credits */
		Sound_play(app->sndSys, 0x56, 2, 5, -1);
		Canvas_setState(app->canvas, 0xe);
		return -1;
	default:
		return 0;
	}
}

/* In-game menu (100) */
static int selectGameMenu(int item, int action)
{
	Applet *app = g_app;
	MenuSystem *ms = app->menuSystem;

	ms->backMenu = 0;
	switch (item) {
	case 3: /* save */
		Canvas_saveState(app->canvas, 3, TEXT_GROUP_MENU, 0x11e);
		return 0;
	case 4: /* load */
		return App_hasSaveData(app) == 0 ? 0x73 : 0x72;
	case 5: /* automap */
		Canvas_setState(app->canvas, 5);
		return action;
	case 10:
	case 11:
		ms->backMenu = 100;
		return action;
	default:
		return action;
	}
}

/* Back / exit rows of the debug menus: action 3 (or 0 on the title map) goes
 * back to the main menu. */
static int selectDebugBack(int item)
{
	Applet *app = g_app;
	int action = app->menuSystem->items[item].action;

	if (action == 0) {
		if (app->canvas->mapId != 0)
			return 0;
	} else if (action != 3) {
		return action;
	}
	Canvas_backToMain(app->canvas, 0);
	return 3;
}

/* Debug cheats (0xca) */
static int selectCheats(Menu *menus, int item)
{
	Applet *app = g_app;
	Player *pl = app->player;
	int i, msg;

	Menus_saveIndexes(menus, 0xc);
	switch (item) {
	case 0:
	case 1:
		return selectDebugBack(item);
	case 2:
		pl->noclip = toggle(pl->noclip);
		break;
	case 3:
		app->game->disableAI = toggle(app->game->disableAI);
		break;
	case 4:
		pl->enableHelp = toggle(pl->enableHelp);
		app->menuSystem->menu = 0;
		Game_saveConfig(app->game);
		return 0xca;
	case 5:
		Player_equipForMap(pl, app->canvas->mapId);
		break;
	case 6:
		Player_addXP(pl, pl->nextLevelXP - pl->currentXP);
		break;
	case 7:
		pl->god = toggle(pl->god);
		break;
	case 8: /* everything */
		pl->weapons = 0x7ff;
		pl->rings = 0x1ff;
		for (i = 0; i < 7; i++)
			Player_give(pl, 3, i, 10, 0);
		for (i = 0; i < 15; i++) {
			if (i == 14)
				Player_give(pl, 0, 14, 1, 0);
			else
				Player_give(pl, 0, i & 0xff, 5, 0);
			app->canvas->numHelpMsgs = 0;
		}
		pl->f260 = 1;
		Player_give(pl, 4, 0, 1000, 0);
		pl->lockLevel = 4;
		msg = 0xb6;
		goto message;
	case 9:
		Game_revealAutomapRect(app->game, 0, 0, 0x20, 0x20);
		return 0xca;
	case 10: /* gold */
		Player_give(pl, 4, 0, 1000, 0);
		msg = 0xb8;
		goto message;
	case 11: /* ammunition */
		for (i = 0; i < 7; i++)
			Player_give(pl, 3, i, 10, 0);
		msg = 0xb9;
		goto message;
	case 12:
		Player_initNotebookFromMap();
		return 0xca;
	case 13:
		app->canvas->showCodes = toggle(app->canvas->showCodes);
		break;
	case 14:
		app->canvas->showMonsters = toggle(app->canvas->showMonsters);
		break;
	case 15:
		for (i = 0; i < 14; i++)
			Player_addStatusEffect(pl, i, 10, 5);
		Player_updateStats(pl);
		break;
	case 16:
		for (i = 0; i < 14; i++) {
			pl->statusEffects[i].turns = 0;
			pl->statusEffects[i].stacks = 0;
			pl->statusEffects[i].amount = 0;
		}
		pl->numStatusEffects = 0;
		Player_updateStats(pl);
		break;
	case 17:
		Game_activateMonsters();
		break;
	default:
		return 0;
	}
	app->menuSystem->menu = 0; /* force a rebuild to show the new values */
	return 0xca;

message:
	Hud_addMessageFile(app->hud, TEXT_GROUP_MENU, msg);
	return 0xca;
}

/* Developer tools (0xcc) */
static int selectDeveloper(Menu *menus, int item)
{
	Applet *app = g_app;

	switch (item) {
	case 0:
	case 1:
		return selectDebugBack(item);
	case 2: /* reset the dev-vars list position */
		menus->savedIndexes[0x17] = 0;
		menus->savedIndexes[0x16] = 0;
		return 0xcd;
	case 3: /* benchmark spin */
		Canvas_enterState9(app->canvas, 0);
		return 0;
	case 4:
		return 0xcc;
	case 5:
	case 6:
		return 0;
	case 7:
		App_fatalError(app, mod_cstr(MOD_STR_FORCED));
		return 0xcc;
	case 8:
		return 0xcc;
	case 9: /* hang (watchdog test) */
		for (;;)
			MenuSystem_infiniteLoop();
	case 10:
		MenuSystem_infiniteLoop();
		return 0xcc;
	case 11: /* exhaust the heap (the block is never freed) */
		(void)malloc((size_t)App_getFreeMemory(app));
		return 0xcc;
	case 12:
		app->canvas->state = -1;
		return 0xcc;
	case 13:
		return 0xcc;
	case 14:
		Game_setRespawn(app->game);
		return 0xcc;
	default:
		return 0;
	}
}

/* Developer switches (0xcd) */
static int selectDevVars(Menu *menus, int item)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	Render *r = app->render;

	Menus_saveIndexes(menus, 0xb);
	switch (item) {
	case 0:
	case 1:
		return selectDebugBack(item);
	case 2:
		cv->weaponCycleKey = toggle(cv->weaponCycleKey);
		Game_saveConfig(app->game);
		break;
	case 3:
		cv->showLocation = toggle(cv->showLocation);
		break;
	case 4:
		break;
	case 5: {
		int frames = cv->animFrames + 1;
		if (frames > 15)
			frames = 2;
		Canvas_setAnimFrames(cv, frames);
		Game_saveConfig(app->game);
		break;
	}
	case 6:  cv->showFPS = toggle(cv->showFPS); break;
	case 7:  r->dbg18824 = toggle(r->dbg18824); break;
	case 8:  r->dbgForceDrawer = toggle(r->dbgForceDrawer); break;
	case 9:  r->dbgSkipSprites = toggle(r->dbgSkipSprites); break;
	case 10: r->dbgSkipFlats = toggle(r->dbgSkipFlats); break;
	case 11: r->dbgSkipWalls = toggle(r->dbgSkipWalls); break;
	case 12: r->dbgSkipSprites2 = toggle(r->dbgSkipSprites2); break;
	case 13: cv->snapView = toggle(cv->snapView); break;
	case 14: cv->showNodes = toggle(cv->showNodes); break;
	case 15: r->dbgClearLineFlag = toggle(r->dbgClearLineFlag); break;
	case 16: break;
	case 17: cv->showDebugVar = toggle(cv->showDebugVar); break;
	default:
		return 0;
	}
	app->menuSystem->menu = 0;
	return 0xcd;
}

/* Store lists (0x12f..0x132, 0x134): pick an item to make an offer for, or
 * leave the store (ending the script that opened it unless something was bought). */
static int selectStoreList(Menu *menus, int menuId, int item, int action, int slot)
{
	Applet *app = g_app;
	MenuSystem *ms = app->menuSystem;

	Menus_saveIndexes(menus, slot);
	if (item != 0) {
		int type = storeWord(action);
		int index = storeWord(action + 1);
		ms->backMenu = menuId;
		/* healing food at full health */
		if (type == 5 && index == 2 &&
		    CombatEntity_getHealth(&app->player->ce) == CombatEntity_getMaxHealth(&app->player->ce))
			return 0x1fa;
		menus->storeItem = action;
		return 0x135;
	}
	if (menus->f4c == 0 && menus->thread != NULL)
		Game_freeScriptThread(app->game, menus->thread);
	menus->thread = NULL;
	return ms->backMenu;
}

/* 0x135 make offer: accepted when it reaches pricePercent of the price */
static int selectOffer(Menu *menus)
{
	Applet *app = g_app;
	MenuSystem *ms = app->menuSystem;
	Player *pl = app->player;
	int offer, w;

	menus->storeItem2 = app->menu->storeItem;
	offer = ms->items[2].action;
	if (pl->gold < offer)
		return 0x136;

	w = menus->storeItem;
	int type = storeWord(w);
	int index = storeWord(w + 1);
	if (type == 3) {
		if (pl->ammo[index] == 99)
			return 0x139;
	} else if (type == 0 && pl->inventory[index] == 99) {
		return 0x139;
	}

	int price = storeWord(w + 2);
	int quantity = storeWord(w + 3);
	if (offer != 0 && menus->pricePercent * price / 100 <= offer) {
		app->statPurchases[w / 4]++;
		Player_buyItem(menus, storeWord(menus->storeItem), storeWord(menus->storeItem + 1), ms->items[2].action,
			       quantity);
		return 0x137;
	}
	return 0x138;
}

/* 0x1f8 other items: lock picks and armour have no "use"; item 14 opens the
 * travel list */
static int selectOtherItem(Menu *menus, int item, int action)
{
	MenuSystem *ms = g_app->menuSystem;

	Menus_saveIndexes(menus, 3);
	if (item < 2)
		return action;
	if (item == 3 || action >= 0x100)
		return 0x1f8;
	if (action == 0xe) {
		ms->backMenu = 0x1f8;
		return 0xc9;
	}
	menus->selectedInvItem = action;
	ms->backMenu = 0x1f8;
	return 0x1f9;
}

/* 0x1f9 "Use <item>?" -> explain why it could not be used */
static int selectUseItem(Menu *menus, int action)
{
	Applet *app = g_app;

	if (action == 1) {
		/* the original passes two leftover registers as the last arguments (unused) */
		int used = Player_useItem(app->player, menus->selectedInvItem, 0, 0);
		Game_snapAllMonsterSprites(app->game, 1);
		if (app->canvas->state != 2)
			return 0;
		if (used == 0) {
			uint32_t idx = (uint32_t)menus->selectedInvItem;
			if (idx < 2)
				return 0x1fa; /* health already full */
			if (idx == 10)
				return 0x1fd;
			if (idx != 11)
				return 0x1fc;
			return (app->player->weapons & 0x10) == 0 ? 0x1fe : 0x1fb;
		}
	}
	return app->menuSystem->backMenu;
}

/* 0x000384c8 */
int Menus_select(Menu *menus, int menuId, int item)
{
	Applet *app = g_app;
	MenuSystem *ms = app->menuSystem;
	Canvas *cv = app->canvas;
	MenuItem *it = &ms->items[item];
	/* the chosen row's action; rows without text count as action 0 */
	int action = (String_length(&it->text) > 0) ? it->action : 0;

	switch (menuId) {
	case 0:
	case 2:
	case 0xf:
		return 0;

	case 1: /* level statistics: next level, or the end screens */
		if (cv->endGame == 0) {
			Canvas_loadMapEx(cv, menus->mapId, 1);
			return 0;
		}
		return 0x12;

	case 3:
		return selectMainMenu(menus, item);
	case 4:
		return selectHelpMenu(item);
	case 5: case 6: case 7: case 8: case 9: case 10: case 11: case 12:
		return 4;

	case 0xd: /* exit? */
		if (action != 1)
			return 3;
		Applet_shutdown(app);
		return 0xd;

	case 0xe: /* erase saved game? */
		if (action != 1)
			return 3;
		Game_deleteSaveFiles();
		return 0x10;

	case 0x10: /* difficulty */
		if (item == 2)
			app->game->difficulty = 1;
		else if (item == 3)
			app->game->difficulty = 2;
		else if (item == 4)
			app->game->difficulty = 4;
		Game_saveConfig(app->game);
		Menu_startGame(menus, 1);
		return 0;

	case 0x11:
	case 0x66:
		return selectOptions(menus, item);

	case 0x12:
		return 0x13;
	case 0x13:
		Sound_play(app->sndSys, 0x56, 2, 5, -1);
		Canvas_setState(cv, 0xe);
		return 0;

	case 0x14: /* enable sound? */
		if (action == 1) {
			app->sndSys->enabled = 1;
			if (cv->exclusiveFeedback != 0)
				cv->vibrateEnabled = 0;
		}
		return 3;

	case 0x15: /* save game? */
		if (action != 0)
			return 0x16;
		Canvas_saveState(cv, 0x43, TEXT_GROUP_MENU, 0x11e);
		return 0;

	case 0x16: /* continue playing? */
		if (action != 0) {
			Canvas_backToMain(cv, 0);
			return 3;
		}
		cv->viewX = cv->destX;
		cv->viewY = cv->destY;
		cv->viewAngle = cv->destAngle;
		Canvas_finishRotation(cv, 1);
		Canvas_requestState12(cv, 1000, 1);
		Canvas_setState(cv, 0x12);
		return 0;

	case 0x18:
		return 0x17;

	case 100:
		return selectGameMenu(item, action);

	case 0x65: /* status */
		if (item == 0)
			return 100;
		if (item == 1 && cv->tellAFriend != 0)
			return 0x96;
		return 0x65;

	case 0x67:
		return selectGameHelp(item);
	case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6d: case 0x6e: case 0x6f:
		return 0x67;

	case 0x6c: /* quit */
		if (ms->backMenu == 100) {
			if (action == 0) { /* save and quit */
				Canvas_saveState(cv, 7, TEXT_GROUP_MENU, 0x11e);
				return 0;
			}
			if (action == 2)
				return 100;
		} else if (action != 1) {
			return ms->backMenu;
		}
		Canvas_backToMain(cv, 0);
		return 3;

	case 0x70:
		return 100;
	case 0x71:
		return 0;

	case 0x72: /* load game? */
	case 0x75: /* restart level? */
		if (action != 1)
			return 100;
		Canvas_startLoading(cv, menuId == 0x72 ? 1 : 2, TEXT_GROUP_MENU, 0x11c);
		Hud_addMessageFile(app->hud, TEXT_GROUP_MENU, 0x11d);
		return 0;

	case 0x73:
		return ms->backMenu;

	case 0x74: /* died: the action is used even for rows without text */
		switch (it->action) {
		case 0:
			if (App_hasSaveData(app) == 0)
				return 0x73;
			Canvas_startLoading(cv, 1, TEXT_GROUP_MENU, 0x11c);
			return 0;
		case 1:
			Canvas_startLoading(cv, 2, TEXT_GROUP_MENU, 0x120);
			return 0;
		case 2:
			return 0x6c;
		default:
			return 0;
		}

	case 0x76: /* save and quit? */
		if (action != 1)
			return 100;
		Canvas_saveState(cv, 0x23, TEXT_GROUP_MENU, 0x11e);
		return 0x76;

	case 0x96: /* Message a Friend */
		if (action == 0x98) {
			menus->contactPage = 1;
			Menu_loadContacts(menus);
			return 0x98;
		}
		return action;

	case 0x97: { /* typed phone number */
		String number;
		String_trim(&number, &app->menuSystem->items[item].text2);
		int next = Menu_sendTellAFriend(menus, &number);
		String_dtor(&number, 0);
		return next;
	}

	case 0x98: /* address book: previous / next page, or send to a contact */
		if (item == 0) {
			if (menus->contactPage == 1)
				return ms->backMenu;
			menus->contactPage--;
			MenuSystem_setMenu(ms, 0x98);
		} else if (item == 1) {
			menus->contactPage++;
			MenuSystem_setMenu(ms, 0x98);
			if (menus->contactPage < menus->numContactPages)
				ms->selectedIndex = 1;
		} else {
			/* contacts are {name, number} String pairs; list rows start at item 3 */
			return Menu_sendTellAFriend(menus, &menus->contacts[(menus->contactFirst + item - 3) * 2 + 1]);
		}
		return 0x98;

	case 0x99: /* sent; send another? */
		if (action == 1)
			return 0x96;
		return ms->backMenu;
	case 0x9a:
		return ms->backMenu;

	case 200: { /* debug */
		int next = app->menuSystem->items[item].action;
		if ((next == 0 && cv->mapId == 0) || next == 3) {
			next = 3;
			Canvas_backToMain(cv, 0);
		}
		ms->backMenu = 200;
		return next;
	}

	case 0xc9: /* change map */
		if ((unsigned)(item - 2) < 14) {
			app->game->automapFlag = 0;
			Canvas_loadMap(cv, item - 1);
			return 0;
		}
		if (item == 0x10) { /* restart this level */
			app->game->automapFlag = 0;
			app->game->skipLevel = 1;
			Canvas_loadMap(cv, cv->mapId);
			return 0;
		}
		return selectDebugBack(item);

	case 0xca:
		return selectCheats(menus, item);
	case 0xcb:
		return 200;
	case 0xcc:
		return selectDeveloper(menus, item);
	case 0xcd:
		return selectDevVars(menus, item);

	case 0x12f: return selectStoreList(menus, menuId, item, action, 6);
	case 0x130: return selectStoreList(menus, menuId, item, action, 7);
	case 0x131: return selectStoreList(menus, menuId, item, action, 8);
	case 0x132: return selectStoreList(menus, menuId, item, action, 9);
	case 0x134: return selectStoreList(menus, menuId, item, action, 10);

	case 0x133: /* store hub: rows 2..5 open 0x12f..0x132 */
		if (item != 0)
			return item + 0x12d;
		if (menus->f4c == 0 && menus->thread != NULL)
			Game_freeScriptThread(app->game, menus->thread);
		menus->thread = NULL;
		return ms->backMenu;

	case 0x135:
		return selectOffer(menus);
	case 0x136:
	case 0x138:
		return 0x135;
	case 0x137: /* bought: keep the store's script alive */
		menus->f4c = 1;
		return ms->backMenu;
	case 0x139:
	case 400:
		return ms->backMenu;

	case 500: /* inventory */
		Menus_saveIndexes(menus, 4);
		return action;

	case 0x1f5: /* weapons: wield */
		Menus_saveIndexes(menus, 1);
		if (item > 1) {
			Player_selectWeapon(app->player, action);
			return 0;
		}
		return action;

	case 0x1f6: /* rings: wear */
		Menus_saveIndexes(menus, 0);
		if (item > 1) {
			Player_selectSpell(app->player, action);
			ms->menu = 0;
			return 0x1f6;
		}
		return action;

	case 0x1f7: /* potions */
		Menus_saveIndexes(menus, 2);
		if (item < 2)
			return action;
		menus->selectedInvItem = action;
		ms->backMenu = 0x1f7;
		return 0x1f9;

	case 0x1f8:
		return selectOtherItem(menus, item, action);

	case 0x1f9:
		return selectUseItem(menus, action);
	case 0x1fa: case 0x1fb: case 0x1fc: case 0x1fd: case 0x1fe:
		return ms->backMenu;

	default:
		return 0;
	}
}
