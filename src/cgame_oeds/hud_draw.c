/*
 * Hud drawing: top bar (messages, hints, target health, NPC mood), bottom
 * status bar (HP gauge, armour, ammo, the wand and its speech bubble), damage
 * arrows, cinematic captions and the controls tutorial overlay.
 */

#include "ui_local.h"

/* HUD strings */
#define MOD_STR_MOOD          0x2e6f4u
#define MOD_STR_ANGRY         0x2e6fcu
#define MOD_STR_TREASURE      0x2e704u
#define MOD_FMT_TO            0x2e714u
#define MOD_STR_INACTIVE_PORTAL 0x2e71cu
#define MOD_STR_TO_DRAGON     0x2e734u
#define MOD_STR_HEALTH        0x2e750u
#define MOD_STR_RING          0x2e748u
#define MOD_STR_ARMOUR        0x2e758u
#define MOD_STR_AMMO          0x2e760u
#define MOD_STR_WAND          0x2e768u
#define MOD_STR_TO_CONTINUE   0x2e770u
#define MOD_STR_PRESS_OK      0x2e77cu
#define MOD_STR_TARGET        0x2e788u
#define MOD_STR_EFFECTS       0x2e790u
#define MOD_STR_STATUS        0x2e798u
#define MOD_STR_NO_AMMO       0x2e7a4u

#define WRAP_NO_LIMIT 9999
#define MAP_HUB       6011   /* DS Dragon's Lair (BREW 6008 hub.bsp) */
#define TUTORIAL_BG   ((int)0xff001d59u)

/* 0x00009830  soft-key bar background: the bar segment's lower half, tiled */
void Hud_drawSoftkeyBarBg(Hud *hud, int x, int y, int w)
{
	int i;

	for (i = 0; i < w; i += hud->barSegment.width)
		Canvas_drawRegion(hud->canvas, &hud->barSegment, 0, 0x14, hud->barSegment.width, 0x14, 0, x + i, y, 0, 0,
		                  NULL);
}

/* 0x0000bf04 */
void Hud_drawLabelBox(Hud *hud, char *str, int x, int y, int bgColor, int borderColor)
{
	int w = (int)strlen(str) * 7 + 6;

	Gfx_setColor(hud->app, bgColor);
	Gfx_fillRect(hud->app, x, y, w, 0x10);
	Gfx_setColor(hud->app, borderColor);
	Gfx_drawRect(hud->app, x, y, w, 0x10);
	Canvas_drawStringRange(hud->canvas, str, x + 3, y + 3, 0, 0, -1, 0);
}

/* 0x0000a02c  captions set by the MESSAGE script op during cinematics */
void Hud_drawCinematicText(Hud *hud)
{
	Canvas *cv = hud->canvas;

	if (hud->cinematicTop[0] != '\0')
		Canvas_drawStringRange(cv, hud->cinematicTop, cv->viewCenterX, hud->topBarH - 5, 0x10, 0, -1, 1);
	if (hud->cinematicBottom[0] != '\0')
		Canvas_drawStringRange(cv, hud->cinematicBottom, cv->viewCenterX,
		                       cv->originY + cv->viewH - hud->topBarH * 2 + 8, 0x10, 0, -1, 0);
}

/* 0x0000a804
 * Buff box, then the damage-direction arrow (18x18 frames: bottom, left, right)
 * while Hud.arrowUntil runs. Direction -1 is the stat flash (no arrow). */
void Hud_drawOverlays(Hud *hud)
{
	Canvas *cv = hud->canvas;
	int x, y, sy;

	Player_drawBuffs(hud->player);
	if (hud->app->tickTime < hud->arrowUntil && hud->arrowDir >= 0) {
		if (hud->arrowSide == 0)
			return;
		if (hud->arrowSide == 1) {
			x = cv->viewW - 0x14;
			y = cv->viewH >> 1;
			sy = 0x24;
		} else if (hud->arrowSide == 3) {
			x = 0x14;
			y = cv->viewH >> 1;
			sy = 0x12;
		} else {
			x = cv->viewW >> 1;
			y = cv->viewH - hud->bottomBarH - 0x19;
			sy = 0;
		}
		Canvas_drawRegion(cv, &hud->attackArrows, 0, sy, 0x12, 0x12, 0, x, y, 0x30, 0, NULL);
	} else if (hud->arrowUntil != 0) {
		hud->arrowUntil = 0;
	}
}

