/*
 * Hud (Applet object 0x128, embedded at Applet+0x1b30b4): the top-bar message
 * queue, the wand's speech-bubble lines ("subtitles"), the wand chat barks
 * (wchats.txt), start-up and shut-down. Drawing lives in hud_draw.c.
 *
 * O&E 1 keeps all text in fixed C buffers inside the Hud (O&E II used String
 * objects): 5 messages of 0x40 chars, 3 bubble lines of 0x12 chars.
 */

#include "ui_local.h"

#define WRAP_NO_LIMIT 9999 /* Text_wrap width meaning "only translate '|'" */
#define MSG_SIZE      0x40
#define MAX_MESSAGES  5
#define LINE_SIZE     0x12
#define MAX_LINES     3
#define NUM_BARK_GROUPS 14
#define MAX_BARKS     20   /* BarkGroup.strings[] */

#define RES_WCHATS    5110 /* wchats.txt */

/* STRDUP (AEEStdLib) */
static char *dupString(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);

	if (d != NULL)
		memcpy(d, s, n);
	return d;
}

static char *msgSlot(Hud *hud, int i)
{
	return &hud->messages[i * MSG_SIZE];
}

static char *lineSlot(Hud *hud, int i)
{
	return &hud->subtitles[i * LINE_SIZE];
}

/* 0x00006908
 * The front message stays up 1.2 s, or 100 ms per character when it is too long
 * for the top bar and has to scroll. */
void Hud_resetMsgTimer(Hud *hud)
{
	int len;

	hud->msgStart = hud->app->tickTime;
	len = (int)strlen(hud->messages);
	hud->msgDuration = (len > hud->topBarChars) ? len * 100 : 1200;
}

/* 0x000265f0 */
void Hud_shiftMsgs(Hud *hud)
{
	int i;

	for (i = 0; i < hud->numMessages - 1; i++) {
		strcpy(msgSlot(hud, i), msgSlot(hud, i + 1));
		hud->msgFlags[i] = hud->msgFlags[i + 1];
	}
	hud->numMessages--;
	msgSlot(hud, hud->numMessages)[0] = '\0';
	hud->msgFlags[hud->numMessages] = 0;
	if (hud->numMessages > 0)
		Hud_resetMsgTimer(hud);
}

/* 0x000033bc
 * Queues a top-bar message. flags bit 0 = priority: clears the queue and
 * doubles the display time; bit 1 shows it in a box under the bar. A message
 * equal to the last one queued is dropped. */
void Hud_addMessage(Hud *hud, char *str, int flags)
{
	int n;

	if (str == NULL)
		return;
	if (flags & 1)
		hud->numMessages = 0;
	if (hud->numMessages > 0 && strcmp(str, msgSlot(hud, hud->numMessages - 1)) == 0)
		return;
	if (hud->numMessages == MAX_MESSAGES)
		Hud_shiftMsgs(hud);

	n = hud->numMessages;
	Text_wrap(str, WRAP_NO_LIMIT, msgSlot(hud, n), MSG_SIZE, 0, '|', NULL);
	hud->msgFlags[n] = flags;
	hud->numMessages = n + 1;
	if (hud->numMessages == 1) {
		Hud_resetMsgTimer(hud);
		if (flags & 1)
			hud->msgDuration <<= 1;
	}
}

/* 0x00012b10
 * Hands out the next message slot so the caller can format the text in place;
 * Hud_finishMessageBuffer commits it. */
char *Hud_getMessageBuffer(Hud *hud, int flags)
{
	if (flags & 1)
		hud->numMessages = 0;
	if (hud->numMessages == MAX_MESSAGES)
		Hud_shiftMsgs(hud);
	msgSlot(hud, hud->numMessages)[0] = '\0';
	hud->msgFlags[hud->numMessages] = flags;
	return msgSlot(hud, hud->numMessages);
}

/* 0x000124a4 */
void Hud_finishMessageBuffer(Hud *hud)
{
	char *slot = msgSlot(hud, hud->numMessages);
	char text[MSG_SIZE];

	/* The original wraps the slot onto itself; wrapping only removes characters,
	 * so going through a copy gives the same text. */
	memcpy(text, slot, MSG_SIZE);
	text[MSG_SIZE - 1] = '\0';
	Text_wrap(text, WRAP_NO_LIMIT, slot, MSG_SIZE, 0, '|', NULL);
	hud->numMessages++;
	if (hud->numMessages == 1)
		Hud_resetMsgTimer(hud);
}

