/*
 * Menu_initMenu (0x1f4b4): builds the item list of every menu id.
 *
 * Menu ids: 1 level done, 3 main menu, 4 help/about, 5..0xc help pages, 0xd..0xf
 * quit/erase prompts, 0x10 difficulty, 0x11 options, 0x12/0x13 game completed /
 * ranking, 0x14 sound prompt, 0x15/0x16 save prompts, 0x17/0x18 More Games,
 * 100 in-game menu, 0x65 status, 0x66 options, 0x67..0x6f game help, 0x6c quit,
 * 0x70 quest log, 0x72..0x76 load/restart/save prompts, 0x96..0x9a Message a
 * Friend, 200..0xcd debug, 0x12f..0x139 stores and barter, 400 item details,
 * 500..0x1fe inventory.
 *
 * Store entries come from the store table in the .mod. An item's action is the
 * entry's word index (entry * 4), which Menus_select uses to read the entry.
 */

#include "game.h"
#include "oe_vm.h"

#define MOD_STORE_TABLE 0x464a0u /* 37 x {u32 itemType, u32 itemIndex, u32 price, u32 quantity} */
#define MOD_STR_CHECK   0x46744u /* "\x87": check mark (equipped) */
#define MOD_STR_DOT     0x46748u /* "\x80": dot glyph */
#define MOD_STR_BAD_MENU 0x4674cu /* fatal error text for an unknown menu id */

enum { TEXT_GROUP_MENU = 3 };
#define ITEM_NO_ICON 0x7f
#define TEXT_ON  0x84
#define TEXT_OFF 0x85

/* Word w of the store table (w = entry*4 + field). */
static int32_t storeWord(int w)
{
	return ModData_s32(&g_mod, MOD_STORE_TABLE + (uint32_t)w * 4);
}

