/*
 * Menu items: the MenuItem setters, the MenuSystem_addItem* family used by the
 * menu builders, and the builders that turn text into item lists (help pages,
 * yes/no prompts, quest notebook, statistics, ranking, item details).
 *
 * Every addItem helper takes the Menu builder (Applet+0x138) only for its three
 * scratch strings; the items themselves always go to g_app->menuSystem.
 */

#include "game.h"

/* Glyph strings in the executable's data area (the bitmap font maps these
 * codes to small icons). */
#define MOD_STR_ELLIPSIS  0x4649cu /* "\x7f": "more text" marker after a cut help line */
#define MOD_STR_CHECK     0x46744u /* "\x87": check mark (quest done / equipped) */

/* String-table group used by the menus. */
enum { TEXT_GROUP_GENERAL = 0, TEXT_GROUP_HELP = 2, TEXT_GROUP_MENU = 3 };

/* MenuItem.param value meaning "no icon". */
#define ITEM_NO_ICON 0x7f

static char *mod_cstr(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

/* The original's "has text" test: differs from an empty String and has at least
 * one character. */
static int str_hasText(String *s)
{
	String empty;
	int differs;

	String_ctor(&empty);
	differs = String_notEquals(s, &empty);
	String_dtor(&empty, 0);
	return differs != 0 && String_length(s) >= 1;
}

static MenuItem *nextItem(void)
{
	MenuSystem *ms = g_app->menuSystem;
	return &ms->items[ms->numItems++];
}

/* 0x00003214 */
void MenuItem_set(MenuItem *item, String *text, String *text2, int flags)
{
	String help;

	String_ctor(&help);
	MenuItem_setFull(item, text, text2, flags, 0, 0, &help);
	String_dtor(&help, 0);
}

/* 0x00003270 Fill an item. Help text is word-wrapped and limited to two lines;
 * a longer help is cut at a word and ends with the "more" glyph. */
void MenuItem_setFull(MenuItem *item, String *text, String *text2, int flags, int action, int param, String *help)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;

	String_setLength(&item->text, 0);
	String_setLength(&item->text2, 0);
	String_setLength(&item->help, 0);

	if (str_hasText(text)) {
		if ((flags & 2) == 0)
			TextUtil_wrapText(app->textUtil, text, 9999, &item->text);
		else
			String_appendString(&item->text, text);
	}
	if (str_hasText(text2)) {
		if ((flags & 2) == 0)
			TextUtil_wrapText(app->textUtil, text2, 9999, &item->text2);
		else
			String_appendString(&item->text2, text2);
	}

	if (str_hasText(help)) {
		/* the help line is narrower when an icon is drawn next to it */
		int width = (param == ITEM_NO_ICON) ? cv->textWidths[5] : cv->textWidths[6];
		int truncated = 0;
		int breaks = 0;
		int lastBreak = 0;
		int i;

		String_setLength(&cv->scratch, 0);
		if ((flags & 2) == 0)
			TextUtil_wrapTextLines(app->textUtil, help, width, &cv->scratch, 0, '|', &truncated);
		else
			String_appendString(&cv->scratch, help);

		/* first '|' becomes a newline, a second one ends the text */
		for (i = 0; i < String_length(&cv->scratch); i++) {
			if (String_charAt(&cv->scratch, i) == '|') {
				breaks++;
				if (breaks > 1) {
					String_setLength(&cv->scratch, i);
					truncated = 1;
					break;
				}
				String_setCharAt(&cv->scratch, i, '\n');
				lastBreak = i;
			}
		}

		if (truncated) {
			int len = String_length(&cv->scratch);
			if (width - 1 <= len - lastBreak) {
				/* make room for the marker: cut at the last space of the line */
				int cut = len - 4;
				int j;
				for (j = len - 4; lastBreak < j; j--) {
					if (String_charAt(&cv->scratch, j) == ' ') {
						cut = j;
						break;
					}
				}
				String_setLength(&cv->scratch, cut + 1);
			}
			String_appendCStr_76f0(&cv->scratch, mod_cstr(MOD_STR_ELLIPSIS));
		}
		String_appendString(&item->help, &cv->scratch);
	}

	item->flags = (uint32_t)flags;
	item->param = param;
	item->action = action;
}