/* 0x00006a78 */
void Hud_calcMaxLineLen(Hud *hud)
{
	int i, len;

	hud->maxSubtitleLen = 0;
	for (i = 0; i < hud->numSubtitles; i++) {
		len = (int)strlen(lineSlot(hud, i));
		if (len > hud->maxSubtitleLen)
			hud->maxSubtitleLen = len;
	}
}

/* 0x000070c0 */
void Hud_clearLines(Hud *hud)
{
	int i;

	/* Like the original this stops one short: the last line keeps its text but
	 * is no longer counted. */
	for (i = 0; i < hud->numSubtitles - 1; i++) {
		lineSlot(hud, i)[0] = '\0';
		hud->subtitleTimes[i] = 0;
	}
	hud->numSubtitles = 0;
}

/* 0x00026684  drops the oldest speech-bubble line */
void Hud_shiftSubtitles(Hud *hud)
{
	int i;

	for (i = 0; i < hud->numSubtitles - 1; i++) {
		strcpy(lineSlot(hud, i), lineSlot(hud, i + 1));
		hud->subtitleTimes[i] = hud->subtitleTimes[i + 1];
	}
	lineSlot(hud, hud->numSubtitles - 1)[0] = '\0';
	hud->subtitleTimes[hud->numSubtitles - 1] = 0;
	hud->numSubtitles--;
}

/* 0x00003928
 * Adds a line to the wand's speech bubble (3 lines, oldest dropped); an empty
 * string clears the bubble. */
void Hud_addTimedMsg(Hud *hud, char *str)
{
	int n;

	if (str == NULL)
		return;
	if (str[0] == '\0') {
		Hud_clearLines(hud);
		return;
	}
	if (hud->numSubtitles == MAX_LINES)
		Hud_shiftSubtitles(hud);

	n = hud->numSubtitles;
	Text_wrap(str, WRAP_NO_LIMIT, lineSlot(hud, n), LINE_SIZE, 0, '|', NULL);
	hud->subtitleTimes[n] = hud->app->tickTime;
	hud->numSubtitles = n + 1;
	Hud_calcMaxLineLen(hud);
}

/* 0x00014764
 * Flashes the status bar (damage indicator slot with direction -1). The
 * argument is the Player; the Hud is reached through its applet. */
void Hud_startStatFlash(void *player)
{
	Applet *app = ((Player *)player)->app;

	app->hud->arrowUntil = app->tickTime + 500;
	app->hud->arrowDir = -1;
}

/* 0x0001f738
 * Picks a fresh starting line in every bark group and restarts the bark
 * timers (next ambient bark in 60 s). */
void Hud_shuffleBarks(Hud *hud)
{
	int k;

	for (k = 0; k < NUM_BARK_GROUPS; k++) {
		if (hud->barks[k].count != 0)
			hud->barks[k].cur = Random_nextByte(&hud->app->random) % hud->barks[k].count;
	}
	hud->lastBarkTime = hud->app->gameTime;
	hud->nextBarkTime = hud->app->gameTime + 60000;
}

/* 0x0002ac38
 * Random wand quip of the given kind. Needs the wand; at most one per 5 s of
 * play time unless forced; fires with the group's chance (n/256, 255 =
 * always). */
void Hud_randomBark(Hud *hud, int kind, int force)
{
	BarkGroup *group = &hud->barks[kind];
	int pick;

	if (!(hud->player->weapons & 1))
		return;
	if (!force && hud->app->gameTime - hud->lastBarkTime <= 4999)
		return;
	if (group->chance <= 0xfe && Random_nextByte(&hud->app->random) > group->chance)
		return;
	if (group->count == 0)
		return;

	hud->lastBarkTime = hud->app->gameTime;
	pick = Random_nextByte(&hud->app->random) % group->count;
	Hud_addTimedMsg(hud, group->strings[pick]);
}

