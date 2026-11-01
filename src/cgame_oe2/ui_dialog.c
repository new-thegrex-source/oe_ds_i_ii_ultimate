/*
 * Dialog boxes (Canvas state 7) and the help-message queue.
 *
 * A dialog's text is word-wrapped once into Canvas.dialogText with '|' line
 * separators; Canvas.dialogLines holds {start, length} per line. Lines are
 * "typed" one at a time (25 ms per character). A dialog may belong to a
 * script thread (dialogOwner) that is resumed when the box is closed.
 *
 * Dialog styles (Canvas.dialogStyle):
 *   0 plain black box            5 dark red box
 *   1 wand speaking (purple)     6 red box, shaking text
 *   2 help / hint with title     7 red box at the top, shaking text
 *   3 parchment scroll           8 player speaking
 *   4 green notice (orange when  9 like 2 with the colours of 4
 *     playing as the familiar)
 */

#include "game.h"

/* Signed-byte bob offsets (same table the font uses for wobbling text). */
#define MOD_WOBBLE_TABLE 0x47168u
/* Fatal-error text "MAX_HELP_MESSAGES". */
#define MOD_STR_MAX_HELP_MESSAGES 0x46460u

/* Dialog flags (Canvas.dialogFlags). */
enum {
	DLG_FORCE = 0x01,     /* shown even during cinematics; cannot be skipped with back */
	DLG_TALL = 0x02,      /* 8 lines from near the top of the view */
	DLG_YESNO = 0x04,     /* "Yes No" choice after the last page */
	DLG_SILENT = 0x08,    /* no opening sound */
};

enum { MAX_HELP_MESSAGES = 16 };

/* Key code of the right soft key / clear (AVK_SOFT2 / AVK_CLR in the original). */
#define AVK_SOFT1 0xe036
#define AVK_SOFT2 0xe037

/*
 * 0x0002fed8
 * Wraps the text for the given style into Canvas.dialogText, builds the line
 * table and resets the typing/scroll state. Plays the opening sound.
 */
