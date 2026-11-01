/*
 * Menu item construction (MenuSystem items, 0x5c bytes each in the original):
 * item text/help wrapping, dividers, yes/no prompts, help-text pages loaded
 * from the .bar, the quest log, the end-game ranking and the statistics pages.
 *
 * Every player-visible string is read from the user's .mod (inline literals in
 * the original's data area).
 */

#include "game.h"

/* ---- strings in the .mod ---- */
#define MOD_S_YES                 0x2e97cu
#define MOD_S_NO                  0x2e978u
#define MOD_S_ORCS_ELVES          0x2ef68u
#define MOD_S_EMPTY               0x2eaf4u
#define MOD_S_SPACE               0x2eb18u
#define MOD_S_PAGE                0x2eaecu
#define MOD_S_SQUIRE              0x2eaa0u
#define MOD_S_KNIGHT              0x2ea58u
#define MOD_S_PALADIN             0x2e9fcu
#define MOD_S_RANK_PAGE           0x2eaa8u
#define MOD_S_RANK_SQUIRE         0x2ea60u
#define MOD_S_RANK_KNIGHT         0x2ea04u
#define MOD_S_RANK_PALADIN        0x2e984u
#define MOD_S_RANKING             0x2eb10u
#define MOD_S_YOUR_SCORE          0x2eb04u
#define MOD_S_YOUR_RANK           0x2eaf8u
#define MOD_S_PLAYER              0x2eb1cu
#define MOD_S_FMT_D_SLASH_D       0x2eb30u
#define MOD_S_FMT_D_SLASH_DPD     0x2eb24u
#define MOD_S_FMT_D_PD            0x2ec28u
#define MOD_S_HEALTH              0x2ec00u
#define MOD_S_HELP_HEALTH         0x2ec08u
#define MOD_S_ARMOR               0x2ebd8u
#define MOD_S_HELP_ARMOR          0x2ebe0u
#define MOD_S_GOLD                0x2ebb4u
#define MOD_S_HELP_GOLD           0x2ebbcu
#define MOD_S_LEVEL               0x2eb9cu
#define MOD_S_HELP_LEVEL          0x2eba4u
#define MOD_S_CUR_XP              0x2eb70u
#define MOD_S_HELP_CUR_XP         0x2eb7cu
#define MOD_S_NEXT_XP             0x2eb38u
#define MOD_S_HELP_NEXT_XP        0x2eb48u
#define MOD_S_DEFENSE             0x2ec30u
#define MOD_S_HELP_DEFENSE        0x2ec3cu
#define MOD_S_STRENGTH            0x2ec54u
#define MOD_S_HELP_STRENGTH       0x2ec60u
#define MOD_S_ACCURACY            0x2ec78u
#define MOD_S_HELP_ACCURACY       0x2ec84u
#define MOD_S_THIS_LEVEL          0x2ed5cu
#define MOD_S_TIME                0x2ed38u
#define MOD_S_HELP_TIME_LEVEL     0x2ed40u
#define MOD_S_SECRETS             0x2ed10u
#define MOD_S_HELP_SECRETS        0x2ed1cu
#define MOD_S_MONSTERS            0x2ece4u
#define MOD_S_HELP_MONSTERS       0x2ecf0u
#define MOD_S_TURNS               0x2ecc0u
#define MOD_S_HELP_TURNS_LEVEL    0x2ecc8u
#define MOD_S_XP_GAINED           0x2ec9cu
#define MOD_S_HELP_XP_GAINED      0x2eca8u
#define MOD_S_OVERALL             0x2ed68u
#define MOD_S_STATISTICS          0x2ed70u
#define MOD_S_HELP_TIME_TOTAL     0x2ed7cu
#define MOD_S_HELP_TURNS_TOTAL    0x2eec8u
#define MOD_S_DEATHS              0x2eea4u
#define MOD_S_HELP_DEATHS         0x2eeacu
#define MOD_S_TOTAL_GOLD          0x2ee84u
#define MOD_S_HELP_TOTAL_GOLD     0x2ee90u
#define MOD_S_GOLD_SPENT          0x2ee60u
#define MOD_S_HELP_GOLD_SPENT     0x2ee6cu
#define MOD_S_HEAVIEST            0x2ee30u
#define MOD_S_HELP_HEAVIEST       0x2ee40u
#define MOD_S_TOTAL_DAMAGE        0x2ee00u
#define MOD_S_HELP_TOTAL_DAMAGE   0x2ee10u
#define MOD_S_GIBBED              0x2edd4u
#define MOD_S_HELP_GIBBED         0x2ede4u
#define MOD_S_DRUNK               0x2eda4u
#define MOD_S_HELP_DRUNK          0x2edb4u
#define MOD_S_FAVORITES           0x2ed98u
#define MOD_S_WEAPON              0x2eed4u
#define MOD_S_HELP_WEAPON         0x2eedcu
#define MOD_S_FMT_INDENT          0x2eef4u
#define MOD_S_INDENT_NONE         0x2eefcu
#define MOD_S_POTION              0x2ef04u
#define MOD_S_HELP_POTION         0x2ef0cu
#define MOD_S_RING                0x2ef24u
#define MOD_S_HELP_RING           0x2ef2cu
#define MOD_S_KILLS100            0x2ef40u
#define MOD_S_NONE                0x2ef4cu
#define MOD_S_FMT_SECRETS100      0x2ef54u

