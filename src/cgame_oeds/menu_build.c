/*
 * Menu_initMenu: builds the item list of every menu (one big switch over the
 * menu id in the original, 0x14db8..0x178a3), plus Menu_startup and the
 * remembered selection per menu.
 *
 * Menu ids: 1 main, 2 help/about, 3 about, 4 how to play, 5 exit?, 6 erase
 * save?, 7 are you sure?, 8 continue/new game, 9 options (front end), 10 level
 * statistics, 0xc game completed, 0xd ranking, 0xe enable sounds?, 0xf
 * continue playing?, 0x10 go to main menu?; 100 game menu, 0x65 status, 0x66
 * options, 0x67 help, 0x68 about, 0x69 how to play, 0x6a quit?, 0x6b armor
 * help, 0x6c effects help, 0x6d items help, 0x6e quest log, 0x6f save (empty),
 * 0x70 load?, 0x71 no saved game, 0x72 died, 0x73 restart level?; 200
 * developer, 0xc9 change map, 0xca cheats, 0xcb memory, 0xcc developer tools,
 * 0xcd vars; 0x12e..0x137 Gaya's store and barter; 400 item details; 500
 * items, 0x1f5 weapons, 0x1f6 rings, 0x1f7 potions, 0x1f8 other, 0x1f9 use?,
 * 0x1fa..0x1fc cannot use.
 *
 * All labels are read from the user's .mod.
 */

#include "game.h"
#include "oe_vm.h"