void Canvas_setupDialog(Canvas *canvas, String *text, int style, int flags)
{
	String *out = &canvas->dialogText;
	TextUtil *tu = g_app->textUtil;
	String tmp;
	int len, i, lineStart;

	String_ctor(&tmp);
	canvas->dialogViewLines = (style == 3) ? 6 : 4;
	String_setLength(out, 0);

	if (flags & DLG_YESNO) {
		/* append the yes/no prompt text; such dialogs can't be dismissed */
		String_setLength(&tmp, 0);
		Text_getString(canvas, 0, 0x44, &tmp);
		String_appendString(text, &tmp);
		flags |= DLG_FORCE;
	}

	if (style == 1 || style == 8) {
		/* Speech: first two lines are quoted (tmp = quote mark; note that for
		 * yes/no dialogs tmp still holds the prompt, Text_getString appends). */
		int width = canvas->textWidths[2];
		int pass;

		Text_getString(canvas, 0, 0x43, &tmp);
		for (pass = 0; pass < 2; pass++) {
			int pos;

			String_appendString(out, &tmp);
			pos = TextUtil_wrapTextEx(tu, text, 0, width - 4, out, 1, '|', NULL);
			String_appendString(out, &tmp);
			pos = TextUtil_wrapTextEx(tu, text, pos, width - 4, out, 1, '|', NULL);
			TextUtil_wrapTextFrom(tu, text, pos, width, out);
			if (pass == 0) {
				if (TextUtil_countLines(tu, out) <= 4)
					break;
				String_setLength(out, 0);
				width = canvas->textWidths[3];
			}
		}
		if (style == 8 && g_app->player->statusEffects[12].turns > 0) {
			String_setLength(&tmp, 0);
			Text_getString(canvas, 0, 0xd0, &tmp);
			String_appendString(out, &tmp);
		}
	} else if (style == 3) {
		TextUtil_wrapText(tu, text, canvas->textWidths[1], out);
		if (canvas->dialogViewLines < TextUtil_countLines(tu, out)) {
			String_setLength(out, 0);
			TextUtil_wrapText(tu, text, canvas->textWidths[0], out);
		}
	} else {
		int lines;

		TextUtil_wrapText(tu, text, canvas->textWidths[2], out);
		lines = TextUtil_countLines(tu, out);
		if (style == 2 || style == 9)
			lines--; /* the first line is the title */
		if (canvas->dialogViewLines < lines) {
			String_setLength(out, 0);
			TextUtil_wrapText(tu, text, canvas->textWidths[3], out);
		}
	}

	len = out->len;
	canvas->dialogNumLines = 0;
	lineStart = 0;
	for (i = 0; i < len; i++) {
		if ((uint8_t)out->buf[i] == '|') {
			canvas->dialogLines[canvas->dialogNumLines * 2] = (int16_t)lineStart;
			canvas->dialogLines[canvas->dialogNumLines * 2 + 1] = (int16_t)(i - lineStart);
			lineStart = i + 1;
			canvas->dialogNumLines++;
		}
	}
	canvas->dialogLines[canvas->dialogNumLines * 2] = (int16_t)lineStart;
	canvas->dialogLines[canvas->dialogNumLines * 2 + 1] = (int16_t)(len - lineStart);
	canvas->dialogNumLines++;

	canvas->dialogTopLine = 0;
	canvas->dialogLineTime = g_app->time;
	canvas->dialogTypeLine = 0;
	canvas->dialogStartTime = g_app->time;
	canvas->dialogItem = NULL;
	canvas->dialogStyle = style;
	canvas->dialogFlags = flags;
	if (flags & DLG_TALL)
		canvas->dialogViewLines = 8;
	else
		canvas->dialogViewLines = (style == 3) ? 6 : 4;

	if ((flags & DLG_SILENT) == 0) {
		if (style == 1)
			Sound_play(g_app->sndSys, 2, 0, 2, -1);
		else if (style == 2)
			Sound_play(g_app->sndSys, 0, 0, 2, -1);
		else if (style != 4 && style != 9)
			Sound_play(g_app->sndSys, 1, 0, 2, -1);
	}
	String_dtor(&tmp, 0);
}

/* 0x0003d66c */
void Canvas_showDialog(Canvas *cv, ScriptThread *owner, String *str, int style, int flags, int extra)
{
	if (Game_isCinematicActive(g_app->game)) {
		if ((flags & DLG_FORCE) == 0)
			return;
		flags |= DLG_FORCE;
	}
	cv->dialogOwner = owner;
	cv->dialogReturn = 0;
	cv->dialogScript = extra;
	Canvas_setupDialog(cv, str, style, flags);
	Canvas_setState(cv, 7);
}

/* 0x0003d640 */
void Canvas_showDialogStr(Canvas *cv, ScriptThread *owner, String *str, int style, int flags)
{
	Canvas_showDialog(cv, owner, str, style, flags, 0);
}

/* 0x0003d6f8 */
void Canvas_showDialogText(Canvas *cv, ScriptThread *owner, int group, int strId, int style, int flags, int extra)
{
	String_setLength(&cv->scratch, 0);
	Text_getString(cv, group, strId, &cv->scratch);
	Canvas_showDialog(cv, owner, &cv->scratch, style, flags, extra);
}

/* 0x0003d750 */
void Canvas_showDialogText0(Canvas *cv, ScriptThread *owner, int strId, int style, int flags)
{
	Canvas_showDialogText(cv, owner, 0, strId, style, flags, 0);
}