static char *mod_cstr(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

static int onOff(int v)
{
	return v ? TEXT_ON : TEXT_OFF;
}

/* ---- item helpers (each builds the temporaries the original built) ---- */

static void addText(Menu *m, String *text, int flags)
{
	String empty;

	String_ctor(&empty);
	MenuSystem_addItem(m, text, &empty, flags);
	String_dtor(&empty, 0);
}

static void addId(Menu *m, int textId, int flags)
{
	String empty;

	String_ctor(&empty);
	MenuSystem_addItemId(m, textId, &empty, flags);
	String_dtor(&empty, 0);
}

static void addBlank(Menu *m, int flags)
{
	String a, b;

	String_ctor(&a);
	String_ctor(&b);
	MenuSystem_addItem(m, &a, &b, flags);
	String_dtor(&b, 0);
	String_dtor(&a, 0);
}

static void addIdHelp(Menu *m, int textId, int flags, int action, int param, int helpId)
{
	String empty;

	String_ctor(&empty);
	MenuSystem_addItemIdHelpId(m, textId, &empty, flags, action, param, helpId);
	String_dtor(&empty, 0);
}

/* Item with an action and no help text (yes/no style choices). */
static void addIdAction(Menu *m, int textId, int flags, int action)
{
	String text2, help;

	String_ctor(&text2);
	String_ctor(&help);
	MenuSystem_addItemIdFull(m, textId, &text2, flags, action, 0, &help);
	String_dtor(&help, 0);
	String_dtor(&text2, 0);
}

/* Debug menu row with an empty value column. */
static void addDebug(Menu *m, int textId, int action)
{
	String empty;

	String_ctor(&empty);
	MenuSystem_addItemIdStr2(m, textId, &empty, 0, action);
	String_dtor(&empty, 0);
}

static void addTitleId(Menu *m, int textId)
{
	addText(m, (String *)Canvas_buildDottedTitleId3(g_app->menuSystem, textId), 0x49);
}

static void addTitleStr(Menu *m, String *s)
{
	addText(m, (String *)Canvas_buildDottedTitle(g_app->menuSystem, s), 0x49);
}

/* A dotted line without text. */
static void addRule(Menu *m)
{
	String empty;

	String_ctor(&empty);
	addTitleStr(m, &empty);
	String_dtor(&empty, 0);
}

/* "label price" row of a store; flags 0x20 = has details. */
static void addStoreItem(Menu *m, String *name, EntityDef *def, int w)
{
	String_setLength(&m->scratch0, 0);
	String_appendInt(&m->scratch0, storeWord(w + 2));
	MenuSystem_addItemFull(m, name, &m->scratch0, 0x20, w, def->subId, &def->description);
}

static EntityDef *storeDef(int w)
{
	return EntityDefs_find(g_app->entityDefs, 6, storeWord(w) & 0xff, storeWord(w + 1));
}

/* Store header: "Exit" (or "Back" to the store hub) and a blank line. */
static void storeHeader(Menu *menu, MenuSystem *ms)
{
	ms->type = 1;
	if (menu->fromStore == 0) {
		ms->backMenu = 0;
		addIdHelp(menu, 0x106, 0, -1, 0x2a, 0x107);
	} else {
		ms->backMenu = 0x133;
		addIdHelp(menu, 0x4b, 0, -1, 0x2a, 0x4d);
	}
	String a, b;
	String_ctorCStr(&a, "");
	String_ctorCStr(&b, "");
	MenuSystem_addItem(menu, &a, &b, 0);
	String_dtor(&b, 0);
	String_dtor(&a, 0);
}

/* Inventory sub-menu header: Back, Exit. */
static void inventoryHeader(Menu *menu, MenuSystem *ms)
{
	ms->backMenu = 500;
	ms->type = 1;
	addIdHelp(menu, 0x4b, 0, 500, 0x2a, 0x4d);
	addIdHelp(menu, 0x62, 0, 0, 0x2a, 99);
}

/* Result screen of a store action: two text lines, blank, "OK". */
static void addResultScreen(Menu *menu, int line1, int line2)
{
	if (line1 >= 0)
		addId(menu, line1, 9);
	addId(menu, line2, 9);
	addBlank(menu, 9);
	addId(menu, 0x103, 8);
}

/* ---- store screens ---- */

/* 0x12f armoury: weapons not yet owned, ammunition, other gear */
static void buildArmory(Menu *menu, MenuSystem *ms)
{
	Applet *app = g_app;
	Player *pl = app->player;
	String name;
	int w, count = 0;

	storeHeader(menu, ms);
	addTitleId(menu, 0xed);
	for (w = 0; w < 5; w += 4) {
		int type = storeWord(w);
		int index = storeWord(w + 1);
		if ((type & 0xff) == 1) {
			/* weapons: skip owned or bought ones (and the ones superseded by bits 6/7) */
			if (index == 1 && (pl->weapons & 0x80))
				continue;
			if (index == 5 && (pl->weapons & 0x40))
				continue;
			uint32_t bit = arm_bit(index);
			if ((pl->weapons & bit) || (pl->boughtWeapons & bit))
				continue;
		}
		count++;
		EntityDef *def = EntityDefs_find(app->entityDefs, 6, type & 0xff, index);
		addStoreItem(menu, &def->name, def, w);
	}
	if (count == 0)
		addId(menu, 0x10a, 9);

	String a, b;
	String_ctorCStr(&a, "");
	String_ctorCStr(&b, "");
	MenuSystem_addItem(menu, &a, &b, 0);
	String_dtor(&b, 0);
	String_dtor(&a, 0);
	addTitleId(menu, 0xf9);

	String_ctor(&name);
	for (w = 8; w < 0x19; w += 4) {
		int type = storeWord(w);
		int index = storeWord(w + 1);
		int defSub, defIndex;
		/* ammunition entries 3..5 are shown with the weapon's entity */
		if (type == 3 && index == 3) {
			defSub = 1;
			defIndex = 10;
		} else if (type == 3 && index == 4) {
			defSub = 1;
			defIndex = 9;
		} else if (type == 3 && index == 5) {
			defSub = 1;
			defIndex = 8;
		} else {
			defSub = type & 0xff;
			defIndex = index;
		}
		EntityDef *def = EntityDefs_find(app->entityDefs, 6, defSub, defIndex);
		String_setLength(&name, 0);
		int qty = storeWord(w + 3);
		if (qty < 2) {
			String_appendString(&name, &def->name);
		} else {
			Canvas_resetEvents(app->canvas);
			Canvas_addTextArgStr(app->canvas, &def->name);
			Canvas_addTextArgInt(app->canvas, qty);
			Text_getString(app->canvas, TEXT_GROUP_MENU, 0x10b, &name);
		}
		addStoreItem(menu, &name, def, w);
	}
	String_dtor(&name, 0);

	String_ctorCStr(&a, "");
	String_ctorCStr(&b, "");
	MenuSystem_addItem(menu, &a, &b, 0);
	String_dtor(&b, 0);
	String_dtor(&a, 0);
	addTitleId(menu, 0xec);
	for (w = 0x1c; w < 0x21; w += 4) {
		EntityDef *def = storeDef(w);
		/* armour upgrades only above the current level */
		if (storeWord(w) != 7 || pl->armorLevel < storeWord(w + 1))
			addStoreItem(menu, &def->name, def, w);
	}
	Menu_loadIndexes(menu, 6);
}

/* 0x130 food: two of the entries depend on whether this is map 2 */
static void buildFood(Menu *menu, MenuSystem *ms)
{
	int isMap2 = g_app->canvas->mapId == 2;

	storeHeader(menu, ms);
	addTitleId(menu, 0x10c);
	for (int w = 0x68; w < 0x75; w += 4) {
		EntityDef *def = storeDef(w);
		if (def->eSubType == 0) {
			if (def->parm == 0xd && !isMap2)
				continue;
			if (def->parm == 0xc && isMap2)
				continue;
		}
		addStoreItem(menu, &def->name, def, w);
	}
	Menu_loadIndexes(menu, 7);
}

/* 0x131 rings not owned yet */
static void buildRingStore(Menu *menu, MenuSystem *ms)
{
	Player *pl = g_app->player;
	int count = 0;

	storeHeader(menu, ms);
	addTitleId(menu, 0xf3);
	for (int w = 0x24; w < 0x3d; w += 4) {
		EntityDef *def = storeDef(w);
		if ((pl->rings & arm_bit(storeWord(w + 1))) == 0) {
			count++;
			addStoreItem(menu, &def->name, def, w);
		}
	}
	if (count == 0)
		addId(menu, 0x10a, 9);
	Menu_loadIndexes(menu, 8);
}

/* 0x132 potions */
static void buildPotionStore(Menu *menu, MenuSystem *ms)
{
	storeHeader(menu, ms);
	addTitleId(menu, 0xf0);
	for (int w = 0x40; w < 0x65; w += 4) {
		EntityDef *def = storeDef(w);
		addStoreItem(menu, &def->name, def, w);
	}
	Menu_loadIndexes(menu, 9);
}

/* 0x133 store hub */
static void buildStoreHub(Menu *menu, MenuSystem *ms)
{
	ms->backMenu = 0;
	ms->type = 1;
	menu->fromStore = 1;
	addIdHelp(menu, 0x106, 0, -1, 0x2a, 0x107);
	String a, b;
	String_ctorCStr(&a, "");
	String_ctorCStr(&b, "");
	MenuSystem_addItem(menu, &a, &b, 0);
	String_dtor(&b, 0);
	String_dtor(&a, 0);
	MenuSystem_addItemIdIdFull(menu, 0x10e, 299, 0, -1, 1, 299);
	MenuSystem_addItemIdIdFull(menu, 0x10f, 299, 0, -1, 0x7f, 299);
	MenuSystem_addItemIdIdFull(menu, 0xf4, 299, 0, -1, 0xc, 299);
	MenuSystem_addItemIdIdFull(menu, 0xf1, 299, 0, -1, 0x17, 299);
}

/* 0x134 trinkets (barter goods) */
static void buildTrinkets(Menu *menu, MenuSystem *ms)
{
	ms->backMenu = 0;
	ms->type = 1;
	addIdHelp(menu, 0x106, 0, -1, 0x2a, 0x107);
	String a, b;
	String_ctorCStr(&a, "");
	String_ctorCStr(&b, "");
	MenuSystem_addItem(menu, &a, &b, 0);
	String_dtor(&b, 0);
	String_dtor(&a, 0);
	addTitleId(menu, 0x110);
	for (int w = 0x78; w < 0x91; w += 4) {
		EntityDef *def = storeDef(w);
		addStoreItem(menu, &def->name, def, w);
	}
	Menu_loadIndexes(menu, 10);
}

/* 0x135 make an offer: asking price and an editable offer (5 digits) */
static void buildOffer(Menu *menu, MenuSystem *ms)
{
	int w = menu->storeItem;

	ms->type = 1;
	ms->maxDigits = 5;
	ms->digitEntry = 1;
	menu->offerActive = 1;
	EntityDef *def = storeDef(w);
	addTitleStr(menu, &def->name);
	MenuSystem_addItemIdIntHelpId(menu, 0x113, storeWord(w + 2), 1, 0, 0, 299);
	/* returning to the same item keeps the last offer */
	int offer = (menu->storeItem != menu->storeItem2) ? storeWord(w + 2) : ms->offerValue;
	MenuSystem_addItemIdInt(menu, 0x114, offer, 0x10, offer, def->subId, &def->description);

	String a, b;
	String_ctorCStr(&a, "");
	String_ctor(&b);
	MenuSystem_addItem(menu, &a, &b, 9);
	String_dtor(&a, 0);
	String_dtor(&b, 0);
	addRule(menu);

	String_setLength(&menu->scratch1, 0);
	String_setLength(&menu->scratch2, 0);
	String_appendString(&menu->scratch2, &def->description);
	MenuSystem_addItemIdFull(menu, 0x115, &menu->scratch1, 8, 0, def->subId, &menu->scratch2);
	MenuSystem_addItemIdFull(menu, 0x116, &menu->scratch1, 8, 0, def->subId, &menu->scratch2);
}

/* ---- inventory ---- */

/* 500 inventory hub */
static void buildInventory(Menu *menu, MenuSystem *ms)
{
	ms->type = 1;
	ms->backMenu = 100;
	addIdHelp(menu, 0x4b, 0, 100, 0x2a, 0x4d);
	addIdHelp(menu, 0x62, 0, 0, 0x2a, 99);
	addTitleId(menu, 100);
	addIdHelp(menu, 0xee, 0, 0x1f5, 0x2a, 0xef);
	addIdHelp(menu, 0xf1, 0, 0x1f7, 0x2a, 0xf2);
	addIdHelp(menu, 0xf4, 0, 0x1f6, 0x2a, 0xf5);
	addIdHelp(menu, 0xf7, 0, 0x1f8, 0x2a, 0xf8);
	Menu_loadIndexes(menu, 4);
}

static void wrapInDots(String *s)
{
	String dot;

	for (int i = 0; i < 2; i++) {
		String_ctorCStr(&dot, mod_cstr(MOD_STR_DOT));
		String_insert(s, 0, &dot);
		String_dtor(&dot, 0);
	}
	String_appendCStr_76f0(s, mod_cstr(MOD_STR_DOT));
	String_appendCStr_76f0(s, mod_cstr(MOD_STR_DOT));
}

/* 0x1f5 weapons with ammunition counts; the wielded one is checked */
static void buildWeapons(Menu *menu, MenuSystem *ms)
{
	Applet *app = g_app;
	Player *pl = app->player;

	inventoryHeader(menu, ms);
	String_setLength(&menu->scratch0, 0);
	String_setLength(&menu->scratch1, 0);
	Text_getString(app->canvas, TEXT_GROUP_MENU, 0x37, &menu->scratch0);
	Text_getString(app->canvas, TEXT_GROUP_MENU, 0xf9, &menu->scratch1);
	TextUtil_dehyphenate(app->textUtil, &menu->scratch0);
	TextUtil_dehyphenate(app->textUtil, &menu->scratch1);
	wrapInDots(&menu->scratch0);
	wrapInDots(&menu->scratch1);
	MenuSystem_addItem(menu, &menu->scratch0, &menu->scratch1, 0x41);

	for (int w = 0; w < 11; w++) {
		if ((pl->weapons & arm_bit(w)) == 0)
			continue;
		String name, ammo;
		String_ctor(&name);
		EntityDef *def = EntityDefs_find(app->entityDefs, 6, 1, w & 0xff);
		if (pl->ce.weapon == w) {
			String_appendCStr_76f0(&name, mod_cstr(MOD_STR_CHECK));
			String_appendCStr_76f0(&name, " ");
		}
		String_appendString(&name, &def->name);
		String_ctor(&ammo);
		int ammoType = app->combat->weapons[w].ammoType;
		if (ammoType == 0) {
			String_appendCStr_76f0(&ammo, mod_cstr(MOD_STR_DOT));
			String_appendCStr_76f0(&ammo, mod_cstr(MOD_STR_DOT));
		} else {
			String_appendInt(&ammo, pl->ammo[ammoType]);
		}
		MenuSystem_addItemFull(menu, &name, &ammo, 0x20, w, def->subId, &def->description);
		String_dtor(&ammo, 0);
		String_dtor(&name, 0);
	}
	Menu_loadIndexes(menu, 1);
}

/* 0x1f6 rings: "no ring" plus every owned ring; the worn one is checked */
static void buildRings(Menu *menu, MenuSystem *ms)
{
	Applet *app = g_app;
	Player *pl = app->player;
	String line;

	inventoryHeader(menu, ms);
	addTitleId(menu, 0xf3);
	String_setLength(&menu->scratch0, 0);
	String_ctor(&line);
	for (int r = 0; r < 9; r++) {
		String_setLength(&line, 0);
		if (r == 0) {
			if (pl->equippedRing == 0) {
				Text_getString(app->canvas, TEXT_GROUP_MENU, 0xfa, &menu->scratch0);
				TextUtil_dehyphenate(app->textUtil, &menu->scratch0);
				String_appendCStr_76f0(&line, mod_cstr(MOD_STR_CHECK));
				String_appendString(&line, &menu->scratch0);
			} else {
				Text_getString(app->canvas, TEXT_GROUP_MENU, 0xfb, &line);
				TextUtil_dehyphenate(app->textUtil, &line);
			}
			String empty;
			String_ctor(&empty);
			MenuSystem_addItemStrHelpId(menu, &line, &empty, 0, 0, ITEM_NO_ICON, 0xfc);
			String_dtor(&empty, 0);
		} else if (pl->rings & arm_bit(r)) {
			EntityDef *def = EntityDefs_find(app->entityDefs, 6, 2, r);
			if (pl->equippedRing == r) {
				String_appendCStr_76f0(&line, mod_cstr(MOD_STR_CHECK));
				String_appendCStr_76f0(&line, " ");
			}
			String_appendString(&line, &def->name);
			String empty;
			String_ctor(&empty);
			MenuSystem_addItemFull(menu, &line, &empty, 0x20, r, def->subId, &def->description);
			String_dtor(&empty, 0);
		}
	}
	String_dtor(&line, 0);
	Menu_loadIndexes(menu, 0);
}

/* inventory slots [from, to) with their counts */
static void addInventoryRange(Menu *menu, int from, int to)
{
	Applet *app = g_app;

	for (int i = from; i < to; i++) {
		int count = app->player->inventory[i];
		if (count == 0)
			continue;
		EntityDef *def = EntityDefs_find(app->entityDefs, 6, 0, i);
		String_setLength(&menu->scratch0, 0);
		String_appendInt(&menu->scratch0, count);
		MenuSystem_addItemFull(menu, &def->name, &menu->scratch0, 0x20, i, def->subId, &def->description);
	}
}

/* 0x1f7 potions */
static void buildPotions(Menu *menu, MenuSystem *ms)
{
	inventoryHeader(menu, ms);
	addTitleId(menu, 0xf0);
	addInventoryRange(menu, 0, 10);
	Menu_loadIndexes(menu, 2);
}

/* 0x1f8 other: lock-pick level, armour, misc items */
static void buildOther(Menu *menu, MenuSystem *ms)
{
	Applet *app = g_app;
	Player *pl = app->player;

	inventoryHeader(menu, ms);
	addTitleId(menu, 0xf6);

	String_setLength(&menu->scratch0, 0);
	Text_getString(app->canvas, TEXT_GROUP_MENU, 0xfd, &menu->scratch0);
	TextUtil_dehyphenate(app->textUtil, &menu->scratch0);
	EntityDef *def = EntityDefs_find(app->entityDefs, 6, 9, pl->lockLevel);
	String_appendInt(&menu->scratch0, pl->lockLevel + 1);
	MenuSystem_addItemFull(menu, &def->name, &menu->scratch0, 0x20, pl->lockLevel, def->subId, &def->description);

	def = EntityDefs_find(app->entityDefs, 6, 7, pl->ce.armorType);
	String_setLength(&menu->scratch0, 0);
	Text_getString(app->canvas, TEXT_GROUP_MENU, 0x139, &menu->scratch0);
	TextUtil_dehyphenate(app->textUtil, &menu->scratch0);
	MenuSystem_addItemFull(menu, &menu->scratch0, &def->name, 0x20, 0x1f8, def->subId, &def->description);

	addInventoryRange(menu, 10, 15);
	Menu_loadIndexes(menu, 3);
}

/* 400 item details: long name (centred), description, then weapon/armour stats */
static void buildDetails(Menu *menu, MenuSystem *ms)
{
	Applet *app = g_app;
	EntityDef *def;
	String *desc = NULL;

	ms->type = 7;
	MenuSystem_setItemsFromText(ms, 0, &menu->detailsText2);
	for (int i = 0; i < ms->numItems; i++)
		ms->items[i].flags |= 8;
	addRule(menu);
	MenuSystem_setItemsFromText(ms, ms->numItems, &menu->detailsText);

	def = menu->detailsDef;
	if (def->eType != 6)
		return;
	String text;
	String_ctor(&text);
	if (def->eSubType == 1) {
		addRule(menu);
		desc = Combat_getWeaponDescription(app->combat, menu->detailsDef->parm);
	} else if (def->eSubType == 7) {
		addRule(menu);
		desc = Combat_getArmorDescription(app->combat, menu->detailsDef->parm);
	} else if (def->eSubType == 0 && def->parm == 10) {
		addRule(menu);
		desc = Combat_getArmorDescription(app->combat, -1);
	}
	if (desc != NULL) {
		String_assign(&text, desc);
		MenuSystem_setItemsFromText(ms, ms->numItems, &text);
	}
	String_dtor(&text, 0);
}

/* ---- debug menus ---- */

/* 200 debug root */
static void buildDebug(Menu *menu, MenuSystem *ms)
{
	ms->type = 1;
	ms->backMenu = (g_app->canvas->mapId == 0) ? 3 : 100;
	addDebug(menu, 0x62, 0);
	addDebug(menu, 0x4b, ms->backMenu);
	addDebug(menu, 0x93, 0xca);
	addDebug(menu, 0x94, 0xc9);
	addDebug(menu, 0x95, 0xcb);
	addDebug(menu, 0x96, 0xcc);
}

/* 0xc9 change map */
static void buildChangeMap(Menu *menu, MenuSystem *ms)
{
	ms->type = 1;
	addDebug(menu, 0x62, 0);
	addDebug(menu, 0x4b, ms->backMenu);
	for (int id = 0xc0; id <= 0xc6; id++)
		addId(menu, id, 0);
	if (ms->backMenu == 0x1f8)
		return; /* opened from the travel item in the inventory */
	addId(menu, 0xcc, 0);
	addId(menu, 0xca, 0);
	addId(menu, 0x65, 0);
	addId(menu, 0xcb, 0);
	addId(menu, 0xc9, 0);
	addId(menu, 200, 0);
	addId(menu, 199, 0);
	if (g_app->canvas->mapId != 0)
		addId(menu, 0xcd, 0);
}

/* 0xca cheats */
static void buildCheats(Menu *menu, MenuSystem *ms)
{
	Applet *app = g_app;

	ms->type = 1;
	addDebug(menu, 0x62, 0);
	addDebug(menu, 0x4b, ms->backMenu);
	MenuSystem_addItemIdId(menu, 0xb1, onOff(app->player->noclip), 0);
	MenuSystem_addItemIdId(menu, 0xb2, onOff(app->game->disableAI), 0);
	MenuSystem_addItemIdId(menu, 0x47, onOff(app->player->enableHelp), 0);
	addId(menu, 0xb3, 0);
	MenuSystem_addItemIdIntHelpId(menu, 0xb4, app->player->level, 0, 0, 0, 299);
	MenuSystem_addItemIdId(menu, 0xb5, onOff(app->player->god), 0);
	addId(menu, 0xb6, 0);
	addId(menu, 0xb7, 0);
	addId(menu, 0xb8, 0);
	addId(menu, 0xb9, 0);
	addId(menu, 0xba, 0);
	MenuSystem_addItemIdId(menu, 0xbc, onOff(app->canvas->showMonsters), 0);
	addId(menu, 0xbd, 0);
	addId(menu, 0xbe, 0);
	addId(menu, 0xbf, 0);
	Menu_loadIndexes(menu, 0xc);
}

/* kilobytes, rounded up */
static int kb(int bytes)
{
	return (bytes + 0x3ff) / 1024;
}

static void addMemRow(Menu *menu, String *tmp, int textId, int value)
{
	String_setLength(tmp, 0);
	String_appendInt(tmp, value);
	MenuSystem_addItemIdStr2(menu, textId, tmp, 0, 0);
}

/* 0xcb memory statistics */
static void buildMemoryStats(Menu *menu, MenuSystem *ms)
{
	Applet *app = g_app;
	Render *r = app->render;
	String tmp;

	ms->type = 1;
	String_ctor(&tmp);
	addMemRow(menu, &tmp, 0x138, kb(app->initMem));
	addMemRow(menu, &tmp, 0x137, (int)(((uint32_t)(App_getUsedMemory(app) - app->initMem) + 0x3ffu) >> 10));
	addMemRow(menu, &tmp, 0x135, kb(App_getFreeMemory(app)));
	addMemRow(menu, &tmp, 0x136, kb(Obj_getField38(app)));
	addMemRow(menu, &tmp, 0xce, kb(r->memStats[0]));
	addMemRow(menu, &tmp, 0xcf, kb(r->memStats[0x16]));
	addMemRow(menu, &tmp, 0xd0, kb(r->memStats[0x18]));
	addMemRow(menu, &tmp, 0xd1, 700);
	addMemRow(menu, &tmp, 0xd2, kb(r->memStats[8]));
	addMemRow(menu, &tmp, 0xd3, 0xb4);
	addMemRow(menu, &tmp, 0xd7, kb(r->memStats[0xc]));
	addMemRow(menu, &tmp, 0xd8, kb(r->memStats[10]));
	addMemRow(menu, &tmp, 0xd9, kb(r->memStats[0xe]));
	addMemRow(menu, &tmp, 0xda, kb(r->memStats[4]));
	addMemRow(menu, &tmp, 0xdb, kb(app->game->entityMem[0]));
	addMemRow(menu, &tmp, 0xd4, kb(r->memStats[0x12]));
	addMemRow(menu, &tmp, 0xd5, kb(app->imageMem));
	addMemRow(menu, &tmp, 0xd6, kb(r->memStats[2]));
	addMemRow(menu, &tmp, 0xdc, app->canvas->viewRect[2]);
	addMemRow(menu, &tmp, 0xdd, app->canvas->viewRect[3]);
	String_dtor(&tmp, 0);
}

/* 0xcc developer tools */
static void buildDeveloper(Menu *menu, MenuSystem *ms)
{
	static const int16_t rows[] = { 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x133, 0x134, 0x9f, 0xa0, 0xa1 };

	ms->type = 1;
	addDebug(menu, 0x62, 0);
	addDebug(menu, 0x4b, ms->backMenu);
	for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
		addId(menu, rows[i], 0);
}

/* 0xcd developer switches */
static void buildDevVars(Menu *menu, MenuSystem *ms)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	Render *r = app->render;

	ms->type = 1;
	ms->backMenu = 0xcc;
	addDebug(menu, 0x62, 0);
	addDebug(menu, 0x4b, ms->backMenu);
	MenuSystem_addItemIdId(menu, 0x83, onOff(cv->weaponCycleKey), 0);
	MenuSystem_addItemIdId(menu, 0xa2, onOff(cv->showLocation), 0);
	MenuSystem_addItemIdId(menu, 0xa3, TEXT_OFF, 0);
	MenuSystem_addItemIdIntHelpId(menu, 0xa4, cv->animFrames, 0, 0, 0, 299);
	MenuSystem_addItemIdId(menu, 0xa5, onOff(cv->showFPS), 0);
	MenuSystem_addItemIdId(menu, 0xa6, onOff(r->dbg18824), 0);
	MenuSystem_addItemIdId(menu, 0xa7, onOff(r->dbgForceDrawer), 0);
	MenuSystem_addItemIdId(menu, 0xa8, onOff(r->dbgSkipSprites), 0);
	MenuSystem_addItemIdId(menu, 0xa9, onOff(r->dbgSkipFlats), 0);
	MenuSystem_addItemIdId(menu, 0xaa, onOff(r->dbgSkipWalls), 0);
	MenuSystem_addItemIdId(menu, 0xab, onOff(r->dbgSkipSprites2), 0);
	MenuSystem_addItemIdId(menu, 0xac, onOff(cv->snapView), 0);
	MenuSystem_addItemIdId(menu, 0xad, onOff(cv->showNodes), 0);
	MenuSystem_addItemIdId(menu, 0xae, onOff(r->dbgClearLineFlag), 0);
	MenuSystem_addItemIdId(menu, 0xaf, TEXT_OFF, 0);
	MenuSystem_addItemIdId(menu, 0xb0, onOff(cv->showDebugVar), 0);
	Menu_loadIndexes(menu, 0xb);
}