/* ---- data in the .mod ---- */
#define MOD_STORE_TABLE             0x2e7a8u /* 29 x int32 {type, index, price, qty} */
#define MOD_S_EMPTY                 0x2eaf4u
#define MOD_S_BACK                  0x2f224u
#define MOD_S_HELP_BACK             0x2f22cu
#define MOD_S_ON                    0x2f29cu
#define MOD_S_OFF                   0x2f2a0u
#define MOD_S_SHOW_LOCATION         0x2f3d8u
#define MOD_S_R_FRAMES              0x2f3ccu
#define MOD_S_R_SPEEDS              0x2f3e8u
#define MOD_S_R_SKIPCULL            0x2f3f4u
#define MOD_S_R_SKIPSTRETCH         0x2f400u
#define MOD_S_R_SKIPBSP             0x2f410u
#define MOD_S_R_SKIPLINES           0x2f41cu
#define MOD_S_R_SKIPPUSHBLOCKS      0x2f428u
#define MOD_S_R_SKIPSPRITES         0x2f43cu
#define MOD_S_R_ONLYRENDER          0x2f44cu
#define MOD_S_FILEDEBUG             0x2f45cu
#define MOD_S_SHOW_NODES            0x2f468u
#define MOD_S_SHOW_LINE_RASTERS     0x2f474u
#define MOD_S_AUTO_SHOW_UNTOUCHED   0x2f484u
#define MOD_S_SHOW_SAVE_SIZE        0x2f498u
#define MOD_S_DONE_SHOPPING         0x2f908u
#define MOD_S_HELP_LEAVE_SHOP       0x2f918u
#define MOD_S_DRAGON                0x2f900u
#define MOD_S_WEAPONS               0x2f770u
#define MOD_S_HELP_STORE_WEAPONS    0x2f8dcu
#define MOD_S_ARMOR                 0x2f8b4u
#define MOD_S_HELP_STORE_ARMOR      0x2f8bcu
#define MOD_S_RINGS                 0x2f744u
#define MOD_S_HELP_STORE_RINGS      0x2f894u
#define MOD_S_POTIONS               0x2f758u
#define MOD_S_HELP_STORE_POTIONS    0x2f870u
#define MOD_S_WEAPONS_TITLE         0x2f924u
#define MOD_S_FMT_NAME_QTY          0x2f92cu
#define MOD_S_ARMOR_TITLE           0x2f934u
#define MOD_S_RINGS_TITLE           0x2f7b4u
#define MOD_S_POTIONS_TITLE         0x2f7e4u
#define MOD_S_ASKING                0x2f964u
#define MOD_S_OFFER                 0x2f998u
#define MOD_S_MAKE_OFFER            0x2f984u
#define MOD_S_CLEAR_CANCEL          0x2f970u
#define MOD_S_NOCLIP                0x2f4a8u
#define MOD_S_DISABLE_AI            0x2f4b0u
#define MOD_S_HELP                  0x2f0a8u
#define MOD_S_EQUIP_FOR_MAP         0x2f4c8u
#define MOD_S_LEVEL_UP              0x2f4bcu
#define MOD_S_GOD_MODE              0x2f520u
#define MOD_S_GIVE_ALL              0x2f514u
#define MOD_S_GIVE_MAP              0x2f508u
#define MOD_S_GIVE_MONEY            0x2f4fcu
#define MOD_S_GIVE_AMMO             0x2f4f0u
#define MOD_S_GIVE_GEMS             0x2f4e4u
#define MOD_S_GIVE_NOTES            0x2f4d8u
#define MOD_S_SHOW_CODES            0x2f52cu
#define MOD_S_SHOW_MONSTERS         0x2f538u
#define MOD_S_INVISIBILITY          0x2f56cu
#define MOD_S_GIVE_BUFFS            0x2f560u
#define MOD_S_CLEAR_BUFFS           0x2f554u
#define MOD_S_KILL_ALL              0x2f548u
#define MOD_S_RESTART_LEVEL_Q       0x2f128u
#define MOD_S_CHEATS                0x2f334u
#define MOD_S_CHANGE_MAP            0x2f328u
#define MOD_S_STATS                 0x2f320u
#define MOD_S_DEVELOPER             0x2f314u
#define MOD_S_MAP_ENTRANCE_HALL     0x2f5f4u
#define MOD_S_MAP_PRISON            0x2f5ecu
#define MOD_S_MAP_GREAT_HALL        0x2f5e0u
#define MOD_S_MAP_LABYRINTH         0x2f5d4u
#define MOD_S_MAP_CATACOMBS         0x2f5c8u
#define MOD_S_MAP_THRONE_ROOM       0x2f5bcu
#define MOD_S_MAP_THE_PIT           0x2f5b4u
#define MOD_S_MAP_TWILIGHT_CAVERN   0x2f5a4u
#define MOD_S_MAP_DRAGONS_LAIR      0x2f594u
#define MOD_S_MAP_MENU              0x2f58cu
#define MOD_S_MAP_ITEMS             0x2f0f8u
#define MOD_S_RESET_LEVEL           0x2f57cu
#define MOD_S_FMT_KB                0x2f604u
#define MOD_S_MEM_TOTAL             0x2f67cu
#define MOD_S_MEM_INIT              0x2f674u
#define MOD_S_MEM_CUR               0x2f66cu
#define MOD_S_MEM_ALLOC             0x2f664u
#define MOD_S_MEM_MAP               0x2f65cu
#define MOD_S_MEM_MSPRITES          0x2f650u
#define MOD_S_MEM_MLINES            0x2f648u
#define MOD_S_MEM_MNODES            0x2f640u
#define MOD_S_MEM_STEXELS           0x2f634u
#define MOD_S_MEM_TTEXELS           0x2f628u
#define MOD_S_MEM_FTEXELS           0x2f61cu
#define MOD_S_MEM_SHAPES            0x2f614u
#define MOD_S_MEM_PALETTES          0x2f608u
#define MOD_S_MEM_RTOTAL            0x2f698u
#define MOD_S_MEM_ENTITIES          0x2f68cu
#define MOD_S_MEM_MENUS             0x2f684u
#define MOD_S_MEM_SOUND             0x2f2b0u
#define MOD_S_MEM_IMAGE             0x2f6a0u
#define MOD_S_VARS                  0x2f3c4u
#define MOD_S_BENCHMARK             0x2f3b8u
#define MOD_S_OPEN_STORE            0x2f3acu
#define MOD_S_PRINT_MONSTERS        0x2f39cu
#define MOD_S_PRINT_MEMORY          0x2f38cu
#define MOD_S_ERROR_APP             0x2f380u
#define MOD_S_ERROR_TIMER           0x2f374u
#define MOD_S_ERROR_LOOP            0x2f368u
#define MOD_S_ERROR_STATE           0x2f35cu
#define MOD_S_SHOW_UNTOUCHED        0x2f34cu
#define MOD_S_PEEK_SAVE_USAGE       0x2f33cu
#define MOD_S_RESUME_GAME           0x2f10cu
#define MOD_S_HELP_RESUME           0x2f118u
#define MOD_S_RINGS_HEADER          0x2f7b4u
#define MOD_S_FMT_EQUIPPED_NONE     0x2f7bcu
#define MOD_S_NONE_BRACKETS         0x2f7c8u
#define MOD_S_HELP_NO_RING          0x2f7d0u
#define MOD_S_FMT_EQUIPPED          0x2f7a8u
#define MOD_S_WEAPON_HEADER         0x2f790u
#define MOD_S_AMMO_HEADER           0x2f79cu
#define MOD_S_NO_AMMO               0x2f7b0u
#define MOD_S_ITEMS_TITLE           0x2f788u
#define MOD_S_HELP_YOUR_WEAPONS     0x2f778u
#define MOD_S_HELP_YOUR_POTIONS     0x2f760u
#define MOD_S_HELP_YOUR_RINGS       0x2f74cu
#define MOD_S_OTHER                 0x2f720u
#define MOD_S_HELP_YOUR_OTHER       0x2f728u
#define MOD_S_OFFER_WAS             0x2f9dcu
#define MOD_S_ACCEPTED              0x2f9ccu
#define MOD_S_REJECTED              0x2f9ecu
#define MOD_S_CANNOT_HOLD           0x2f950u
#define MOD_S_OF_THAT_ITEM          0x2f93cu
#define MOD_S_OFFER_EXCEEDS         0x2f9b8u
#define MOD_S_AVAILABLE_GOLD        0x2f9a4u
#define MOD_S_OTHER_TITLE           0x2f7ecu
#define MOD_S_FMT_USE               0x2f7f4u
#define MOD_S_CANNOT_DRINK          0x2f818u
#define MOD_S_HEALTH_MAX            0x2f800u
#define MOD_S_EFFECT_MAX            0x2f858u
#define MOD_S_CANNOT_USE_KIT        0x2f844u
#define MOD_S_ARMOR_MAX             0x2f830u
#define MOD_S_OK                    0x2f7fcu
#define MOD_S_GO_TO_MAIN            0x2f2ecu
#define MOD_S_SAVE_GAME_SP          0x2f2e0u
#define MOD_S_DONT_SAVE             0x2f2d4u
#define MOD_S_CANCEL                0x2f2c8u
#define MOD_S_ITEMS                 0x2f0f8u
#define MOD_S_HELP_YOUR_STUFF       0x2f100u
#define MOD_S_QUEST_LOG             0x2f0ecu
#define MOD_S_SAVE_GAME             0x2f0e0u
#define MOD_S_LOAD_GAME             0x2f0d4u
#define MOD_S_VIEW_MAP              0x2f0b8u
#define MOD_S_HELP_VIEW_MAP         0x2f0c4u
#define MOD_S_STATUS                0x2f0b0u
#define MOD_S_HELP_ABOUT_SHORT      0x2ef84u
#define MOD_S_OPTIONS               0x2f0a0u
#define MOD_S_RESTART_LEVEL         0x2f070u
#define MOD_S_HELP_RESTART          0x2f080u
#define MOD_S_MAIN                  0x2f068u
#define MOD_S_MAIN_MENU             0x2f05cu
#define MOD_S_HELP_TITLE            0x2f21cu
#define MOD_S_ONSCREEN_HELP         0x2f1e8u
#define MOD_S_HELP_ONSCREEN         0x2f1f8u
#define MOD_S_HOW_TO_PLAY           0x2f1dcu
#define MOD_S_ABOUT_GAME            0x2efdcu
#define MOD_S_ARMOR_HELP            0x2f1bcu
#define MOD_S_HELP_ARMOR_HELP       0x2f1c8u
#define MOD_S_ITEMS_HELP            0x2f198u
#define MOD_S_HELP_ITEMS_HELP       0x2f1a4u
#define MOD_S_EFFECTS_HELP          0x2f16cu
#define MOD_S_HELP_EFFECTS_HELP     0x2f17cu
#define MOD_S_CREDITS               0x2f164u
#define MOD_S_QUIT_TO_MAIN          0x2f300u
#define MOD_S_LOAD_GAME_Q           0x2f138u
#define MOD_S_NO_SAVED_GAME         0x2f154u
#define MOD_S_BACK_WIDE             0x2f144u
#define MOD_S_START_GAME            0x2ef9cu
#define MOD_S_OPTIONS_PAD           0x2ef90u
#define MOD_S_HELP_ABOUT            0x2ef84u
#define MOD_S_EXIT_PAD              0x2ef78u
#define MOD_S_BACK_PAD14            0x2effcu
#define MOD_S_HOW_TO_PLAY_PAD       0x2efecu
#define MOD_S_ABOUT_GAME_PAD        0x2efdcu
#define MOD_S_CREDITS_PAD           0x2efccu
#define MOD_S_EXIT_GAME_Q           0x2f00cu
#define MOD_S_ERASE_SAVE            0x2f018u
#define MOD_S_ARE_YOU_SURE          0x2f04cu
#define MOD_S_CONTINUE              0x2efc0u
#define MOD_S_NEW_GAME              0x2efb4u
#define MOD_S_BACK_PAD8             0x2efa8u
#define MOD_S_VIBRATE_OPT           0x2f2a4u
#define MOD_S_SOUND_OPT             0x2f2b0u
#define MOD_S_NONE                  0x2ef4cu
#define MOD_S_VIBRATE               0x2f290u
#define MOD_S_SOUND                 0x2f288u
#define MOD_S_FX_OPT                0x2f298u
#define MOD_S_VOLUME                0x2f2c0u
#define MOD_S_FMT_PERCENT           0x2f2b8u
#define MOD_S_STATISTICS_UC         0x2f260u
#define MOD_S_PRESS_OK              0x2f254u
#define MOD_S_CONTINUE_PAD          0x2f248u
#define MOD_S_ORCS_ELVES_UC         0x2f278u
#define MOD_S_COMPLETED             0x2f26cu
#define MOD_S_ENABLE_SOUNDS         0x2f6a8u
#define MOD_S_CONTINUE_PLAYING      0x2f70cu
#define MOD_S_YES_PLEASE            0x2f700u
#define MOD_S_NO_THANKS             0x2f6f4u
#define MOD_S_TRY_AGAIN             0x2f6e8u
#define MOD_S_LOAD_SAVED_GAME       0x2f6d8u
#define MOD_S_RESTART_LEVEL_PAD     0x2f6c8u
#define MOD_S_MAIN_MENU_PAD         0x2f6b8u

enum { SND_LEVEL_END = 5148 };
enum { MAP_M08 = 6010, MAP_GAYA = 6011, MAP_MENU = 6012 }; /* DS: last level, Dragon's Lair, menu map (BREW 6007, 6008, 6009) */