/* 0x0000a8b4  tutorial overlay naming the HUD elements */
void Hud_drawControlsHelp(Hud *hud)
{
	Canvas *cv = hud->canvas;
	Image *anchors = &hud->chatAnchors;
	int bottom = cv->originY + cv->viewH;
	int y;

	Gfx_setColor(hud->app, UI_BLACK);
	Gfx_fillRect(hud->app, cv->viewCenterX - 0x28, cv->viewCenterY - 0xe, 0x50, 0x1a);
	Gfx_setColor(hud->app, UI_WHITE);
	Gfx_drawRect(hud->app, cv->viewCenterX - 0x28, cv->viewCenterY - 0xe, 0x50, 0x1a);
	Canvas_drawStringRange(cv, Ui_str(MOD_STR_PRESS_OK), cv->viewCenterX, cv->viewCenterY, 0x12, 0, -1, 0);
	Canvas_drawStringRange(cv, Ui_str(MOD_STR_TO_CONTINUE), cv->viewCenterX, cv->viewCenterY, 0x10, 0, -1, 0);

	/* the anchor arrows are drawn black-on-blue through blend 0xe */
	Render_setBlendColors(hud->render, UI_BLACK, TUTORIAL_BG);
	Hud_drawLabelBox(hud, Ui_str(MOD_STR_WAND), 0, bottom - 0x46, TUTORIAL_BG, UI_WHITE);
	Canvas_drawRegion(cv, anchors, 9, 9, 9, 9, 0, 4, bottom - 0x36, 0, 0xe, NULL);
	Hud_drawLabelBox(hud, Ui_str(MOD_STR_AMMO), 0x30, bottom - 0x34, TUTORIAL_BG, UI_WHITE);
	Canvas_drawRegion(cv, anchors, 9, 9, 9, 9, 0, 0x45, bottom - 0x24, 0, 0xe, NULL);
	Hud_drawLabelBox(hud, Ui_str(MOD_STR_ARMOUR), 0x5c, bottom - 0x34, TUTORIAL_BG, UI_WHITE);
	Canvas_drawRegion(cv, anchors, 9, 9, 9, 9, 0, 0x70, bottom - 0x24, 0, 0xe, NULL);
	Hud_drawLabelBox(hud, Ui_str(MOD_STR_HEALTH), 100, bottom - 0x4c, TUTORIAL_BG, UI_WHITE);
	Canvas_drawRegion(cv, anchors, 9, 0, 9, 9, 0, 0x94, bottom - 0x48, 0, 0xe, NULL);
	Gfx_drawLine(hud->app, 0x9d, bottom - 0x44, 0x9d, bottom - 0x1c);
	Hud_drawLabelBox(hud, Ui_str(MOD_STR_RING), 0x10, hud->topBarH + 0x23, TUTORIAL_BG, UI_WHITE);
	Canvas_drawRegion(cv, anchors, 0, 0, 9, 9, 0, 0x22, hud->topBarH + 0x24, 2, 0xe, NULL);
	Gfx_drawLine(hud->app, 0x26, hud->topBarH + 8, 0x26, hud->topBarH + 0x1a);
	y = hud->topBarH + 8;
	Gfx_drawLine(hud->app, 0x14, y, 0x26, y);
	Hud_drawLabelBox(hud, Ui_str(MOD_STR_TARGET), 0x3c, hud->topBarH + 8, TUTORIAL_BG, UI_WHITE);
	Canvas_drawRegion(cv, anchors, 0, 0, 9, 9, 0, 0x49, hud->topBarH + 9, 2, 0xe, NULL);
	Gfx_drawLine(hud->app, 0x4d, hud->topBarH, 0x4d, 0xf);
	if (hud->player->numEffects != 0) {
		Gfx_setColor(hud->app, TUTORIAL_BG);
		Gfx_fillRect(hud->app, 0x74, 0x13, 0x36, 0x1c);
		Gfx_setColor(hud->app, UI_WHITE);
		Gfx_drawRect(hud->app, 0x74, 0x13, 0x36, 0x1c);
		Canvas_drawStringRange(cv, Ui_str(MOD_STR_STATUS), 0x76, 0x15, 0, 0, -1, 0);
		Canvas_drawStringRange(cv, Ui_str(MOD_STR_EFFECTS), 0x76, 0x22, 0, 0, -1, 0);
		Canvas_drawRegion(cv, anchors, 9, 9, 9, 9, 0, 0x8c, 0x2f, 0, 0xe, NULL);
	}
}

