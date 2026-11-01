/*
 * Canvas dialogs (state 7): word-wrapped text boxes in nine styles, typed out
 * line by line, paged with the keys; the help-message queue that turns item
 * pick-ups into "You got a new ..." help dialogs; the password prompt.
 *
 * Styles (Canvas.dialogStyle): 0 plain (black), 1 wand speech (purple, wand
 * icon, indented paragraphs), 2 help (black, title bar), 3 parchment scroll,
 * 4 item acquired (green, item icon), 5..8 red (6/7 typewriter text, 7 at the
 * top of the view).
 */

#include "ui_local.h"

/* help texts for new items */
#define MOD_STR_GOT_POTION   0x2d368u
#define MOD_STR_HELP_POTION  0x2d308u
#define MOD_STR_GOT_ITEM     0x2d3dcu
#define MOD_STR_HELP_ITEM    0x2d380u
#define MOD_STR_GOT_WEAPON   0x2d3f0u
#define MOD_STR_HELP_WEAPON  0x2d408u
#define MOD_STR_GOT_RING     0x2d4f0u
#define MOD_STR_HELP_RING    0x2d494u
#define MOD_STR_GOT_ARMOR    0x2d504u
#define MOD_FMT_HELP4        0x2d518u
#define MOD_FMT_HELP3        0x2d528u
#define MOD_STR_INDENT       0x2d300u /* paragraph indent of style 1 */

#define DIALOG_TEXT_SIZE 0x400
/* Text_wrap 0x2a84 can write its NUL one byte past the size it is given (see re/notes/classic-1.0.md). The
 * phone's stray byte zeroed the low byte of the next Canvas field (the dialogItem pointer); on a 64-bit
 * host it would corrupt that pointer, so the wraps get one byte less. No game text comes near the limit (the
 * longest is 943 characters), so the output is the same. */
#define DIALOG_WRAP_ROOM (DIALOG_TEXT_SIZE - 1)
#define HELP_QUEUE_SIZE  8

/* dialog flags */
enum {
	DLG_NO_CLOSE = 0x01,  /* the cancel keys do not close it (cinematics) */
	DLG_TALL = 0x02,      /* 8 lines, from below the top bar */
	DLG_SILENT = 0x08,    /* no opening sound */
};

/* key actions (Canvas_getKeyAction) */
enum {
	ACT_UP = 1,     /* UP, 2 */
	ACT_DOWN = 2,   /* DOWN, 8 */
	ACT_LEFT = 3,   /* LEFT, 4 */
	ACT_RIGHT = 4,  /* RIGHT, 6 */
	ACT_MENU = 5,   /* SOFT1, 0 */
	ACT_SELECT = 6, /* SELECT, 5 */
	ACT_SKIP = 0xe, /* 9 */
	ACT_BACK = 0xf, /* CLR */
};

/* 0x00007288 */
void Canvas_closeDialog(Canvas *canvas)
{
	uint8_t style = canvas->dialogStyle;

	if (style != 2 && style != 4 && style != 3 && style != 0) {
		/* talking put the wand up; give back the weapon held before */
		Player *player = canvas->app->player;
		if (player->prevWeapon != player->ce.weapon && player->ce.weapon == 0)
			Player_selectWeapon(player, player->prevWeapon);
	}
	Player_addPausedTime(canvas->app->player, canvas->app->tickTime - canvas->feb8);
	canvas->dialogText[0] = '\0';
	Canvas_setState(canvas, Game_isCinematicActive(canvas->game) ? 0x14 : 3);
	if (canvas->dialogStyle == 4)
		Sound_play(canvas->app->sound, 0x1411, 0, 3, -1);
	if (canvas->fec8 != 0)
		Canvas_setState(canvas, 2);
}

/* 0x0001f378
 * Wraps the text into Canvas.dialogText ('|' line marks) and builds the line
 * table, then resets the typing state and plays the style's sound. */