/* ---- front end and in-game menus ---- */

/* 0x11 / 0x66 options: vibration, sound, volume (or one combined row on
 * phones where sound and vibration exclude each other) */
static void buildOptions(Menu *menu, MenuSystem *ms, int menuId)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;

	ms->maxDigits = 5;
	if (menuId == 0x66) {
		ms->backMenu = 100;
		ms->type = 1;
	} else {
		ms->backMenu = 3;
		ms->type = 8;
	}
	addId(menu, 0x4b, 0);
	Canvas_resetEvents(cv);
	if (cv->exclusiveFeedback == 0) {
		MenuSystem_addItemIdId(menu, 0x89, onOff(cv->vibrateEnabled), 0);
		MenuSystem_addItemIdId(menu, 0x87, onOff(app->sndSys->enabled), 0);
	} else {
		int mode;
		if (app->sndSys->enabled != 0)
			mode = 0x86;
		else if (cv->vibrateEnabled != 0)
			mode = 0x88;
		else
			mode = 0x41;
		String_setLength(&menu->scratch0, 0);
		Text_getString(cv, TEXT_GROUP_MENU, 0x8a, &menu->scratch0);
		String_setLength(&menu->scratch1, 0);
		Text_getString(cv, TEXT_GROUP_MENU, mode, &menu->scratch1);
		TextUtil_dehyphenate(app->textUtil, &menu->scratch1);
		MenuSystem_addItem(menu, &menu->scratch0, &menu->scratch1, 0);
	}
	menu->volume = app->sndSys->volume;
	Menus_setVolumeItem(menu, ms->numItems++);

	/* port: the engine's settings (cgame_common/oe_options.c, handled in selectOptions) */
	for (int i = 0; i < Options_count(); i++) {
		char label[32], value[16];
		String a, b;
		Options_row(i, label, value);
		String_ctor(&a);
		String_ctor(&b);
		String_appendCStr_76f0(&a, label);
		String_appendCStr_76f0(&b, value);
		MenuSystem_addItem(menu, &a, &b, 0);
		String_dtor(&b, 0);
		String_dtor(&a, 0);
	}
}