/* 0x000045e8 */
void MenuSystem_addItem(Menu *tmp, String *text, String *text2, int flags)
{
	String help;

	String_ctor(&help);
	MenuSystem_addItemFull(tmp, text, text2, flags, 0, ITEM_NO_ICON, &help);
	String_dtor(&help, 0);
}

/* 0x00004644 */
void MenuSystem_addItemFull(Menu *tmp, String *text, String *text2, int flags, int action, int param, String *help)
{
	(void)tmp;
	MenuItem_setFull(nextItem(), text, text2, flags, action, param, help);
}

/* 0x000046bc */
void MenuSystem_addItemStrHelpId(Menu *tmp, String *text, String *text2, int flags, int action, int param, int helpId)
{
	String empty;
	int differs;

	String_setLength(&tmp->scratch0, 0);
	String_setLength(&tmp->scratch2, 0);
	String_ctor(&empty);
	differs = String_notEquals(text, &empty);
	String_dtor(&empty, 0);
	if (differs)
		String_appendString(&tmp->scratch0, text);
	Text_getString(g_app->canvas, TEXT_GROUP_MENU, helpId, &tmp->scratch2);
	MenuItem_setFull(nextItem(), &tmp->scratch0, text2, flags, action, param, &tmp->scratch2);
}

/* 0x000047a4 */
void MenuSystem_addItemId(Menu *tmp, int textId, String *text2, int flags)
{
	String help;

	String_setLength(&tmp->scratch0, 0);
	Text_getString(g_app->canvas, TEXT_GROUP_MENU, textId, &tmp->scratch0);
	MenuItem *item = nextItem();
	String_ctor(&help);
	MenuItem_setFull(item, &tmp->scratch0, text2, flags, 0, ITEM_NO_ICON, &help);
	String_dtor(&help, 0);
}

/* 0x00004868 */
void MenuSystem_addItemIdHelpId(Menu *tmp, int textId, String *text2, int flags, int action, int param, int helpId)
{
	Canvas *cv = g_app->canvas;

	String_setLength(&tmp->scratch0, 0);
	String_setLength(&tmp->scratch2, 0);
	Text_getString(cv, TEXT_GROUP_MENU, textId, &tmp->scratch0);
	Text_getString(cv, TEXT_GROUP_MENU, helpId, &tmp->scratch2);
	MenuItem_setFull(nextItem(), &tmp->scratch0, text2, flags, action, param, &tmp->scratch2);
}

/* 0x00004934 */
void MenuSystem_addItemIdFull(Menu *tmp, int textId, String *text2, int flags, int action, int param, String *help)
{
	String_setLength(&tmp->scratch0, 0);
	Text_getString(g_app->canvas, TEXT_GROUP_MENU, textId, &tmp->scratch0);
	MenuItem_setFull(nextItem(), &tmp->scratch0, text2, flags, action, param, help);
}

/* 0x000049d4 Item with a value column (copied to scratch1), no icon, empty help (299). */
void MenuSystem_addItemIdStr2(Menu *tmp, int textId, String *text2, int flags, int action)
{
	String empty;
	int differs;

	String_setLength(&tmp->scratch1, 0);
	String_ctor(&empty);
	differs = String_notEquals(&empty, text2);
	String_dtor(&empty, 0);
	if (differs)
		String_appendString(&tmp->scratch1, text2);
	MenuSystem_addItemIdHelpId(tmp, textId, &tmp->scratch1, flags, action, ITEM_NO_ICON, 299);
}

/* 0x00004a60 */
void MenuSystem_addItemIdId(Menu *tmp, int textId, int text2Id, int flags)
{
	Canvas *cv = g_app->canvas;
	String help;

	String_setLength(&tmp->scratch0, 0);
	String_setLength(&tmp->scratch1, 0);
	Text_getString(cv, TEXT_GROUP_MENU, textId, &tmp->scratch0);
	Text_getString(cv, TEXT_GROUP_MENU, text2Id, &tmp->scratch1);
	MenuItem *item = nextItem();
	String_ctor(&help);
	MenuItem_setFull(item, &tmp->scratch0, &tmp->scratch1, flags, 0, ITEM_NO_ICON, &help);
	String_dtor(&help, 0);
}