/* 0x0002b1b0 */
void Hud_freeBarks(Hud *hud)
{
	int k, i;

	for (k = 0; k < NUM_BARK_GROUPS; k++) {
		for (i = 0; i < hud->barks[k].count; i++) {
			free(hud->barks[k].strings[i]);
			hud->barks[k].strings[i] = NULL;
		}
		hud->barks[k].count = 0;
	}
}

/* 0x0001b6f4
 * Parses wchats.txt: one group per line, "chance,line,line,...", chance in
 * percent; blank lines and "//" comments are skipped before a group, and a
 * blank or comment line after the last group ends the file. Up to 14 groups. */
void Hud_loadWandChats(Hud *hud)
{
	InputStream is;
	char *data, *text, *end, *line, *nl, *p;
	int size, group, fields, i, atEnd;

	InputStream_ctor(&is, hud->app, 1);
	if (!Resource_open(&is, RES_WCHATS, 1)) {
		App_setErrorCode(g_app, 0x16);
		InputStream_dtor(&is, 0);
		return;
	}
	data = InputStream_getBuffer(&is);
	if (data == NULL) {
		InputStream_dtor(&is, 0);
		return;
	}

	/* The original parses the resource buffer in place and relies on a NUL after
	 * it; work on a NUL-terminated copy instead. */
	size = InputStream_getSize(&is);
	text = malloc((size_t)size + 1);
	if (text == NULL) {
		InputStream_dtor(&is, 0);
		return;
	}
	memcpy(text, data, (size_t)size);
	text[size] = '\0';
	end = text + size;
	for (p = text; p != end; p++) {
		if (*p == '\r')
			*p = '\n';
	}

	nl = NULL;
	line = text;
	atEnd = 0;
	group = 0;
	for (;;) {
		/* next line that is neither empty nor a "//" comment */
		int content = 0;
		do {
			if (nl != NULL)
				line = nl + 1;
			if (line >= end)
				goto done;
			nl = strchr(line, '\n');
			if (nl == NULL || nl > end) {
				atEnd = 1;
				nl = end;
			} else {
				*nl = '\0';
			}
			if (line[0] == '/' ? line[1] != '/' : line[0] != '\0') {
				content = 1;
				break;
			}
		} while (!atEnd);
		if (!content)
			break; /* the file ended on a comment or blank line */

		/* split on ',' into the chance and the lines */
		fields = 1;
		p = line;
		while ((p = strchr(p, ',')) != NULL && p < nl) {
			*p = '\0';
			fields++;
			p++;
		}
		if (fields == 1)
			break;

		hud->barks[group].chance = (atoi(line) << 16) / 0x6400; /* percent -> /256 */
		hud->barks[group].count = 0;
		p = line + strlen(line);
		for (i = 1; p++, i < fields; i++) {
			BarkGroup *g = &hud->barks[group];
			if (g->count < MAX_BARKS) /* the original has no bound check */
				g->strings[g->count++] = dupString(p);
			p += strlen(p);
		}
		group++;
		if (group > NUM_BARK_GROUPS - 1 || atEnd)
			break;
	}
done:
	free(text);
	InputStream_dtor(&is, 0);
}

/* 0x00027cb4
 * Loads the 19 HUD images and sets the bottom-bar layout: nine {x, y, w, h}
 * boxes, x relative to the view centre and y to the bottom of the view:
 * 0 HP number, 1 HP gauge, 2 armour number, 3 armour icon, 4 ammo number,
 * 5 ammo icon, 6 wand, 7 speech bubble (x set per frame), 8 bubble without the
 * wand. The small layout leaves the gauge and the icons at 0 (not drawn). */