/* Menu types (MenuSystem.type). */
enum { MENU_TYPE_LIST = 1, MENU_TYPE_MAIN = 4, MENU_TYPE_TEXT = 5, MENU_TYPE_PROMPT = 6, MENU_TYPE_DETAILS = 7,
       MENU_TYPE_OPTIONS = 8 };

/* Item flags (MenuItem.flags). */
enum {
	ITEM_UNSELECTABLE = 0x1, ITEM_DISABLED = 0x4, ITEM_CENTERED = 0x8, ITEM_NUMBER = 0x10, ITEM_DETAILS = 0x20,
	ITEM_SLIDER = 0x80
};
#define ITEM_PROMPT_LINE (ITEM_UNSELECTABLE | ITEM_CENTERED)        /* 9 */
#define ITEM_DIVIDER     (0x40 | ITEM_CENTERED | ITEM_UNSELECTABLE) /* 0x49 */
#define ITEM_OWNED       (ITEM_DETAILS | ITEM_DISABLED)             /* 0x24 */

#define ICON_MENU 0x2a
#define NO_ICON   0xff
#define EQUIPPED_MARK 0x87 /* glyph before the equipped weapon / ring */

/* Menu.indexes slots. */
enum {
	SLOT_RINGS = 0, SLOT_WEAPONS = 1, SLOT_POTIONS = 2, SLOT_OTHER = 3, SLOT_ITEMS = 4, SLOT_STORE = 5,
	SLOT_STORE_WEAPONS = 6, SLOT_STORE_ARMOR = 7, SLOT_STORE_RINGS = 8, SLOT_STORE_POTIONS = 9, SLOT_VARS = 10,
	SLOT_CHEATS = 11
};