/* 0x00004b48 */
void MenuSystem_addItemIdIdFull(Menu *tmp, int textId, int text2Id, int flags, int action, int param, int helpId)
{
	Canvas *cv = g_app->canvas;

	String_setLength(&tmp->scratch0, 0);
	String_setLength(&tmp->scratch1, 0);
	String_setLength(&tmp->scratch2, 0);
	Text_getString(cv, TEXT_GROUP_MENU, textId, &tmp->scratch0);
	Text_getString(cv, TEXT_GROUP_MENU, text2Id, &tmp->scratch1);
	Text_getString(cv, TEXT_GROUP_MENU, helpId, &tmp->scratch2);
	MenuItem_setFull(nextItem(), &tmp->scratch0, &tmp->scratch1, flags, action, param, &tmp->scratch2);
}

/* 0x00004c34 */
void MenuSystem_addItemIdInt(Menu *tmp, int textId, int value, int flags, int action, int param, String *help)
{
	String_setLength(&tmp->scratch0, 0);
	String_setLength(&tmp->scratch1, 0);
	String_setLength(&tmp->scratch2, 0);
	Text_getString(g_app->canvas, TEXT_GROUP_MENU, textId, &tmp->scratch0);
	String_appendInt(&tmp->scratch1, value);
	String_appendString(&tmp->scratch2, help);
	MenuItem_setFull(nextItem(), &tmp->scratch0, &tmp->scratch1, flags, action, param, &tmp->scratch2);
}

/* 0x00004d10 */
void MenuSystem_addItemIdIntHelpId(Menu *tmp, int textId, int value, int flags, int action, int param, int helpId)
{
	Canvas *cv = g_app->canvas;

	String_setLength(&tmp->scratch0, 0);
	String_setLength(&tmp->scratch1, 0);
	String_setLength(&tmp->scratch2, 0);
	Text_getString(cv, TEXT_GROUP_MENU, textId, &tmp->scratch0);
	String_appendInt(&tmp->scratch1, value);
	Text_getString(cv, TEXT_GROUP_MENU, helpId, &tmp->scratch2);
	MenuItem_setFull(nextItem(), &tmp->scratch0, &tmp->scratch1, flags, action, param, &tmp->scratch2);
}

/* ---- small wrappers for the builders below ---- */

static void addTextItem(Menu *m, String *text, int flags)
{
	String empty;

	String_ctor(&empty);
	MenuSystem_addItem(m, text, &empty, flags);
	String_dtor(&empty, 0);
}

static void addIdItem(Menu *m, int textId, int flags)
{
	String empty;

	String_ctor(&empty);
	MenuSystem_addItemId(m, textId, &empty, flags);
	String_dtor(&empty, 0);
}

static void addBlankItem(Menu *m, int flags)
{
	String a, b;

	String_ctor(&a);
	String_ctor(&b);
	MenuSystem_addItem(m, &a, &b, flags);
	String_dtor(&b, 0);
	String_dtor(&a, 0);
}

static void addLiteralItem(Menu *m, char *text, int flags)
{
	String s;

	String_ctorCStr(&s, text);
	addTextItem(m, &s, flags);
	String_dtor(&s, 0);
}

/* Section title "...... TEXT ......" (flags 0x49: header, centred, unselectable). */
static void addTitleId(Menu *m, int textId, int alt)
{
	MenuSystem *ms = g_app->menuSystem;
	String *title = (String *)(alt ? Canvas_buildDottedTitleId3b(ms, textId) : Canvas_buildDottedTitleId3(ms, textId));
	addTextItem(m, title, 0x49);
}

/* Dotted title as a plain centred row (flags 8), as the About and ranking
 * pages add it. */
static void addCentredTitleId(Menu *m, int textId)
{
	addTextItem(m, (String *)Canvas_buildDottedTitleId3(g_app->menuSystem, textId), 8);
}

/* 0x00002f90 Prompt text (split at '\n'), a blank line, then Yes (action 1) and
 * No (action 0). defaultYes == 1 preselects Yes. */