/* 100 in-game menu; inventory, quest log and map are disabled in familiar form
 * or under status effect 9 */
static void buildGameMenu(Menu *menu, MenuSystem *ms)
{
	Player *pl = g_app->player;

	ms->selectedIndex = 1;
	ms->backMenu = 0;
	ms->type = 1;
	int restricted = (pl->isFamiliar != 0 || pl->statusEffects[9].turns != 0) ? 4 : 0;
	addIdHelp(menu, 0x62, 0, 0, 0x2a, 99);
	addIdHelp(menu, 0x65, restricted, 500, 0x2a, 0x66);
	addIdHelp(menu, 0x67, restricted, 0x70, 0x2a, 0x68);
	addIdHelp(menu, 0x69, 0, 0x71, 0x2a, 0x6a);
	addIdHelp(menu, 0x6b, 0, 0x72, 0x2a, 0x6c);
	addIdHelp(menu, 0x6d, restricted, 0, 0x2a, 0x6e);
	addIdHelp(menu, 0x6f, 0, 0x65, 0x2a, 0x70);
	addIdHelp(menu, 0x47, 0, 0x67, 0x2a, 0x48);
	addIdHelp(menu, 0x45, 0, 0x66, 0x2a, 0x46);
	addIdHelp(menu, 0x71, 0, 0x75, 0x2a, 0x72);
	addIdHelp(menu, 0x73, 0, 0x76, 0x2a, 0x74);
	addIdHelp(menu, 0x75, 0, 0x6c, 0x2a, 0x75);
}