static char *S(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

static int32_t storeField(int rec, int field)
{
	return ModData_s32(&g_mod, MOD_STORE_TABLE + (uint32_t)(rec + field) * 4);
}

static char *onOff(int v)
{
	return S(v ? MOD_S_ON : MOD_S_OFF);
}

/* Plain item: label only. */
static void add(Menu *menu, uint32_t label, int flags, int action)
{
	MenuSystem_addItem(menu, S(label), NULL, flags, action, 0, NULL);
}

/* Debug row "label   value". */
static void addValue(Menu *menu, uint32_t label, char *value)
{
	MenuSystem_addItem(menu, S(label), value, 0, 0, 0, NULL);
}

/* Game-menu row with the menu icon and a help line. */
static void addHelp(Menu *menu, uint32_t label, int flags, int action, uint32_t help)
{
	MenuSystem_addItem(menu, S(label), NULL, flags, action, ICON_MENU, S(help));
}

static void addDivider(Menu *menu, MenuSystem *ms, char *title)
{
	MenuSystem_addItem(menu, MenuSystem_buildDivider(ms, title), NULL, ITEM_DIVIDER, 0, 0, NULL);
}

/* Two-line message followed by a blank line and "Ok". */
static void addMessage(Menu *menu, MenuSystem *ms, uint32_t line2)
{
	MenuSystem_addItem(menu, S(line2), NULL, ITEM_PROMPT_LINE, 0, 0, NULL);
	MenuSystem_addItem(menu, NULL, NULL, ITEM_PROMPT_LINE, 0, 0, NULL);
	add(menu, MOD_S_OK, ITEM_CENTERED, 0);
}

/* 0x00028150 */
int Menu_startup(Menu *menu)
{
	memset(menu->indexes, 0, sizeof menu->indexes);
	menu->storeRecord = 0;
	Applet *app = menu->app;
	menu->canvas = app->canvas;
	menu->game = app->game;
	menu->render = app->render;
	menu->player = app->player;
	return 1;
}

/* 0x00018f04 Restore the remembered selection/scroll of a menu. */
void Menu_loadIndexes(Menu *menu, int slot)
{
	menu->app->menuSystem->selected = menu->indexes[slot * 2];
	menu->app->menuSystem->scroll = menu->indexes[slot * 2 + 1];
}

/* ---- front end ---- */

static void buildOptions(Menu *menu, MenuSystem *ms, int menuId)
{
	if (menuId == 0x66) {
		ms->backMenu = 100;
		ms->type = MENU_TYPE_LIST;
	} else {
		ms->backMenu = 1;
		ms->type = MENU_TYPE_OPTIONS;
	}
	add(menu, MOD_S_BACK, 0, 0);

	Canvas *cv = menu->canvas;
	char *value;
	uint32_t label;
	if (cv->barOpt7 == 0) {
		addValue(menu, MOD_S_VIBRATE_OPT, onOff(cv->vibrate));
		value = onOff(menu->app->sound->enabled);
		label = MOD_S_SOUND_OPT;
	} else {
		if (menu->app->sound->enabled)
			value = S(MOD_S_SOUND);
		else
			value = S(cv->vibrate ? MOD_S_VIBRATE : MOD_S_NONE);
		label = MOD_S_FX_OPT;
	}
	addValue(menu, label, value);

	if (menu->app->sound->enabled) {
		MenuItem *it = &ms->items[ms->numItems];
		MenuItem_setFull(it, S(MOD_S_VOLUME), S(MOD_S_EMPTY), ITEM_SLIDER, 0, 0, NULL);
		int volume = menu->app->sound->volume * 100 / 100;
		it = &ms->items[ms->numItems++];
		App_snprintf(it->text2, sizeof it->text2, S(MOD_S_FMT_PERCENT), volume);
	} else {
		MenuSystem_addItem(menu, NULL, NULL, 0, 0, 0, NULL);
	}

	/* port: the engine's settings (cgame_common/oe_options.c, handled in selectOptions) */
	for (int i = 0; i < Options_count(); i++) {
		char label[sizeof ms->items[0].text], value[sizeof ms->items[0].text2];
		Options_row(i, label, value);
		MenuSystem_addItem(menu, label, value, 0, 0, 0, NULL);
	}
}

/* ---- in-game menus ---- */

static void buildGameMenu(Menu *menu, MenuSystem *ms)
{
	ms->selected = 1;
	ms->type = MENU_TYPE_LIST;
	ms->backMenu = 0;
	/* items and the map are unavailable while effect 0 runs; the quest log in Gaya's lair */
	int busy = menu->player->effects[0].turns != 0 ? 5 : 0;
	int questFlags = menu->canvas->mapId == MAP_GAYA ? 5 : 0;

	addHelp(menu, MOD_S_RESUME_GAME, 0, 0, MOD_S_HELP_RESUME);
	addHelp(menu, MOD_S_ITEMS, busy, 500, MOD_S_HELP_YOUR_STUFF);
	addHelp(menu, MOD_S_QUEST_LOG, questFlags, 0x6e, MOD_S_QUEST_LOG);
	addHelp(menu, MOD_S_SAVE_GAME, 0, 0x6f, MOD_S_SAVE_GAME);
	addHelp(menu, MOD_S_LOAD_GAME, 0, 0x70, MOD_S_LOAD_GAME);
	addHelp(menu, MOD_S_VIEW_MAP, busy, 0, MOD_S_HELP_VIEW_MAP);
	addHelp(menu, MOD_S_STATUS, 0, 0x65, MOD_S_STATUS);
	addHelp(menu, MOD_S_HELP_ABOUT_SHORT, 0, 0x67, MOD_S_HELP);
	addHelp(menu, MOD_S_OPTIONS, 0, 0x66, MOD_S_OPTIONS);
	addHelp(menu, MOD_S_RESTART_LEVEL, 0, 0x73, MOD_S_HELP_RESTART);
	addHelp(menu, MOD_S_MAIN_MENU, 0, 0x6a, MOD_S_MAIN);
}

static void buildGameHelp(Menu *menu, MenuSystem *ms)
{
	ms->type = MENU_TYPE_LIST;
	ms->backMenu = 100;
	addHelp(menu, MOD_S_BACK, 0, 100, MOD_S_HELP_BACK);
	addDivider(menu, ms, S(MOD_S_HELP_TITLE));
	addHelp(menu, MOD_S_ONSCREEN_HELP, 0, 0, MOD_S_HELP_ONSCREEN);
	addHelp(menu, MOD_S_HOW_TO_PLAY, 0, 0, MOD_S_HOW_TO_PLAY);
	addHelp(menu, MOD_S_ABOUT_GAME, 0, 0, MOD_S_ABOUT_GAME);
	addHelp(menu, MOD_S_ARMOR_HELP, 0, 0, MOD_S_HELP_ARMOR_HELP);
	addHelp(menu, MOD_S_ITEMS_HELP, 0, 0, MOD_S_HELP_ITEMS_HELP);
	addHelp(menu, MOD_S_EFFECTS_HELP, 0, 0, MOD_S_HELP_EFFECTS_HELP);
	addHelp(menu, MOD_S_CREDITS, 0, 0, MOD_S_CREDITS);
}

/* "Save / Don't Save / Cancel" prompt before leaving for the main menu. */
static void addGoToMainPrompt(Menu *menu)
{
	MenuSystem_addItem(menu, S(MOD_S_GO_TO_MAIN), NULL, ITEM_PROMPT_LINE, 0, 0, NULL);
	MenuSystem_addItem(menu, NULL, NULL, 0, 0, 0, NULL);
	add(menu, MOD_S_SAVE_GAME_SP, ITEM_CENTERED, 0);
	add(menu, MOD_S_DONT_SAVE, ITEM_CENTERED, 1);
	add(menu, MOD_S_CANCEL, ITEM_CENTERED, 2);
}

/* ---- inventory ---- */

static void addInventoryHeader(Menu *menu, MenuSystem *ms, uint32_t title)
{
	ms->type = MENU_TYPE_LIST;
	ms->backMenu = 500;
	MenuSystem_addItem(menu, S(MOD_S_BACK), NULL, 0, 500, ICON_MENU, S(MOD_S_HELP_BACK));
	addHelp(menu, MOD_S_RESUME_GAME, 0, 0, MOD_S_HELP_RESUME);
	if (title != 0)
		addDivider(menu, ms, S(title));
}

static void buildItems(Menu *menu, MenuSystem *ms)
{
	ms->type = MENU_TYPE_LIST;
	ms->backMenu = 100;
	MenuSystem_addItem(menu, S(MOD_S_BACK), NULL, 0, 100, ICON_MENU, S(MOD_S_HELP_BACK));
	addHelp(menu, MOD_S_RESUME_GAME, 0, 0, MOD_S_HELP_RESUME);
	addDivider(menu, ms, S(MOD_S_ITEMS_TITLE));
	EntityDef *armor = EntityDefs_find(menu->app->entityDefs, 6, 7, menu->player->ce.armorType);
	MenuSystem_addItem(menu, armor->longName, NULL, ITEM_DETAILS, 500, armor->icon, armor->description);
	addHelp(menu, MOD_S_WEAPONS, 0, 0x1f5, MOD_S_HELP_YOUR_WEAPONS);
	addHelp(menu, MOD_S_POTIONS, 0, 0x1f7, MOD_S_HELP_YOUR_POTIONS);
	addHelp(menu, MOD_S_RINGS, 0, 0x1f6, MOD_S_HELP_YOUR_RINGS);
	addHelp(menu, MOD_S_OTHER, 0, 0x1f8, MOD_S_HELP_YOUR_OTHER);
	Menu_loadIndexes(menu, SLOT_ITEMS);
}

static void buildWeapons(Menu *menu, MenuSystem *ms)
{
	char name[32], ammo[32];

	addInventoryHeader(menu, ms, 0);
	MenuSystem_addItem(menu, S(MOD_S_WEAPON_HEADER), S(MOD_S_AMMO_HEADER), 0x41, 0, 0, NULL);
	Menu_loadIndexes(menu, SLOT_WEAPONS);
	for (int w = 0; w <= 10; w++) {
		if ((menu->player->weapons & arm_bit(w)) == 0)
			continue;
		EntityDef *def = EntityDefs_find(menu->app->entityDefs, 6, 1, w & 0xff);
		if (menu->player->ce.weapon == w)
			App_snprintf(name, sizeof name, S(MOD_S_FMT_EQUIPPED), EQUIPPED_MARK, def->name);
		else
			strcpy(name, def->name);
		int ammoType = menu->app->combat->weapons[w].ammoType;
		char *value;
		if (ammoType == 0) {
			value = S(MOD_S_NO_AMMO);
		} else {
			App_snprintf(ammo, sizeof ammo, "%d", menu->player->ammo[ammoType]);
			value = ammo;
		}
		MenuSystem_addItem(menu, name, value, ITEM_DETAILS, w, def->icon, def->description);
	}
}

static void buildRings(Menu *menu, MenuSystem *ms)
{
	char name[64];

	addInventoryHeader(menu, ms, MOD_S_RINGS_HEADER);
	Menu_loadIndexes(menu, SLOT_RINGS);
	for (int r = 0; r <= 8; r++) {
		if (r == 0) {
			/* "[None]": take the ring off */
			if (menu->player->ring == 0)
				App_snprintf(name, sizeof name, S(MOD_S_FMT_EQUIPPED_NONE), EQUIPPED_MARK);
			else
				strcpy(name, S(MOD_S_NONE_BRACKETS));
			MenuSystem_addItem(menu, name, NULL, 0, r, 0, S(MOD_S_HELP_NO_RING));
		} else if (menu->player->rings & arm_bit(r)) {
			EntityDef *def = EntityDefs_find(menu->app->entityDefs, 6, 2, r);
			if (menu->player->ring == r)
				App_snprintf(name, sizeof name, S(MOD_S_FMT_EQUIPPED), EQUIPPED_MARK, def->name);
			else
				strcpy(name, def->name);
			MenuSystem_addItem(menu, name, NULL, ITEM_DETAILS, r, def->icon, def->description);
		}
	}
}

/* Items index from..to-1 the player owns, with their counts. */
static void addOwnedItems(Menu *menu, int from, int to)
{
	char count[32];
	for (int i = from; i < to; i++) {
		if (menu->player->items[i] == 0)
			continue;
		App_snprintf(count, sizeof count, "%d", menu->player->items[i]);
		EntityDef *def = EntityDefs_find(menu->app->entityDefs, 6, 0, i);
		MenuSystem_addItem(menu, def->name, count, ITEM_DETAILS, i, def->icon, def->description);
	}
}

static void buildPotions(Menu *menu, MenuSystem *ms)
{
	addInventoryHeader(menu, ms, MOD_S_POTIONS_TITLE);
	Menu_loadIndexes(menu, SLOT_POTIONS);
	addOwnedItems(menu, 0, 11);
}

static void buildOther(Menu *menu, MenuSystem *ms)
{
	addInventoryHeader(menu, ms, MOD_S_OTHER_TITLE);
	Menu_loadIndexes(menu, SLOT_OTHER);
	addOwnedItems(menu, 11, 13);
	/* quest items (gems), listed with action 0x100 + index */
	for (int q = 0; q <= 2; q++) {
		if ((menu->player->questItems & arm_bit(q)) == 0)
			continue;
		EntityDef *def = EntityDefs_find(menu->app->entityDefs, 6, 8, q);
		MenuSystem_addItem(menu, def->name, S(MOD_S_EMPTY), ITEM_DETAILS, q + 0x100, def->icon, def->description);
	}
}

/* Item description page (from Menus_showDetails). */
static void buildDetails(Menu *menu, MenuSystem *ms)
{
	char desc[192];

	ms->type = MENU_TYPE_DETAILS;
	MenuSystem_setItemsFromText(ms, 0, menu->detailText2);
	for (int i = 0; i < ms->numItems; i++)
		ms->items[i].flags |= ITEM_CENTERED;
	addDivider(menu, ms, NULL);
	MenuSystem_setItemsFromText(ms, ms->numItems, menu->detailText1);

	EntityDef *def = menu->detailDef;
	if (def->eType != 6)
		return;
	if (def->eSubType == 1) {
		addDivider(menu, ms, NULL);
		Combat_getWeaponDescription(menu->app->combat, menu->detailDef->parm, desc);
	} else if (def->eSubType == 7) {
		addDivider(menu, ms, NULL);
		Combat_getArmorDescription(menu->app->combat, menu->detailDef->parm, desc);
	} else if (def->eSubType == 0 && def->parm == 0xc) {
		/* armour repair kit */
		addDivider(menu, ms, NULL);
		Combat_getArmorDescription(menu->app->combat, -1, desc);
	} else {
		return;
	}
	MenuSystem_setItemsFromText(ms, ms->numItems, desc);
}

/* ---- Gaya's store ---- */

static void addStoreHeader(Menu *menu, MenuSystem *ms, uint32_t title)
{
	ms->type = MENU_TYPE_LIST;
	ms->backMenu = 0x12e;
	MenuSystem_addItem(menu, S(MOD_S_BACK), NULL, 0, -1, ICON_MENU, S(MOD_S_HELP_BACK));
	addDivider(menu, ms, S(title));
}

static EntityDef *storeDef(Menu *menu, int rec)
{
	return EntityDefs_find(menu->app->entityDefs, 6, storeField(rec, 0) & 0xff, storeField(rec, 1));
}

static void buildStore(Menu *menu, MenuSystem *ms)
{
	ms->type = MENU_TYPE_LIST;
	addHelp(menu, MOD_S_DONE_SHOPPING, 0, 0, MOD_S_HELP_LEAVE_SHOP);
	addDivider(menu, ms, S(MOD_S_DRAGON));
	MenuSystem_addItem(menu, S(MOD_S_WEAPONS), NULL, 0, 0x12f, 1, S(MOD_S_HELP_STORE_WEAPONS));
	MenuSystem_addItem(menu, S(MOD_S_ARMOR), NULL, 0, 0x130, 0x27, S(MOD_S_HELP_STORE_ARMOR));
	MenuSystem_addItem(menu, S(MOD_S_RINGS), NULL, 0, 0x131, 0x12, S(MOD_S_HELP_STORE_RINGS));
	MenuSystem_addItem(menu, S(MOD_S_POTIONS), NULL, 0, 0x132, 0x14, S(MOD_S_HELP_STORE_POTIONS));
	uint16_t map = menu->canvas->mapId;
	ms->backMenu = (map == 0 || map == MAP_MENU) ? 1 : 0;
	Menu_loadIndexes(menu, SLOT_STORE);
}

static void buildStoreWeapons(Menu *menu, MenuSystem *ms)
{
	char price[32], name[32], priceShort[12];

	addStoreHeader(menu, ms, MOD_S_WEAPONS_TITLE);
	int first = ms->numItems;

	/* weapons: records 0..3 */
	for (int rec = 0; rec < 0xd; rec += 4) {
		EntityDef *def = storeDef(menu, rec);
		App_snprintf(price, sizeof price, "%d", storeField(rec, 2));
		MenuSystem_addItem(menu, def->name, price, ITEM_DETAILS, rec, def->icon, def->description);
	}
	/* ammo: records 4..9 (ammo type 4 only with weapon 9) */
	for (int rec = 0x10; rec < 0x25; rec += 4) {
		int type = storeField(rec, 0);
		int index = storeField(rec, 1);
		if (index == 4 && (menu->player->weapons & (1u << 9)) == 0)
			continue;
		EntityDef *def;
		if (type == 3 && index == 3)
			def = EntityDefs_find(menu->app->entityDefs, 6, 1, 7);
		else
			def = EntityDefs_find(menu->app->entityDefs, 6, type & 0xff, index);
		App_snprintf(priceShort, sizeof priceShort, "%d", storeField(rec, 2));
		int qty = storeField(rec, 3);
		if (qty < 2)
			strcpy(name, def->name);
		else
			App_snprintf(name, sizeof name, S(MOD_S_FMT_NAME_QTY), def->name, qty);
		MenuSystem_addItem(menu, name, priceShort, ITEM_DETAILS, rec, def->icon, def->description);
	}

	/* grey out the weapons the player already has */
	uint32_t owned = menu->player->weapons;
	if (owned & (1u << 2))
		ms->items[first].flags |= ITEM_DISABLED;
	if (owned & (1u << 3))
		ms->items[first + 1].flags |= ITEM_DISABLED;
	if (owned & (1u << 6)) {
		ms->items[first + 2].flags |= ITEM_DISABLED;
		ms->items[first + 3].flags |= ITEM_DISABLED;
	} else if (owned & (1u << 5)) {
		ms->items[first + 2].flags |= ITEM_DISABLED;
	}
	Menu_loadIndexes(menu, SLOT_STORE_WEAPONS);
}

static void buildStoreArmor(Menu *menu, MenuSystem *ms)
{
	char price[32];

	addStoreHeader(menu, ms, MOD_S_ARMOR_TITLE);
	for (int rec = 0x28; rec < 0x35; rec += 4) {
		EntityDef *def = storeDef(menu, rec);
		App_snprintf(price, sizeof price, "%d", storeField(rec, 2));
		int flags = ITEM_DETAILS;
		if (storeField(rec, 0) == 7 && storeField(rec, 1) < (int)(int8_t)menu->player->armorOwned)
			flags = ITEM_OWNED;
		MenuSystem_addItem(menu, def->name, price, flags, rec, def->icon, def->description);
	}
	Menu_loadIndexes(menu, SLOT_STORE_ARMOR);
}

static void buildStoreRings(Menu *menu, MenuSystem *ms)
{
	char price[32];

	addStoreHeader(menu, ms, MOD_S_RINGS_TITLE);
	for (int rec = 0x38; rec < 0x45; rec += 4) {
		EntityDef *def = storeDef(menu, rec);
		int flags = ITEM_DETAILS;
		if (menu->player->rings & arm_bit(storeField(rec, 1)))
			flags = ITEM_OWNED;
		App_snprintf(price, sizeof price, "%d", storeField(rec, 2));
		MenuSystem_addItem(menu, def->name, price, flags, rec, def->icon, def->description);
	}
	Menu_loadIndexes(menu, SLOT_STORE_RINGS);
}

static void buildStorePotions(Menu *menu, MenuSystem *ms)
{
	char price[32];

	addStoreHeader(menu, ms, MOD_S_POTIONS_TITLE);
	for (int rec = 0x48; rec < 0x71; rec += 4) {
		EntityDef *def = storeDef(menu, rec);
		App_snprintf(price, sizeof price, "%d", storeField(rec, 2));
		MenuSystem_addItem(menu, def->name, price, ITEM_DETAILS, rec, def->icon, def->description);
	}
	Menu_loadIndexes(menu, SLOT_STORE_POTIONS);
}

/* Barter screen for Menu.storeRecord: asking price and the offer field. */
static void buildOffer(Menu *menu, MenuSystem *ms)
{
	char buf[32];
	int rec = menu->storeRecord;

	ms->type = MENU_TYPE_LIST;
	EntityDef *def = storeDef(menu, rec);
	addDivider(menu, ms, def->name);
	App_snprintf(buf, sizeof buf, "%d", storeField(rec, 2));
	MenuSystem_addItem(menu, S(MOD_S_ASKING), buf, ITEM_UNSELECTABLE, 0, 0, NULL);
	/* the last offer for this item is kept while browsing back to it */
	int offer = (menu->storeRecord == menu->offerIndex) ? ms->editValue : storeField(menu->storeRecord, 2);
	App_snprintf(buf, sizeof buf, "%d", offer);
	MenuSystem_addItem(menu, S(MOD_S_OFFER), buf, ITEM_NUMBER, offer, def->icon, def->description);
	MenuSystem_addItem(menu, S(MOD_S_EMPTY), NULL, ITEM_PROMPT_LINE, 0, 0, NULL);
	addDivider(menu, ms, NULL);
	MenuSystem_addItem(menu, S(MOD_S_MAKE_OFFER), NULL, ITEM_CENTERED, 0, def->icon, def->description);
	MenuSystem_addItem(menu, S(MOD_S_CLEAR_CANCEL), NULL, ITEM_CENTERED, 0, def->icon, def->description);
	ms->editingNumber = 1;
}

/* ---- developer ---- */

static void buildDeveloper(Menu *menu, MenuSystem *ms)
{
	ms->type = MENU_TYPE_LIST;
	uint16_t map = menu->canvas->mapId;
	ms->backMenu = (map != 0 && map != MAP_MENU) ? 100 : 1;
	add(menu, MOD_S_BACK, 0, 0);
	add(menu, MOD_S_CHEATS, 0, 0);
	add(menu, MOD_S_CHANGE_MAP, 0, 0);
	add(menu, MOD_S_STATS, 0, 0);
	add(menu, MOD_S_DEVELOPER, 0, 0);
}

static void buildChangeMap(Menu *menu, MenuSystem *ms)
{
	static const uint32_t names[] = {
		MOD_S_BACK, MOD_S_MAP_ENTRANCE_HALL, MOD_S_MAP_PRISON, MOD_S_MAP_GREAT_HALL, MOD_S_MAP_LABYRINTH,
		MOD_S_MAP_CATACOMBS, MOD_S_MAP_THRONE_ROOM, MOD_S_MAP_THE_PIT, MOD_S_MAP_TWILIGHT_CAVERN,
		MOD_S_MAP_DRAGONS_LAIR, MOD_S_MAP_MENU, MOD_S_MAP_ITEMS, MOD_S_RESET_LEVEL,
	};
	ms->type = MENU_TYPE_LIST;
	ms->backMenu = 200;
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
		add(menu, names[i], 0, 0);
}

static void buildCheats(Menu *menu, MenuSystem *ms)
{
	Player *pl = menu->player;
	char buf[32];

	ms->type = MENU_TYPE_LIST;
	ms->backMenu = 200;
	add(menu, MOD_S_BACK, 0, 0);
	addValue(menu, MOD_S_NOCLIP, onOff(pl->noclip));
	addValue(menu, MOD_S_DISABLE_AI, onOff(menu->game->disableAI));
	addValue(menu, MOD_S_HELP, onOff(menu->player->helpEnabled));
	add(menu, MOD_S_EQUIP_FOR_MAP, 0, 0);
	App_snprintf(buf, sizeof buf, "%d", menu->player->level);
	addValue(menu, MOD_S_LEVEL_UP, buf);
	addValue(menu, MOD_S_GOD_MODE, onOff(menu->player->god));
	add(menu, MOD_S_GIVE_ALL, 0, 0);
	add(menu, MOD_S_GIVE_MAP, 0, 0);
	add(menu, MOD_S_GIVE_MONEY, 0, 0);
	add(menu, MOD_S_GIVE_AMMO, 0, 0);
	add(menu, MOD_S_GIVE_GEMS, 0, 0);
	add(menu, MOD_S_GIVE_NOTES, 0, 0);
	addValue(menu, MOD_S_SHOW_CODES, onOff(menu->canvas->showCodes));
	addValue(menu, MOD_S_SHOW_MONSTERS, onOff(menu->canvas->showMonsters));
	addValue(menu, MOD_S_INVISIBILITY, onOff(menu->player->invisible));
	add(menu, MOD_S_GIVE_BUFFS, 0, 0);
	add(menu, MOD_S_CLEAR_BUFFS, 0, 0);
	add(menu, MOD_S_KILL_ALL, 0, 0);
	Menu_loadIndexes(menu, SLOT_CHEATS);
}

/* Kilobytes, rounded up (signed division, as the original). */
static int kbSigned(int bytes)
{
	return (bytes + 0x3ff) / 1024;
}

static int kbUnsigned(int bytes)
{
	return (int)((uint32_t)(bytes + 0x3ff) >> 10);
}

static void addMem(Menu *menu, uint32_t label, int kb)
{
	char buf[32];
	App_snprintf(buf, sizeof buf, S(MOD_S_FMT_KB), kb);
	addValue(menu, label, buf);
}

static void buildMemory(Menu *menu, MenuSystem *ms)
{
	Applet *app = menu->app;
	Render *r = menu->render;

	ms->type = MENU_TYPE_LIST;
	addMem(menu, MOD_S_MEM_TOTAL, (int)((app->ramTotal + 0x3ff) >> 10));
	addMem(menu, MOD_S_MEM_INIT, kbSigned(app->heapAtInit));
	addMem(menu, MOD_S_MEM_CUR, kbUnsigned(App_getMemUsed(app)));
	addMem(menu, MOD_S_MEM_ALLOC, kbUnsigned(App_getMemUsed(app) - app->heapAtInit));
	addMem(menu, MOD_S_MEM_MAP, kbSigned(r->memMap));
	addMem(menu, MOD_S_MEM_MSPRITES, kbUnsigned(r->numSprites * 0x2c));
	addMem(menu, MOD_S_MEM_MLINES, kbUnsigned(r->numLines * 0x20));
	addMem(menu, MOD_S_MEM_MNODES, kbUnsigned(r->numNodes * 0x1c));
	addMem(menu, MOD_S_MEM_STEXELS, kbSigned(r->memSTexels));
	addMem(menu, MOD_S_MEM_TTEXELS, kbSigned(r->memTTexels));
	addMem(menu, MOD_S_MEM_FTEXELS, 0x40);
	addMem(menu, MOD_S_MEM_SHAPES, kbSigned(r->memShapes));
	addMem(menu, MOD_S_MEM_PALETTES, kbSigned(r->memPalettes));
	/* renderer total includes the 64K of flats */
	addMem(menu, MOD_S_MEM_RTOTAL,
	       kbUnsigned(r->memSTexels + r->memTTexels + r->memShapes + r->memPalettes + 0x10000));
	addMem(menu, MOD_S_MEM_ENTITIES, 0x15);
	addMem(menu, MOD_S_MEM_MENUS, 9);
	int sound = 0;
	for (int i = 0; i < 10; i++)
		sound += app->sound->channels[i].size;
	addMem(menu, MOD_S_MEM_SOUND, kbSigned(sound));
	addMem(menu, MOD_S_MEM_IMAGE, kbSigned(app->imageMem));
	ms->backMenu = 200;
}

static void buildDevTools(Menu *menu, MenuSystem *ms)
{
	static const uint32_t names[] = {
		MOD_S_BACK, MOD_S_VARS, MOD_S_BENCHMARK, MOD_S_OPEN_STORE, MOD_S_PRINT_MONSTERS, MOD_S_PRINT_MEMORY,
		MOD_S_ERROR_APP, MOD_S_ERROR_TIMER, MOD_S_ERROR_LOOP, MOD_S_ERROR_STATE, MOD_S_SHOW_UNTOUCHED,
		MOD_S_PEEK_SAVE_USAGE,
	};
	ms->type = MENU_TYPE_LIST;
	ms->backMenu = 200;
	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
		add(menu, names[i], 0, 0);
}

static void buildVars(Menu *menu, MenuSystem *ms)
{
	Canvas *cv = menu->canvas;
	Render *r = menu->render;
	char frames[32];

	ms->type = MENU_TYPE_LIST;
	ms->backMenu = 0xcc;
	add(menu, MOD_S_BACK, 0, 0);
	App_snprintf(frames, sizeof frames, "%d", cv->animFrames);
	addValue(menu, MOD_S_SHOW_LOCATION, onOff(cv->showLocation));
	addValue(menu, MOD_S_R_FRAMES, frames);
	addValue(menu, MOD_S_R_SPEEDS, onOff(menu->canvas->showSpeeds));
	addValue(menu, MOD_S_R_SKIPCULL, onOff(r->rSkipCull));
	addValue(menu, MOD_S_R_SKIPSTRETCH, onOff(menu->render->rNoTexture));
	addValue(menu, MOD_S_R_SKIPBSP, onOff(menu->render->rSkipBSP));
	addValue(menu, MOD_S_R_SKIPLINES, onOff(menu->render->rSkipLines));
	addValue(menu, MOD_S_R_SKIPPUSHBLOCKS, onOff(menu->render->rSkipDynLines));
	addValue(menu, MOD_S_R_SKIPSPRITES, onOff(menu->render->rSkipSprites));
	addValue(menu, MOD_S_R_ONLYRENDER, onOff(menu->canvas->cameraMode));
	addValue(menu, MOD_S_FILEDEBUG, onOff(menu->app->fileDebug));
	addValue(menu, MOD_S_SHOW_NODES, onOff(menu->canvas->showNodes));
	addValue(menu, MOD_S_SHOW_LINE_RASTERS, onOff(menu->render->rShowLines));
	addValue(menu, MOD_S_AUTO_SHOW_UNTOUCHED, onOff(menu->game->autoShowUntouched));
	addValue(menu, MOD_S_SHOW_SAVE_SIZE, onOff(menu->canvas->showSaveSize));
	Menu_loadIndexes(menu, SLOT_VARS);
}

/* Prompt line(s) + blank + choices for the end/system dialogs. */
static void addChoice(Menu *menu, uint32_t label, int action)
{
	add(menu, label, ITEM_CENTERED, action);
}

/* 0x00014db8 Build the item list of menu `menuId` (MenuSystem_setMenu). */
void Menu_initMenu(Menu *menu, int menuId)
{
	MenuSystem *ms = menu->app->menuSystem;
	char buf[32];

	menu->app->hud->msg2[0] = '\0';
	ms->scroll = 0;
	ms->selected = 0;
	ms->numItems = 0;

	switch (menuId) {
	/* ---- front end ---- */
	case 1:
		ms->type = MENU_TYPE_MAIN;
		ms->background = &ms->logo;
		ms->backMenu = -1;
		add(menu, MOD_S_START_GAME, ITEM_CENTERED, 0);
		add(menu, MOD_S_OPTIONS_PAD, ITEM_CENTERED, 0);
		add(menu, MOD_S_HELP_ABOUT, ITEM_CENTERED, 0);
		add(menu, MOD_S_EXIT_PAD, ITEM_CENTERED, 0);
		break;
	case 2:
		ms->type = MENU_TYPE_MAIN;
		ms->background = &ms->logo;
		ms->backMenu = 1;
		add(menu, MOD_S_BACK_PAD14, ITEM_CENTERED, 0);
		add(menu, MOD_S_HOW_TO_PLAY_PAD, ITEM_CENTERED, 0);
		add(menu, MOD_S_ABOUT_GAME_PAD, ITEM_CENTERED, 0);
		add(menu, MOD_S_CREDITS_PAD, ITEM_CENTERED, 0);
		break;
	case 3:
		ms->type = MENU_TYPE_TEXT;
		ms->background = NULL;
		ms->backMenu = 2;
		MenuSystem_loadLegals(menu);
		break;
	case 4:
		ms->type = MENU_TYPE_TEXT;
		ms->background = NULL;
		ms->backMenu = 2;
		MenuSystem_loadHelp(menu);
		break;
	case 5:
	case 6:
	case 7:
		ms->type = MENU_TYPE_PROMPT;
		MenuSystem_setYesNo(menu, S(menuId == 5 ? MOD_S_EXIT_GAME_Q
						      : menuId == 6 ? MOD_S_ERASE_SAVE : MOD_S_ARE_YOU_SURE), 0);
		ms->background = NULL;
		ms->backMenu = 1;
		break;
	case 8:
		ms->type = MENU_TYPE_MAIN;
		ms->background = &ms->logo;
		ms->backMenu = 1;
		add(menu, MOD_S_CONTINUE, ITEM_CENTERED, 0);
		add(menu, MOD_S_NEW_GAME, ITEM_CENTERED, 0);
		add(menu, MOD_S_BACK_PAD8, ITEM_CENTERED, 0);
		break;
	case 9:
	case 0x66:
		buildOptions(menu, ms, menuId);
		break;
	case 10: /* level finished */
		MenuSystem_addItem(menu, menu->render->mapName, NULL, ITEM_CENTERED, 0, 0, NULL);
		add(menu, MOD_S_STATISTICS_UC, ITEM_CENTERED, 0);
		MenuSystem_addItem(menu, NULL, NULL, 0, 0, 0, NULL);
		Menu_buildStatistics(menu, 1, 0, 0);
		MenuSystem_addItem(menu, NULL, NULL, 0, 0, 0, NULL);
		add(menu, MOD_S_PRESS_OK, ITEM_CENTERED, 0);
		add(menu, MOD_S_CONTINUE_PAD, ITEM_CENTERED, 0);
		Sound_play(menu->app->sound, SND_LEVEL_END, 2, 5, -1);
		menu->canvas->numKeys = 0;
		ms->type = MENU_TYPE_TEXT;
		ms->background = NULL;
		ms->backMenu = -1;
		break;
	case 0xc: /* game completed */
		add(menu, MOD_S_ORCS_ELVES_UC, ITEM_CENTERED, 0);
		add(menu, MOD_S_COMPLETED, ITEM_CENTERED, 0);
		MenuSystem_addItem(menu, NULL, NULL, 0, 0, 0, NULL);
		Menu_buildStatistics(menu, 0, 0, 1);
		MenuSystem_addItem(menu, NULL, NULL, 0, 0, 0, NULL);
		add(menu, MOD_S_PRESS_OK, ITEM_CENTERED, 0);
		add(menu, MOD_S_CONTINUE_PAD, ITEM_CENTERED, 0);
		ms->type = MENU_TYPE_TEXT;
		ms->background = NULL;
		ms->backMenu = -1;
		break;
	case 0xd:
		ms->type = MENU_TYPE_TEXT;
		ms->background = NULL;
		ms->backMenu = -1;
		MenuSystem_fillRanking(menu);
		break;
	case 0xe:
		MenuSystem_setYesNo(menu, S(MOD_S_ENABLE_SOUNDS), 1);
		ms->scroll = 0;
		ms->backMenu = -1;
		ms->type = MENU_TYPE_MAIN;
		ms->background = &ms->logo;
		break;
	case 0xf:
		ms->type = MENU_TYPE_PROMPT;
		MenuSystem_addItem(menu, S(MOD_S_CONTINUE_PLAYING), NULL, ITEM_PROMPT_LINE, 0, 0, NULL);
		MenuSystem_addItem(menu, NULL, NULL, 0, 0, 0, NULL);
		addChoice(menu, MOD_S_YES_PLEASE, 0);
		addChoice(menu, MOD_S_NO_THANKS, 1);
		ms->backMenu = -1;
		break;
	case 0x10:
		ms->type = MENU_TYPE_PROMPT;
		addGoToMainPrompt(menu);
		break;

	/* ---- in game ---- */
	case 100:
		buildGameMenu(menu, ms);
		break;
	case 0x65:
		MenuSystem_addItem(menu, S(MOD_S_BACK), NULL, 0, 0, NO_ICON, S(MOD_S_HELP_BACK));
		Menu_buildStatistics(menu, menu->canvas->mapId != MAP_GAYA, 1, 1);
		ms->type = MENU_TYPE_LIST;
		ms->backMenu = 100;
		break;
	case 0x67:
		buildGameHelp(menu, ms);
		break;
	case 0x68:
		ms->type = MENU_TYPE_TEXT;
		ms->selected = 1;
		ms->backMenu = 0x67;
		MenuSystem_loadLegals(menu);
		break;
	case 0x69:
		ms->type = MENU_TYPE_TEXT;
		ms->selected = 1;
		ms->backMenu = 0x67;
		MenuSystem_loadHelp(menu);
		break;
	case 0x6a:
		if (ms->backMenu == 100)
			addGoToMainPrompt(menu);
		else
			MenuSystem_setYesNo(menu, S(MOD_S_QUIT_TO_MAIN), 0);
		ms->type = MENU_TYPE_PROMPT;
		break;
	case 0x6b:
		ms->type = MENU_TYPE_TEXT;
		ms->selected = 1;
		ms->backMenu = 0x67;
		MenuSystem_loadHelpArmour(menu);
		break;
	case 0x6c:
		ms->type = MENU_TYPE_TEXT;
		ms->selected = 1;
		ms->backMenu = 0x67;
		MenuSystem_loadHelpEffects(menu);
		break;
	case 0x6d:
		ms->type = MENU_TYPE_TEXT;
		ms->selected = 1;
		ms->backMenu = 0x67;
		MenuSystem_loadHelpItems(menu);
		break;
	case 0x6e:
		ms->type = MENU_TYPE_DETAILS;
		ms->backMenu = 100;
		ms->background = NULL;
		MenuSystem_loadNotebook(menu);
		break;
	case 0x70:
	case 0x73:
		ms->type = MENU_TYPE_PROMPT;
		ms->backMenu = 100;
		MenuSystem_setYesNo(menu, S(menuId == 0x70 ? MOD_S_LOAD_GAME_Q : MOD_S_RESTART_LEVEL_Q), 1);
		break;
	case 0x71:
		ms->type = MENU_TYPE_PROMPT;
		ms->selected = 2;
		MenuSystem_addItem(menu, S(MOD_S_NO_SAVED_GAME), NULL, ITEM_PROMPT_LINE, 0, 0, NULL);
		MenuSystem_addItem(menu, NULL, NULL, 0, 0, 0, NULL);
		addChoice(menu, MOD_S_BACK_WIDE, 0);
		break;
	case 0x72: /* died */
		ms->type = MENU_TYPE_PROMPT;
		ms->backMenu = 0x72;
		MenuSystem_addItem(menu, S(MOD_S_TRY_AGAIN), NULL, ITEM_PROMPT_LINE, 0, 0, NULL);
		MenuSystem_addItem(menu, NULL, NULL, 0, 0, 0, NULL);
		addChoice(menu, MOD_S_LOAD_SAVED_GAME, 0);
		addChoice(menu, MOD_S_RESTART_LEVEL_PAD, 1);
		addChoice(menu, MOD_S_MAIN_MENU_PAD, 2);
		break;

	/* ---- developer ---- */
	case 200:
		buildDeveloper(menu, ms);
		break;
	case 0xc9:
		buildChangeMap(menu, ms);
		break;
	case 0xca:
		buildCheats(menu, ms);
		break;
	case 0xcb:
		buildMemory(menu, ms);
		break;
	case 0xcc:
		buildDevTools(menu, ms);
		break;
	case 0xcd:
		buildVars(menu, ms);
		break;

	/* ---- Gaya's store ---- */
	case 0x12e:
		buildStore(menu, ms);
		break;
	case 0x12f:
		buildStoreWeapons(menu, ms);
		break;
	case 0x130:
		buildStoreArmor(menu, ms);
		break;
	case 0x131:
		buildStoreRings(menu, ms);
		break;
	case 0x132:
		buildStorePotions(menu, ms);
		break;
	case 0x133:
		buildOffer(menu, ms);
		break;
	case 0x134:
		ms->type = MENU_TYPE_PROMPT;
		MenuSystem_addItem(menu, S(MOD_S_OFFER_EXCEEDS), NULL, ITEM_PROMPT_LINE, 0, 0, NULL);
		addMessage(menu, ms, MOD_S_AVAILABLE_GOLD);
		break;
	case 0x135:
	case 0x136:
		ms->type = MENU_TYPE_PROMPT;
		MenuSystem_addItem(menu, S(MOD_S_OFFER_WAS), NULL, ITEM_PROMPT_LINE, 0, 0, NULL);
		addMessage(menu, ms, menuId == 0x135 ? MOD_S_ACCEPTED : MOD_S_REJECTED);
		break;
	case 0x137:
		ms->type = MENU_TYPE_PROMPT;
		MenuSystem_addItem(menu, S(MOD_S_CANNOT_HOLD), NULL, ITEM_PROMPT_LINE, 0, 0, NULL);
		addMessage(menu, ms, MOD_S_OF_THAT_ITEM);
		break;

	/* ---- inventory ---- */
	case 400:
		buildDetails(menu, ms);
		break;
	case 500:
		buildItems(menu, ms);
		break;
	case 0x1f5:
		buildWeapons(menu, ms);
		break;
	case 0x1f6:
		buildRings(menu, ms);
		break;
	case 0x1f7:
		buildPotions(menu, ms);
		break;
	case 0x1f8:
		buildOther(menu, ms);
		break;
	case 0x1f9: {
		EntityDef *def = EntityDefs_find(menu->app->entityDefs, 6, 0, (int8_t)menu->useItem);
		App_snprintf(buf, sizeof buf, S(MOD_S_FMT_USE), def->name);
		MenuSystem_setYesNo(menu, buf, 1);
		ms->type = MENU_TYPE_PROMPT;
		ms->scroll = 0;
		break;
	}
	case 0x1fa:
	case 0x1fb:
		ms->type = MENU_TYPE_PROMPT;
		MenuSystem_addItem(menu, S(MOD_S_CANNOT_DRINK), NULL, ITEM_PROMPT_LINE, 0, 0, NULL);
		addMessage(menu, ms, menuId == 0x1fa ? MOD_S_HEALTH_MAX : MOD_S_EFFECT_MAX);
		break;
	case 0x1fc:
		ms->type = MENU_TYPE_PROMPT;
		MenuSystem_addItem(menu, S(MOD_S_CANNOT_USE_KIT), NULL, ITEM_PROMPT_LINE, 0, 0, NULL);
		addMessage(menu, ms, MOD_S_ARMOR_MAX);
		break;

	default: /* 0, 0xb, 0x6f and unknown ids: no items */
		break;
	}
}
