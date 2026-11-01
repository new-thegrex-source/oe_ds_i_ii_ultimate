/*
 * MenuSystem (Applet+0x13c): the active menu's item list, selection, scrolling,
 * key handling (lists, number entry, volume slider, cheat codes), opening and
 * closing menus, and drawing.
 *
 * Menu ids < 0x12 are front-end menus (drawn over the grey map or the scrolling
 * world-map picture); ids >= 100 are in-game menus.
 */

#include "game.h"

/* Data in the executable (read from the user's .mod). */
#define MOD_CURSOR_BOB    0x47168u /* 4 x s8: horizontal cursor bob, one step per 100 ms */
#define MOD_STR_STRIKE    0x45ddcu /* "\x80": glyph drawn over disabled items */
#define MOD_STR_ARMOR     0x45e00u /* "\x86": armour icon glyph (store / inventory header) */
#define MOD_STR_HEALTH    0x45e04u /* "\x85": health icon glyph */
#define MOD_STR_DIGITS    0x45e08u /* "____________": number-entry field */
#define MOD_STR_SLIDER    0x45e18u /* "<     >": volume slider arrows */
#define MOD_STR_NO_MAP    0x46700u /* fatal error texts */
#define MOD_STR_NO_LOGO   0x4671cu

#define COLOR_BLACK   ((int)0xff000000u)
#define COLOR_FRAME   ((int)0xff555555u)
#define TEXT_GREY     0xad55 /* disabled item (RGB565) */
#define TEXT_HEADER   0xd6da /* section header / gold line (RGB565) */

enum { TEXT_GROUP_MENU = 3 };

/* Menu types (MenuSystem.type). */
enum {
	MENU_TYPE_LIST = 1, MENU_TYPE_LOGO = 4, MENU_TYPE_TEXT = 5, MENU_TYPE_PROMPT = 6,
	MENU_TYPE_DETAILS = 7, MENU_TYPE_OPTIONS = 8, MENU_TYPE_MOREGAMES = 9
};

/* Item flags (MenuItem.flags). */
enum {
	ITEM_UNSELECTABLE = 0x1, ITEM_DISABLED = 0x4, ITEM_CENTERED = 0x8, ITEM_NUMBER = 0x10,
	ITEM_DETAILS = 0x20, ITEM_HEADER = 0x40, ITEM_SLIDER = 0x80, ITEM_LARGE = 0x100,
	ITEM_HIGHLIGHT = 0x200, ITEM_INFO = 0x400
};