void Canvas_setupDialog(Canvas *canvas, char *text, int style, int flags)
{
	char *dst = canvas->dialogText;
	int len, i, start, snd;

	if (style == 1) {
		/* the first two lines are indented, the rest uses the full width */
		int used1, used2, pos;

		strcpy(dst, Ui_str(MOD_STR_INDENT));
		used1 = Text_wrap(text, canvas->textCols[2] - 4, dst + 4, DIALOG_WRAP_ROOM - 4, 1, '|', NULL);
		pos = (int)strlen(dst);
		strcpy(dst + pos, Ui_str(MOD_STR_INDENT));
		used2 = Text_wrap(text + used1, canvas->textCols[2] - 4, dst + pos + 4, DIALOG_WRAP_ROOM - pos - 4, 1, '|',
		                  NULL);
		pos = (int)strlen(dst);
		Text_wrap(text + used1 + used2, canvas->textCols[2], dst + pos, DIALOG_WRAP_ROOM - pos, 0, '|', NULL);
	} else if (style == 3) {
		Text_wrap(text, canvas->textCols[2] - 2, dst, DIALOG_WRAP_ROOM, 0, '|', NULL);
	} else {
		Text_wrap(text, canvas->textCols[2], dst, DIALOG_WRAP_ROOM, 0, '|', NULL);
		if (style == 0 || style == 5 || style == 8) {
			/* talking to someone ends invisibility */
			Player_removeBuff(canvas->app->player, 5);
			Player_updateStats(canvas->app->player);
		}
	}

	len = (int)strlen(dst);
	canvas->dialogNumLines = 0;
	start = 0;
	for (i = 0; i < len; i++) {
		if (dst[i] == '|') {
			canvas->dialogLines[canvas->dialogNumLines * 2] = (int16_t)start;
			canvas->dialogLines[canvas->dialogNumLines * 2 + 1] = (int16_t)(i - start);
			canvas->dialogNumLines++;
			start = i + 1;
		}
	}
	canvas->dialogLines[canvas->dialogNumLines * 2] = (int16_t)start;
	canvas->dialogLines[canvas->dialogNumLines * 2 + 1] = (int16_t)(len - start);
	canvas->dialogNumLines++;

	canvas->dialogScroll = 0;
	canvas->dialogTime = canvas->app->tickTime;
	canvas->febc = 0;
	canvas->feb8 = canvas->app->tickTime;
	canvas->dialogItem = NULL;
	canvas->dialogFlags = (uint8_t)flags;
	canvas->dialogStyle = (uint8_t)style;
	if (flags & DLG_TALL)
		canvas->linesPerPage = 8;
	else if (style == 3)
		canvas->linesPerPage = 6;
	else
		canvas->linesPerPage = 4;

	if (flags & DLG_SILENT)
		return;
	if (style == 1)
		snd = 0x141e;
	else if (style == 2)
		snd = 0x13bd;
	else if (style == 4)
		return;
	else
		snd = 0x141d;
	Sound_play(canvas->app->sound, snd, 0, 3, -1);
}

/* 0x000277f4
 * Opens a dialog. owner is the script thread to resume when it closes
 * (Canvas.dialogHasOwner = extra says whether there is one). */
void Canvas_showDialog(Canvas *cv, void *owner, void *text, int style, int flags, int extra)
{
	if (Game_isCinematicActive(cv->game))
		flags |= DLG_NO_CLOSE;
	cv->fec8 = 0;
	cv->dialogHasOwner = extra;
	cv->dialogOwner = owner;
	Canvas_setState(cv, 7);
	Canvas_setupDialog(cv, text, style, flags);
}

/* 0x000278c4
 * The PASSWORD prompt (combination lock, state 8). With the debug "show codes"
 * option the answer is put in the wand's bubble: the PASSWORD command (op 10)
 * of the facing entity's tile event holds it as a map string, each character
 * stored minus 'P'. */