/* 0x0000accc
 * Health pips of the monster in front of the player, or of the last one hit
 * for 750 ms after it died. The shown value drains over 250 ms. */
void Hud_drawTargetHealth(Hud *hud)
{
	Applet *app = hud->app;
	Entity *ent = hud->player->facing;
	Entity *last = hud->lastTarget;
	int maxHp, hp, dt, pips, filled, w, x, third, color, i;
	uint8_t sub;

	if ((ent == NULL || ent->def->eType != 2) && last != NULL && (last->info & 0x40020000)) {
		if (hud->targetChangeTime == 0) {
			hud->targetChangeTime = app->tickTime;
			ent = last;
		} else if (app->tickTime - hud->targetChangeTime <= 0x2ed) {
			ent = last;
		}
	}
	if (ent == NULL || ent->monster == NULL ||
	    ((ent->def->eSubType == 2 || ent->def->eSubType == 0xa) && !(ent->info & 0x20000000))) {
		/* nothing, or a mimic / disguised monster not yet revealed */
		hud->lastTarget = NULL;
		return;
	}

	maxHp = CombatEntity_getMaxHealth(&ent->monster->ce);
	hp = CombatEntity_getHealth(&ent->monster->ce);
	if (hud->lastTarget == ent) {
		if (hud->targetLastHp != hp) {
			hud->targetLastHp = hp;
			hud->targetChangeTime = app->tickTime;
		}
	} else {
		hud->lastTarget = ent;
		hud->targetShownHp = hp;
		hud->targetLastHp = hp;
		hud->targetChangeTime = 0;
	}
	if (hud->targetShownHp > maxHp)
		hud->targetShownHp = maxHp;

	dt = app->tickTime - hud->targetChangeTime;
	if (dt < 0xfb)
		hp = hud->targetShownHp - dt * (hud->targetShownHp - hud->targetLastHp) / 250;
	else
		hud->targetShownHp = hp;

	sub = ent->def->eSubType;
	if (sub == 0xe)
		pips = 25;
	else if (sub == 0xc)
		pips = 20;
	else if (sub == 0xd)
		pips = (ent->monster->flags & 8) ? 40 : 25;
	else if (sub == 0xf)
		pips = (((((maxHp << 16) / 0xc800) * 0xc00) >> 8) + 0xff) >> 8;  /* maxHp * 12 / 200 */
	else if (maxHp > 200)
		pips = 25;
	else
		pips = (((((maxHp << 16) / 0xc800) * 0x3200) >> 8) + 0xff) >> 8; /* maxHp / 4 */

	filled = (((pips * 0x100 * ((hp << 16) / (maxHp << 8))) >> 8) + 0xff) >> 8;
	w = pips * 3 + 8;
	x = hud->canvas->viewCenterX - (w >> 1);
	Gfx_setColor(app, UI_BLACK);
	Gfx_fillRect(hud->app, x, hud->topBarH + 0xc, w, 9);
	Gfx_setColor(hud->app, (int)0xffaaaaaau);
	Gfx_drawRect(hud->app, x, hud->topBarH + 0xc, w, 9);

	third = pips / 3;
	if (filled <= third)
		color = (int)0xffff0000u;
	else if (pips - filled > third)
		color = (int)0xffff8800u;
	else
		color = (int)0xff00ff00u;
	Gfx_setColor(hud->app, color);
	x += 5;
	for (i = 0; i < filled; i++) {
		Gfx_fillRect(hud->app, x, hud->topBarH + 0x10, 2, 2);
		x += 3;
	}
}

/* 0x0000aeac
 * Mood meter of the NPC in front of the player (Player.mood 0..255, eased over
 * 256 ms); below 0x20 a flashing "Angry" replaces the indicator. */