static char *mod_cstr(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

static int cursorBob(void)
{
	int step = (g_app->time / 100) % 4;
	return ModData_s8(&g_mod, MOD_CURSOR_BOB + step);
}

/* Store and barter screens (and the screens they return to) use the scroll frame. */
static int isStoreMenu(int id)
{
	return id >= 0x12d && id < 0x139;
}

/* 0x0003da88 */
int MenuSystem_startup(MenuSystem *ms)
{
	Applet *app = g_app;

	Image_load(app, 8, &ms->imgCursor, 1);
	Image_load(app, 0x26, &ms->imgScroll, 1);
	Image_load(app, 0x25, &ms->imgLogo, 1);
	ms->numItems = 0;
	ms->imgBG = NULL;
	ms->menu = 0;
	ms->backMenu = -1;
	ms->oldMenu = 0;
	ms->selectedIndex = 0;
	ms->scrollIndex = 0;
	ms->type = 0;
	ms->visibleItems = 0;
	ms->cheatCombo = 0;
	ms->digitCount = 0;
	ms->animPeriod = 0x28;
	ms->digitEntry = 0;

	/* Pan path over the front-end world-map picture: 12 keys {x, y, duration ms}
	 * (immediates in the original code). */
	static const int32_t keys[36] = {
		0xa0, 0x52, 0x1042,   0x90, 0x90, 0xba6,    0xa0, 0xbb, 0xe94,    0xcf, 0xdc, 0xbef,
		0xec, 0x101, 0x96c,   0xe3, 0x125, 0x133f,  0xa6, 0x152, 0x11a0,  0x78, 0x186, 0xdc5,
		0x73, 0x1bc, 0x1179,  0x9b, 500, 0xf36,     0xaa, 0x22e, 0x2469,  0xa0, 700, 4000,
	};
	for (int i = 0; i < 36; i++)
		ms->bgKeys[i] = keys[i];
	ms->numBgKeys = 12;
	MenuSystem_adjustPoints(ms);
	ms->bgKeyIdx = 0;
	ms->bgNextKey = 1;
	return 1;
}

/* 0x00005968 Turn the pan keys (picture coordinates of the view centre) into
 * top-left offsets for this screen size. */
void MenuSystem_adjustPoints(MenuSystem *menu)
{
	Canvas *cv = g_app->canvas;

	for (int i = 0; i < menu->numBgKeys; i++) {
		int32_t *k = &menu->bgKeys[i * 3];
		int right = k[0] + cv->screenCenterX;
		if (right - 300 > 0)
			k[0] -= right - 0x127;
		int left = k[0] - cv->screenCenterX;
		if (left < 0)
			k[0] += left;
		k[0] -= cv->screenCenterX;
		k[1] -= cv->screenCenterY;
		if (k[0] < 0)
			k[0] = 0;
		if (k[1] < 0)
			k[1] = 0;
	}
}

/* Release the BREW animated images of the More Games screen. The port never
 * loads them (ISHELL_LoadResObject "anim%d.bci" has no PC equivalent), so the
 * slots stay NULL. */
static void releaseMoreGamesAnims(MenuSystem *ms)
{
	for (int i = 0; i < 2; i++)
		ms->anim[i] = NULL;
}

/* 0x0003c068 */
void MenuSystem_shutdown(MenuSystem *ms)
{
	Applet *app = g_app;

	App_freeImage(app, &ms->imgCursor);
	App_freeImage(app, &ms->imgScroll);
	App_freeImage(app, &ms->imgLogo);
	App_freeImage(app, &ms->imgMenuMap);
	App_freeImage(app, &ms->imgMoreGames);
	App_freeImage(app, &ms->img8);
	App_freeImage(app, &ms->img9);
	releaseMoreGamesAnims(ms);
	for (int i = 0; i < 96; i++) {
		String_release(&ms->items[i].text);
		String_release(&ms->items[i].text2);
		String_release(&ms->items[i].help);
	}
}

/* 0x00027638 Front-end pictures: the scrolling world map and the logo. */
void MenuSystem_loadMainImages(void)
{
	Applet *app = g_app;
	MenuSystem *ms = app->menuSystem;

	if (ms->imgMenuMap.bitmap == NULL) {
		Image_load(app, 0x22, &ms->imgMenuMap, 0);
		ms->bgKeyIdx = 0;
		ms->bgNextKey = 1;
		app->game->cameraStartTime = (int32_t)GETUPTIMEMS();
		if (ms->imgMenuMap.bitmap == NULL)
			App_fatalError(app, mod_cstr(MOD_STR_NO_MAP));
	}
	if (ms->imgLogo.bitmap == NULL) {
		Image_load(app, 0x25, &ms->imgLogo, 1);
		if (ms->imgLogo.bitmap == NULL)
			App_fatalError(app, mod_cstr(MOD_STR_NO_LOGO));
	}
}

/* 0x0002772c More Games page: arrows, page picture, title/caption/footer texts
 * kept in items 0..2. */
void MenuSystem_loadMoreGames(MenuSystem *ms, int page)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;

	if (ms->img8.bitmap == NULL) {
		Image_load(app, 8, &ms->img8, 1);
		Image_load(app, 9, &ms->img9, 1);
		for (int i = 0; i < 2; i++) {
			char name[32];
			App_snprintf(name, sizeof name, "anim%d.bci", i);
			ms->anim[i] = NULL; /* BREW animated image: not available on PC */
		}
		ms->animFrame = 0;
	}

	if (ms->moreGamesPage != page) {
		if (ms->imgMoreGames.bitmap != NULL)
			App_freeImage(app, &ms->imgMoreGames);
		Image_load(app, (page + 0x23) & 0xffff, &ms->imgMoreGames, 1);
	} else if (ms->imgMoreGames.bitmap == NULL) {
		Image_load(app, (page + 0x23) & 0xffff, &ms->imgMoreGames, 1);
	}

	String_setLength(&ms->items[0].text, 0);
	String_setLength(&ms->items[0].help, 0);
	Text_getString(cv, TEXT_GROUP_MENU, 0x44, &ms->items[0].text);
	String_setLength(&ms->items[1].text, 0);
	String_setLength(&ms->items[1].help, 0);
	Text_getString(cv, TEXT_GROUP_MENU, (int16_t)(page + 0x160), &ms->items[1].text);
	String_setLength(&ms->items[2].text, 0);
	String_setLength(&ms->items[2].help, 0);
	Text_getString(cv, TEXT_GROUP_MENU, 0x15e, &ms->items[2].text);
	ms->moreGamesPage = page;
}

/* 0x0003cb08 Menu "tick" sound, except on text pages and the sound prompt. */
void MenuSystem_playOpenSound(MenuSystem *ms)
{
	if (ms->type != MENU_TYPE_TEXT && ms->type != MENU_TYPE_DETAILS && ms->menu != 0x14)
		Sound_play(g_app->sndSys, 0x15, 0, 2, -1);
}

/* 0x0001356c True while the grey 3D map is the menu background. */
int MenuSystem_isBaseMenu(MenuSystem *menuSys)
{
	return menuSys->menu < 2 && g_app->canvas->mapId != 7;
}

/* Tail shared by MenuSystem_setMenu: tidy up the More Games screen when leaving
 * or entering it. */
static void moreGamesTransition(MenuSystem *ms, int oldMenu, int menuId)
{
	if (oldMenu == 0x17) {
		if (menuId == 0x18) {
			ms->animFrame = 0;
			return;
		}
		Applet *app = g_app;
		App_freeImage(app, &ms->imgMoreGames);
		App_freeImage(app, &ms->img8);
		App_freeImage(app, &ms->img9);
		releaseMoreGamesAnims(ms);
		return;
	}
	if (oldMenu == 0x18) {
		/* the original stops the page animation here (IImage_Stop); none in the port */
		return;
	}
	if (menuId == 0x18)
		ms->animFrame = 0;
}