void Canvas_showChatDialog(Canvas *cv, void *owner, void *text)
{
	Entity *facing;
	Render *render;
	int ev, first, count, k, i;
	char code[5];

	Canvas_setState(cv, 8);
	cv->dialogHasOwner = 1;
	cv->dialogOwner = owner;
	Canvas_setupDialog(cv, text, 0, 0);
	if (!cv->showCodes)
		return;

	facing = cv->app->player->facing;
	if (facing == NULL)
		return;
	ev = Render_findTileEvent(cv->app->render, facing->linkIndex);
	if (ev < 0)
		return;
	render = cv->app->render;
	first = (int)((render->tileEvents[ev] & 0xfffff) >> 9);	/* (DS: 11 bits from bit 9) */
	count = (int)((render->tileEvents[ev] & 0x7ffffff) >> 20);
	for (k = 0; k < count; k++) {
		if (render->commands[first + k].op == 10) {
			const char *enc = cv->app->render->mapStrings[render->commands[first + k].arg1 & 0xff];
			for (i = 0; i < 4; i++)
				code[i] = (char)(enc[i] + 'P');
			code[4] = '\0';
			Hud_addTimedMsg(cv->app->hud, code);
			return;
		}
	}
}

/* 0x000078ec
 * Shows the next queued help message (when playing, or forced). Type 0/2 are
 * texts (2 without sound); type 1 is an EntityDef just picked up, explained
 * with "NAME|You got a new ...|description||how to use it". */
void Canvas_dequeueHelpDialog(Canvas *canvas, int force)
{
	char upper[32];
	char buf[600];
	int flags = 1;
	int type, i;
	void *owner;

	if (canvas->helpCount == 0)
		return;
	if (!force && canvas->state != 3 && canvas->game->monsterTurnMode == 0)
		return;

	type = canvas->helpQueue[0].type;
	if (type == 0 || type == 2) {
		/* Player_showHelp(2) (floor ring pickup) queues a NULL text; the original strcpy'd from
		 * address 0. No map places a ring on the floor; an empty box instead of a crash. */
		strcpy(buf, canvas->helpQueue[0].data ? (const char *)canvas->helpQueue[0].data : "");
	} else if (type == 1) {
		EntityDef *def = canvas->helpQueue[0].data;
		const char *src = def->longName;
		char *d = upper;
		const char *title, *help = NULL;

		while (*src != '\0') {
			uint8_t c = (uint8_t)*src++;
			if ((uint8_t)(c - 'a') <= 25)
				c -= 0x20;
			*d++ = (char)c;
		}
		*d = '\0';

		switch (def->eSubType) {
		case 0: /* consumables: potions are 0..10 */
			if (def->parm >= 0 && def->parm < 11) {
				title = Ui_str(MOD_STR_GOT_POTION);
				help = Ui_str(MOD_STR_HELP_POTION);
			} else {
				title = Ui_str(MOD_STR_GOT_ITEM);
				help = Ui_str(MOD_STR_HELP_ITEM);
			}
			break;
		case 1:
			title = Ui_str(MOD_STR_GOT_WEAPON);
			if (!(canvas->app->render->mapFlagBits & 2)) /* maps without weapons skip the hint */
				help = Ui_str(MOD_STR_HELP_WEAPON);
			break;
		case 2:
			title = Ui_str(MOD_STR_GOT_RING);
			help = Ui_str(MOD_STR_HELP_RING);
			break;
		case 7:
			title = Ui_str(MOD_STR_GOT_ARMOR);
			break;
		default:
			return; /* left in the queue, as in the original */
		}
		if (help != NULL)
			App_snprintf(buf, sizeof buf, Ui_str(MOD_FMT_HELP4), upper, title, def->description, help);
		else
			App_snprintf(buf, sizeof buf, Ui_str(MOD_FMT_HELP3), upper, title, def->description);
	} else {
		/* unknown types are dequeued without a text (the original shows its
		 * uninitialised buffer) */
		buf[0] = '\0';
	}
	if (canvas->helpQueue[0].type == 2)
		flags |= DLG_SILENT;
	owner = canvas->helpQueue[0].owner;

	for (i = 1; i < canvas->helpCount; i++)
		canvas->helpQueue[i - 1] = canvas->helpQueue[i];
	canvas->helpCount--;
	if (canvas->player->helpEnabled)
		Canvas_showDialog(canvas, owner, buf, 2, flags, owner != NULL);
	else if (owner != NULL)
		ScriptThread_run(owner);
}