void Hud_drawMoodMeter(Hud *hud)
{
	Canvas *cv = hud->canvas;
	int mood = hud->player->mood;
	int boxX = cv->originX + cv->viewW - 0x3c;
	int boxY = cv->originY + 2;
	int dt, px, flags;

	if (hud->moodTarget != mood) {
		hud->moodTarget = mood;
		hud->moodTime = hud->app->tickTime;
	}
	dt = hud->app->tickTime - hud->moodTime;
	if (dt <= 0x100)
		mood = hud->moodShown - ((dt * (hud->moodShown - hud->moodTarget)) >> 8);
	else
		hud->moodShown = mood;

	px = (((((mood << 16) / 0xe000) * 0x3200) >> 8) + 0x80) >> 8; /* mood * 50 / 224 */
	if (px > 0x32)
		px = 0x32;

	Canvas_drawImage(cv, &hud->moodMeter, cv->originX + cv->viewW, cv->originY, 8, 0);
	Canvas_drawString(hud->canvas, Ui_str(MOD_STR_MOOD), hud->canvas->viewW / 2 + 0x14, hud->topBarH / 2, 0x28, 0);
	if (mood < 0x20) {
		Gfx_setColor(hud->app, UI_BLACK);
		Gfx_fillRect(hud->app, boxX, boxY, 0x34, 0xf);
		hud->canvas->textColor = (uint16_t)Color_rgb888to565(hud->render, (int)0xffff0000u);
		flags = 0x10;
		if (((hud->app->tickTime & 0x1fff) >> 8) == 0) {
			flags = 0x11; /* large for 256 ms every 8 s */
		} else {
			int r = Random_nextByte(&hud->app->random);
			boxX += r & 2;
			boxY += (r >> 4) & 2;
		}
		Canvas_drawStringRange(hud->canvas, Ui_str(MOD_STR_ANGRY), boxX + 0x19, boxY + 4, 0x30, 0, -1, flags);
	} else {
		Canvas_drawImage(hud->canvas, &hud->moodIndicator, boxX + px, hud->topBarH / 2, 0x30, 0);
	}
}

/* 0x0000c040
 * Top bar: background, corner pieces, equipped ring, then one text line: the
 * front queued message (scrolling when too long), the forced message, a second
 * script message, or the name of what the player faces with an action icon.
 * Some texts are shown wrapped in a box under the bar instead. */