/* 0x00009e24 */
void Canvas_closeDialog(Canvas *canvas)
{
	canvas->dialogPortrait = -1;
	/* The original reported (player, time the dialog was open) to an empty
	 * Player method here. */
	Player_stub();
	String_setLength(&canvas->dialogText, 0);

	if (Game_isCinematicActive(g_app->game))
		Canvas_setState(canvas, 0x11);
	else
		Canvas_setState(canvas, canvas->oldState == 4 ? 4 : 3);

	if (canvas->dialogStyle == 4 || canvas->dialogStyle == 9)
		Sound_play(g_app->sndSys, 0xd, 0, 2, -1);
	if (canvas->dialogReturn != 0)
		Canvas_setState(canvas, 2);
	if (canvas->dialogStyle == 3 || canvas->dialogStyle == 4) {
		Game_snapAllMonsterSprites(g_app->game, 1);
		Game_advanceTurn(g_app->game);
	}
}

/* Wobble offset used by the yes/no cursor. */
static int wobble(int index)
{
	return ModData_s8(&g_mod, MOD_WOBBLE_TABLE + index);
}

/* 0x0000ac7c */
void Canvas_drawDialog(Canvas *canvas)
{
	Hud *hud = g_app->hud;
	Image *corners = &canvas->imgNpcCorners;
	int16_t boxX = 0, boxY, boxW, boxH;
	int color = (int)0xff000000;
	int textFlags = 0;
	int textX = 1;
	int headerLines = 0;
	int i, y;

	if (canvas->dialogText.len == 0)
		return;

	boxW = canvas->viewRect[2];
	boxH = 0x36;
	boxY = (int16_t)(canvas->viewRect[3] - 0x36);

	switch (canvas->dialogStyle) {
	case 1:
		boxY = (int16_t)(canvas->viewRect[3] - 0x5e);
		textFlags = 8;
		color = (int)0xff6633cc;
		corners = &canvas->imgWandCorners;
		break;
	case 3:
		textX = 0xf;
		boxY = (int16_t)(canvas->renderRect[1] + (canvas->renderRect[3] >> 1));
		boxH = 0x4e;
		Canvas_drawDialogFrame(canvas, boxX, boxY - 0xc, boxW, 100, 0);
		break;
	case 4:
	case 9:
		color = g_app->player->isFamiliar ? (int)0xffcc6600 : (int)0xff006600;
		break;
	case 5:
		color = (int)0xff800000;
		break;
	case 7:
		boxY = 0x1e;
		/* fall through */
	case 6:
		textFlags = 4;
		color = (int)0xff800000;
		break;
	case 8:
		boxY = (int16_t)(canvas->viewRect[3] - 0x5e);
		color = 0x2864;
		corners = &canvas->imgPlayerCorners;
		break;
	default:
		break;
	}

	if (canvas->dialogFlags & DLG_TALL) {
		boxY = (int16_t)(canvas->renderRect[1] + 0x28);
		boxH = (int16_t)(canvas->viewRect[3] - boxY - 0x14);
	}

	if (canvas->dialogStyle == 2 || canvas->dialogStyle == 9) {
		/* box with a grey title bar holding the first line */
		headerLines = 1;
		Graphics_setColor(canvas->graphics, color);
		Graphics_fillRect(canvas->graphics, boxX, boxY, boxW, boxH);
		Graphics_setColor(canvas->graphics, (int)0xff666666);
		Graphics_fillRect(canvas->graphics, boxX, boxY - 14, boxW, 14);
		Graphics_setColor(canvas->graphics, -1);
		Graphics_drawRect(canvas->graphics, boxX, boxY - 14, boxW - 1, 14);
		Graphics_drawRect(canvas->graphics, boxX, boxY, boxW - 1, boxH - 1);
		if (canvas->dialogPortrait != -1) {
			Canvas_drawRegion(canvas, &hud->imgBottombarIcon, 0, canvas->dialogPortrait * 20, 14, 20, 0,
					  boxX + 2, boxY - 16, 0, 0, 0);
			Canvas_drawRegion(canvas, &hud->imgBottombarIcon, 0, canvas->dialogPortrait * 20, 14, 20, 0,
					  boxW - 16, boxY - 16, 0, 0, 0);
		}
		Canvas_drawStringRange(canvas, &canvas->dialogText, canvas->screenCenterX, boxY - 12, 0x10,
				       canvas->dialogLines[0], canvas->dialogLines[1], textFlags);
	} else if (canvas->dialogStyle == 4) {
		Graphics_setColor(canvas->graphics, color);
		Graphics_fillRect(canvas->graphics, boxX, boxY, boxW, boxH);
		Graphics_setColor(canvas->graphics, -1);
		Graphics_drawRect(canvas->graphics, boxX, boxY, boxW - 1, boxH - 1);
		if (canvas->dialogItem != NULL) {
			/* item notice: icon + name bar above the box (dialogItem is never
			 * set by the shipped code; read as an EntityDef). */
			EntityDef *item = (EntityDef *)canvas->dialogItem;

			headerLines = 1;
			Graphics_setColor(canvas->graphics, color);
			Graphics_fillRect(canvas->graphics, boxX, boxY - 0x1a, boxW, 0x1a);
			Graphics_setColor(canvas->graphics, -1);
			Graphics_drawRect(canvas->graphics, boxX, boxY - 0x1a, boxW - 1, 0x1a);
			Canvas_drawLargeIcon(canvas, item->subId, boxX + 2, boxY - 12, 0x20, 0, 0);
			Canvas_drawStringRange(canvas, &canvas->dialogText, boxX + ((boxW + 0x18) >> 1), boxY - 12, 0x30,
					       canvas->dialogLines[0], canvas->dialogLines[1], textFlags);
		}
	} else if (canvas->dialogStyle != 3) {
		/* rounded box with corner pieces and a speech-bubble tail */
		int bottom = boxY + boxH;

		Render_setBlendColors(g_app->render, (int)0xff000000, color);
		Graphics_setColor(canvas->graphics, color);
		Graphics_fillRect(canvas->graphics, boxX + 2, boxY + 1, boxW - 4, boxH - 2);
		Graphics_drawLine(canvas->graphics, boxX + 1, boxY + 6, boxX + 1, bottom - 6);
		Graphics_drawLine(canvas->graphics, boxW - 2, boxY + 6, boxW - 2, bottom - 6);
		Graphics_setColor(canvas->graphics, -1);
		Graphics_drawLine(canvas->graphics, boxX + 6, boxY, boxW - 6, boxY);
		Graphics_drawLine(canvas->graphics, boxX + 6, bottom - 1, boxW - 6, bottom - 1);
		Graphics_drawLine(canvas->graphics, boxX, boxY + 6, boxX, bottom - 6);
		Graphics_drawLine(canvas->graphics, boxW - 1, boxY + 6, boxW - 1, bottom - 6);
		Canvas_drawRegion(canvas, corners, 0, 0, 6, 6, 0, boxX, boxY, 5, 10, 0);
		Canvas_drawRegion(canvas, corners, 0, 6, 6, 6, 0, boxX, bottom, 6, 10, 0);
		Canvas_drawRegion(canvas, corners, 6, 0, 6, 6, 0, boxW, boxY, 9, 10, 0);
		Canvas_drawRegion(canvas, corners, 6, 6, 6, 6, 0, boxW, bottom, 10, 10, 0);

		if (canvas->dialogStyle == 1) {
			Canvas_drawRegion(canvas, &hud->imgWandChat, 0, 0, 0xf, 9, 0,
					  boxX + canvas->screenCenterX - 0x40, bottom - 1, 4, 0, 0);
			if (canvas->dialogTopLine < 2) {
				/* the wand's face bobs on the first page */
				int bob = 2 - ((int)g_app->time / 500) % 3;
				int top = canvas->dialogTopLine;

				Canvas_drawRegion(canvas, &hud->imgWandSmall, 0, top * 10, 20, 20 - top * 10, 0,
						  boxX + bob + 3, boxY + bob + 1, 0, 0, 0);
			}
		} else if (canvas->dialogStyle == 8) {
			Canvas_drawImage(canvas, &canvas->imgPlayerChat, boxX + canvas->screenCenterX, bottom - 1, 4, 0);
			if (canvas->dialogTopLine < 2) {
				int top = canvas->dialogTopLine;

				Canvas_drawRegion(canvas, &hud->imgPlayerSmall, 0, top * 10, 20, 20 - top * 10, 0,
						  boxX + 5, boxY + 3, 0, 0, 0);
			}
		} else if (canvas->dialogStyle != 7) {
			Canvas_drawRegion(canvas, &canvas->imgNpcChat, 0, 0, 0xf, 9, 0,
					  boxX + canvas->screenCenterX - 0x40, boxY + 1, 6, 10, 0);
		}
	}

	/* body lines, typed out one line at a time */
	if (canvas->dialogTopLine < headerLines)
		canvas->dialogTopLine = headerLines;
	y = boxY + 2;
	for (i = 0; i < canvas->dialogViewLines; i++) {
		int line = canvas->dialogTopLine + i;
		int start, len, shown;

		if (line >= canvas->dialogNumLines)
			break;
		start = canvas->dialogLines[line * 2];
		len = canvas->dialogLines[line * 2 + 1];
		if (canvas->dialogTypeLine == i) {
			shown = ((int)g_app->time - canvas->dialogLineTime) / 25;
			if (shown >= len) {
				canvas->dialogTypeLine = i + 1;
				canvas->dialogLineTime = g_app->time;
				shown = len;
			}
		} else {
			shown = (canvas->dialogTypeLine > i) ? len : 0;
		}
		Canvas_drawStringRange(canvas, &canvas->dialogText, boxX + textX, y, 0, start, shown, textFlags);
		y += 12;
	}

	if ((canvas->dialogFlags & DLG_YESNO) &&
	    canvas->dialogNumLines - canvas->dialogViewLines <= canvas->dialogTopLine) {
		int bob = wobble(((int)g_app->time / 200) % 4);
		int choiceY = boxY + canvas->dialogViewLines * 12 - 10;
		int choiceX = (boxW >> 1) - 0x36;
		String yesNo;

		String_ctor(&yesNo);
		Text_getString(canvas, 0, 0x21, &yesNo);
		TextUtil_dehyphenate(g_app->textUtil, &yesNo);
		Canvas_drawStringRange(canvas, &yesNo, choiceX + textX, choiceY, 0, 0, 0x15, textFlags);
		Graphics_drawRect(canvas->graphics, choiceX - 1, choiceY - 2, 0x21, 0xd);
		Graphics_drawRect(canvas->graphics, choiceX + 0x3f, choiceY - 2, 0x25, 0xd);
		Canvas_drawImageSimple(canvas, &g_app->menuSystem->imgCursor,
				       choiceX - 0x10 + bob + (canvas->dialogChoice != 0 ? 0x3f : 0), choiceY, 0);
		String_dtor(&yesNo, 0);
	}

	if (canvas->dialogNumLines > canvas->dialogViewLines) {
		int bottomLine = canvas->dialogTopLine + canvas->dialogViewLines;

		if (bottomLine > canvas->dialogNumLines)
			bottomLine = canvas->dialogNumLines;
		Canvas_drawScrollBar(canvas, boxX + boxW - 2, boxY + 2, boxH - 4, canvas->dialogTopLine - headerLines,
				     bottomLine - headerLines, canvas->dialogNumLines - headerLines,
				     canvas->dialogViewLines);
	}
}