/* DS 0x0201b7e0: a script's help message; its thread waits until the message is closed. 0 when
 * help is off (or the queue is full): the thread goes on at once */
int Canvas_addScriptHelp(Canvas *canvas, char *str, void *owner)
{
	int n;

	if (!canvas->player->helpEnabled || canvas->helpCount == HELP_QUEUE_SIZE)
		return 0;
	n = canvas->helpCount;
	canvas->helpCount = n + 1;
	canvas->helpQueue[n].type = 0;
	canvas->helpQueue[n].data = str;
	canvas->helpQueue[n].owner = owner;
	if (canvas->state == 3)
		Canvas_dequeueHelpDialog(canvas, 0);
	return 1;
}

/* 0x0000db60 */
void Canvas_addHelpMessage(Canvas *canvas, char *str, int type)
{
	int n;

	if (!canvas->player->helpEnabled || canvas->helpCount == HELP_QUEUE_SIZE)
		return;
	n = canvas->helpCount;
	canvas->helpCount = n + 1;
	canvas->helpQueue[n].type = (uint8_t)type;
	canvas->helpQueue[n].data = str;
	canvas->helpQueue[n].owner = NULL;
	if (canvas->state == 3)
		Canvas_dequeueHelpDialog(canvas, 0);
}

/* 0x0000db14  help for a newly acquired item (EntityDef) */
void Canvas_addHelpText(Canvas *canvas, void *def)
{
	int n;

	if (!canvas->player->helpEnabled || canvas->helpCount == HELP_QUEUE_SIZE)
		return;
	n = canvas->helpCount;
	canvas->helpCount = n + 1;
	canvas->helpQueue[n].type = 1;
	canvas->helpQueue[n].data = def;
	canvas->helpQueue[n].owner = NULL;
	if (canvas->state == 3)
		Canvas_dequeueHelpDialog(canvas, 0);
}