void MenuSystem_addTextLines(Menu *tmp, String *text, int flags)
{
	MenuSystem *ms = g_app->menuSystem;
	String line, empty;

	if (String_indexOf(text, '\n', 0) < 0) {
		addTextItem(tmp, text, 9);
	} else {
		int from = 0;
		int end;
		while ((end = String_indexOf(text, '\n', from)) >= 0) {
			String_substring(&line, text, from, end);
			addTextItem(tmp, &line, 9);
			String_dtor(&line, 0);
			from = end + 1;
		}
		String_substring(&line, text, from, 0);
		addTextItem(tmp, &line, 9);
		String_dtor(&line, 0);
	}
	addBlankItem(tmp, 9);

	String_ctor(&line);
	String_ctor(&empty);
	MenuSystem_addItemIdFull(tmp, 0x121, &line, 8, 1, 0, &empty);
	String_dtor(&empty, 0);
	String_dtor(&line, 0);

	String_ctor(&line);
	String_ctor(&empty);
	MenuSystem_addItemIdFull(tmp, 0x122, &line, 8, 0, 0, &empty);
	String_dtor(&empty, 0);
	String_dtor(&line, 0);

	ms->selectedIndex = (flags == 1) ? ms->numItems - 2 : ms->numItems - 1;
}

/* 0x000031b0 */
void MenuSystem_setYesNo(Menu *tmp, int strId, int defaultYes)
{
	String_setLength(&tmp->scratch0, 0);
	Text_getString(g_app->canvas, TEXT_GROUP_MENU, strId, &tmp->scratch0);
	MenuSystem_addTextLines(tmp, &tmp->scratch0, defaultYes);
}

/* Add one '|'-separated segment of a help text: "@Title" segments become a
 * dotted section title. */
static void addHelpSegment(Menu *tmp, String *seg, int flags, int isTitle)
{
	if (isTitle) {
		String rest;
		String_substring(&rest, seg, 1, 0);
		String *title = (String *)Canvas_buildDottedTitle(g_app->menuSystem, &rest);
		addTextItem(tmp, title, flags | 10);
		String_dtor(&rest, 0);
	} else {
		addTextItem(tmp, seg, flags | 2);
	}
}

/* 0x000027f0 Word-wrap a text and add one item per line. */
void MenuSystem_loadHelpItems(Menu *tmp, String *text, int flags)
{
	Applet *app = g_app;
	String wrapped, copy, seg;
	int from = 0;
	int end;

	String_ctor(&wrapped);
	TextUtil_wrapText(app->textUtil, text, app->canvas->textWidths[0], &wrapped);
	String_ctorCopy(&copy, &wrapped);

	while ((end = String_indexOf(&copy, '|', from)) >= 0) {
		String_substring(&seg, &copy, from, end);
		int isTitle = String_length(&seg) >= 1 && String_charAt(&seg, 0) == '@';
		addHelpSegment(tmp, &seg, flags, isTitle);
		from = end + 1;
		String_dtor(&seg, 0);
	}
	if (from < String_length(&copy)) {
		String_substring(&seg, &copy, from, 0);
		addHelpSegment(tmp, &seg, flags, String_charAt(&seg, 0) == '@');
		String_dtor(&seg, 0);
	}
	String_dtor(&copy, 0);
	String_dtor(&wrapped, 0);
}

/* 0x00002a1c Help page idx of the help string group, read from strings.bin. */
void MenuSystem_loadHelpResource(Menu *tmp, int idx)
{
	Applet *app = g_app;
	String text;

	app->menuSystem->scrollIndex = 0;
	app->menuSystem->selectedIndex = 0;
	String_ctor(&text);
	Text_readStringFromRes(app->canvas, TEXT_GROUP_HELP, idx, &text);
	MenuSystem_loadHelpItems(tmp, &text, 0);
	String_dtor(&text, 0);
}

/* 0x00002750 "About" page: title, version line, legal text (help page 8). */
void MenuSystem_loadAbout(Menu *tmp)
{
	addCentredTitleId(tmp, 0x123);
	addIdItem(tmp, 0x81, 0);
	MenuSystem_loadHelpResource(tmp, 8);
}

/* 0x00002aa4 Quest log: map name, then each notebook entry (check mark when
 * done, "[Failed]" line and greyed text when failed). Scrolled to the end. */