int Hud_startup(Hud *hud, int largeScreen)
{
	Applet *app = hud->app;
	int16_t *L = hud->layout;

	hud->canvas = app->canvas;
	hud->game = app->game;
	hud->render = app->render;
	hud->player = app->player;
	hud->numMessages = 0;
	hud->numSubtitles = 0;
	hud->cinematicTop[0] = '\0';
	hud->cinematicBottom[0] = '\0';
	hud->topBarChars = (app->canvas->viewW - 4) / 7;
	hud->topBarH = 0x14;

	Image_load(app, 5031, 1, &hud->attackArrows);
	Image_load(hud->app, 5008, 1, &hud->actions);
	Image_load(hud->app, 5194, 1, &hud->bottomCenter);
	Image_load(hud->app, 5146, 1, &hud->bottomLeft);
	Image_load(hud->app, 5160, 1, &hud->bottomRight);
	Image_load(hud->app, 5161, 1, &hud->topCenter);
	Image_load(hud->app, 5169, 1, &hud->topLeft);
	Image_load(hud->app, 5174, 1, &hud->topRight);
	Image_load(hud->app, 5175, 1, &hud->ringBox);
	Image_load(hud->app, 5018, 1, &hud->wandChat);
	Image_load(hud->app, 5016, 1, &hud->wand);
	Image_load(hud->app, 5055, 1, &hud->iconWand);
	Image_load(hud->app, 5141, 0, &hud->barSegment);
	Image_load(hud->app, 5116, 1, &hud->moodMeter);
	Image_load(hud->app, 5051, 1, &hud->moodIndicator);
	Image_load(hud->app, 5162, 1, &hud->largeIcons);
	Image_load(hud->app, 5119, 1, &hud->chatAnchors);
	Image_load(hud->app, 5155, 1, &hud->hammerGone);
	Image_load(hud->app, 5035, 1, &hud->wandGlow);

	if (!largeScreen) {
		hud->largeLayout = 0;
		L[0] = 0x25;   L[1] = -0x12;  L[2] = 0x18;   L[3] = 0x10;
		L[8] = 2;      L[9] = -0xf;   L[10] = 0x1a;  L[11] = 0xe;
		L[16] = -0x1d; L[17] = -0xf;  L[18] = 0x1a;  L[19] = 0xe;
		L[24] = -0x3e; L[25] = -0x1c; L[26] = 0x18;  L[27] = 0x18;
	} else {
		hud->largeLayout = 1;
		L[4] = 0x35;   L[5] = -0x22;  L[6] = 0x22;   L[7] = 0x1d;
		L[0] = 0x36;   L[1] = -0x21;  L[2] = 0x1e;   L[3] = 0x18;
		L[8] = 0x10;   L[9] = -0x13;  L[10] = 0x20;  L[11] = 0x13;
		L[12] = -6;    L[13] = -0x14; L[14] = 0x20;  L[15] = 0x13;
		L[16] = -0x21; L[17] = -0x13; L[18] = 0x20;  L[19] = 0x13;
		L[20] = -0x35; L[21] = -0x14; L[22] = 0x20;  L[23] = 0x13;
		L[24] = -0x54; L[25] = -0x21; L[26] = 0x1c;  L[27] = 0x1c;
		L[32] = 0;     L[33] = -0x46; L[34] = 0x1c;  L[35] = 0x1c;
		L[29] = -0x69; L[30] = 0;     L[31] = 0;
	}
	hud->bottomBarH = 0xf;
	Hud_loadWandChats(hud);
	return 1;
}

/* 0x00026bf0 */
void Hud_shutdown(Hud *hud)
{
	App_freeImage(hud->app, &hud->actions);
	App_freeImage(hud->app, &hud->attackArrows);
	App_freeImage(hud->app, &hud->topCenter);
	App_freeImage(hud->app, &hud->topLeft);
	App_freeImage(hud->app, &hud->topRight);
	App_freeImage(hud->app, &hud->ringBox);
	App_freeImage(hud->app, &hud->bottomCenter);
	App_freeImage(hud->app, &hud->bottomLeft);
	App_freeImage(hud->app, &hud->bottomRight);
	App_freeImage(hud->app, &hud->wand);
	App_freeImage(hud->app, &hud->iconWand);
	App_freeImage(hud->app, &hud->wandChat);
	App_freeImage(hud->app, &hud->barSegment);
	App_freeImage(hud->app, &hud->moodMeter);
	App_freeImage(hud->app, &hud->largeIcons);
	App_freeImage(hud->app, &hud->chatAnchors);
	App_freeImage(hud->app, &hud->moodIndicator);
	App_freeImage(hud->app, &hud->hammerGone);
	App_freeImage(hud->app, &hud->wandGlow);
	Hud_freeBarks(hud);
}