/* 0x00007a80 */
void Canvas_drawDialog(Canvas *canvas)
{
	int w, y, h;
	int textX = 1;
	int textFlags = 0;
	int fill = UI_BLACK;
	int border = UI_WHITE;
	int headerLines = 0; /* lines shown in the title bar, not in the body */
	int i, lineY;

	if (canvas->dialogText[0] == '\0')
		return;

	/* the box spans the view's width (x = 0) */
	w = canvas->viewW;
	h = 0x36;
	y = canvas->viewH - 0x36;
	switch (canvas->dialogStyle) {
	case 1:
		y = canvas->viewH - 0x5e;
		textFlags = 8;
		fill = (int)0xff6633ccu;
		break;
	case 3:
		textX = 0xf;
		y = canvas->sceneY + (canvas->sceneH >> 1);
		h = 0x4e;
		Canvas_drawDialogFrame(canvas, 0, y - 0xc, w, 100, 0);
		break;
	case 4:
		fill = (int)0xff006600u;
		break;
	case 5:
	case 8:
		fill = (int)0xff800000u;
		break;
	case 6:
		textFlags = 4;
		fill = (int)0xff800000u;
		break;
	case 7:
		y = 0x1e;
		textFlags = 4;
		fill = (int)0xff800000u;
		break;
	default:
		break;
	}
	if (canvas->dialogFlags & DLG_TALL) {
		y = canvas->sceneY + 0x28;
		h = canvas->viewH - y - 0x14;
	}
	y = (int16_t)y;
	h = (int16_t)h;

	if (canvas->dialogStyle == 2) {
		/* help: first line in a grey title bar */
		headerLines = 1;
		Gfx_setColor(canvas->app, fill);
		Gfx_fillRect(canvas->app, 0, y, w, h);
		Gfx_setColor(canvas->app, (int)0xff666666u);
		Gfx_fillRect(canvas->app, 0, y - 0xe, w, 0xe);
		Gfx_setColor(canvas->app, border);
		Gfx_drawRect(canvas->app, 0, y - 0xe, w - 1, 0xe);
		Gfx_drawRect(canvas->app, 0, y, w - 1, h - 1);
		Canvas_drawStringRange(canvas, canvas->dialogText, canvas->viewCenterX, y - 0xc, 0x10,
		                       canvas->dialogLines[0], canvas->dialogLines[1], textFlags);
	} else if (canvas->dialogStyle == 4) {
		/* item acquired: icon and first line in a bar above */
		Gfx_setColor(canvas->app, fill);
		Gfx_fillRect(canvas->app, 0, y, w, h);
		Gfx_setColor(canvas->app, border);
		Gfx_drawRect(canvas->app, 0, y, w - 1, h - 1);
		if (canvas->dialogItem != NULL) {
			headerLines = 1;
			Gfx_setColor(canvas->app, fill);
			Gfx_fillRect(canvas->app, 0, y - 0x1a, w, 0x1a);
			Gfx_setColor(canvas->app, border);
			Gfx_drawRect(canvas->app, 0, y - 0x1a, w - 1, 0x1a);
			Canvas_drawLargeIcon(canvas, canvas->dialogItem->icon, 2, y - 0xc, 0x20, 0, 0);
			Canvas_drawStringRange(canvas, canvas->dialogText, (w + 0x18) >> 1, y - 0xc, 0x30,
			                       canvas->dialogLines[0], canvas->dialogLines[1], textFlags);
		}
	} else if (canvas->dialogStyle != 3) {
		/* rounded box: fill, double edge, corner pieces tinted through blend 0xe */
		int bottom = y + h;

		Render_setBlendColors(canvas->app->render, 0, fill);
		Gfx_setColor(canvas->app, fill);
		Gfx_fillRect(canvas->app, 2, y + 1, w - 4, h - 2);
		Gfx_drawLine(canvas->app, 1, y + 6, 1, bottom - 6);
		Gfx_drawLine(canvas->app, w - 2, y + 6, w - 2, bottom - 6);
		Gfx_setColor(canvas->app, border);
		Gfx_drawLine(canvas->app, 6, y, w - 6, y);
		Gfx_drawLine(canvas->app, 6, bottom - 1, w - 6, bottom - 1);
		Gfx_drawLine(canvas->app, 0, y + 6, 0, bottom - 6);
		Gfx_drawLine(canvas->app, w - 1, y + 6, w - 1, bottom - 6);
		Canvas_drawRegion(canvas, &canvas->dialogCorners, 0, 0, 6, 6, 0, 0, y, 5, 0xe, NULL);
		Canvas_drawRegion(canvas, &canvas->dialogCorners, 0, 6, 6, 6, 0, 0, bottom, 6, 0xe, NULL);
		Canvas_drawRegion(canvas, &canvas->dialogCorners, 6, 0, 6, 6, 0, w, y, 9, 0xe, NULL);
		Canvas_drawRegion(canvas, &canvas->dialogCorners, 6, 6, 6, 6, 0, w, bottom, 10, 0xe, NULL);
		if (canvas->dialogStyle == 1) {
			/* the wand speaking: bubble tail below, wand icon peeking in at the top */
			Canvas_drawRegion(canvas, &canvas->hud->wandChat, 0, 0, 0xf, 9, 0, canvas->viewCenterX - 0x40,
			                  bottom - 1, 4, 0, NULL);
			if (canvas->dialogScroll < 2) {
				int scroll = canvas->dialogScroll;
				int jitter = 2 - (canvas->app->tickTime / 500) % 3;
				Canvas_drawRegion(canvas, &canvas->hud->iconWand, 0, scroll * 10, 0x14, 0x14 - scroll * 10, 0,
				                  jitter + 3, y + jitter + 1, 0, 0, NULL);
			}
		} else if (canvas->dialogStyle != 7) {
			Canvas_drawImage(canvas, &canvas->chatHook, canvas->viewCenterX - 0x40, y + 1, 6, 0xe);
		}
	}

	if (canvas->dialogScroll < headerLines)
		canvas->dialogScroll = headerLines;

	/* body: the current page, the line being typed at 25 ms per character */
	lineY = y + 2;
	for (i = 0; i < canvas->linesPerPage; i++) {
		int line = canvas->dialogScroll + i;
		int start, lineLen, shown;

		if (line >= canvas->dialogNumLines)
			break;
		start = canvas->dialogLines[line * 2];
		lineLen = canvas->dialogLines[line * 2 + 1];
		if (canvas->febc == i) {
			shown = (canvas->app->tickTime - canvas->dialogTime) / 25;
			if (shown >= lineLen) {
				canvas->febc = i + 1;
				canvas->dialogTime = canvas->app->tickTime;
				shown = lineLen;
			}
		} else {
			shown = (i < canvas->febc) ? lineLen : 0;
		}
		Canvas_drawStringRange(canvas, canvas->dialogText, textX, lineY, 0, start, shown, textFlags);
		lineY += 0xc;
	}

	if (canvas->dialogNumLines > canvas->linesPerPage) {
		int last = canvas->dialogScroll + canvas->linesPerPage;
		if (last > canvas->dialogNumLines)
			last = canvas->dialogNumLines;
		Canvas_drawScrollBar(canvas, w - 2, y + 2, h - 4, canvas->dialogScroll - headerLines, last - headerLines,
		                     canvas->dialogNumLines - headerLines);
	}
}