void MenuSystem_loadNotebook(Menu *tmp)
{
	Applet *app = g_app;
	MenuSystem *ms = app->menuSystem;
	Canvas *cv = app->canvas;
	Player *pl = app->player;
	String a, b;
	int i;

	String_ctor(&a);
	MenuItem_set(nextItem(), &app->render->mapName, &a, 8);
	String_dtor(&a, 0);

	String_ctor(&a);
	String *line = (String *)Canvas_buildDottedTitle(ms, &a);
	String_ctor(&b);
	MenuItem_set(nextItem(), line, &b, 0x49);
	String_dtor(&b, 0);
	String_dtor(&a, 0);

	for (i = 0; i < pl->numNotebookIndexes; i++) {
		int flags = 0;

		String_setLength(&tmp->detailsText, 0);
		Text_getString(cv, cv->mapStringGroup, pl->notebookIndexes[i], &tmp->detailsText);
		if (Player_isQuestFailed(pl, i) == 0) {
			if (Player_isQuestDone(pl, i) != 0) {
				String_ctorCStr(&a, mod_cstr(MOD_STR_CHECK));
				String_insert(&tmp->detailsText, 0, &a);
				String_dtor(&a, 0);
				String_ctorCStr(&a, " ");
				String_insert(&tmp->detailsText, 1, &a);
				String_dtor(&a, 0);
			}
		} else {
			String_setLength(&cv->scratch, 0);
			Text_getString(cv, TEXT_GROUP_MENU, 0x15c, &cv->scratch);
			MenuItem *item = nextItem();
			String_ctor(&a);
			MenuItem_set(item, &cv->scratch, &a, 0);
			String_dtor(&a, 0);
			flags = 4;
		}
		MenuSystem_loadHelpItems(tmp, &tmp->detailsText, flags);

		MenuItem *gap = nextItem();
		String_ctor(&a);
		String_ctor(&b);
		MenuItem_set(gap, &a, &b, 0);
		String_dtor(&b, 0);
		String_dtor(&a, 0);

		gap = nextItem();
		String_ctor(&a);
		String_ctor(&b);
		MenuItem_set(gap, &a, &b, 0);
		String_dtor(&b, 0);
		String_dtor(&a, 0);
	}

	int top = ms->numItems - (cv->viewRect[3] - 0x1a) / 12;
	ms->scrollIndex = top;
	ms->selectedIndex = top;
	if (top < 0) {
		ms->selectedIndex = 0;
		ms->scrollIndex = 0;
	}
}

/* 0x0000182c "base/max", or "base/(max+mod)" / "base+mod" when a modifier applies. */
int MenuSystem_buildModStat(Menu *self, int base, int max, int mod, String *outStr)
{
	(void)self;
	int sign = (mod < 1) ? '-' : '+';

	String_setLength(outStr, 0);
	if (mod == 0) {
		String_appendInt(outStr, base);
		if (max != -1) {
			String_appendCStr_76f0(outStr, "/");
			String_appendInt(outStr, max);
		}
	} else {
		String_appendInt(outStr, base);
		if (max != -1) {
			String_appendCStr_76f0(outStr, "/(");
			String_appendInt(outStr, max);
		}
		String_appendChar(outStr, sign);
		String_appendInt(outStr, mod);
		if (max != -1)
			String_appendCStr_76f0(outStr, ")");
	}
	return 0;
}

/* 0x00001d54 End-of-game ranking. The thief (bonus) score picks a rank text;
 * on the harder difficulties a low score is raised to the next threshold. */