/* 0x0003a504 Open menu menuId (0 closes the menu and resumes play). */
void MenuSystem_setMenu(MenuSystem *ms, int menuId)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	int oldMenu = ms->menu;

	ms->cheatCombo = 0;
	ms->digitCount = 0;

	if (menuId == 0 || menuId > 0x11) {
		if (oldMenu > 0 && oldMenu < 0x12) {
			/* leaving the front end: restore the in-game view */
			app->render->palettesLoaded = 0;
			Render_setScreenRect(app->render, cv->renderRect);
		} else if (oldMenu < 0) {
			Graphics_setColor(cv->graphics, COLOR_BLACK);
			Graphics_fillRect(cv->graphics, 0, 0, cv->screenRect[2], cv->screenRect[3]);
		}
	} else if (oldMenu == 0 || oldMenu > 0x11) {
		/* entering the front end */
		if (menuId < 2) {
			cv->fadeStart = 0;
			if (cv->mapId != 7) {
				Render_makeGrayscale(app->render);
				Render_setScreenRect(app->render, cv->viewRect);
				int tile = app->render->spawnTile;
				if (tile != 0) {
					cv->viewX = (tile % 32) * 64 + 32;
					cv->viewY = (tile / 32) * 64 + 32;
				}
			}
		} else if (oldMenu != 0x17) {
			Sound_play(app->sndSys, 0x56, 3, 5, -1); /* title music */
		}
	}
	moreGamesTransition(ms, oldMenu, menuId);

	ms->menu = menuId;
	ms->oldMenu = oldMenu;

	if (menuId == 0) {
		ms->numItems = 0;
		String_setLength(&app->hud->str1228, 0);
		if (cv->state != 2)
			return;
		Canvas_setState(cv, 3);
		Player_selectWeapon(app->player, app->player->ce.weapon);
		return;
	}

	Menu_initMenu(app->menu, menuId);

	/* make sure the remembered selection is a selectable item */
	if (ms->selectedIndex < ms->numItems) {
		MenuItem *it = &ms->items[ms->selectedIndex];
		if (String_length(&it->text) == 0 || (it->flags & ITEM_UNSELECTABLE) != 0)
			MenuSystem_moveSelection(ms, 1);
	} else {
		MenuSystem_moveSelection(ms, -1);
	}
	Canvas_setState(cv, 2);
}

/* 0x00007c0c Back / Clear key. */
void MenuSystem_back(MenuSystem *menu)
{
	Applet *app = g_app;
	int cur = menu->menu;

	if (cur == 3 || cur == 0x14) {
		Applet_shutdown(app);
		return;
	}
	int target = menu->backMenu;
	if (target == -1 || target == cur)
		return;
	if (menu->digitEntry == 0) {
		if (target == 3 && cur == 200)
			Canvas_backToMain(app->canvas, 0);
		else
			MenuSystem_setMenu(menu, target);
	} else {
		menu->digitEntry = 0;
	}
	MenuSystem_playOpenSound(menu);
}

/* 0x0002c9ac Move the selection by delta, skipping blank and unselectable rows
 * (wrapping around), and keep it inside the visible window. Text pages just
 * scroll. */
void MenuSystem_moveSelection(MenuSystem *menu, int delta)
{
	if (menu->menu == 0x18)
		return;
	/* More Games (0x17) fills items[0..2] without counting them, so setMenu
	 * arrives here with numItems 0. The original then walks backwards from
	 * items[-1] through the MenuSystem header looking for a "selectable" row;
	 * in C that is out of bounds, so park the selection on row 0 instead,
	 * with no flags (row 0 may still carry 0x400 from the 0x18 text page,
	 * which would disable the Select soft key). */
	if (menu->numItems <= 0) {
		menu->selectedIndex = 0;
		menu->items[0].flags = 0;
		return;
	}

	if (menu->type == MENU_TYPE_TEXT || menu->type == MENU_TYPE_DETAILS) {
		if (delta < 0) {
			if (menu->scrollIndex >= 1)
				menu->scrollIndex += delta;
		} else if (delta > 0) {
			if (menu->numItems - menu->visibleItems > menu->scrollIndex)
				menu->scrollIndex += delta;
		}
		menu->selectedIndex = menu->scrollIndex;
		return;
	}

	for (;;) {
		menu->selectedIndex += delta;
		if (menu->selectedIndex >= menu->numItems || menu->selectedIndex < 0)
			break;
		MenuItem *it = &menu->items[menu->selectedIndex];
		if (String_length(&it->text) != 0 && (it->flags & ITEM_UNSELECTABLE) == 0)
			goto placed;
	}
	/* wrapped past an end */
	{
		int idx = (menu->selectedIndex < 0) ? menu->numItems - 1 : 0;
		for (;;) {
			menu->selectedIndex = idx;
			MenuItem *it = &menu->items[idx];
			if (String_length(&it->text) != 0 && (it->flags & ITEM_UNSELECTABLE) == 0)
				break;
			idx = menu->selectedIndex + delta;
		}
	}

placed:
	if (menu->visibleItems == 0)
		return;
	int sel = menu->selectedIndex;
	if (delta < 0) {
		int top = sel - menu->visibleItems + 1;
		if (top > menu->scrollIndex) {
			menu->scrollIndex = top;
			return;
		}
	} else if (sel > menu->scrollIndex + menu->visibleItems - 1) {
		menu->scrollIndex = sel - menu->visibleItems + 1;
		return;
	}
	if (sel < menu->scrollIndex)
		menu->scrollIndex = sel;
}