/* 0x67 game help topics */
static void buildGameHelp(Menu *menu, MenuSystem *ms)
{
	ms->type = 1;
	ms->backMenu = 100;
	addIdHelp(menu, 0x4b, 0, 100, 0x2a, 0x4d);
	addTitleId(menu, 0x7d);
	addIdHelp(menu, 0x7b, 0, 0, 0x2a, 0x7c);
	addIdHelp(menu, 0x4e, 0, 0, 0x2a, 0x4f);
	addIdHelp(menu, 0x50, 0, 0, 0x2a, 0x51);
	addIdHelp(menu, 0x52, 0, 0, 0x2a, 0x53);
	addIdHelp(menu, 0x54, 0, 0, 0x2a, 0x55);
	addIdHelp(menu, 0x56, 0, 0, 0x2a, 0x57);
	addIdHelp(menu, 0x58, 0, 0, 0x2a, 0x59);
	addIdHelp(menu, 0x5a, 0, 0, 0x2a, 0x5b);
	addIdHelp(menu, 0x5d, 0, 0, 0x2a, 0x5e);
}

/* Text page from the help string group. The front-end pages drop the background image; the in-game ones
 * (back to 0x67) leave imgBG as it is (0x1f5ac/0x1f648/0x20a8c branch past the store). */
