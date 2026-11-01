/*
 * MenuSystem (Applet+0x1b3b44, pointer at Applet+0x130): the active menu's
 * item list, selection and scrolling, key handling (lists, number entry for
 * barter offers, the volume slider, cheat codes), opening/closing menus and
 * drawing.
 *
 * Menu ids 1..10 are front-end menus drawn over the slowly spinning menu map
 * (ui.bsp) or the grey world; ids >= 100 are in-game menus on a scroll frame.
 */

#include "game.h"

/* ---- data in the .mod ---- */
#define MOD_CURSOR_BOB        0x2c798u /* 4 x s8 {-1,0,1,0}: cursor x bob, one step per 100 ms */
#define MOD_S_HEALTH_CHEAT    0x2c79cu
#define MOD_S_WEAPONS_CHEAT   0x2c7a4u
#define MOD_S_NUMBER_FIELD    0x2c7b4u
#define MOD_S_FMT_PERCENT     0x2c7bcu
#define MOD_S_FMT_AVG         0x2c7c4u
#define MOD_S_FMT_STATUS      0x2c7d4u
#define MOD_S_DIGITS          0x2c7e8u
#define MOD_S_SLIDER          0x2c7f0u
#define MOD_S_FMT_GOLD        0x2c7f8u

/* Images in the .bar. */
enum { RES_CURSOR = 5033, RES_SCROLL_ARROWS = 5034, RES_OE_LOGO = 5021, RES_SPINNERS = 5042 };
enum { SND_MENU_MOVE = 5067 };

/* Menu types (MenuSystem.type). */
enum { MENU_TYPE_LIST = 1, MENU_TYPE_MAIN = 4, MENU_TYPE_TEXT = 5, MENU_TYPE_PROMPT = 6, MENU_TYPE_DETAILS = 7,
       MENU_TYPE_OPTIONS = 8 };

/* Item flags (MenuItem.flags). */
enum {
	ITEM_UNSELECTABLE = 0x1, ITEM_DISABLED = 0x4, ITEM_CENTERED = 0x8, ITEM_NUMBER = 0x10, ITEM_DETAILS = 0x20,
	ITEM_HEADER = 0x40, ITEM_SLIDER = 0x80, ITEM_LARGE = 0x100, ITEM_HIGHLIGHT = 0x200
};

/* Canvas_getKeyAction results used here. */
enum { ACTION_LEFT = 3, ACTION_RIGHT = 4, ACTION_MENU = 5, ACTION_SELECT = 6, ACTION_BACK = 0xf };

/* Cheat codes: digits are accumulated units-first, so the number typed as
 * 3666 is stored as 6663. */
enum {
	CHEAT_DEBUG_MENU = 6663,  /* typed 3666 */
	CHEAT_BENCHMARK = 6633,   /* typed 3366 */
	CHEAT_HEALTH = 92634,     /* typed 43629 */
	CHEAT_WEAPONS = 2334      /* typed 4332 */
};

enum { MAP_M08 = 0x1777 }; /* 6007 Twilight Cavern, the last level */
enum { CANVAS_MENU = 2, CANVAS_PLAYING = 3 };

#define COLOR_BLACK  ((int)0xff000000u)
#define COLOR_FRAME  ((int)0xff555555u)
#define TEXT_GREY    0xad55 /* disabled item (RGB565) */
#define TEXT_HEADER  0xd6da /* divider / gold line (RGB565) */
#define NO_ICON      0xff