void Hud_drawTopBar(Hud *hud)
{
	Applet *app = hud->app;
	Canvas *cv = hud->canvas;
	Entity *ent = hud->player->facing;
	char buf[0x80];
	char wrapped[0x80 + 1]; /* + 1: Text_wrap 0x2a84 can put its NUL one past the size it is given */
	int textX = 1;
	int scroll = 0;
	int boxed = 0;
	int icon = -1;
	int x, len, elapsed, over;

	Gfx_setColor(hud->app, UI_BLACK);
	Gfx_fillRect(hud->app, 0, 0, hud->canvas->viewW, hud->topBarH);
	Gfx_setColor(hud->app, (int)0xff88713eu);
	Gfx_drawRect(hud->app, 0, 0, hud->canvas->viewW - 1, hud->topBarH - 1);
	for (x = 0; x < hud->canvas->viewW; x += hud->barSegment.width)
		Canvas_drawRegion(hud->canvas, &hud->barSegment, 0, 0, hud->barSegment.width, 0x14, 0, x, 0, 0, 0, NULL);
	Canvas_drawImage(hud->canvas, &hud->topLeft, 0, hud->topBarH, 5, 0);
	Canvas_drawImage(hud->canvas, &hud->topRight, hud->canvas->viewW, hud->topBarH, 9, 0);
	Canvas_drawImage(hud->canvas, &hud->topCenter, hud->canvas->viewW / 2, hud->topBarH, 0x11, 0);

	/* gold rule under the bar between the corner pieces */
	if ((uint32_t)(hud->topLeft.width + hud->topRight.width + hud->topCenter.width) < (uint32_t)cv->viewW) {
		int midL = cv->viewW / 2 - (int)((uint32_t)(hud->topCenter.width + 1) >> 1);
		int midR = cv->viewW / 2 + (int)((uint32_t)hud->topCenter.width >> 1);

		Gfx_setColor(hud->app, (int)0xffeecc5au);
		Gfx_drawLine(hud->app, hud->topLeft.width, hud->topBarH, midL, hud->topBarH);
		Gfx_drawLine(hud->app, midR, hud->topBarH, cv->viewW - hud->topRight.width, hud->topBarH);
		Gfx_setColor(hud->app, (int)0xff865e03u);
		Gfx_drawLine(hud->app, hud->topLeft.width, hud->topBarH + 1, midL, hud->topBarH + 1);
		Gfx_drawLine(hud->app, midR, hud->topBarH + 1, cv->viewW - hud->topRight.width, hud->topBarH + 1);
	}

	if (hud->player->ring != 0) {
		Canvas_drawImage(hud->canvas, &hud->ringBox, 0, hud->topBarH, 5, 0);
		Canvas_drawRegion(hud->canvas, &hud->canvas->ringIcons, 0, hud->player->ring * 0x12 - 0x12, 0x12, 0x12, 0,
		                  (int)((uint32_t)hud->ringBox.width >> 1),
		                  hud->topBarH + (int)((uint32_t)hud->ringBox.height >> 1) - 1, 0x30, 0, NULL);
	}

	/* expire the front message 150 ms after its time */
	if (hud->numMessages > 0 && app->tickTime - hud->msgStart > hud->msgDuration + 0x96)
		Hud_shiftMsgs(hud);

	if (hud->numMessages > 0) {
		strcpy(buf, hud->messages);
		if (hud->msgFlags[0] & 2)
			boxed = 1;
		elapsed = app->tickTime - hud->msgStart;
		if (elapsed > hud->msgDuration)
			goto done;
		over = (int)strlen(buf) - hud->topBarChars;
		if (over > 0 && elapsed > 750) {
			scroll = (elapsed - 750) / 100; /* 100 ms per character after 750 ms */
			if (scroll > over)
				scroll = over;
		}
		if (boxed)
			goto box;
		goto plain;
	}

	if (hud->forcedMsg[0] != '\0') {
		strcpy(buf, hud->forcedMsg);
		goto plain;
	}
	if (hud->msg2[0] != '\0') {
		strcpy(buf, hud->msg2);
		goto plain;
	}

	/* what the player is facing */
	if (cv->state == 2 || ent == NULL || ent->def->eType == 0 || ent->def->eType == 0xc)
		goto done;
	Text_wrap(ent->name, WRAP_NO_LIMIT, buf, 0x80, 0, '|', NULL);
	icon = 0;
	switch (ent->def->eType) {
	case 7: /* NPCs */
		if (ent->def->eSubType == 5) {
			if (Entity_distSqFrom(ent, cv->viewX, cv->viewY) <= 0x4000) {
				int ev = Render_findTileEvent(hud->render, 0x22b);
				icon = 1;
				if (ev != -1 && ((hud->render->tileEvents[ev] & 0x7fffffff) >> 27) != 0)
					icon = 3;
			}
			break;
		}
		if (ent->def->eSubType == 1)
			goto nearUse;
		if (ent->def->eSubType == 2)
			goto box;
		break;
	case 3:
		if (Entity_distSqFrom(ent, cv->viewX, cv->viewY) <= 0x1000)
			icon = 1;
		break;
	case 2: /* monsters */
		if (ent->def->eSubType == 0xa) {
			if (!(ent->info & 0x20000000))
				goto done;
		} else if (ent->def->eSubType == 2 && !(ent->info & 0x20000000)) {
			strcpy(buf, Ui_str(MOD_STR_TREASURE)); /* an unrevealed mimic */
		}
		break;
	case 5:
	nearUse:
		if (Entity_distSqFrom(ent, cv->viewX, cv->viewY) <= 0x1000)
			icon = 2;
		if (ent->def->eType == 5 && ent->def->parm == 1)
			goto box;
		break;
	case 0xf: /* fire portal */
		if (cv->mapId == MAP_HUB) {
			if (ent->param & 0x100) {
				App_snprintf(buf, 0x80, Ui_str(MOD_FMT_TO), hud->game->levelNames + (ent->param & 0xff) * 0x18);
				goto box;
			}
			strcpy(buf, Ui_str(MOD_STR_INACTIVE_PORTAL));
		} else if (hud->player->firePortals & 2) {
			App_snprintf(buf, 0x80, Ui_str(MOD_STR_TO_DRAGON));
			goto box;
		}
		break;
	default:
		break;
	}

	/* action icon (16x16 frames of hud_actions) left of the text */
	Canvas_drawRegion(hud->canvas, &hud->actions, 0, icon << 4, 0x10, 0x10, 0, textX, hud->topBarH / 2 - 7, 5, 0,
	                  NULL);
	textX += 0x10;

plain:
	len = (int)strlen(buf);
	if (len * 9 + textX + 9 > hud->canvas->viewW)
		len = (hud->canvas->viewW - textX) / 7 - 1;
	Canvas_drawStringRange(hud->canvas, buf, textX, (hud->topBarH >> 1) - 5, 0, scroll, len, 0);
	goto done;

box: {
		/* wrapped text in a black box centred under the bar */
		int boxW, boxH, half, lines, y, i, lineStart;

		Text_wrap(buf, hud->canvas->textCols[2], wrapped, 0x80, 0, '|', NULL);
		boxW = (int)strlen(wrapped) * 7 + 10;
		if (hud->canvas->viewW < boxW)
			boxW = hud->canvas->viewW;
		lines = (int)((uint32_t)strlen(wrapped) / (uint32_t)hud->canvas->textCols[2]) + 1;
		Gfx_setColor(hud->app, UI_BLACK);
		half = boxW / 2;
		boxH = lines * 12 + 8;
		y = hud->topBarH / 2 + hud->topBarH + 0xf;
		Gfx_fillRect(hud->app, hud->canvas->viewW / 2 - half, y, boxW - 1, boxH);
		Gfx_setColor(hud->app, (int)0xffaaaaaau);
		Gfx_drawRect(hud->app, hud->canvas->viewW / 2 - half, y, boxW - 1, boxH);

		y = hud->topBarH / 2 + hud->topBarH + 0x15;
		lineStart = 0;
		i = 0;
		for (;;) {
			char c = wrapped[i++];
			if (c == '\0')
				break;
			if (c == '|') {
				Canvas_drawStringRange(hud->canvas, wrapped + lineStart, hud->canvas->viewW / 2, y, 0x11, 0,
				                       i - lineStart - 1, 0);
				y += 12;
				lineStart = i;
			}
		}
		if (lineStart != i)
			Canvas_drawStringRange(hud->canvas, wrapped + lineStart, hud->canvas->viewW / 2, y, 0x11, 0,
			                       i - lineStart, 0);
	}

done:
	Hud_drawTargetHealth(hud);
	if (ent != NULL && ent->def->eType == 7 && ent->def->eSubType == 5)
		Hud_drawMoodMeter(hud);
}