static void helpPage(Menu *menu, MenuSystem *ms, int backMenu, int page)
{
	ms->type = 5;
	if (backMenu != 0x67)
		ms->imgBG = NULL;
	ms->backMenu = backMenu;
	MenuSystem_loadHelpResource(menu, page);
}

/* 1 / 0x12 statistics screens shown after a level / the game */
static void buildLevelDone(Menu *menu, MenuSystem *ms)
{
	Applet *app = g_app;

	addText(menu, &app->render->mapName, 8);
	addId(menu, 0x7e, 8);
	addBlank(menu, 0);
	Menu_buildStatistics(menu, 1, 0, 0);
	addBlank(menu, 0);
	addId(menu, 0x7f, 8);
	addId(menu, 0x42, 8);
	Sound_play(app->sndSys, 0x54, 2, 5, -1);
	app->canvas->numKeys = 0;
	ms->type = 5;
	ms->imgBG = NULL;
	ms->backMenu = -1;
}

static void buildGameDone(Menu *menu, MenuSystem *ms)
{
	addId(menu, 0x80, 8);
	addId(menu, 0x82, 8);
	addBlank(menu, 0);
	Menu_buildStatistics(menu, 0, 0, 1);
	addBlank(menu, 0);
	addId(menu, 0x7f, 8);
	addId(menu, 0x42, 8);
	ms->type = 5;
	ms->imgBG = NULL;
	ms->backMenu = -1;
}