/* 0x00037f34 */
void MenuSystem_scrollDown(MenuSystem *ms)
{
	if (ms->type == MENU_TYPE_MOREGAMES)
		return;
	int old = ms->selectedIndex;
	MenuSystem_moveSelection(ms, 1);
	if (ms->selectedIndex != old)
		MenuSystem_playOpenSound(ms);
}

/* 0x000380d8 */
void MenuSystem_scrollUp(MenuSystem *ms)
{
	if (ms->type == MENU_TYPE_MOREGAMES)
		return;
	int old = ms->selectedIndex;
	MenuSystem_moveSelection(ms, -1);
	if (ms->selectedIndex != old)
		MenuSystem_playOpenSound(ms);
}

/* 0x00037f78 Page down (More Games: next page). Stops at the last item instead
 * of wrapping. */
void MenuSystem_scrollPageDown(MenuSystem *ms)
{
	if (ms->type == MENU_TYPE_MOREGAMES) {
		MenuSystem_playOpenSound(g_app->menuSystem);
		MenuSystem_loadMoreGames(ms, ms->moreGamesPage == 1 ? 0 : ms->moreGamesPage + 1);
		return;
	}
	int start = ms->selectedIndex;
	for (int i = 0; i < ms->visibleItems; i++) {
		if (ms->numItems - 1 == ms->selectedIndex)
			return;
		MenuSystem_moveSelection(ms, 1);
		if (ms->selectedIndex < start) {
			MenuSystem_moveSelection(ms, -1);
			return;
		}
	}
}

/* 0x0003802c Page up (More Games: previous page). */
void MenuSystem_scrollPageUp(MenuSystem *ms)
{
	if (ms->type == MENU_TYPE_MOREGAMES) {
		MenuSystem_playOpenSound(g_app->menuSystem);
		MenuSystem_loadMoreGames(ms, ms->moreGamesPage == 0 ? 1 : ms->moreGamesPage - 1);
		return;
	}
	int start = ms->selectedIndex;
	for (int i = 0; i < ms->visibleItems; i++) {
		if (ms->selectedIndex == 0)
			return;
		MenuSystem_moveSelection(ms, -1);
		if (start <= ms->selectedIndex) {
			MenuSystem_moveSelection(ms, 1);
			return;
		}
	}
}

/* 0x0000c538 Select key: item details (flag 0x20) or activate. */
int MenuSystem_select(MenuSystem *menu)
{
	Applet *app = g_app;

	if (menu->type == MENU_TYPE_TEXT || menu->type == MENU_TYPE_DETAILS)
		return menu->type;
	menu->cheatCombo = 0;
	menu->digitCount = 0;
	uint32_t flags = menu->items[menu->selectedIndex].flags;
	if (flags & ITEM_DETAILS) {
		Sound_play(app->sndSys, 6, 0, 2, -1);
		Menus_showDetails(app->menu, menu->menu, menu->selectedIndex);
		return 1;
	}
	if (flags & ITEM_INFO)
		return (int)flags;
	return MenuSystem_activateItem(menu);
}

/* 0x0000c5e0 Activate the selected item: start number entry / the volume
 * slider, or run the menu action and open the menu it returns. */
int MenuSystem_activateItem(MenuSystem *menu)
{
	Applet *app = g_app;
	int next;

	menu->cheatCombo = 0;
	menu->digitCount = 0;

	if (menu->type == MENU_TYPE_MOREGAMES) {
		MenuSystem_playOpenSound(app->menuSystem);
		next = 0x18;
	} else {
		if (menu->items[menu->selectedIndex].flags & ITEM_DISABLED)
			return 0;
		if (menu->menu != 0x14)
			Sound_play(app->sndSys, 6, 0, 2, -1);

		int sel = menu->selectedIndex;
		uint32_t flags = menu->items[sel].flags;
		if (flags & ITEM_NUMBER) {
			menu->digitEntry = 1;
			menu->f1760 = 0;
			menu->items[sel].action = 0;
			String_setLength(&menu->items[menu->selectedIndex].text2, 0);
			return 1;
		}
		if (flags & ITEM_SLIDER) {
			menu->sliderEditing = 1;
			return 1;
		}
		next = Menus_select(app->menu, menu->menu, sel);
		if (next < 0 || menu->menu == next) {
			if (menu->items[menu->selectedIndex].flags & ITEM_DETAILS) {
				MenuSystem_select(menu);
				return 1;
			}
			return 0;
		}
	}
	MenuSystem_setMenu(menu, next);
	return 0;
}