/* 0x00013774
 * Keys in a dialog: CLR / left / right / SOFT1 close it (unless DLG_NO_CLOSE);
 * select pages on (first finishing the typing); up/down scroll; 9 or SOFT2
 * closes it and lets the owner script run on without further dialogs. */
void Canvas_handleDialogEvents(Canvas *canvas, int avk)
{
	int action = Canvas_getKeyAction(canvas, avk);
	int lpp;

	if (!(canvas->dialogFlags & DLG_NO_CLOSE) &&
	    (action == ACT_BACK || action == ACT_LEFT || action == ACT_RIGHT || action == ACT_MENU)) {
		if (canvas->dialogHasOwner)
			Counter_dec(canvas->dialogOwner);
		Canvas_closeDialog(canvas);
		return;
	}

	if (action == ACT_SELECT || action == ACT_BACK) {
		lpp = canvas->linesPerPage;
		if (canvas->febc >= lpp || canvas->febc >= canvas->dialogNumLines - canvas->dialogScroll) {
			/* page fully typed: next page, or close */
			if (canvas->dialogScroll < canvas->dialogNumLines - lpp) {
				canvas->dialogTime = canvas->app->tickTime;
				canvas->febc = 0;
				canvas->dialogScroll += lpp;
				return;
			}
			Canvas_closeDialog(canvas);
			if (canvas->dialogHasOwner)
				ScriptThread_run(canvas->dialogOwner);
			return;
		}
		canvas->febc = lpp; /* finish typing the page */
		return;
	}

	if (action == ACT_UP) {
		canvas->dialogScroll--;
		if (canvas->dialogScroll < 0)
			canvas->dialogScroll = 0;
		return;
	}
	if (action == ACT_DOWN) {
		int maxScroll;

		canvas->dialogScroll++;
		maxScroll = canvas->dialogNumLines - canvas->linesPerPage;
		if (canvas->dialogScroll <= maxScroll) {
			canvas->dialogTime = canvas->app->tickTime;
			canvas->febc = canvas->linesPerPage - 1;
			return;
		}
		canvas->dialogScroll = maxScroll;
		if (maxScroll < 0)
			canvas->dialogScroll = 0;
		return;
	}

	if (action != ACT_SKIP && avk != AVK_SOFT2)
		return;
	Canvas_closeDialog(canvas);
	if (canvas->dialogHasOwner) {
		canvas->game->suppressDialog = 1;
		ScriptThread_run(canvas->dialogOwner);
		canvas->game->suppressDialog = 0;
	}
}