/* Help-text resources in the .bar. */
enum {
	RES_HELP = 5041, RES_LEGALS = 5118, RES_HELP_ITEMS = 5133, RES_HELP_ARMOUR = 5151, RES_HELP_EFFECTS = 5176
};

enum { ERROR_RESOURCE_MISSING = 0x16 };

/* Item flags (MenuItem.flags). */
enum {
	ITEM_UNSELECTABLE = 0x1, ITEM_RAW = 0x2, ITEM_CENTERED = 0x8, ITEM_HEADER_ROW = 0x49,
	ITEM_TEXT_LINE = 0x2, ITEM_TITLE_LINE = 0xa, ITEM_BIG_CENTERED = 0x308
};

#define LINE_BREAK '|'
#define NO_ICON 0xff
#define DIVIDER_GLYPH ((char)0x80)

static char *S(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

static MenuItem *nextItem(Menu *menu)
{
	MenuSystem *ms = menu->app->menuSystem;
	return &ms->items[ms->numItems++];
}

/* 0x0000271c Fill one menu item. Text and value are word-wrapped (no width
 * limit, '|' line breaks) unless flag 2 asks for a raw copy; the help line is
 * wrapped to the help box (narrower when the item has an icon) and cut to two
 * lines with "...". */
void MenuItem_setFull(MenuItem *item, char *text, char *text2, int flags, int action, int param, char *help)
{
	if (text == NULL)
		item->text[0] = '\0';
	else if (flags & ITEM_RAW)
		App_snprintf(item->text, sizeof item->text, text); /* the original uses the text as format */
	else
		Text_wrap(text, 9999, item->text, sizeof item->text, 0, LINE_BREAK, NULL);

	if (text2 == NULL)
		item->text2[0] = '\0';
	else if (flags & ITEM_RAW)
		App_snprintf(item->text2, sizeof item->text2, text2);
	else
		Text_wrap(text2, 9999, item->text2, sizeof item->text2, 0, LINE_BREAK, NULL);

	if (help == NULL) {
		item->help[0] = '\0';
	} else {
		Canvas *cv = g_app->canvas;
		int width = (param == NO_ICON) ? cv->textCols[3] : cv->textCols[4];
		char buf[512];
		int truncated = 0;

		if (flags & ITEM_RAW)
			App_snprintf(buf, 0x28, help);
		else
			Text_wrap(help, width, buf, 0x28, 0, LINE_BREAK, &truncated);

		/* first break becomes a newline, a second one ends the text */
		int lines = 0;
		int lastBreak = 0;
		char *p = buf;
		while ((p = strchr(p, LINE_BREAK)) != NULL) {
			lines++;
			if (lines >= 2) {
				*p = '\0';
				truncated = 1;
				break;
			}
			*p = '\n';
			lastBreak = (int)(p - buf);
		}

		if (truncated) {
			char *lineStart = buf + lastBreak;
			int lastLen = (int)strlen(lineStart + 1);
			if (lastLen < width - 3 && (uint32_t)(strlen(buf) + 3) < 4u) {
				/* only reachable with an empty text (as compiled in the original) */
				int at = lastLen + lastBreak + 1;
				buf[at] = '.';
				buf[at + 1] = '.';
				buf[at + 2] = '.';
				buf[at + 3] = '\0';
			} else {
				/* drop whole words from the end until "..." fits */
				char *sp = strrchr(buf, ' ');
				while (sp != NULL && lineStart < sp) {
					int tail = (int)(buf + strlen(buf) - sp);
					if (tail > 3) {
						memcpy(sp, "...", 4);
						break;
					}
					*sp = '\0';
					sp = strrchr(buf, ' ');
				}
				int end = lastLen + lastBreak + 1;
				if ((sp == NULL || sp <= lineStart) && end >= 3) {
					/* no space on the last line: overwrite its last three characters */
					buf[end - 1] = '.';
					buf[end - 2] = '.';
					buf[end - 3] = '.';
				}
			}
		}
		/* the help field is 0x28 bytes and the wrapped text is limited to that */
		buf[sizeof item->help - 1] = '\0';
		strcpy(item->help, buf);
	}

	item->param = param;
	item->flags = (uint16_t)flags;
	item->action = action;
}

/* 0x00003190 Append an item to the current menu. */
void MenuSystem_addItem(Menu *menu, char *text, char *text2, int flags, int action, int param, char *help)
{
	MenuItem_setFull(nextItem(menu), text, text2, flags, action, param, help);
}

/* 0x00005cd4 Divider line: " TITLE " centred in 15 characters, padded with
 * the 0x80 rule glyph (three glyphs in the middle when there is no title). */
char *MenuSystem_buildDivider(MenuSystem *ms, char *title)
{
	int len = 0;
	int pos = 0;

	ms->title[0] = '\0';
	if (title != NULL)
		len = (int)strlen(title);
	int pad = (15 - (len + 2)) / 2;

	for (int i = 0; i < pad; i++)
		ms->title[pos++] = DIVIDER_GLYPH;
	if (title == NULL) {
		ms->title[pos] = DIVIDER_GLYPH;
		ms->title[pos + 1] = DIVIDER_GLYPH;
		ms->title[pos + 2] = DIVIDER_GLYPH;
		pos += 3;
	} else {
		ms->title[pos] = ' ';
		strcpy(ms->title + pos + 1, title);
		pos += 1 + len;
		ms->title[pos++] = ' ';
	}
	for (int i = 0; i < pad; i++)
		ms->title[pos++] = DIVIDER_GLYPH;
	ms->title[pos] = '\0';
	return ms->title;
}

/* 0x00002624 Yes/No prompt: the question (one item per '\n' line), a blank
 * line, "Yes" (action 1) and "No" (action 0); selects Yes or No. */
void MenuSystem_setYesNo(Menu *menu, char *text, int defaultYes)
{
	MenuSystem *ms = menu->app->menuSystem;
	char line[32];

	if (strchr(text, '\n') != NULL) {
		char *nl;
		while ((nl = strchr(text, '\n')) != NULL) {
			int len = (int)(nl - text);
			if (len > (int)sizeof line - 1)
				len = (int)sizeof line - 1;
			memcpy(line, text, (size_t)len);
			line[len] = '\0';
			MenuSystem_addItem(menu, line, NULL, 9, 0, 0, NULL);
			text = nl + 1;
		}
	}
	MenuSystem_addItem(menu, text, NULL, 9, 0, 0, NULL);
	MenuSystem_addItem(menu, NULL, NULL, 9, 0, 0, NULL);
	MenuSystem_addItem(menu, S(MOD_S_YES), NULL, ITEM_CENTERED, 1, 0, NULL);
	MenuSystem_addItem(menu, S(MOD_S_NO), NULL, ITEM_CENTERED, 0, 0, NULL);
	ms->selected = defaultYes ? ms->numItems - 2 : ms->numItems - 1;
}

/* 0x00001fe0 Word-wrap a text to the menu width and add one item per line:
 * empty lines become blank items, "=Title" lines become dividers. */
void MenuSystem_addTextLines(Menu *menu, char *text)
{
	char buf[0x800 + 1]; /* + 1: Text_wrap 0x2a84 can put its NUL one past the size it is given */

	Text_wrap(text, menu->canvas->textCols[0], buf, 0x800, 0, LINE_BREAK, NULL);
	char *end = buf + strlen(buf);
	char *line = buf;
	char *brk;

	while ((brk = strchr(line, LINE_BREAK)) != NULL) {
		MenuItem *item;
		if (brk == line) {
			item = nextItem(menu);
			MenuItem_setFull(item, NULL, NULL, 0, 0, 0, NULL);
		} else {
			/* the line is moved to the front of the buffer (already consumed) */
			int len = (int)(brk - line);
			memmove(buf, line, (size_t)len);
			buf[len] = '\0';
			if (buf[0] == '=') {
				char *title = MenuSystem_buildDivider(menu->app->menuSystem, buf + 1);
				item = nextItem(menu);
				MenuItem_setFull(item, title, NULL, ITEM_TITLE_LINE, 0, 0, NULL);
			} else {
				item = nextItem(menu);
				MenuItem_setFull(item, buf, NULL, ITEM_TEXT_LINE, 0, 0, NULL);
			}
		}
		line = brk + 1;
	}
	if (line != end)
		MenuItem_setFull(nextItem(menu), line, NULL, ITEM_TEXT_LINE, 0, 0, NULL);
}

/* 0x000258c4 Replace the items from `first` on with the lines of a wrapped
 * text (raw copies); numItems ends after the last line. */
void MenuSystem_setItemsFromText(MenuSystem *ms, int first, char *text)
{
	char wrapped[0x200 + 1]; /* + 1: see MenuSystem_addTextLines */
	char line[0x80];

	ms->numItems = first;
	Text_wrap(text, ms->canvas->textCols[0], wrapped, 0x200, 0, LINE_BREAK, NULL);

	char *s = wrapped;
	while (s != NULL) {
		char *end = strchr(s, LINE_BREAK);
		char *next;
		if (end == NULL) {
			end = s + strlen(s);
			next = NULL;
		} else {
			next = end + 1;
		}
		int len = (int)(end - s);
		if (len > (int)sizeof line - 1)
			len = (int)sizeof line - 1;
		memcpy(line, s, (size_t)len);
		line[len] = '\0';
		MenuItem_setFull(&ms->items[first], line, NULL, ITEM_RAW, 0, 0, NULL);
		s = next;
		first++;
	}
	ms->numItems = first;
}

/* Shared body of the help-page loaders: inflate a text resource and add its
 * lines (optionally after the "Orcs & Elves" heading). */
static void loadTextPage(Menu *menu, int resId, int errorIfMissing, int withHeading)
{
	InputStream is;
	char text[1024];

	InputStream_ctor(&is, menu->app, 1);
	if (Resource_open(&is, resId, 1) == 0) {
		if (errorIfMissing)
			App_setErrorCode(g_app, ERROR_RESOURCE_MISSING);
	} else {
		int size = InputStream_getSize(&is);
		if (size > (int)sizeof text - 1)
			size = (int)sizeof text - 1;
		strncpy(text, (const char *)InputStream_getBuffer(&is), (size_t)size);
		text[size] = '\0';
		if (withHeading) {
			MenuItem_setFull(nextItem(menu), S(MOD_S_ORCS_ELVES), NULL, ITEM_CENTERED, 0, 0, NULL);
			MenuSystem_addItem(menu, S(MOD_S_EMPTY), NULL, 0, 0, 0, NULL);
		}
		MenuSystem_addTextLines(menu, text);
	}
	InputStream_dtor(&is, 0);
}

/* 0x00001d04 "About the Game": legals.txt under the game title. */
void MenuSystem_loadLegals(Menu *menu)
{
	loadTextPage(menu, RES_LEGALS, 1, 1);
}

/* 0x00001de4 Armor help page. */
void MenuSystem_loadHelpArmour(Menu *menu)
{
	loadTextPage(menu, RES_HELP_ARMOUR, 1, 0);
}

/* 0x00001ee4 Status-effects help page (no error when missing). */
void MenuSystem_loadHelpEffects(Menu *menu)
{
	loadTextPage(menu, RES_HELP_EFFECTS, 0, 0);
}

/* 0x00001f58 "How to Play" page. */
void MenuSystem_loadHelp(Menu *menu)
{
	loadTextPage(menu, RES_HELP, 1, 0);
}

/* 0x00002180 Item-usage help page. */
void MenuSystem_loadHelpItems(Menu *menu)
{
	loadTextPage(menu, RES_HELP_ITEMS, 1, 0);
}

/* 0x00002208 Quest log: map name, divider, then every notebook entry (map
 * string) followed by two blank lines; opens scrolled to the last page. */
void MenuSystem_loadNotebook(Menu *menu)
{
	MenuSystem *ms = menu->app->menuSystem;

	MenuItem_setFull(&ms->items[ms->numItems++], menu->render->mapName, NULL, ITEM_CENTERED, 0, 0, NULL);
	char *div = MenuSystem_buildDivider(ms, NULL);
	MenuItem_setFull(&ms->items[ms->numItems++], div, NULL, ITEM_HEADER_ROW, 0, 0, NULL);

	for (int i = 0; i < menu->player->notebookCount; i++) {
		MenuSystem_addTextLines(menu, menu->render->mapStrings[menu->player->notebook[i]]);
		MenuItem_setFull(nextItem(menu), NULL, NULL, 0, 0, 0, NULL);
		MenuItem_setFull(nextItem(menu), NULL, NULL, 0, 0, 0, NULL);
	}

	int top = ms->numItems - (menu->canvas->viewH - 0x1a) / 12;
	ms->scroll = top;
	ms->selected = top;
	if (top < 0) {
		ms->selected = 0;
		ms->scroll = 0;
	}
}

/* 0x000016ac End-game ranking page: score (Player_calcScore) and rank. */
void MenuSystem_fillRanking(Menu *menu)
{
	MenuSystem *ms = menu->app->menuSystem;
	char score[32];
	uint32_t rank, comment;

	int value = Player_calcScore(menu->player);
	App_snprintf(score, sizeof score, "%d", value);
	if (value >= 17500) {
		rank = MOD_S_PALADIN;
		comment = MOD_S_RANK_PALADIN;
	} else if (value >= 13500) {
		rank = MOD_S_KNIGHT;
		comment = MOD_S_RANK_KNIGHT;
	} else if (value >= 9000) {
		rank = MOD_S_SQUIRE;
		comment = MOD_S_RANK_SQUIRE;
	} else {
		rank = MOD_S_PAGE;
		comment = MOD_S_RANK_PAGE;
	}

	MenuSystem_addItem(menu, S(MOD_S_SPACE), NULL, 0, 0, 0, NULL);
	MenuSystem_addItem(menu, MenuSystem_buildDivider(ms, S(MOD_S_RANKING)), NULL, ITEM_CENTERED, 0, 0, NULL);
	MenuSystem_addItem(menu, S(MOD_S_EMPTY), NULL, 0, 0, 0, NULL);
	MenuSystem_addItem(menu, S(MOD_S_YOUR_SCORE), NULL, ITEM_CENTERED, 0, 0, NULL);
	MenuSystem_addItem(menu, score, NULL, ITEM_BIG_CENTERED, 0, 0, NULL);
	MenuSystem_addItem(menu, S(MOD_S_YOUR_RANK), NULL, ITEM_CENTERED, 0, 0, NULL);
	MenuSystem_addItem(menu, S(rank), NULL, ITEM_BIG_CENTERED, 0, 0, NULL);
	MenuSystem_addItem(menu, S(MOD_S_EMPTY), NULL, 0, 0, 0, NULL);
	MenuSystem_addTextLines(menu, S(comment));
	MenuSystem_addItem(menu, S(MOD_S_EMPTY), NULL, 0, 0, 0, NULL);
}

/* One "Label: value" statistics row with its help line. */
static void addStat(Menu *menu, uint32_t label, char *value, uint32_t help)
{
	MenuSystem_addItem(menu, S(label), value, 0, 0, NO_ICON, S(help));
}

static void addStatInt(Menu *menu, uint32_t label, int value, uint32_t help)
{
	char buf[32];
	App_snprintf(buf, sizeof buf, "%d", value);
	addStat(menu, label, buf, help);
}

/* "base" or "base+bonus" for a modified combat stat. */
static void formatModStat(char *buf, int cur, int base)
{
	int diff = cur - base;
	if (diff == 0)
		App_snprintf(buf, 32, "%d", base);
	else
		App_snprintf(buf, 32, S(MOD_S_FMT_D_PD), base, diff);
}

/* Favourite (weapon / potion / ring) row: "  Name" or "  None". */
static void addFavourite(Menu *menu, int best, int found, int subType)
{
	char buf[32];
	if (!found) {
		MenuSystem_addItem(menu, S(MOD_S_INDENT_NONE), NULL, 0, 0, NO_ICON, S(MOD_S_SPACE));
	} else {
		EntityDef *def = EntityDefs_find(menu->app->entityDefs, 6, subType, best & 0xff);
		App_snprintf(buf, sizeof buf, S(MOD_S_FMT_INDENT), def->name);
		MenuSystem_addItem(menu, buf, NULL, ITEM_UNSELECTABLE, 0, 0, NULL);
	}
}

/* Maps listed under "100% KILLS" / "100% SECRETS". */
static void addLevelList(Menu *menu, uint32_t mask)
{
	if (mask == 0) {
		MenuSystem_addItem(menu, S(MOD_S_NONE), NULL, 0, 0, NO_ICON, S(MOD_S_SPACE));
		return;
	}
	for (int i = 0; i < menu->game->numLevelNames; i++) {
		if (mask & arm_bit(i))
			MenuSystem_addItem(menu, menu->game->levelNames + i * 0x18, S(MOD_S_EMPTY), 0, 0, NO_ICON,
					   S(MOD_S_SPACE));
	}
}

/* 0x00010b2c Statistics rows: player stats, this level, overall totals,
 * favourites and the maps finished with all kills / all secrets. */
void Menu_buildStatistics(Menu *menu, int levelStats, int playerStats, int gameStats)
{
	Applet *app = menu->app;
	MenuSystem *ms = app->menuSystem;
	Player *pl = menu->player;
	int levelTime = app->gameTime - menu->game->levelStartTime;
	char buf[32];

	if (playerStats) {
		MenuSystem_addItem(menu, MenuSystem_buildDivider(ms, S(MOD_S_PLAYER)), NULL, ITEM_HEADER_ROW, 0, 0, NULL);

		int maxHp = CombatEntity_getMaxHealth(&pl->ce);
		int baseMaxHp = CombatEntity_getMaxHealth(&pl->baseCe);
		if (maxHp - baseMaxHp == 0) {
			int base = CombatEntity_getMaxHealth(&pl->baseCe);
			int hp = CombatEntity_getHealth(&pl->ce);
			App_snprintf(buf, sizeof buf, S(MOD_S_FMT_D_SLASH_D), hp, base, 0);
		} else {
			int base = CombatEntity_getMaxHealth(&pl->baseCe);
			int hp = CombatEntity_getHealth(&pl->ce);
			App_snprintf(buf, sizeof buf, S(MOD_S_FMT_D_SLASH_DPD), hp, base, maxHp - baseMaxHp);
		}
		addStat(menu, MOD_S_HEALTH, buf, MOD_S_HELP_HEALTH);

		int maxArmor = CombatEntity_getMaxArmorPoints(&pl->ce);
		int armor = CombatEntity_getArmorPoints(&pl->ce);
		App_snprintf(buf, sizeof buf, S(MOD_S_FMT_D_SLASH_D), armor, maxArmor);
		addStat(menu, MOD_S_ARMOR, buf, MOD_S_HELP_ARMOR);

		addStatInt(menu, MOD_S_GOLD, pl->gold, MOD_S_HELP_GOLD);
		addStatInt(menu, MOD_S_LEVEL, pl->level, MOD_S_HELP_LEVEL);
		addStatInt(menu, MOD_S_CUR_XP, pl->xp, MOD_S_HELP_CUR_XP);
		addStatInt(menu, MOD_S_NEXT_XP, pl->nextLevelXP, MOD_S_HELP_NEXT_XP);

		int cur = CombatEntity_getDefense(&pl->ce);
		formatModStat(buf, cur, CombatEntity_getDefense(&pl->baseCe));
		addStat(menu, MOD_S_DEFENSE, buf, MOD_S_HELP_DEFENSE);

		cur = CombatEntity_getStrength(&pl->ce);
		formatModStat(buf, cur, CombatEntity_getStrength(&pl->baseCe));
		addStat(menu, MOD_S_STRENGTH, buf, MOD_S_HELP_STRENGTH);

		cur = CombatEntity_getAccuracy(&pl->ce);
		formatModStat(buf, cur, CombatEntity_getAccuracy(&pl->baseCe));
		addStat(menu, MOD_S_ACCURACY, buf, MOD_S_HELP_ACCURACY);
	}

	if (levelStats) {
		int found, total;

		MenuSystem_addItem(menu, MenuSystem_buildDivider(ms, S(MOD_S_THIS_LEVEL)), NULL, ITEM_HEADER_ROW, 0, 0,
				   NULL);
		Player_formatTime(pl, buf, sizeof buf, levelTime);
		addStat(menu, MOD_S_TIME, buf, MOD_S_HELP_TIME_LEVEL);
		Player_countSecrets(pl, &found, &total);
		App_snprintf(buf, sizeof buf, S(MOD_S_FMT_D_SLASH_D), found, total);
		addStat(menu, MOD_S_SECRETS, buf, MOD_S_HELP_SECRETS);
		Game_countMonsters(pl, &found, &total);
		App_snprintf(buf, sizeof buf, S(MOD_S_FMT_D_SLASH_D), found, total);
		addStat(menu, MOD_S_MONSTERS, buf, MOD_S_HELP_MONSTERS);
		addStatInt(menu, MOD_S_TURNS, pl->levelTurns, MOD_S_HELP_TURNS_LEVEL);
		addStatInt(menu, MOD_S_XP_GAINED, pl->levelXP, MOD_S_HELP_XP_GAINED);
	}

	if (gameStats) {
		uint32_t title = (levelStats == 0 && playerStats == 0) ? MOD_S_STATISTICS : MOD_S_OVERALL;
		MenuSystem_addItem(menu, MenuSystem_buildDivider(ms, S(title)), NULL, ITEM_HEADER_ROW, 0, 0, NULL);

		Player_formatTime(pl, buf, sizeof buf, (app->gameTime - pl->timeStart) + pl->playTime);
		addStat(menu, MOD_S_TIME, buf, MOD_S_HELP_TIME_TOTAL);
		if (playerStats == 0)
			addStatInt(menu, MOD_S_LEVEL, pl->level, MOD_S_HELP_LEVEL);
		addStatInt(menu, MOD_S_TURNS, pl->totalTurns, MOD_S_HELP_TURNS_TOTAL);
		addStatInt(menu, MOD_S_DEATHS, pl->deaths, MOD_S_HELP_DEATHS);
		addStatInt(menu, MOD_S_TOTAL_GOLD, app->statGoldGained, MOD_S_HELP_TOTAL_GOLD);
		addStatInt(menu, MOD_S_GOLD_SPENT, app->statGoldSpent, MOD_S_HELP_GOLD_SPENT);
		addStatInt(menu, MOD_S_HEAVIEST, app->statMaxHit, MOD_S_HELP_HEAVIEST);
		addStatInt(menu, MOD_S_TOTAL_DAMAGE, app->statDamage, MOD_S_HELP_TOTAL_DAMAGE);
		addStatInt(menu, MOD_S_GIBBED, app->statGibs, MOD_S_HELP_GIBBED);
		addStatInt(menu, MOD_S_DRUNK, app->statDrunkTurns, MOD_S_HELP_DRUNK);

		MenuSystem_addItem(menu, MenuSystem_buildDivider(ms, S(MOD_S_FAVORITES)), NULL, ITEM_HEADER_ROW, 0, 0,
				   NULL);

		/* most used weapon (0..10) */
		int best = -1, most = 0;
		for (int i = 0; i < 11; i++) {
			if (app->statWeaponUse[i] > most) {
				most = app->statWeaponUse[i];
				best = i;
			}
		}
		addStat(menu, MOD_S_WEAPON, NULL, MOD_S_HELP_WEAPON);
		addFavourite(menu, best, best >= 0, 1);

		/* most used potion (items 2..12; the health potions don't count) */
		best = -1;
		most = 0;
		for (int i = 0; i < 13; i++) {
			if (i != 0 && i != 1 && app->statItemUse[i] > most) {
				most = app->statItemUse[i];
				best = i;
			}
		}
		addStat(menu, MOD_S_POTION, NULL, MOD_S_HELP_POTION);
		addFavourite(menu, best, best >= 0, 0);

		/* ring worn longest (1..8) */
		best = -1;
		most = 0;
		for (int i = 1; i < 9; i++) {
			if (app->statRingTurns[i] > most) {
				most = app->statRingTurns[i];
				best = i;
			}
		}
		addStat(menu, MOD_S_RING, NULL, MOD_S_HELP_RING);
		addFavourite(menu, best, best >= 1, 2);

		MenuSystem_addItem(menu, MenuSystem_buildDivider(ms, S(MOD_S_KILLS100)), NULL, ITEM_HEADER_ROW, 0, 0,
				   NULL);
		addLevelList(menu, pl->killsDoneMaps);

		App_snprintf(buf, sizeof buf, S(MOD_S_FMT_SECRETS100), 0x80, 0x80);
		MenuSystem_addItem(menu, buf, NULL, ITEM_HEADER_ROW, 0, 0, NULL);
		addLevelList(menu, pl->secretsDoneMaps);
	}
}