static char *S(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

/* Horizontal cursor bob: -1, 0, 1, 0 every 100 ms. */
static int cursorBob(Applet *app)
{
	return ModData_s8(&g_mod, MOD_CURSOR_BOB + (uint32_t)((app->tickTime / 100) % 4));
}

/* 0x00027a84 */
int MenuSystem_startup(MenuSystem *ms)
{
	Applet *app = ms->app;

	ms->canvas = app->canvas;
	ms->game = app->game;
	ms->render = app->render;
	ms->player = app->player;
	Image_load(app, RES_CURSOR, 1, &ms->cursor);
	Image_load(ms->app, RES_SCROLL_ARROWS, 0, &ms->scrollArrows);
	Image_load(ms->app, RES_OE_LOGO, 1, &ms->logo);
	Image_load(ms->app, RES_SPINNERS, 1, &ms->spinners);
	ms->numItems = 0;
	ms->background = NULL;
	ms->menuId = 0;
	ms->prevMenu = 0;
	ms->backMenu = -1;
	ms->selected = 0;
	ms->scroll = 0;
	ms->type = 0;
	ms->visibleItems = 0;
	ms->dirty = 0;
	ms->cheatValue = 0;
	ms->cheatDigits = 0;
	ms->editingNumber = 0;
	ms->spinPeriod = 0x28;
	return 1;
}

/* 0x00026b68 */
void MenuSystem_shutdown(MenuSystem *ms)
{
	App_freeImage(ms->app, &ms->cursor);
	App_freeImage(ms->app, &ms->scrollArrows);
	App_freeImage(ms->app, &ms->logo);
	App_freeImage(ms->app, &ms->spinners);
	ms->background = NULL;
}

/* 0x000271f4 Menu "tick" sound, except on text and detail pages. */
void MenuSystem_playOpenSound(MenuSystem *ms)
{
	if (ms->type != MENU_TYPE_TEXT && ms->type != MENU_TYPE_DETAILS)
		Sound_play(ms->app->sound, SND_MENU_MOVE, 0, 3, -1);
}

/* 0x0000d24c Front-end menu drawn over the 3D menu background. */
int MenuSystem_isBaseMenu(MenuSystem *menuSys)
{
	return menuSys->menuId < 0xb && menuSys->canvas->mapId != MAP_M08;
}

/* 0x00025a98 Open menu `menuId` (0 closes the menu and resumes play). */
void MenuSystem_setMenu(MenuSystem *ms, int menuId)
{
	int old = ms->menuId;
	Canvas *cv = ms->canvas;

	ms->cheatValue = 0;
	ms->cheatDigits = 0;

	if (menuId != 0 && menuId < 0xb && (old == 0 || old > 0xb)) {
		/* entering the front end */
		if (menuId < 10) {
			Canvas_loadMenuMap(cv);
		} else if (cv->mapId != MAP_M08) {
			/* level-end screen: the current level, grey, as background */
			Render_makeGrayscale(ms->render);
			Render_setScreenRect(ms->render, &ms->canvas->originX);
			int tile = ms->render->menuSpawnTile;
			if (tile != 0) {
				ms->canvas->viewX = (tile % 32) * 64 + 32;
				ms->canvas->viewY = (tile / 32) * 64 + 32;
			}
			ms->render->noViewOffset = 1;
		}
	} else if ((menuId > 0xb || menuId == 0) && old != 0 && old < 0xb) {
		/* leaving the front end */
		if (old >= 10) {
			Render_loadPalettes(ms->render);
			ms->render->noViewOffset = 0;
			Render_setScreenRect(ms->render, &ms->canvas->sceneX);
		} else {
			Gfx_setColor(ms->app, COLOR_BLACK);
			IDISPLAY_ClearScreen(MAKE_RGB(0, 0, 0));
			IDISPLAY_Update();
			Canvas_unloadMap(ms->canvas);
		}
	}

	ms->prevMenu = ms->menuId;
	ms->menuId = menuId;

	if (menuId == 0) {
		ms->numItems = 0;
		ms->app->hud->msg2[0] = '\0';
		if (ms->canvas->state != CANVAS_MENU)
			return;
		Canvas_setState(ms->canvas, CANVAS_PLAYING);
		Player_selectWeapon(ms->player, ms->player->ce.weapon);
		return;
	}

	Menu_initMenu(ms->app->menu, menuId);

	/* make sure the remembered selection is a selectable item */
	if (ms->selected < ms->numItems) {
		MenuItem *it = &ms->items[ms->selected];
		if (it->text[0] == '\0' || (it->flags & ITEM_UNSELECTABLE) != 0)
			MenuSystem_moveSelection(ms, 1);
	} else {
		MenuSystem_moveSelection(ms, -1);
	}
	Canvas_setState(ms->canvas, CANVAS_MENU);
	ms->dirty = 1;
}

/* 0x00005978 Back key: cancels number entry, else opens the parent menu. */
void MenuSystem_back(MenuSystem *ms)
{
	if (ms->editingNumber == 0) {
		if (ms->backMenu != -1)
			MenuSystem_setMenu(ms, ms->backMenu);
	} else {
		ms->editingNumber = 0;
		ms->dirty = 1;
	}
}

static int isSkippable(MenuSystem *ms, int i)
{
	return ms->items[i].text[0] == '\0' || (ms->items[i].flags & ITEM_UNSELECTABLE) != 0;
}

/* 0x0001c94c Move the selection by delta, skipping blank and unselectable
 * rows (wrapping around), and keep it inside the visible window. Text and
 * detail pages only scroll. */
void MenuSystem_moveSelection(MenuSystem *menu, int delta)
{
	if (menu->type == MENU_TYPE_TEXT || menu->type == MENU_TYPE_DETAILS) {
		if ((delta < 0 && menu->scroll > 0) ||
		    (delta > 0 && menu->scroll < menu->numItems - menu->visibleItems))
			menu->scroll += delta;
		menu->selected = menu->scroll;
		return;
	}

	/* The original walks the item array without bounds when no item is
	 * selectable (never the case for a built menu); stop instead. */
	if (menu->numItems <= 0) {
		menu->selected = 0;
		return;
	}

	for (;;) {
		menu->selected += delta;
		if (menu->selected >= menu->numItems || menu->selected < 0)
			break;
		if (!isSkippable(menu, menu->selected))
			goto placed;
	}
	/* wrapped past an end */
	menu->selected = (menu->selected >= 0) ? 0 : menu->numItems - 1;
	for (int guard = 0; isSkippable(menu, menu->selected) && guard < 96; guard++) {
		int next = menu->selected + delta;
		if (next < 0 || next >= 96)
			break;
		menu->selected = next;
	}

placed:
	if (menu->visibleItems == 0)
		return;
	int sel = menu->selected;
	if (delta < 0) {
		int top = sel - menu->visibleItems + 1;
		if (top > menu->scroll) {
			menu->scroll = top;
			return;
		}
	} else if (sel > menu->scroll + menu->visibleItems - 1) {
		menu->scroll = sel - menu->visibleItems + 1;
		return;
	}
	if (sel < menu->scroll)
		menu->scroll = sel;
}

/* 0x00024374 */
void MenuSystem_scrollDown(MenuSystem *ms)
{
	MenuSystem_moveSelection(ms, 1);
	ms->dirty = 1;
}

/* 0x0002443c */
void MenuSystem_scrollUp(MenuSystem *ms)
{
	MenuSystem_moveSelection(ms, -1);
	ms->dirty = 1;
}

/* 0x00024390 Page down: up to one page of steps, stopping at the last item
 * instead of wrapping. */
void MenuSystem_scrollPageDown(MenuSystem *ms)
{
	int start = ms->selected;

	for (int i = 0; i < ms->visibleItems && ms->numItems - 1 != ms->selected; i++) {
		MenuSystem_moveSelection(ms, 1);
		if (ms->selected < start) {
			MenuSystem_moveSelection(ms, -1);
			break;
		}
	}
	ms->dirty = 1;
}

/* 0x000243e8 Page up, stopping at the first item. */
void MenuSystem_scrollPageUp(MenuSystem *ms)
{
	int start = ms->selected;

	for (int i = 0; i < ms->visibleItems && ms->selected != 0; i++) {
		MenuSystem_moveSelection(ms, -1);
		if (start <= ms->selected) {
			MenuSystem_moveSelection(ms, 1);
			break;
		}
	}
	ms->dirty = 1;
}

/* 0x0000897c "Details" soft key: item description page for items with flag 0x20. */
void MenuSystem_select(MenuSystem *ms)
{
	ms->cheatValue = 0;
	ms->cheatDigits = 0;
	if (ms->items[ms->selected].flags & ITEM_DETAILS)
		Menus_showDetails(ms->app->menu, ms->menuId, ms->selected);
}

/* 0x00008a84 Activate the selected item: start number entry / the volume
 * slider, or run the menu action and open the menu it returns. */
int MenuSystem_activateItem(MenuSystem *ms)
{
	ms->cheatValue = 0;
	ms->cheatDigits = 0;

	int sel = ms->selected;
	int flags = ms->items[sel].flags;
	if (flags & ITEM_DISABLED)
		return 0;
	if (flags & ITEM_NUMBER) {
		ms->editingNumber = 1;
		ms->f2319 = 0;
		ms->items[sel].action = 0;
		ms->items[ms->selected].text2[0] = '\0';
		return 1;
	}
	if (flags & ITEM_SLIDER) {
		ms->sliderEdit = 1;
		return 1;
	}
	int next = Menus_select(ms->app->menu, ms->menuId, sel);
	if (next >= 0 && ms->menuId != next)
		MenuSystem_setMenu(ms, next);
	return 0;
}

/* Right-aligned number in the 5-character value field. */
static void formatNumberField(MenuItem *item, int value)
{
	strcpy(item->text2, S(MOD_S_NUMBER_FIELD));
	int i = 4;
	do {
		item->text2[i--] = (char)('0' + value % 10);
		value /= 10;
	} while (value > 0);
}

/* 0x0000883c Keys while typing the barter offer. */
void MenuSystem_handleNumberEntry(MenuSystem *ms, int key)
{
	int sel = ms->selected;
	MenuItem *item = &ms->items[sel];
	int next;

	unsigned digit = (unsigned)(key - AVK_0);
	if (digit < 10) {
		if (item->action > 9999)
			return;
		item->action = item->action * 10 + (int)digit;
		formatNumberField(item, item->action);
		return;
	}

	if (key == AVK_SELECT || key == AVK_POUND || key == AVK_SOFT2) {
		if (item->text2[0] == '\0')
			return;
		ms->editingNumber = 0;
		ms->editValue = item->action;
		next = Menus_select(ms->app->menu, ms->menuId, sel);
	} else if (key == AVK_LEFT || key == AVK_CLR || key == AVK_STAR) {
		/* delete the last digit; clearing an empty field leaves */
		int old = item->action;
		item->action = old / 10;
		if (item->action != 0)
			goto redraw;
		if (old != 0) {
			item->text2[0] = '\0';
			return;
		}
		next = ms->backMenu;
	} else if (key == AVK_SOFT1) {
		next = ms->backMenu;
	} else {
		goto redraw;
	}

	if (next >= 0 && ms->menuId != next) {
		ms->editingNumber = 0;
		MenuSystem_setMenu(ms, next);
		return;
	}
redraw:
	formatNumberField(item, item->action);
}

/* 0x00008b04 Keys while the volume slider is active. */
void MenuSystem_handleVolumeKey(MenuSystem *ms, int key)
{
	int sel = ms->selected;
	int action = Canvas_getKeyAction(ms->canvas, key);

	if (action == ACTION_SELECT || action == ACTION_BACK || action == ACTION_MENU) {
		ms->sliderEdit = 0;
		return;
	}
	if (action == ACTION_LEFT)
		Sound_volumeDown(ms->app->sound, 10);
	else if (action == ACTION_RIGHT)
		Sound_volumeUp(ms->app->sound, 10);
	else
		return;
	App_snprintf(ms->items[sel].text2, sizeof ms->items[sel].text2, S(MOD_S_FMT_PERCENT),
		     ms->app->sound->volume * 100 / 100);
	Game_saveConfig(ms->game);
}

/* 0x0000dbac Digit keys in a menu build a cheat code (first digit = units).
 * Returns 1 when the code was recognised. */
int MenuSystem_enterDigit(MenuSystem *menuSys, int digit)
{
	int scale = 1;
	for (int i = 0; i < menuSys->cheatDigits; i++)
		scale *= 10;
	menuSys->cheatValue += scale * digit;

	switch (menuSys->cheatValue) {
	case CHEAT_DEBUG_MENU:
		menuSys->cheatValue = 0;
		menuSys->cheatDigits = 0;
		menuSys->selected = 0;
		menuSys->scroll = 0;
		MenuSystem_setMenu(menuSys, 200);
		return 1;
	case CHEAT_BENCHMARK:
		menuSys->cheatValue = 0;
		menuSys->cheatDigits = 0;
		if (menuSys->menuId >= 100) {
			MenuSystem_setMenu(menuSys, 0);
			Canvas_enterState10(menuSys->canvas, 0);
		} else if (menuSys->menuId == 1) {
			Canvas_enterState10(menuSys->canvas, 1);
		}
		return 1;
	case CHEAT_HEALTH:
		menuSys->cheatValue = 0;
		menuSys->cheatDigits = 0;
		Hud_addMessage(menuSys->app->hud, S(MOD_S_HEALTH_CHEAT), 0);
		CombatEntity_setMaxHealth(&menuSys->player->ce, 999);
		CombatEntity_setHealth(&menuSys->player->ce, 999);
		return 1;
	case CHEAT_WEAPONS:
		menuSys->cheatValue = 0;
		menuSys->cheatDigits = 0;
		Hud_addMessage(menuSys->app->hud, S(MOD_S_WEAPONS_CHEAT), 0);
		/* the original stores 0x781, then 0x781|0xc, then 0x781|0x4c */
		menuSys->player->weapons = 0x781;
		menuSys->player->weapons = 0x781u | 0x4cu;
		for (int i = 0; i < 5; i++)
			Player_give(menuSys->player, 3, i, 99, 0);
		return 1;
	default:
		menuSys->cheatDigits++;
		return 0;
	}
}

static int isStoreMenu(int id)
{
	return id >= 0x12d && id <= 0x136;
}

/* Background of front-end menus: the spinning menu map (or the cinematic
 * camera), the benchmark result and the fade-in. */
static void paintMenuMap(MenuSystem *menu)
{
	Applet *app = menu->app;
	Game *game = menu->game;

	if (game->skippable == 0 || game->activeCamera == NULL) {
		int angle = (app->tickTime / menu->spinPeriod) & 0x3ff;
		menu->canvas->viewAngle = angle;
		Render_render(menu->render, menu->canvas->viewX, menu->canvas->viewY, 0x24, angle);
	} else {
		int now = (int)GETUPTIMEMS();
		Camera_update(game->activeCamera, game->cameraKey, now - game->cameraStart);
		Camera_apply(menu->game->activeCamera);
	}

	Canvas *cv = menu->canvas;
	if (cv->benchmark) {
		int render = cv->benchRender;
		int frames = cv->benchFrames;
		int hundredths = render * 100 / frames;
		int avg = render / frames;
		App_snprintf(cv->printMsg, sizeof cv->printMsg, S(MOD_S_FMT_AVG), avg, hundredths - avg * 100);
		Canvas_drawString(menu->canvas, menu->canvas->printMsg, 0, menu->canvas->viewH - 12, 0, 0);
	}

	int elapsed = app->tickTime - menu->canvas->fadeStart;
	if (elapsed <= 3000) {
		int t = (elapsed << 16) / 0xbb800;
		Render_fadeView(menu->render, ((t * 0xff00) >> 16) & 0xff);
	}
}

/* 0x0001d0a0 Draw the current menu. */
void MenuSystem_paint(MenuSystem *menu)
{
	Applet *app = menu->app;
	Canvas *cv = menu->canvas;

	menu->dirty = 0;
	Gfx_setColor(app, COLOR_BLACK);
	IDISPLAY_ClearScreen(MAKE_RGB(0, 0, 0)); /* IDISPLAY_DrawRect(0,0,screenW,screenH, fill) */

	MenuItem *selItem = &menu->items[menu->selected];
	int top = 0;

	/* ---- background ---- */
	if (isStoreMenu(menu->menuId) || isStoreMenu(menu->backMenu)) {
		Canvas_drawDialogFrame(cv, 0, 0, cv->viewW, cv->viewH, menu->menuId != 500);
		top = 0xd;
	} else if (menu->menuId >= 100) {
		Canvas_drawDialogFrame(cv, 0, 0, cv->viewW, cv->viewH, 0);
		top = 0xf;
	} else {
		Gfx_fillRect(app, 0, 0, cv->viewW, cv->viewH);
		if (MenuSystem_isBaseMenu(menu))
			paintMenuMap(menu);
		if (menu->background != NULL)
			Canvas_drawImage(menu->canvas, menu->background, menu->canvas->viewCenterX, 0, 0x11, 0);
	}

	/* ---- health/armour header on the inventory screens ---- */
	int y = top;
	if (menu->menuId >= 500 && menu->menuId <= 0x1fc) {
		char header[32];
		CombatEntity *ce = &menu->player->ce;
		y = top + 0xc;
		int maxArmor = CombatEntity_getMaxArmorPoints(ce);
		int armor = CombatEntity_getArmorPoints(ce);
		int maxHp = CombatEntity_getMaxHealth(ce);
		int hp = CombatEntity_getHealth(ce);
		App_snprintf(header, sizeof header, S(MOD_S_FMT_STATUS), 0x85, hp, maxHp, 0x86, armor, maxArmor);
		Canvas_drawStringRange(cv, header, cv->originX + (cv->viewW >> 1), top + 3, 0x30, 0, -1, 0);
		Gfx_setColor(app, COLOR_BLACK);
		Gfx_drawLine(app, menu->canvas->originX, top + 10, menu->canvas->viewW, top + 10);
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
	case MENU_TYPE_PROMPT:
		leftX = 0;
		y = (cv->sceneH - app->menuSystem->numItems * 12) / 2 + app->hud->topBarH;
		break;
	default:
		leftX = (menu->type == MENU_TYPE_OPTIONS) ? 0xd : 0x28;
		y = 0x50;
		break;
	}

	int id = menu->menuId;
	int showHelp = id != 0xc && id != 10 && selItem->help[0] != '\0';
	int storeFooter = isStoreMenu(id) || id == 500;

	int avail;
	if (storeFooter || showHelp)
		avail = cv->viewH - y - 0x30;
	else if (id >= 100)
		avail = cv->viewH - 0x1a;
	else
		avail = cv->viewH - y;
	menu->visibleItems = avail / 12;

	int visible = menu->visibleItems;
	if (visible > 0 && visible < menu->numItems)
		Canvas_drawScrollBar(cv, cv->viewW, y, visible * 12, menu->scroll, menu->scroll + visible, menu->numItems);

	int allLarge = (id == 1 || id == 0xe || (id >= 5 && id <= 8)) && menu->canvas->largeScreen;

	/* ---- items ---- */
	for (int i = menu->scroll; i < menu->numItems; i++) {
		MenuItem *it = &menu->items[i];
		int rightX = menu->canvas->viewW - 4;
		int large = allLarge || (it->flags & ITEM_LARGE) != 0;
		int lineH = large ? 0x11 : 12;
		int charW = large ? 10 : 7;
		int style = 0;

		if (it->flags & ITEM_DISABLED) {
			style = 0x10;
			menu->canvas->textColor = TEXT_GREY;
		} else if (it->flags & ITEM_HEADER) {
			style = 0x10;
			menu->canvas->textColor = TEXT_HEADER;
		} else if (it->flags & ITEM_HIGHLIGHT) {
			style = 8;
		}

		if (it->text[0] != '\0') {
			int x;
			if (it->flags & ITEM_CENTERED) {
				int len = (int)(uint16_t)strlen(it->text);
				int cx = menu->canvas->viewCenterX;
				if (menu->visibleItems != 0 && menu->numItems > menu->visibleItems)
					cx -= 6;
				x = cx - ((charW * len * 0x80) >> 8);
			} else {
				/* right-aligned value column (always drawn; empty values draw nothing) */
				x = leftX;
				if (menu->visibleItems != 0)
					rightX -= 7;
				if (it->flags & ITEM_NUMBER) {
					y += 2;
					Canvas_drawStringRange(menu->canvas, S(MOD_S_DIGITS), rightX, y + 2, 9, 0, -1,
							       menu->editingNumber ? (large | 8) : large);
				} else if (menu->sliderEdit != 0 && (it->flags & ITEM_SLIDER)) {
					Canvas_drawStringRange(menu->canvas, S(MOD_S_SLIDER), rightX + 3, y, 9, 0, -1,
							       style | large);
					rightX -= 7;
				}
				Canvas_drawStringRange(menu->canvas, it->text2, rightX, y, 9, 0, -1, style | large);
			}

			/* cursor */
			if (menu->type != MENU_TYPE_TEXT && menu->type != MENU_TYPE_DETAILS && menu->selected == i) {
				int cursorX;
				if (((it->flags & ITEM_NUMBER) && menu->editingNumber) ||
				    ((it->flags & ITEM_SLIDER) && menu->sliderEdit != 0)) {
					cursorX = cursorBob(app) + (rightX - charW * 6);
				} else {
					cursorX = cursorBob(app) + x;
					if ((it->flags & ITEM_DISABLED) == 0)
						style |= 8;
				}
				Canvas_drawImage(menu->canvas, &menu->cursor, cursorX, y + lineH / 2, 0x28, 0);
				x += 2;
			}

			if (it->text[0] != '\0')
				Canvas_drawStringRange(menu->canvas, it->text, x, y, 0, 0, -1, style | large);
		}
		y += lineH;
		if (menu->visibleItems != 0 && menu->scroll + menu->visibleItems - 1 == i)
			break;
	}

	/* ---- gold line (store and inventory) ---- */
	if (storeFooter) {
		char gold[32];
		App_snprintf(gold, sizeof gold, S(MOD_S_FMT_GOLD), menu->player->gold);
		menu->canvas->textColor = TEXT_HEADER;
		cv = menu->canvas;
		Canvas_drawString(cv, gold, cv->viewW / 2, cv->viewH - 0x26, 0x12, 0x10);
	}

	/* ---- help line of the selected item, with its icon ---- */
	if (showHelp) {
		int lineY = menu->canvas->viewH - 0x26;
		int textX = 4;
		Gfx_setColor(app, COLOR_BLACK);
		Gfx_drawLine(app, 4, lineY, menu->canvas->viewW, lineY);
		if (selItem->param != NO_ICON) {
			Canvas_drawLargeIcon(menu->canvas, selItem->param, 6, lineY + 3, 0, 0, 0);
			Gfx_setColor(app, COLOR_FRAME);
			Gfx_drawRect(app, 5, lineY + 2, 0x19, 0x19);
			textX = 0x1f;
		}
		Canvas_drawString(menu->canvas, selItem->help, textX, lineY + 3, 0, 0);
	}
}
