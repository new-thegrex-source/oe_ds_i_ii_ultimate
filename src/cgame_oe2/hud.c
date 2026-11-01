/*
 * Hud (Applet+0x134): message queue for the top bar, timed subtitle lines
 * shown in the speech bubble, companion barks, startup and shutdown.
 * The drawing code lives in hud_draw.c.
 */

#include "game.h"

/* TextUtil_wrapText width meaning "never wrap, only translate '|'" */
#define WRAP_NO_LIMIT 9999

/* 0x000093a0 */
void Hud_resetMsgTimer(Hud *hud)
{
	int len = hud->messages[0].len;

	hud->msgTime = g_app->time;
	/* Long (scrolling) messages stay up 100 ms per character. */
	hud->msgDuration = (len > hud->maxChars) ? len * 100 : 1000;
}

/* 0x0003b92c */
void Hud_shiftMsgs(Hud *hud)
{
	int i;

	for (i = 0; i < hud->numMessages - 1; i++) {
		String_setLength(&hud->messages[i], 0);
		String_appendString(&hud->messages[i], &hud->messages[i + 1]);
		hud->msgFlags[i] = hud->msgFlags[i + 1];
	}
	hud->numMessages--;
	String_setLength(&hud->messages[hud->numMessages], 0);
	hud->msgFlags[hud->numMessages] = 0;
	if (hud->numMessages > 0)
		Hud_resetMsgTimer(hud);
}

/* 0x000050a8 */
void Hud_addMessageStrFlags(Hud *hud, String *str, int flags)
{
	int n;

	/* The original compares against an empty String, which only matches a
	 * String without a buffer. */
	if (str->buf == NULL)
		return;

	if (flags & 1) {
		/* priority message: replaces the whole queue */
		hud->numMessages = 0;
	} else {
		if (hud->numMessages > 0 && String_equals_144a0(str, &hud->messages[hud->numMessages - 1]))
			return; /* same as the last queued message */
		if (hud->numMessages == 3)
			Hud_shiftMsgs(hud);
	}

	n = hud->numMessages;
	String_setLength(&hud->messages[n], 0);
	TextUtil_wrapText(g_app->textUtil, str, WRAP_NO_LIMIT, &hud->messages[n]);
	hud->msgFlags[n] = flags;
	hud->numMessages = n + 1;
	if (hud->numMessages == 1) {
		Hud_resetMsgTimer(hud);
		if (flags & 1)
			hud->msgDuration <<= 1;
	}
}

/* 0x000050a0 */
void Hud_addMessageStr(Hud *hud, String *str)
{
	Hud_addMessageStrFlags(hud, str, 0);
}

/* 0x000051d4 */
void Hud_addMessageEx(Hud *hud, int file, int strId, int flags)
{
	Canvas *cv = g_app->canvas;

	String_setLength(&cv->scratch, 0);
	Text_getString(cv, file, strId, &cv->scratch);
	Hud_addMessageStrFlags(hud, &cv->scratch, flags);
}

/* 0x000051bc */
void Hud_addMessage(Hud *hud, int strId)
{
	Hud_addMessageEx(hud, 0, strId, 0);
}

/* 0x000051cc */
void Hud_addMessageFile(Hud *hud, int file, int strId)
{
	Hud_addMessageEx(hud, file, strId, 0);
}

/* 0x0000523c */
void Hud_addMessageFlags(Hud *hud, int strId, int flags)
{
	Hud_addMessageEx(hud, 0, strId, flags);
}

/* 0x0001beac
 * Hands out the next message slot so the caller can build the text in place;
 * Hud_finishMessageBuffer commits it. */
String *Hud_getMessageBuffer(Hud *hud, int flags)
{
	if (flags & 1)
		hud->numMessages = 0;
	if (hud->numMessages == 3)
		Hud_shiftMsgs(hud);
	String_setLength(&hud->messages[hud->numMessages], 0);
	hud->msgFlags[hud->numMessages] = flags;
	return &hud->messages[hud->numMessages];
}

/* 0x0001bf08 */
String *Hud_getMessageBufferDefault(Hud *hud)
{
	return Hud_getMessageBuffer(hud, 0);
}

/* 0x0001b10c */
void Hud_finishMessageBuffer(Hud *hud)
{
	String *slot = &hud->messages[hud->numMessages];

	/* Re-wrap the text built in the slot (translates '|' line marks). */
	String_setLength(&hud->msgBuffer, 0);
	String_appendString(&hud->msgBuffer, slot);
	String_setLength(slot, 0);
	TextUtil_wrapText(g_app->textUtil, &hud->msgBuffer, WRAP_NO_LIMIT, slot);
	hud->numMessages++;
	if (hud->numMessages == 1)
		Hud_resetMsgTimer(hud);
}

/* 0x00009574 */
void Hud_calcMaxLineLen(Hud *hud)
{
	int i;

	hud->maxLineLen = 0;
	for (i = 0; i < hud->numLines; i++) {
		if (hud->lines[i].len > hud->maxLineLen)
			hud->maxLineLen = hud->lines[i].len;
	}
}