void MenuSystem_fillRanking(Menu *tmp)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	int bonus = Player_calcBonusScore(app->player);
	int score = Player_calcScore(app->player);
	int difficulty = app->game->difficulty;
	int rank = -1;

	/* rank name / description string ids (group 0) for ranks 0..3 */
	static const struct { int text, help; } ranks[4] = { { 0x13, 0x14 }, { 0x15, 0x16 }, { 0x17, 0x18 }, { 0x19, 0x1a } };

	addLiteralItem(tmp, " ", 0);

	if (difficulty == 1) {
		addCentredTitleId(tmp, 0x124);
	} else if (difficulty == 2) {
		addCentredTitleId(tmp, 0x125);
		if (bonus < 15000) {
			bonus = 15000;
			rank = 1;
		} else if (bonus < 18000) {
			bonus = 18000;
			rank = 2;
		} else if (bonus < 20000) {
			bonus = 20000;
			rank = 3;
		}
	} else {
		addCentredTitleId(tmp, 0x126);
		if (bonus < 15000) {
			bonus = 18000;
			rank = 2;
		} else if (bonus < 18000) {
			bonus = 20000;
			rank = 3;
		}
	}
	if (rank < 0) {
		/* exactly 20000 is the top rank; anything from 18000 (even above 20000) is rank 2 */
		if (bonus == 20000)
			rank = 3;
		else if (bonus >= 18000)
			rank = 2;
		else if (bonus < 15000)
			rank = 0;
		else
			rank = 1;
	}
	int rankText = ranks[rank].text;
	int rankHelp = ranks[rank].help;

	addLiteralItem(tmp, " ", 0);
	addIdItem(tmp, 0x127, 8);
	String_setLength(&tmp->scratch0, 0);
	String_appendInt(&tmp->scratch0, bonus);
	addTextItem(tmp, &tmp->scratch0, 0x308);

	addIdItem(tmp, 0x128, 8);
	String_setLength(&tmp->scratch0, 0);
	String_appendInt(&tmp->scratch0, score);
	addTextItem(tmp, &tmp->scratch0, 0x308);

	addIdItem(tmp, 0x129, 8);
	String_setLength(&cv->scratch, 0);
	Text_getString(cv, TEXT_GROUP_GENERAL, rankText, &cv->scratch);
	addTextItem(tmp, &cv->scratch, 0x308);

	addLiteralItem(tmp, "", 0);
	String_setLength(&cv->scratch, 0);
	Text_getString(cv, TEXT_GROUP_GENERAL, rankHelp, &cv->scratch);
	MenuSystem_loadHelpItems(tmp, &cv->scratch, 0);
}

/* Statistics rows are "label ... value" with a help line; flags 0x400 = info only. */
static void addStatRow(Menu *menu, int textId, String *value, int helpId)
{
	MenuSystem_addItemIdHelpId(menu, textId, value, 0x400, 0, ITEM_NO_ICON, helpId);
}

static void addStatInt(Menu *menu, int textId, int value, int helpId)
{
	MenuSystem_addItemIdIntHelpId(menu, textId, value, 0x400, 0, ITEM_NO_ICON, helpId);
}

/* "Most used ..." row: the item's name, or "None" (0x3a). */
static void addFavouriteRow(Menu *menu, int labelId, int helpId, EntityDef *def)
{
	String empty;

	String_ctor(&empty);
	MenuSystem_addItemIdHelpId(menu, labelId, &empty, 0x400, 0, ITEM_NO_ICON, helpId);
	String_dtor(&empty, 0);

	if (def == NULL) {
		String_ctor(&empty);
		MenuSystem_addItemIdHelpId(menu, 0x3a, &empty, 0x401, 0, ITEM_NO_ICON, 300);
		String_dtor(&empty, 0);
	} else {
		String name, text2;
		String_ctorCStr(&name, "  ");
		String_appendString(&name, &def->name);
		String_ctor(&text2);
		MenuSystem_addItem(menu, &name, &text2, 0x401);
		String_dtor(&text2, 0);
		String_dtor(&name, 0);
	}
}

/* List of levels whose bit is set in mask, or "None" (0x41). */
static void addLevelList(Menu *menu, uint32_t mask)
{
	Applet *app = g_app;
	String empty;

	if (mask == 0) {
		String_ctor(&empty);
		MenuSystem_addItemIdHelpId(menu, 0x41, &empty, 0x400, 0, ITEM_NO_ICON, 300);
		String_dtor(&empty, 0);
		return;
	}
	for (int i = 0; i < app->game->numLevels; i++) {
		if (mask & arm_bit(i)) {
			String_ctor(&empty);
			MenuSystem_addItemIdHelpId(menu, app->game->levelNames[i], &empty, 0x400, 0, ITEM_NO_ICON, 300);
			String_dtor(&empty, 0);
		}
	}
}