/* 0x0000cb50
 * The wand's speech bubble: up to three lines typed at 50 ms per character,
 * each line expiring 3 s after it was added. Placed over the wand, or centred
 * when the wand is not the current weapon. */
void Hud_drawSpeechBubble(Hud *hud)
{
	Player *player = hud->player;
	int16_t *wandBox = &hud->layout[0x18];
	int16_t *centreBox = &hud->layout[0x1c];
	int16_t *box = wandBox;
	int baseY, y, x, n, tailX, tailY, tailSy, i;

	if (hud->canvas->state == 7)
		return;
	if (hud->numSubtitles == 0)
		return;
	if (hud->subtitleTimes[0] + 3000 < hud->app->tickTime) {
		Hud_shiftSubtitles(hud);
		if (hud->numSubtitles == 0)
			return;
	}
	Hud_calcMaxLineLen(hud);

	if (player->ce.weapon == 0) {
		int state = hud->canvas->state;
		if (state == 4) {
			Combat *combat = hud->app->combat;
			/* hidden while the wand itself is firing */
			if (combat->attacker == NULL && combat->state != 2 && combat->shotToggle != 0 &&
			    hud->app->tickTime < combat->attackFrameUntil)
				return;
		}
		if (state == 0xd) {
			if (!(player->weapons & 1))
				return;
		} else {
			hud->layout[0x1c] = (int16_t)(hud->canvas->viewW / 2 - (int16_t)(hud->maxSubtitleLen * 7 + 9));
			box = centreBox;
		}
	} else if (!(player->weapons & 1)) {
		box = &hud->layout[0x20];
	}

	baseY = hud->canvas->viewH + box[1];
	y = baseY - 0xc;
	x = box[2] / 2 + hud->canvas->viewCenterX + box[0];
	n = hud->numSubtitles * 0xc;
	Gfx_setColor(hud->app, (int)0xff6633ccu);
	Gfx_fillRect(hud->app, x, y - (n - 0xc) - 2, hud->maxSubtitleLen * 7 + 4, n + 2);
	Gfx_setColor(hud->app, UI_WHITE);
	Gfx_drawRect(hud->app, x, y - (n - 0xc) - 2, hud->maxSubtitleLen * 7 + 4, n + 2);

	/* the bubble's tail */
	tailY = baseY;
	tailSy = 0;
	if (box == centreBox) {
		tailX = hud->maxSubtitleLen * 7 + x - 0xc;
	} else if (box != wandBox) {
		tailY = y - hud->numSubtitles * 0xc + 2;
		tailX = x + 0x18;
		tailSy = 9;
	} else {
		tailX = x + 8;
	}
	Canvas_drawRegion(hud->canvas, &hud->wandChat, 0, tailSy, 0xf, 9, 0, tailX, tailY, 5, 0, NULL);

	for (i = 0; i < hud->numSubtitles; i++) {
		Canvas_drawStringRange(hud->canvas, &hud->subtitles[i * 0x12], x + 1, y, 0, 0,
		                       (hud->app->tickTime - hud->subtitleTimes[i]) / 50, 0);
		y -= 0xc;
	}
}