/* 0x96 Message a Friend */
static void buildTellAFriend(Menu *menu, MenuSystem *ms)
{
	ms->imgBG = NULL;
	ms->selectedIndex = 5;
	ms->backMenu = 0x65;
	ms->type = 6;
	addId(menu, 0x140, 9);
	addId(menu, 0x141, 9);
	addId(menu, 299, 9);
	addBlank(menu, 1);
	addIdHelp(menu, 0x142, 8, ms->backMenu, ITEM_NO_ICON, 0x4d);
	addIdHelp(menu, 0x143, 8, 0x98, ITEM_NO_ICON, 0x145);
	addIdHelp(menu, 0x144, 8, 0x97, ITEM_NO_ICON, 0x146);
}

/* 0x97 type a phone number (10 digits) */
static void buildPhoneEntry(Menu *menu, MenuSystem *ms)
{
	ms->imgBG = NULL;
	ms->backMenu = 0x96;
	ms->type = 1;
	ms->selectedIndex = 3;
	ms->digitEntry = 1;
	ms->maxDigits = 10;
	addId(menu, 0x13c, 9);
	addId(menu, 0x13d, 9);
	addBlank(menu, 9);
	MenuSystem_addItemIdIdFull(menu, 0x13e, 299, 0x10, 0, 0, 299);
	addBlank(menu, 9);
	addRule(menu);
	addId(menu, 0x13f, 9);
	addId(menu, 0x116, 9);
}

/* 0x9a message not sent */
static void buildSendFailed(Menu *menu, MenuSystem *ms)
{
	ms->imgBG = NULL;
	ms->backMenu = 0x96;
	ms->type = 6;
	addId(menu, 0x151, 9);
	addId(menu, 0x152, 9);
	addId(menu, 299, 9);
	MenuSystem_loadHelpItems(menu, &menu->scratch2, 9);
	addBlank(menu, 1);
	addId(menu, 0x4b, 8);
}

/* 0x1f9 "Use <item>?" */
static void buildUseItem(Menu *menu, MenuSystem *ms)
{
	Applet *app = g_app;

	Canvas_resetEvents(app->canvas);
	EntityDef *def = EntityDefs_find(app->entityDefs, 6, 0, menu->selectedInvItem);
	Canvas_addTextArgStr(app->canvas, &def->name);
	MenuSystem_setYesNo(menu, 0xfe, 1);
	ms->scrollIndex = 0;
	ms->type = 6;
}