/* 0x00018f9c Player / level / game statistics sections. */
void Menu_buildStatistics(Menu *menu, int levelStats, int playerStats, int gameStats)
{
	Applet *app = g_app;
	Player *pl = app->player;
	Canvas *cv = app->canvas;
	String *out = &cv->scratch;
	int levelTime = app->playTime - app->game->levelStartTime;
	int lockpickRating = 0;
	int counts[2];

	/* Player+0x26c packs two 16-bit counters; the row shows their ratio. */
	uint32_t packed = (uint32_t)pl->lockpickTries;
	if (packed != 0) {
		int32_t num = (int32_t)((packed & 0xff00ffffu) << 8);
		int32_t den = (int32_t)packed >> 16;
		/* the original divides unguarded (ARM runtime div-by-zero trap) */
		lockpickRating = (den != 0 ? num / den : 0);
		lockpickRating = (lockpickRating >> 6) + 1;
	}

	if (playerStats) {
		addTitleId(menu, 0, 0);

		int bonus = CombatEntity_getMaxHealth(&pl->ce) - CombatEntity_getMaxHealth(&pl->baseCe);
		MenuSystem_buildModStat(menu, CombatEntity_getHealth(&pl->ce), CombatEntity_getMaxHealth(&pl->baseCe), bonus, out);
		addStatRow(menu, 1, out, 2);

		MenuSystem_buildModStat(menu, CombatEntity_getArmorPoints(&pl->ce), CombatEntity_getMaxArmorPoints(&pl->ce), 0, out);
		addStatRow(menu, 3, out, 4);

		addStatInt(menu, 5, pl->gold, 6);
		addStatInt(menu, 7, pl->level, 8);
		addStatInt(menu, 9, pl->currentXP, 10);
		addStatInt(menu, 0xb, pl->nextLevelXP, 0xc);

		bonus = CombatEntity_getDefense(&pl->ce) - CombatEntity_getDefense(&pl->baseCe);
		MenuSystem_buildModStat(menu, CombatEntity_getDefense(&pl->baseCe), -1, bonus, out);
		addStatRow(menu, 0xd, out, 0xe);

		bonus = CombatEntity_getStrength(&pl->ce) - CombatEntity_getStrength(&pl->baseCe);
		MenuSystem_buildModStat(menu, CombatEntity_getStrength(&pl->baseCe), -1, bonus, out);
		addStatRow(menu, 0xf, out, 0x10);

		bonus = CombatEntity_getAccuracy(&pl->ce) - CombatEntity_getAccuracy(&pl->baseCe);
		MenuSystem_buildModStat(menu, CombatEntity_getAccuracy(&pl->baseCe), -1, bonus, out);
		addStatRow(menu, 0x11, out, 0x12);
	}

	if (levelStats) {
		addTitleId(menu, 0x13, 0);
		Player_formatTime(pl, levelTime, out);
		addStatRow(menu, 0x14, out, 0x15);

		Render_countSecrets(pl, counts);
		MenuSystem_buildModStat(menu, counts[0], counts[1], 0, out);
		addStatRow(menu, 0x16, out, 0x17);

		Game_countMonsters(pl, counts);
		MenuSystem_buildModStat(menu, counts[0], counts[1], 0, out);
		addStatRow(menu, 0x18, out, 0x19);

		addStatInt(menu, 0x1a, pl->moves, 0x1b);
		if (!gameStats)
			addStatInt(menu, 0x34, lockpickRating, 0x35);
		addStatInt(menu, 0x1c, pl->xpGained, 0x1d);
	}

	if (gameStats) {
		addTitleId(menu, (levelStats == 0 && playerStats == 0) ? 0x1f : 0x1e, 0);
		String_setLength(out, 0);
		Player_formatTime(pl, app->playTime - pl->playTime + pl->totalTime, out);
		addStatRow(menu, 0x14, out, 0x20);
		if (!playerStats)
			addStatInt(menu, 7, pl->level, 8);
		addStatInt(menu, 0x1a, pl->totalMoves, 0x21);
		addStatInt(menu, 0x22, pl->totalDeaths, 0x23);
		addStatInt(menu, 0x24, app->statGoldGained, 0x25);
		addStatInt(menu, 0x26, app->statGoldSpent, 0x27);
		addStatInt(menu, 0x28, app->statBiggestHit, 0x29);
		addStatInt(menu, 0x2a, app->statDamageDealt, 0x2b);
		addStatInt(menu, 0x2c, app->statGibs, 0x2d);
		addStatInt(menu, 0x2e, app->stat5c8, 0x2f);
		addStatInt(menu, 0x30, app->stat5d0, 0x31);
		addStatInt(menu, 0x32, app->statSteals, 0x33);
		addStatInt(menu, 0x34, lockpickRating, 0x35);

		addTitleId(menu, 0x36, 0);

		/* favourite weapon: most shots fired */
		int best = 0, bestIdx = -1, i;
		for (i = 0; i < 11; i++) {
			if (best < app->statShotsByWeapon[i]) {
				best = app->statShotsByWeapon[i];
				bestIdx = i;
			}
		}
		addFavouriteRow(menu, 0x38, 0x39, bestIdx < 0 ? NULL : EntityDefs_find(app->entityDefs, 6, 1, bestIdx & 0xff));

		/* favourite item: most used, skipping the two health potions */
		best = 0;
		bestIdx = -1;
		for (i = 0; i < 15; i++) {
			if (i != 0 && i != 1 && app->statItemsUsed[i] > best) {
				best = app->statItemsUsed[i];
				bestIdx = i;
			}
		}
		addFavouriteRow(menu, 0x3b, 0x3c, bestIdx < 0 ? NULL : EntityDefs_find(app->entityDefs, 6, 0, bestIdx & 0xff));

		/* favourite ring: most turns worn (ring 0 = none) */
		best = 0;
		bestIdx = -1;
		for (i = 1; i < 9; i++) {
			if (best < app->statTurnsByRing[i]) {
				best = app->statTurnsByRing[i];
				bestIdx = i;
			}
		}
		addFavouriteRow(menu, 0x3d, 0x3e, bestIdx < 1 ? NULL : EntityDefs_find(app->entityDefs, 6, 2, bestIdx & 0xff));

		addTitleId(menu, 0x3f, 1);
		addLevelList(menu, pl->killedMonstersLevels);
		addTitleId(menu, 0x40, 1);
		addLevelList(menu, pl->foundSecretsLevels);
	}
}