/* 0x00009884
 * Bottom status bar: HP gauge (large layout) and number, armour number and
 * icon, the wand (jittering while it talks), ammo count and icon, then the
 * speech bubble. HP changes ease over 250 ms and flash; low HP / armour
 * blink. */
void Hud_drawStatusBar(Hud *hud)
{
	Canvas *cv = hud->canvas;
	Player *player = hud->player;
	CombatEntity *ce = &player->ce;
	int16_t *L = hud->layout;
	int cx = cv->viewCenterX;
	int bottom = (int16_t)(cv->originY + cv->viewH);
	int hp, maxHp, armor, maxArmor, dt, fillH, ratio, textFlags, x;
	int hpFlash = 0, armorFlash = 0;
	int large = hud->largeLayout != 0;

	hp = CombatEntity_getHealth(ce);
	maxHp = CombatEntity_getMaxHealth(ce);
	armor = CombatEntity_getArmorPoints(ce);
	maxArmor = CombatEntity_getMaxArmorPoints(ce);

	if (maxHp < hud->fromHp) {
		hud->lastHp = maxHp;
		hud->fromHp = maxHp;
	}
	if (hud->lastHp != hp) {
		hud->lastHp = hp;
		hud->hpChangeTime = hud->app->tickTime;
	}
	dt = hud->app->tickTime - hud->hpChangeTime;
	if (dt < 0xfb)
		hp = hud->fromHp - dt * (hud->fromHp - hud->lastHp) / 250;
	else
		hud->fromHp = hp;

	if (hud->lastHp != hp || (hp <= maxHp >> 2 && ((hud->app->tickTime / 491) & 1)))
		hpFlash = 1;
	if (armor <= maxArmor >> 2 && (hud->app->tickTime & 0x200))
		armorFlash = 1;

	/* HP gauge (box 1) */
	ratio = (hp << 16) / (maxHp << 8);
	fillH = (((ratio * L[7] * 0x100) >> 8) + 0x80) >> 8;
	Gfx_setColor(hud->app, (int)0xff333333u);
	Gfx_fillRect(hud->app, L[4] + cx, L[5] + bottom, L[6], L[7] - fillH);
	Gfx_setColor(hud->app, hpFlash ? (int)0xffff882fu : (int)0xffcc0000u);
	Gfx_fillRect(hud->app, L[4] + cx, L[5] + bottom + L[7] - fillH, L[6], fillH);

	Canvas_drawImage(hud->canvas, &hud->bottomCenter, cx, hud->canvas->viewH, 0x12, 0);
	if (hud->canvas->viewW > 0xc6) {
		for (x = cx - (int)((uint32_t)hud->bottomCenter.width >> 1); x >= 0; x -= hud->bottomLeft.width)
			Canvas_drawImage(hud->canvas, &hud->bottomLeft, x, hud->canvas->viewH, 10, 0);
		for (x = (int)((uint32_t)hud->bottomCenter.width >> 1) + cx; x < hud->canvas->originX + hud->canvas->viewW;
		     x += hud->bottomRight.width)
			Canvas_drawImage(hud->canvas, &hud->bottomRight, x, hud->canvas->viewH, 6, 0);
	}

	/* the wand (box 6) */
	if (player->weapons & 1) {
		int jx = 0, jy = 0;
		int talking = hud->numSubtitles > 0 &&
		              hud->app->tickTime < hud->subtitleTimes[hud->numSubtitles - 1] + 500;

		if (!talking && hud->canvas->state == 7 && hud->canvas->dialogStyle == 1 &&
		    hud->app->tickTime % 1000 < 400)
			talking = 1; /* wand dialog: jitter 400 ms of every second */
		if (talking) {
			jx = 2 - (int)(((uint32_t)Random_nextInt(&hud->app->random) & 0x7fff) % 3);
			jy = 1 - (int)(((uint32_t)Random_nextInt(&hud->app->random) & 0x7fff) % 3);
		}
		Canvas_drawImage(hud->canvas, &hud->wand, L[0x1a] / 2 + L[0x18] + cx + jx, L[0x1b] / 2 + L[0x19] + bottom + jy,
		                 0x30, 0);
		if ((uint8_t)hud->canvas->glowPalette != 0xff)
			Canvas_drawImage(hud->canvas, &hud->wandGlow, L[0x1a] / 2 + L[0x18] + cx + jx,
			                 L[0x1b] / 2 + L[0x19] + bottom + jy, 0x30, (uint8_t)hud->canvas->glowPalette);
	}

	/* HP number (box 0): large colour-cycling font while flashing */
	App_snprintf(hud->hpText, 4, "%d", hp);
	Canvas_drawStringRange(hud->canvas, hud->hpText, L[2] / 2 + L[0] + cx, L[3] / 2 + L[1] + bottom, 0x30, 0, -1,
	                       hpFlash ? 9 : 0);

	/* armour number (box 2), red when low; armour icon (box 3) */
	App_snprintf(hud->armorText, 4, "%d", (uint16_t)player->ce.armor);
	textFlags = large;
	if (armorFlash) {
		hud->canvas->textColor = 0xc800;
		textFlags |= 0x10;
	}
	Canvas_drawStringRange(hud->canvas, hud->armorText, L[10] / 2 + L[8] + cx, L[0xb] / 2 + L[9] + bottom, 0x30, 0,
	                       -1, textFlags);
	Canvas_drawRegion(hud->canvas, &hud->largeIcons, 0, (int8_t)player->armorOwned * 0x14, 0xe, 0x14, 0,
	                  L[0xe] / 2 + L[0xc] + cx, L[0xf] / 2 + L[0xd] + bottom, 0x30, 0, NULL);

	/* ammo count (box 4) and icon (box 5) */
	if (player->weapons != 0) {
		Combat *combat = hud->app->combat;
		int weapon = player->ce.weapon;
		int ammoType = combat->weapons[weapon].ammoType;
		int icon;

		if (ammoType == 0)
			strcpy(hud->ammoText, Ui_str(MOD_STR_NO_AMMO));
		else
			App_snprintf(hud->ammoText, 3, "%d", player->ammo[ammoType]);
		Canvas_drawStringRange(hud->canvas, hud->ammoText, L[0x12] / 2 + L[0x10] + cx, L[0x13] / 2 + L[0x11] + bottom,
		                       0x30, 0, 2, large);

		ammoType = combat->weapons[weapon].ammoType;
		if (ammoType == 1)
			icon = 4;
		else if (ammoType == 2)
			icon = 5;
		else if (ammoType == 3)
			icon = 6;
		else if (ammoType == 0)
			icon = (player->ce.weapon == 8) ? 9 : 8;
		else
			icon = 7;
		Canvas_drawRegion(hud->canvas, &hud->largeIcons, 0, icon * 0x14, 0xe, 0x14, 0, L[0x16] / 2 + L[0x14] + cx,
		                  L[0x17] / 2 + L[0x15] + bottom, 0x30, 0, NULL);
	}
	Hud_drawSpeechBubble(hud);
}

/* 0x00008df8
 * The whole HUD; then, for a few seconds after Player.timestamp is set (op
 * 0x61), one to three "hammer gone" images stacked diagonally, cycling every
 * 512 ms of play time. */
void Hud_draw(Hud *hud)
{
	int t = hud->player->timestamp;
	Image *img = &hud->hammerGone;

	Hud_drawTopBar(hud);
	Hud_drawOverlays(hud);
	Hud_drawStatusBar(hud);
	if (t == 0)
		return;
	switch ((((uint32_t)(hud->app->gameTime - t) >> 9) + 1) % 5) {
	case 3:
	case 4:
		Canvas_drawImage(hud->canvas, img, 0x4b, 0x4b, 0, 0);
		/* fall through */
	case 2:
		Canvas_drawImage(hud->canvas, img, 0x37, 0x37, 0, 0);
		/* fall through */
	case 1:
		Canvas_drawImage(hud->canvas, img, 0x23, 0x23, 0, 0);
		break;
	default:
		break;
	}
}