/* 0x00009b0c */
void Hud_clearLines(Hud *hud)
{
	int i;

	/* The original stops one short of numLines; the last line keeps its text
	 * but is no longer counted. */
	for (i = 0; i < hud->numLines - 1; i++) {
		String_setLength(&hud->lines[i], 0);
		hud->lineTimes[i] = 0;
	}
	hud->numLines = 0;
	hud->bubbleMode = 0;
}

/* 0x0003ba3c */
void Hud_shiftSubtitles(Hud *hud)
{
	int i;

	Player_showHelp(g_app->player, 0xf, 0);
	for (i = 0; i < hud->numLines - 1; i++) {
		String_setLength(&hud->lines[i], 0);
		String_appendString(&hud->lines[i], &hud->lines[i + 1]);
		hud->lineTimes[i] = hud->lineTimes[i + 1];
	}
	hud->numLines--;
}

/* 0x00005774
 * Queues a speech-bubble line (up to 3, oldest dropped). An empty (but
 * allocated) string clears the bubble. */
void Hud_addTimedMsgStr(Hud *hud, String *str)
{
	int n;

	if (str->buf == NULL) /* original: equals an empty String */
		return;
	if (str->len == 0) {
		Hud_clearLines(hud);
		return;
	}
	if (hud->numLines == 3)
		Hud_shiftSubtitles(hud);

	n = hud->numLines;
	String_setLength(&hud->lines[n], 0);
	TextUtil_wrapText(g_app->textUtil, str, WRAP_NO_LIMIT, &hud->lines[n]);
	hud->lineTimes[n] = g_app->time;
	hud->numLines = n + 1;
	Hud_calcMaxLineLen(hud);
}

/* 0x0000586c */
void Hud_addTimedMsgFile(Hud *hud, int file, int strId)
{
	Canvas *cv = g_app->canvas;

	String_setLength(&cv->scratch, 0);
	Text_getString(cv, file, strId, &cv->scratch);
	Hud_addTimedMsgStr(hud, &cv->scratch);
}

/* 0x00005860 */
void Hud_addTimedMsg(Hud *hud, int strId)
{
	Hud_addTimedMsgFile(hud, 0, strId);
}

/* 0x0001efc0 */
void Hud_startStatFlash(void)
{
	Hud *hud = g_app->hud;

	hud->damageDirUntil = g_app->time + 500;
	hud->damageFlash = -1;
}

/* 0x00030500
 * Picks a fresh starting bark in every group and restarts the bark cooldown. */
void Hud_shuffleBarks(Hud *hud)
{
	int k;

	for (k = 0; k < 12; k++) {
		if (hud->barks[k].count != 0)
			hud->barks[k].cur = (int)((uint32_t)Random_nextInt(&g_app->random) & 0xff) % hud->barks[k].count;
	}
	hud->lastBarkTime = g_app->playTime;
}

/* 0x000431d4
 * Random flavour line ("bark") of the given kind, spoken by the wand. Barks are
 * rate limited to one per 5 s unless forced, and fire with the group's percent
 * chance. */
void Hud_randomBarkEx(Hud *hud, int kind, int force)
{
	Player *player = g_app->player;
	BarkGroup *group = &hud->barks[kind];
	int needsCompanion = 1;
	int pick;

	/* Kinds 8 and 10 (and 11 once the player reached map 2) are not gated
	 * on the companion flag. */
	if (kind == 8 || kind == 10 || (kind == 0xb && player->highestMap == 2))
		needsCompanion = 0;

	if (player->isFamiliar != 0)
		return;
	if (needsCompanion && (player->f260 == 0 || kind == 0xb))
		return;
	if (group->count == 0 || g_app->canvas->state == 0x11 || (player->weapons & 1) == 0)
		return; /* no lines, cinematic running, or no wand */

	if (!force && g_app->playTime - hud->lastBarkTime < 5000)
		return;

	if (Random_nextInt(&g_app->random) % 100 >= (group->chance & 0xff))
		return;

	hud->lastBarkTime = g_app->playTime;
	pick = (int)((uint32_t)Random_nextInt(&g_app->random) & 0xff) % group->count;
	Hud_addTimedMsgStr(hud, &group->strings[pick]);
	if (kind == 0xb)
		hud->barks[0xb].chance = 0; /* one-shot */
}

/* 0x000431cc */
void Hud_randomBark(Hud *hud, int kind)
{
	Hud_randomBarkEx(hud, kind, 0);
}