/* 0x0003a058 Split a '|'-separated text into raw lines starting at item `first`. */
void MenuSystem_setItemsFromText(MenuSystem *ms, int first, String *text)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	String wrapped, part;
	int from = 0;
	int end;

	ms->numItems = first;
	String_ctor(&wrapped);
	TextUtil_wrapText(app->textUtil, text, cv->textWidths[0], &wrapped);

	while ((end = String_indexOf(&wrapped, '|', from)) >= 0) {
		String_setLength(&cv->scratch, 0);
		if (from != end) {
			String_substring(&part, &wrapped, from, end);
			String_appendString(&cv->scratch, &part);
			String_dtor(&part, 0);
		}
		String_ctor(&part);
		MenuItem_set(&ms->items[first++], &cv->scratch, &part, 2);
		String_dtor(&part, 0);
		from = end + 1;
	}

	String_setLength(&cv->scratch, 0);
	String_substring(&part, &wrapped, from, 0);
	String_appendString(&cv->scratch, &part);
	String_dtor(&part, 0);
	String_ctor(&part);
	MenuItem_set(&ms->items[first], &cv->scratch, &part, 2);
	String_dtor(&part, 0);
	ms->numItems = first + 1;
	String_dtor(&wrapped, 0);
}

/* 0x0003b604 Volume row "Volume  NN%" (flag 0x80 = slider), or blank when sound is off. */
void Menus_setVolumeItem(Menu *menus, int item)
{
	Applet *app = g_app;
	MenuItem *it = &app->menuSystem->items[item];

	if (app->sndSys->enabled == 0) {
		String a, b;
		String_ctor(&a);
		String_ctor(&b);
		MenuItem_set(it, &a, &b, 0);
		String_dtor(&b, 0);
		String_dtor(&a, 0);
	} else {
		String_setLength(&menus->scratch0, 0);
		Text_getString(app->canvas, TEXT_GROUP_MENU, 0x8c, &menus->scratch0);
		TextUtil_dehyphenate(app->textUtil, &menus->scratch0);
		String_setLength(&menus->scratch1, 0);
		String_appendChar(String_appendInt(&menus->scratch1, menus->volume), '%');
		MenuItem_set(it, &menus->scratch0, &menus->scratch1, 0x80);
	}
	menus->soundItem = item;
}