/*
 * 0x0001d2e0
 * Keys while a dialog is open. Returns the Canvas state (the original's return
 * value was whatever was left in r0; callers do not use it).
 */
int Canvas_handleDialogEvents(Canvas *canvas, int avk)
{
	int action = Canvas_getKeyAction(canvas, avk);

	if ((canvas->dialogFlags & DLG_FORCE) == 0 && (avk == AVK_SOFT1 || action == 0xf || action == 5)) {
		/* dismissed: the owning script does not get resumed */
		if (canvas->dialogScript != 0)
			Counter_dec(canvas->dialogOwner);
		Canvas_closeDialog(canvas);
		Canvas_restorePrevWeapon(canvas);
	} else if (action == 6 || action == 0xf) {
		int view = canvas->dialogViewLines;

		if (canvas->dialogTypeLine < view && canvas->dialogNumLines - canvas->dialogTopLine > canvas->dialogTypeLine) {
			/* finish typing the page */
			canvas->dialogTypeLine = view;
		} else if (canvas->dialogNumLines - view > canvas->dialogTopLine) {
			canvas->dialogTopLine += view;
			canvas->dialogLineTime = g_app->time;
			canvas->dialogTypeLine = 0;
		} else {
			Canvas_closeDialog(canvas);
			if (canvas->dialogScript != 0)
				ScriptThread_run(canvas->dialogOwner);
			Canvas_restorePrevWeapon(canvas);
			if ((canvas->dialogFlags & DLG_YESNO) && canvas->dialogChoice != 0)
				Canvas_attemptSteal(canvas);
		}
	} else if (action == 1) {
		if (--canvas->dialogTopLine < 0)
			canvas->dialogTopLine = 0;
	} else if (action == 2) {
		int last = canvas->dialogNumLines - canvas->dialogViewLines;

		canvas->dialogTopLine++;
		if (last >= canvas->dialogTopLine) {
			canvas->dialogLineTime = g_app->time;
			canvas->dialogTypeLine = canvas->dialogViewLines - 1;
		} else {
			canvas->dialogTopLine = last;
			if (last < 0)
				canvas->dialogTopLine = 0;
		}
	} else if ((action == 3 || action == 4) && (canvas->dialogFlags & DLG_YESNO)) {
		if (canvas->dialogNumLines - canvas->dialogViewLines <= canvas->dialogTopLine) {
			if (action == 3) {
				if (canvas->dialogChoice != 0)
					canvas->dialogChoice = 0;
			} else if (canvas->dialogChoice == 0) {
				canvas->dialogChoice = 1;
			}
		}
	} else if (action == 0xe || avk == AVK_SOFT2) {
		/* skip: resume the script with the "skipping" flag raised */
		Canvas_closeDialog(canvas);
		if (canvas->dialogScript != 0) {
			g_app->game->f9be8 = 1;
			ScriptThread_run(canvas->dialogOwner);
			g_app->game->f9be8 = 0;
		}
		Canvas_restorePrevWeapon(canvas);
	}

	if (canvas->state == 3) {
		Canvas_dequeueHelpDialog(canvas, 0);
		return 3;
	}
	return canvas->state;
}