/* 0x0001f4b4 */
void Menu_initMenu(Menu *menu, int menuId)
{
	Applet *app = g_app;
	MenuSystem *ms = app->menuSystem;

	if (menuId > 99)
		String_setLength(&app->hud->str1228, 0);
	ms->scrollIndex = 0;
	ms->numItems = 0;
	ms->selectedIndex = 0;

	switch (menuId) {
	case 1:
		buildLevelDone(menu, ms);
		break;

	case 3: /* main menu */
		ms->type = 4;
		ms->backMenu = -1;
		MenuSystem_loadMainImages();
		ms->imgBG = &ms->imgLogo;
		if (App_hasSaveData(app) == 0)
			addId(menu, 299, 4); /* no "Continue" without a save */
		else
			addId(menu, 0x42, 8);
		addId(menu, 0x43, 8);
		addId(menu, 0x45, 8);
		addId(menu, 0x47, 8);
		addId(menu, 0x44, 8);
		break;

	case 4: { /* help / about */
		static const int16_t rows[] = { 0x4c, 0x4e, 0x50, 0x52, 0x54, 0x56, 0x58, 0x5a, 0x5d, 0x49 };
		ms->type = 4;
		ms->imgBG = &ms->imgLogo;
		ms->backMenu = 3;
		for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++)
			addId(menu, rows[i], 8);
		break;
	}

	case 5:  helpPage(menu, ms, 4, 4); break;
	case 6:  helpPage(menu, ms, 4, 5); break;
	case 7:  helpPage(menu, ms, 4, 6); break;
	case 9:  helpPage(menu, ms, 4, 1); break;
	case 10: helpPage(menu, ms, 4, 2); break;
	case 11: helpPage(menu, ms, 4, 3); break;
	case 12: helpPage(menu, ms, 4, 7); break;

	case 8: /* about */
		ms->type = 5;
		ms->imgBG = NULL;
		ms->backMenu = 3;
		MenuSystem_loadAbout(menu);
		break;

	case 0xd:
	case 0xe:
	case 0xf: /* exit / erase saves / are you sure */
		ms->type = 6;
		MenuSystem_setYesNo(menu, 0x5f + (menuId - 0xd), 0);
		ms->imgBG = NULL;
		ms->backMenu = 3;
		break;

	case 0x10: /* difficulty */
		ms->type = 6;
		addId(menu, 0x12d, 9);
		addBlank(menu, 9);
		addId(menu, 0x12e, 8);
		addId(menu, 0x12f, 8);
		addId(menu, 0x130, 8);
		ms->imgBG = NULL;
		ms->backMenu = 3;
		break;

	case 0x11:
	case 0x66:
		buildOptions(menu, ms, menuId);
		break;

	case 0x12:
		buildGameDone(menu, ms);
		break;

	case 0x13:
		ms->type = 5;
		ms->imgBG = NULL;
		ms->backMenu = -1;
		MenuSystem_fillRanking(menu);
		break;

	case 0x14: /* enable sound? */
		MenuSystem_setYesNo(menu, 0xe5, 1);
		ms->backMenu = -1;
		ms->scrollIndex = 0;
		ms->type = 4;
		ms->imgBG = &ms->imgLogo;
		MenuSystem_loadMainImages();
		break;

	case 0x15: /* save game? */
		ms->type = 6;
		addId(menu, 0x90, 9);
		addBlank(menu, 0);
		addIdAction(menu, 0x121, 8, 0);
		addIdAction(menu, 0x122, 8, 1);
		ms->backMenu = -1;
		break;

	case 0x16: /* continue playing? */
		ms->type = 6;
		addId(menu, 0x12a, 9);
		addBlank(menu, 0);
		addIdAction(menu, 0xea, 8, 0);
		addIdAction(menu, 0xeb, 8, 1);
		break;

	case 0x17: /* More Games */
		ms->backMenu = 3;
		ms->type = 9;
		MenuSystem_loadMoreGames(ms, app->menuSystem->moreGamesPage);
		break;

	case 0x18: /* More Games: page text */
		ms->backMenu = 0x17;
		ms->type = 6;
		ms->imgBG = NULL;
		String_setLength(&menu->scratch0, 0);
		Text_getString(app->canvas, TEXT_GROUP_MENU, 0x15f, &menu->scratch0);
		MenuSystem_loadHelpItems(menu, &menu->scratch0, 0x409);
		break;

	case 100:
		buildGameMenu(menu, ms);
		break;

	case 0x65: /* status */
		addIdHelp(menu, 0x4b, 0, 0, ITEM_NO_ICON, 0x4d);
		if (app->canvas->tellAFriend != 0)
			addIdHelp(menu, 0x13a, 0, 0, ITEM_NO_ICON, 0x13b);
		Menu_buildStatistics(menu, 1, 1, 1);
		ms->type = 1;
		ms->backMenu = 100;
		break;

	case 0x67:
		buildGameHelp(menu, ms);
		break;

	case 0x68: helpPage(menu, ms, 0x67, 1); break;
	case 0x69: helpPage(menu, ms, 0x67, 2); break;
	case 0x6a: helpPage(menu, ms, 0x67, 3); break;
	case 0x6b: helpPage(menu, ms, 0x67, 7); break;
	case 0x6d: helpPage(menu, ms, 0x67, 4); break;
	case 0x6e: helpPage(menu, ms, 0x67, 5); break;
	case 0x6f: helpPage(menu, ms, 0x67, 6); break;

	case 0x6c: /* quit to main menu */
		if (ms->backMenu == 100) {
			addId(menu, 0x8d, 9);
			addBlank(menu, 0);
			addIdAction(menu, 0x8e, 8, 0); /* save and quit */
			addIdAction(menu, 0x8f, 8, 1); /* quit */
			addIdAction(menu, 0x91, 8, 2); /* cancel */
		} else {
			MenuSystem_setYesNo(menu, 0x92, 0);
		}
		ms->type = 6;
		break;

	case 0x70: /* quest log */
		ms->backMenu = 100;
		ms->type = 7;
		MenuSystem_loadNotebook(menu);
		break;

	case 0x72: /* load game? */
		ms->backMenu = 100;
		ms->type = 6;
		MenuSystem_setYesNo(menu, 0x79, 1);
		break;

	case 0x73: /* no saved game */
		ms->selectedIndex = 2;
		ms->type = 6;
		addId(menu, 0x7a, 9);
		addBlank(menu, 0);
		addId(menu, 0x4b, 8);
		break;

	case 0x74: /* died: load / restart level / main menu */
		ms->backMenu = 0x74;
		ms->type = 6;
		addId(menu, 0xe6, 9);
		addBlank(menu, 0);
		addIdAction(menu, 0xe7, 8, 0);
		addIdAction(menu, 0xe8, 8, 1);
		addIdAction(menu, 0xe9, 8, 2);
		break;

	case 0x75: /* restart level? */
		ms->backMenu = 100;
		ms->type = 6;
		MenuSystem_setYesNo(menu, 0x77, 1);
		break;

	case 0x76: /* save and quit? */
		ms->backMenu = 100;
		ms->type = 6;
		MenuSystem_setYesNo(menu, 0x78, 1);
		break;

	case 0x96: buildTellAFriend(menu, ms); break;
	case 0x97: buildPhoneEntry(menu, ms); break;
	case 0x98: /* address book page */
		ms->imgBG = NULL;
		ms->backMenu = 0x96;
		ms->type = 1;
		Menu_buildContactsPage(menu);
		break;
	case 0x99: /* message sent, send another? */
		MenuSystem_setYesNo(menu, 0x147, 1);
		ms->backMenu = 0x65;
		ms->type = 6;
		break;
	case 0x9a: buildSendFailed(menu, ms); break;

	case 200:   buildDebug(menu, ms); break;
	case 0xc9:  buildChangeMap(menu, ms); break;
	case 0xca:  buildCheats(menu, ms); break;
	case 0xcb:  buildMemoryStats(menu, ms); break;
	case 0xcc:  buildDeveloper(menu, ms); break;
	case 0xcd:  buildDevVars(menu, ms); break;

	case 0x12f: buildArmory(menu, ms); break;
	case 0x130: buildFood(menu, ms); break;
	case 0x131: buildRingStore(menu, ms); break;
	case 0x132: buildPotionStore(menu, ms); break;
	case 0x133: buildStoreHub(menu, ms); break;
	case 0x134: buildTrinkets(menu, ms); break;
	case 0x135: buildOffer(menu, ms); break;
	case 0x136: ms->type = 6; addResultScreen(menu, 0x117, 0x118); break; /* not enough gold */
	case 0x137: ms->type = 6; addResultScreen(menu, 0x119, 0x11a); break; /* offer accepted */
	case 0x138: ms->type = 6; addResultScreen(menu, 0x119, 0x11b); break; /* offer refused */
	case 0x139: ms->type = 6; addResultScreen(menu, 0x111, 0x112); break; /* cannot carry more */

	case 400: buildDetails(menu, ms); break;

	case 500:   buildInventory(menu, ms); break;
	case 0x1f5: buildWeapons(menu, ms); break;
	case 0x1f6: buildRings(menu, ms); break;
	case 0x1f7: buildPotions(menu, ms); break;
	case 0x1f8: buildOther(menu, ms); break;
	case 0x1f9: buildUseItem(menu, ms); break;
	case 0x1fa: ms->type = 6; addResultScreen(menu, 0xff, 0x100); break;
	case 0x1fb: ms->type = 6; addResultScreen(menu, -1, 0x101); break;
	case 0x1fc: ms->type = 6; addResultScreen(menu, 0x108, 0x109); break;
	case 0x1fd: ms->type = 6; addResultScreen(menu, 0x104, 0x105); break;
	case 0x1fe: ms->type = 6; addResultScreen(menu, -1, 0x102); break;

	default: /* 0, 2, 0x71 and unknown ids */
		App_fatalError(app, mod_cstr(MOD_STR_BAD_MENU));
		break;
	}
}