/* 0x0000c1c0 Keys while typing a number (phone number 0x97, barter offer 0x135). */
void MenuSystem_handleNumberEntry(MenuSystem *menu, int key)
{
	Applet *app = g_app;
	Menu *menus = app->menu;
	int sel = menu->selectedIndex;
	MenuItem *item = &menu->items[sel];
	String *field = &item->text2;
	String tmp;
	int next = -1;

	/* the field is kept exactly maxDigits wide, right-aligned */
	while (String_length(field) < menu->maxDigits) {
		String_ctorCStr(&tmp, " ");
		String_insert(field, 0, &tmp);
		String_dtor(&tmp, 0);
	}
	while (String_length(field) > menu->maxDigits)
		String_deleteCharAt(field, 0);

	unsigned digit = (unsigned)(key - AVK_0);
	if (digit < 10) {
		if (menus->offerActive == 0) {
			String_trim(&tmp, field);
			int used = String_length(&tmp);
			String_dtor(&tmp, 0);
			if (menu->maxDigits <= used)
				return;
			String_deleteCharAt(field, 0);
			String_appendInt(field, (int)digit);
			item->action = String_toInt(field);
			return;
		}
		/* the first digit replaces the suggested price */
		String_setLength(field, 0);
		String_appendInt(field, (int)digit);
		item->action = String_toInt(field);
		menus->offerActive = 0;
		return;
	}

	if (key == AVK_SELECT || key == AVK_POUND || key == AVK_SOFT2) {
		String_trim(&tmp, field);
		int len = String_length(&tmp);
		String_dtor(&tmp, 0);
		if (len == 0)
			return;
		int id = menu->menu;
		if (id == 0x97 && len != 10)
			return;
		menu->digitEntry = 0;
		if (id == 0x135)
			menu->offerValue = menu->items[menu->selectedIndex].action;
		next = Menus_select(menus, id, menu->selectedIndex);
	} else if (key == AVK_CLR || key == AVK_STAR) {
		if (menus->offerActive != 0) {
			String_setLength(field, 0);
			item->action = 0;
			menus->offerActive = 0;
			return;
		}
		/* delete the last digit */
		String_ctorCStr(&tmp, " ");
		String_insert(field, 0, &tmp);
		String_dtor(&tmp, 0);
		String_setLength(field, menu->maxDigits);
		int old = item->action;
		item->action = String_toInt(field);
		if (item->action != 0)
			return;
		if (old != 0)
			return;
		next = menu->backMenu; /* clear on an empty field leaves */
	} else if (key == AVK_SOFT1) {
		next = menu->backMenu;
	}

	if (next >= 0 && menu->menu != next) {
		menu->digitEntry = 0;
		MenuSystem_setMenu(menu, next);
	}
}

/* 0x0000c728 Keys while the volume slider is active. */
void MenuSystem_handleVolumeKey(MenuSystem *menu, int key)
{
	Applet *app = g_app;
	int sel = menu->selectedIndex;
	int action = Canvas_getKeyAction(app->canvas, key);

	if (action == 6 || action == 0xf || action == 5) {
		menu->sliderEditing = 0;
		return;
	}
	if (action == 3)
		Sound_volumeDown(app->sndSys, 10);
	else if (action == 4)
		Sound_volumeUp(app->sndSys, 10);
	else
		return;

	String *s = &menu->items[sel].text2;
	String_setLength(s, 0);
	String_appendChar(String_appendInt(s, app->sndSys->volume), '%');
	MenuSystem_playOpenSound(menu);
	Game_saveConfig(app->game);
}

/* "Give everything" cheat (same as debug cheat menu item 8). */
static void cheatGiveAll(Applet *app)
{
	Player *pl = app->player;
	int i;

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
}

/* 0x000142bc Digit keys in a menu build a code (first digit = units):
 * 3666 debug menu, 1666 skip level, 4332 give all, 3366 benchmark spin. */
int MenuSystem_enterDigit(MenuSystem *menuSys, int digit)
{
	Applet *app = g_app;
	int scale = 1;

	for (int i = 0; i < menuSys->digitCount; i++)
		scale *= 10;
	menuSys->cheatCombo += scale * digit;

	switch (menuSys->cheatCombo) {
	case 6663:
		menuSys->cheatCombo = 0;
		menuSys->digitCount = 0;
		menuSys->selectedIndex = 0;
		menuSys->scrollIndex = 0;
		MenuSystem_setMenu(menuSys, 200);
		return 1;
	case 6661:
		app->game->skipLevel = 1;
		Canvas_loadMap(app->canvas, app->canvas->mapId);
		return 1;
	case 2334:
		cheatGiveAll(app);
		CombatEntity_setHealth(&app->player->ce, CombatEntity_getMaxHealth(&app->player->ce));
		break;
	case 6633:
		menuSys->cheatCombo = 0;
		menuSys->digitCount = 0;
		if (menuSys->menu > 99)
			Canvas_enterState9(app->canvas, 0);
		break;
	}
	menuSys->digitCount++;
	return 0;
}

/* 0x00002eb8 Debug "hang" test (developer menu): never returns. */
void MenuSystem_infiniteLoop(void)
{
	Applet *volatile sink;
	for (;;)
		sink = g_app;
	(void)sink;
}

/* 0x00037ce4 Draw the world-map picture panned along the key path (Catmull-Rom
 * between keys); the 600-pixel-high picture wraps vertically. */