/* Common tail of the Canvas_addHelp* functions: queue overflow is fatal, and
 * while playing the head of the queue is shown at once. Returns the slot to
 * fill, or NULL when help is disabled. */
static HelpMsg *helpSlot(Canvas *canvas)
{
	if (g_app->player->enableHelp == 0)
		return NULL;
	if (canvas->numHelpMsgs == MAX_HELP_MESSAGES) {
		App_fatalError(g_app, (char *)ModData_str(&g_mod, MOD_STR_MAX_HELP_MESSAGES));
		return NULL;
	}
	return &canvas->helpQueue[canvas->numHelpMsgs++];
}

/* 0x000140dc  Queue an item/monster description. */
int Canvas_addHelpEntity(Canvas *canvas, EntityDef *entityDef)
{
	HelpMsg *msg = helpSlot(canvas);

	if (msg == NULL)
		return 0;
	msg->type = 1;
	msg->value = entityDef;
	msg->owner = NULL;
	if (canvas->state == 3)
		Canvas_dequeueHelpDialog(canvas, 0);
	return 0;
}

/* 0x0001417c  Queue a ready-made string (type 0, or 2 = shown with flags 9). */
int Canvas_addHelpMessage(Canvas *canvas, String *str, int type, ScriptThread *owner)
{
	HelpMsg *msg = helpSlot(canvas);

	if (msg == NULL)
		return 0;
	msg->type = type;
	msg->value = str;
	msg->owner = owner;
	if (canvas->state == 3)
		Canvas_dequeueHelpDialog(canvas, 0);
	return 0;
}