/* 0x0003df30 */
int Hud_startup(Hud *hud, int largeScreen)
{
	Applet *app = g_app;
	int16_t *L = hud->layout;

	hud->numMessages = 0;
	hud->numLines = 0;
	hud->maxChars = (app->canvas->viewRect[2] - 4) / 7;

	Image_load(app, 0x10, &hud->imgAttackArrows, 1);
	Image_load(app, 0x0f, &hud->imgActions, 1);
	Image_load(app, 0x12, &hud->imgBottomCenter, 1);
	Image_load(app, 0x13, &hud->imgBottomLeft, 1);
	Image_load(app, 0x14, &hud->imgBottomRight, 1);
	Image_load(app, 0x16, &hud->imgTopCenter, 1);
	Image_load(app, 0x17, &hud->imgTopLeft, 1);
	Image_load(app, 0x18, &hud->imgTopRight, 1);
	Image_load(app, 0x19, &hud->imgTopRingBox, 1);
	Image_load(app, 0x1b, &hud->imgWandChat, 1);
	Image_load(app, 0x1a, &hud->imgWand, 1);
	Image_load(app, 0x1c, &hud->imgWandSmall, 1);
	Image_load(app, 0x07, &hud->imgBottombarIcon, 1);
	Image_load(app, 0x2c, &hud->imgChatAnchors, 1);
	Image_load(app, 0x15, &hud->imgPlayerSmall, 1);

	/* Bottom-bar element boxes {x,y,w,h}, x relative to the screen centre and
	 * y relative to the bottom of the view:
	 * 0 HP text, 1 HP gauge, 2 armor text, 3 armor icon, 4 ammo text,
	 * 5 ammo icon, 6 wand, 7 bubble (x computed per frame), 8 no-wand bubble,
	 * 9 NPC bubble. */
	if (!largeScreen) {
		hud->largeScreen = 0;
		L[0] = 0x25;   L[1] = -0x12;  L[2] = 0x18;   L[3] = 0x10;
		L[8] = 2;      L[9] = -0xf;   L[10] = 0x1a;  L[11] = 0xe;
		L[16] = -0x1d; L[17] = -0xf;  L[18] = 0x1a;  L[19] = 0xe;
		L[24] = -0x3e; L[25] = -0x1c; L[26] = 0x18;  L[27] = 0x18;
	} else {
		hud->largeScreen = 1;
		L[4] = 0x37;   L[5] = -0x22;  L[6] = 0x22;   L[7] = 0x1d;
		L[0] = 0x38;   L[1] = -0x20;  L[2] = 0x1e;   L[3] = 0x18;
		L[8] = 0xe;    L[9] = -0x15;  L[10] = 0x20;  L[11] = 0x13;
		L[12] = -4;    L[13] = -0x16; L[14] = 0x20;  L[15] = 0x13;
		L[16] = -0x21; L[17] = -0x15; L[18] = 0x20;  L[19] = 0x13;
		L[20] = -0x33; L[21] = -0x16; L[22] = 0x20;  L[23] = 0x13;
		L[24] = -0x54; L[25] = -0x21; L[26] = 0x1c;  L[27] = 0x1c;
		L[32] = 0;     L[33] = -0x46; L[34] = 0x1c;  L[35] = 0x1c;
		L[29] = -0x69; L[30] = 0;     L[31] = 0;
	}
	L[36] = 0; L[37] = -0x44; L[38] = 0; L[39] = 0;

	/* 0x2a838 fills hud->barks from string group 19 although it is filed
	 * under Combat; the original passes the Hud. */
	return Combat_loadQuips((void *)hud) != 0;
}

/* 0x0003c1b0 */
void Hud_shutdown(Hud *hud)
{
	Applet *app = g_app;
	int i, j;

	App_freeImage(app, &hud->imgAttackArrows);
	App_freeImage(app, &hud->imgActions);
	App_freeImage(app, &hud->imgBottomCenter);
	App_freeImage(app, &hud->imgBottomLeft);
	App_freeImage(app, &hud->imgBottomRight);
	App_freeImage(app, &hud->imgTopCenter);
	App_freeImage(app, &hud->imgTopLeft);
	App_freeImage(app, &hud->imgTopRight);
	App_freeImage(app, &hud->imgTopRingBox);
	App_freeImage(app, &hud->imgWandChat);
	App_freeImage(app, &hud->imgWand);
	App_freeImage(app, &hud->imgWandSmall);
	App_freeImage(app, &hud->imgBottombarIcon);
	App_freeImage(app, &hud->imgChatAnchors);
	App_freeImage(app, &hud->imgPlayerSmall);

	for (i = 0; i < 3; i++)
		String_release(&hud->messages[i]);
	for (i = 0; i < 3; i++)
		String_release(&hud->lines[i]);
	for (i = 0; i < 12; i++)
		for (j = 0; j < 20; j++)
			String_release(&hud->barks[i].strings[j]);

	String_release(&hud->caption1);
	String_release(&hud->caption2);
	String_release(&hud->msgBuffer);
	String_release(&hud->str1218);
	String_release(&hud->str1228);
	String_release(&hud->forcedMsg);
	String_release(&hud->str1248);
	String_release(&hud->str1258);
	String_release(&hud->str1268);
}