void MenuSystem_scrollBackground(MenuSystem *ms)
{
	Applet *app = g_app;
	Game *game = app->game;
	uint32_t elapsed = GETUPTIMEMS() - (uint32_t)game->cameraStartTime;
	int duration = ms->bgKeys[ms->bgKeyIdx * 3 + 2];

	/* t = elapsed / duration in 16.16 */
	int64_t num = (int64_t)(int32_t)(elapsed << 16) * 65536;
	int64_t den = (int32_t)((uint32_t)duration << 16);
	int t = (int)(num / den);

	if (duration <= (int)elapsed) {
		t = 0;
		game->cameraStartTime = (int32_t)GETUPTIMEMS();
		ms->bgKeyIdx++;
		ms->bgNextKey++;
		if (ms->bgNextKey >= ms->numBgKeys) {
			ms->bgKeyIdx = 0;
			ms->bgNextKey = 1;
		}
	}

	int cur = ms->bgKeyIdx;
	int nxt = ms->bgNextKey;
	int x1 = ms->bgKeys[cur * 3], y1 = ms->bgKeys[cur * 3 + 1];
	int x2 = ms->bgKeys[nxt * 3], y2 = ms->bgKeys[nxt * 3 + 1];
	int x3 = x2, y3 = y2;
	int x0 = x1, y0 = y1;
	if (nxt < ms->numBgKeys - 2) {
		x3 = ms->bgKeys[nxt * 3 + 3];
		y3 = ms->bgKeys[nxt * 3 + 4];
	}
	if (cur != 0) {
		x0 = ms->bgKeys[cur * 3 - 3];
		y0 = ms->bgKeys[cur * 3 - 2];
	}

	int x = Camera_spline(game->cameras, t, x0, x1, x2, x3);
	int y = Camera_spline(game->cameras, t, y0, y1, y2, y3);
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x >= 300)
		x = 299;
	if (y >= 600)
		y = 599;

	Canvas *cv = app->canvas;
	int w = cv->viewRect[2];
	int h = cv->viewRect[3];
	int below = 600 - y;
	if (below <= h) {
		/* the part past the bottom edge comes from the top of the picture */
		Canvas_drawRegion(cv, &ms->imgMenuMap, x, 0, w, h - below, 0, 0, below, 0, 0, 0);
		h = below;
	}
	Canvas_drawRegion(cv, &ms->imgMenuMap, x, y, w, h, 0, 0, 0, 0, 0, 0);
}

/* The "More Games" page. */
static void paintMoreGames(MenuSystem *menu)
{
	Canvas *cv = g_app->canvas;

	MenuSystem_scrollBackground(menu);
	Canvas_drawStringFlags(cv, &menu->items[0].text, cv->screenCenterX, 5, 0x10, 1);
	Canvas_drawImage(cv, &menu->imgMoreGames, cv->screenCenterX, 0x23, 0x11, 0);
	int x = cursorBob() + (int)((uint32_t)(cv->viewRect[2] - menu->imgMoreGames.width) >> 2);
	Canvas_drawImage(cv, &menu->img9, x, 0x49, 0x11, 0);
	Canvas_drawImage(cv, &menu->img8, cv->viewRect[2] - x, 0x49, 0x11, 0);
	Canvas_drawString(cv, &menu->items[1].text, cv->screenCenterX, 0x87, 0x10);
	Canvas_drawStringFlags(cv, &menu->items[2].text, cv->screenCenterX, 0x9b, 0x10, 8);
}

/* Background for front-end menus over the grey 3D map: slow spin (or the
 * cinematic camera), optional profiling line, fade. */
static void paintBaseMapBackground(MenuSystem *menu)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	Game *game = app->game;

	if (Game_isCinematicActive(game) == 0) {
		int angle = (app->time / menu->animPeriod) & 0x3ff;
		cv->viewAngle = angle;
		Render_render(app->render, cv->viewX, cv->viewY, 0x24, angle);
	} else {
		int t = (int)GETUPTIMEMS() - game->cameraStartTime;
		Camera_update(game->activeCamera, game->cameraKeyIdx, t);
		Camera_apply(game->activeCamera);
	}

	if (cv->profiling != 0) {
		int frames = cv->profileSums[0];
		int total = cv->profileSums[1];
		String s;
		String_ctor(&s);
		String_appendCStr_76f0(&s, "Avg: ");
		/* unguarded division in the original */
		int avg = frames ? total / frames : 0;
		String_appendInt(&s, avg);
		String_appendCStr_76f0(&s, ".");
		String_appendInt(&s, (frames ? total * 100 / frames : 0) - avg * 100);
		String_appendCStr_76f0(&s, "ms");
		Canvas_drawString(cv, &s, 0, cv->viewRect[3] - 0xc, 0);
		String_dtor(&s, 0);
	}
	Canvas_updateFade(cv);
}

/* "<health> hp/max <armour> ap/max" header line of the store and inventory screens. */
static void paintStatusHeader(int top)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	CombatEntity *ce = &app->player->ce;
	String *s = &cv->scratch;

	String_setLength(s, 0);
	int maxArmor = CombatEntity_getMaxArmorPoints(ce);
	int armor = CombatEntity_getArmorPoints(ce);
	int maxHp = CombatEntity_getMaxHealth(ce);
	int hp = CombatEntity_getHealth(ce);
	String_appendCStr_76f0(s, mod_cstr(MOD_STR_HEALTH));
	String_appendCStr_76f0(s, " ");
	String_appendInt(s, hp);
	String_appendCStr_76f0(s, "/");
	String_appendInt(s, maxHp);
	String_appendCStr_76f0(s, " ");
	String_appendCStr_76f0(s, mod_cstr(MOD_STR_ARMOR));
	String_appendCStr_76f0(s, " ");
	String_appendInt(s, armor);
	String_appendCStr_76f0(s, "/");
	String_appendInt(s, maxArmor);
	Canvas_drawStringRange(cv, s, cv->viewRect[0] + (cv->viewRect[2] >> 1), top + 3, 0x30, 0, -1, 0);
	Graphics_setColor(cv->graphics, COLOR_BLACK);
	Graphics_drawLine(cv->graphics, cv->viewRect[0], top + 10, cv->viewRect[2], top + 10);
}