/* 0x00014220  Queue a string-table entry. */
int Canvas_addHelpText(Canvas *canvas, int group, int id, ScriptThread *owner)
{
	HelpMsg *msg = helpSlot(canvas);

	if (msg == NULL)
		return 0;
	msg->type = 3;
	msg->value = (void *)(intptr_t)(id | group << 16);
	msg->owner = owner;
	if (canvas->state == 3)
		Canvas_dequeueHelpDialog(canvas, 0);
	return 0;
}

/* 0x00014210 */
int Canvas_addHelpTextId(Canvas *canvas, int id)
{
	return Canvas_addHelpText(canvas, 0, id, NULL);
}

/*
 * 0x0000aa54
 * Shows the head of the help queue as a style-2 dialog. Without `force` it
 * only does so while playing (states 3 and 0x13) or during the monsters' turn.
 */
void Canvas_dequeueHelpDialog(Canvas *canvas, int force)
{
	HelpMsg *head = &canvas->helpQueue[0];
	String *text = &canvas->scratch;
	ScriptThread *owner;
	int flags = 1;
	int i;

	if (canvas->numHelpMsgs == 0)
		return;
	if (!force && canvas->state != 3 && canvas->state != 0x13 && g_app->game->monstersTurn == 0)
		return;

	String_setLength(&canvas->scratch, 0);
	if (head->type == 0 || head->type == 2) {
		text = (String *)head->value;
	} else if (head->type == 3) {
		int packed = (int)(intptr_t)head->value;

		Text_getString(canvas, packed >> 16, (int16_t)packed, &canvas->scratch);
	} else if (head->type == 1) {
		/* "NAME|description" plus a category hint for some entity types */
		EntityDef *def = (EntityDef *)head->value;
		String upper;
		int hintId = -1;

		String_ctor(&upper);
		String_toUpper(&upper, &def->longName);
		String_appendString(&canvas->scratch, &upper);
		String_dtor(&upper, 0);
		switch (def->eSubType) {
		case 0:
			hintId = (def->parm >= 0 && def->parm < 10) ? 0x22 : 0x23;
			break;
		case 1:
			/* ARM register shifts of 32 or more give 0 */
			if (arm_bit(def->parm) & 7u) {
			} else {
				hintId = 0x24;
			}
			break;
		case 2:
			hintId = 0x25;
			break;
		case 7:
			break;
		default:
			return; /* nothing to say; the entry stays queued */
		}
		String_appendString(String_appendCStr_76f0(&canvas->scratch, "|"), &def->description);
		if (hintId != -1) {
			String_appendCStr_76f0(&canvas->scratch, " ");
			Text_getString(canvas, 0, hintId, &canvas->scratch);
		}
	}
	if (head->type == 2)
		flags = 9;

	/* pop the head (slot 15 ends up holding a copy of the new head) */
	owner = head->owner;
	for (i = 0; i < MAX_HELP_MESSAGES - 1; i++)
		canvas->helpQueue[i] = canvas->helpQueue[i + 1];
	canvas->helpQueue[MAX_HELP_MESSAGES - 1] = canvas->helpQueue[0];
	canvas->numHelpMsgs--;

	if (g_app->player->enableHelp != 0)
		Canvas_showDialog(canvas, owner, text, 2, flags, owner != NULL);
}

/* 0x0000ac74 */
void Canvas_dequeueHelpDialog0(Canvas *canvas)
{
	Canvas_dequeueHelpDialog(canvas, 0);
}