/* 0x0002d4fc Draw the current menu. */
void MenuSystem_paint(MenuSystem *menu)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;

	if (menu->menu == 0x18) {
		if (menu->anim[menu->moreGamesPage] != NULL) {
			/* animated BREW image page (never loaded in the port) */
			if (menu->animFrame != 0)
				return;
			menu->animFrame = 1;
			return;
		}
	} else if (menu->menu == 0x66 || menu->menu == 0x11) {
		/* volume may have been changed with the side keys */
		if (app->menu->volume != app->sndSys->volume) {
			app->menu->volume = app->sndSys->volume;
			Menus_setVolumeItem(app->menu, app->menu->soundItem);
		}
	}

	Graphics_setColor(cv->graphics, COLOR_BLACK);
	Graphics_fillRect(cv->graphics, 0, 0, cv->screenRect[2], cv->screenRect[3]);

	if (menu->type == MENU_TYPE_MOREGAMES) {
		paintMoreGames(menu);
		return;
	}

	/* ---- background ---- */
	int top = 1;
	MenuItem *selItem = &menu->items[menu->selectedIndex];
	if (isStoreMenu(menu->menu) || isStoreMenu(menu->backMenu)) {
		Canvas_drawDialogFrame(cv, 0, 0, cv->viewRect[2], cv->viewRect[3], 1);
		top = 0xd;
	} else if (menu->menu < 100) {
		if (MenuSystem_isBaseMenu(menu) == 0) {
			if (menu->imgMenuMap.bitmap != NULL && menu->menu > 2 && menu->menu < 0x15)
				MenuSystem_scrollBackground(menu);
		} else {
			paintBaseMapBackground(menu);
		}
		if (menu->imgBG != NULL)
			Canvas_drawImage(cv, menu->imgBG, cv->screenCenterX, 1, 0x11, 0);
	} else {
		Canvas_drawDialogFrame(cv, 0, 0, cv->viewRect[2], cv->viewRect[3], 0);
		top = 0xf;
	}

	/* ---- status header on store / inventory screens ---- */
	int y = top;
	int id = menu->menu;
	if ((id >= 0x12c && id <= 0x133) || (id >= 500 && id <= 0x1fd)) {
		y = top + 0xc;
		paintStatusHeader(top);
	}

	/* ---- layout ---- */
	int leftX;
	switch (menu->type) {
	case 2:
		leftX = 0x5a;
		break;
	case 3:
		leftX = 0x32;
		break;
	case MENU_TYPE_LIST:
	case MENU_TYPE_TEXT:
	case MENU_TYPE_DETAILS:
		leftX = 0xd;
		break;
	default:
		if (menu->menu == 0x18) {
			MenuSystem_scrollBackground(menu);
			Canvas_drawImage(cv, &menu->imgMoreGames, cv->screenCenterX, 0x23, 0x11, 0);
			leftX = 0;
			y = (cv->renderRect[3] - app->menuSystem->numItems * 12) / 2 + 0x4e;
		} else if (menu->type == MENU_TYPE_PROMPT) {
			leftX = 0;
			y = (cv->renderRect[3] - app->menuSystem->numItems * 12) / 2;
		} else {
			leftX = (menu->type == MENU_TYPE_OPTIONS) ? 0xd : 0x28;
			y = 0x50;
		}
		break;
	}

	int showHelp = 0;
	id = menu->menu;
	if (id != 0x12 && id != 1 && id != 0x18 && String_length(&selItem->help) > 0)
		showHelp = 1;
	int viewH = cv->viewRect[3];
	int storeFooter = id >= 0x12d && (id < 0x139 || id == 500);

	int avail;
	if (storeFooter)
		avail = viewH - y - 0x30;
	else if (showHelp)
		avail = viewH - y - 0x28;
	else if (id < 100)
		avail = viewH - y;
	else
		avail = viewH - 0x1a;
	menu->visibleItems = avail / 12;
	if (id > 4 && id < 0xd)
		y = (viewH - menu->visibleItems * 12) >> 1;

	int visible = menu->visibleItems;
	int hasScrollBar = visible > 0 && menu->numItems > visible;
	if (hasScrollBar)
		Canvas_drawScrollBar(cv, cv->viewRect[2], y, visible * 12, menu->scrollIndex, menu->scrollIndex + visible,
				     menu->numItems, visible);

	int allLarge = 0;
	id = menu->menu;
	if ((id == 3 || id == 0x10 || id == 0x14 || (id > 0xc && id < 0x10)) && cv->largeScreen != 0)
		allLarge = 1;

	/* ---- items ---- */
	for (int i = menu->scrollIndex; i < menu->numItems; i++) {
		MenuItem *it = &menu->items[i];
		String *text = &it->text;
		int viewW = cv->viewRect[2];
		int rightX = viewW - 4;
		int large = allLarge || (it->flags & ITEM_LARGE);
		int lineH = large ? 0x11 : 12;
		int charW = large ? 10 : 7;
		int font = large ? 1 : 0;
		int style = 0;

		if (it->flags & ITEM_DISABLED) {
			cv->textColor = TEXT_GREY;
			style = 0x10;
		} else if (it->flags & ITEM_HEADER) {
			cv->textColor = TEXT_HEADER;
			style = 0x10;
		} else if (it->flags & ITEM_HIGHLIGHT) {
			style = 8;
		}

		int x = leftX;
		if (String_length(text) > 0) {
			if ((it->flags & ITEM_CENTERED) == 0) {
				/* right-aligned value column */
				if (String_length(&it->text2) >= 1 || (it->flags & ITEM_NUMBER)) {
					if (menu->visibleItems != 0)
						rightX = viewW - 0xb;
					if (it->flags & ITEM_NUMBER) {
						String field, cut;
						String_ctorCStr(&field, mod_cstr(MOD_STR_DIGITS));
						String_substring(&cut, &field, 0, menu->maxDigits);
						String_assign(&field, &cut);
						String_dtor(&cut, 0);
						y += 2;
						Canvas_drawStringRange(cv, &field, rightX, y + 2, 9, 0, -1,
								       menu->digitEntry ? (font | 8) : font);
						String_dtor(&field, 0);
					} else if (menu->sliderEditing != 0 && (it->flags & ITEM_SLIDER)) {
						String arrows;
						String_ctorCStr(&arrows, mod_cstr(MOD_STR_SLIDER));
						Canvas_drawStringRange(cv, &arrows, rightX + 3, y, 9, 0, -1, style | font);
						String_dtor(&arrows, 0);
						rightX -= 7;
					}
					Canvas_drawStringRange(cv, &it->text2, rightX, y, 9, 0, -1, style | font);
				}
			} else {
				int len = String_length(text);
				int cx = cv->screenCenterX;
				if (menu->visibleItems != 0 && menu->numItems > menu->visibleItems)
					cx -= 6;
				x = cx - ((charW * len) >> 1);
			}

			/* cursor */
			if (menu->type != MENU_TYPE_TEXT && menu->type != MENU_TYPE_DETAILS && menu->menu != 0x18 &&
			    menu->selectedIndex == i) {
				int cursorX;
				if (((it->flags & ITEM_NUMBER) && menu->digitEntry != 0) ||
				    ((it->flags & ITEM_SLIDER) && menu->sliderEditing != 0)) {
					cursorX = cursorBob() + (rightX - charW * (menu->maxDigits + 1));
				} else {
					cursorX = cursorBob() + x;
					if ((it->flags & ITEM_DISABLED) == 0)
						style |= 8;
				}
				Canvas_drawImageSimple(cv, &menu->imgCursor, cursorX, y + lineH / 2, 0x28);
				x += 2;
			}

			if (String_length(text) > 0) {
				Canvas_drawStringRange(cv, text, x, y, 0, 0, String_length(text), style | font);
				if (it->flags & ITEM_DISABLED) {
					String_setLength(&cv->scratch, 0);
					for (int c = 0; c < String_length(text); c++)
						String_appendCStr_76f0(&cv->scratch, mod_cstr(MOD_STR_STRIKE));
					Canvas_drawString(cv, &cv->scratch, x, y, 0);
				}
			}
		}
		y += lineH;
		if (menu->visibleItems != 0 && menu->scrollIndex + menu->visibleItems - 1 == i)
			break;
	}

	/* ---- gold line (stores) ---- */
	if (storeFooter) {
		String_setLength(&cv->scratch, 0);
		Canvas_resetEvents(cv);
		Canvas_addTextArgInt(cv, app->player->gold);
		Text_getString(cv, 0, 0xb4, &cv->scratch);
		cv->textColor = TEXT_HEADER;
		TextUtil_dehyphenate(app->textUtil, &cv->scratch);
		Canvas_drawStringFlags(cv, &cv->scratch, cv->viewRect[2] / 2, cv->viewRect[3] - 0x26, 0x12, 0x10);
	}

	/* ---- help line of the selected item, with its icon ---- */
	if (showHelp) {
		int hx = 6;
		int h = cv->viewRect[3];
		Graphics_setColor(cv->graphics, COLOR_BLACK);
		Graphics_drawLine(cv->graphics, 6, h - 0x26, cv->viewRect[2], h - 0x26);
		if (selItem->param != 0x7f) {
			Graphics_setColor(cv->graphics, COLOR_BLACK);
			Graphics_fillRect(cv->graphics, 7, h - 0x24, 0x19, 0x19);
			Canvas_drawLargeIcon(cv, selItem->param, 8, h - 0x23, 0, 0, 0);
			Graphics_setColor(cv->graphics, COLOR_FRAME);
			Graphics_drawRect(cv->graphics, 7, h - 0x24, 0x19, 0x19);
			hx = 0x1e;
		}
		Canvas_drawString(cv, &selItem->help, hx + 3, h - 0x23, 0);
	}
}
