/*
 * Hud drawing: top bar (messages / facing-target hint), bottom status bar
 * (HP gauge, armor, ammo, wand), speech bubble, notifications, target health
 * pips, damage-direction arrows, cinematic captions and the controls overlay.
 */

#include <stdio.h>

#include "game.h"

/* Combat distance table: s32[15] = (64*k)^2, k = 1..15. Entry 0 is "within
 * one tile". */
#define MOD_TILE_DIST_SQ 0x45ebcu

/* Anchors (Canvas_drawRegion / Canvas_drawStringRange) */
#define ANCHOR_RIGHT   0x08
#define ANCHOR_HCENTER 0x10
#define ANCHOR_BOTTOM  0x02
#define ANCHOR_VCENTER 0x20

/* String_appendInt (0x7750) is still an untyped core prototype; this is what
 * it does: format with "%d" and append. */
static void hud_appendInt(String *s, int v)
{
	char buf[16];
	snprintf(buf, sizeof buf, "%d", v);
	String_appendCStr_40274(s, buf);
}

/* 0x0000e4d8
 * Centred black box below the top bar with the message word-wrapped; '|'
 * separates lines. */
void Hud_drawNotification(Hud *hud, String *msg)
{
	Canvas *cv = g_app->canvas;
	String *text = &cv->scratch;
	String line;
	int len, boxW, numLines, boxH, y, from, bar;

	if (cv->hideHud != 0)
		return;

	String_setLength(text, 0);
	TextUtil_wrapText(g_app->textUtil, msg, cv->textWidths[2], text);
	len = text->len;
	boxW = len * 7 + 10;
	if (boxW > cv->viewRect[2])
		boxW = cv->viewRect[2];
	numLines = len / cv->textWidths[2] + 1;
	y = 0x33;
	from = 0;
	if (String_indexOf(text, '|', 0) >= 0)
		numLines++;

	boxH = numLines * 12 + 8;
	Graphics_setColor(cv->graphics, (int)0xff000000);
	Graphics_fillRect(cv->graphics, cv->viewRect[2] / 2 - boxW / 2, 0x2d, boxW - 1, boxH);
	Graphics_setColor(cv->graphics, (int)0xffaaaaaa);
	Graphics_drawRect(cv->graphics, cv->viewRect[2] / 2 - boxW / 2, 0x2d, boxW - 1, boxH);

	while ((bar = String_indexOf(text, '|', from)) >= 0) {
		String_substring(&line, text, from, bar);
		Canvas_drawStringRange(cv, &line, cv->viewRect[2] / 2, y, ANCHOR_HCENTER | 1, 0, bar - from, 0);
		String_dtor(&line, 0);
		y += 12;
		from = bar + 1;
	}
	len = text->len;
	String_substring(&line, text, from, 0);
	Canvas_drawStringRange(cv, &line, cv->viewRect[2] / 2, y, ANCHOR_HCENTER | 1, 0, len - from, 0);
	String_dtor(&line, 0);
}

/* 0x0000fd14
 * Pip bar with the health of the monster being faced (or the last one hit, for
 * 750 ms). Long-lived monsters get more pips; bosses have fixed counts. */
void Hud_drawTargetHealth(Hud *hud)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	Entity *facing, *target, *last;
	EntityMonster *mon;
	int maxHp, hp, shown, elapsed, pips, filled, w, x, third, i;
	uint8_t sub;

	if (cv->hideHud != 0)
		return;

	facing = app->player->facingEntity;
	if (facing != NULL && facing->def->eType == 2) {
		target = facing;
	} else {
		last = hud->targetEnt;
		target = NULL;
		if (last != NULL && (last->info & 0x20020000u) != 0) {
			if (hud->hpAnimTime == 0) {
				hud->hpAnimTime = app->time;
				target = last;
			} else if (app->time - hud->hpAnimTime < 750) {
				target = last;
			}
		}
		if (target == NULL) {
			if (facing == NULL)
				goto clear;
			target = facing;
		}
	}

	mon = target->monster;
	if (mon == NULL || (mon->flags & 0x200) != 0)
		goto clear;
	if (target->def->eSubType == 2 && (target->info & 0x10000000u) == 0)
		goto clear;

	maxHp = CombatEntity_getMaxHealth(&mon->ce);
	hp = CombatEntity_getHealth(&target->monster->ce);
	if (hud->targetEnt != target) {
		hud->targetEnt = target;
		hud->hpShown2 = hp;
		hud->hpShown = hp;
		hud->hpAnimTime = 0;
	} else if (hud->hpShown != hp) {
		hud->hpShown2 = hud->hpShown;
		hud->hpShown = hp;
		hud->hpAnimTime = app->time;
	}
	if (hud->hpShown2 > maxHp)
		hud->hpShown2 = maxHp;

	/* the drop animates over 250 ms */
	elapsed = app->time - hud->hpAnimTime;
	if (elapsed > 250) {
		hud->hpShown2 = hp;
		shown = hp;
	} else {
		shown = hud->hpShown2 - elapsed * (hud->hpShown2 - hud->hpShown) / 250;
	}

	sub = target->def->eSubType;
	if (sub == 0xb)
		pips = 20;
	else if (sub == 0xc)
		pips = (((((maxHp << 16) / 0xc800) * 0xc00) >> 8) + 0xff) >> 8;
	else if (maxHp > 200)
		pips = 25;
	else
		pips = (((((maxHp << 16) / 0xc800) * 0x3200) >> 8) + 0xff) >> 8;

	filled = (((((shown << 16) / (maxHp << 8)) * (pips << 8)) >> 8) + 0xff) >> 8;
	if (filled == 0 && shown > 0)
		filled = 1;

	w = pips * 3 + 8;
	x = cv->screenCenterX - (w >> 1);
	Graphics_setColor(cv->graphics, (int)0xff000000);
	Graphics_fillRect(cv->graphics, x, 0x20, w, 9);
	Graphics_setColor(cv->graphics, (int)0xffaaaaaa);
	Graphics_drawRect(cv->graphics, x, 0x20, w, 9);

	third = pips / 3;
	if (third >= filled)
		Graphics_setColor(cv->graphics, (int)0xffff0000);  /* under a third: red */
	else if (third < pips - filled)
		Graphics_setColor(cv->graphics, (int)0xffff8800);  /* orange */
	else
		Graphics_setColor(cv->graphics, (int)0xff00ff00);  /* green */

	x += 5;
	for (i = 0; i < filled; i++) {
		Graphics_fillRect(cv->graphics, x, 0x24, 2, 2);
		x += 3;
	}
	return;

clear:
	hud->targetEnt = NULL;
}

/* 0x00011aec
 * Top bar: background, ornaments, equipped ring, then either the current
 * message (scrolling if too long) or a hint about what the player faces. */
void Hud_drawTopBar(Hud *hud)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	Graphics *g = cv->graphics;
	Entity *facing = app->player->facingEntity;
	String text;
	int textX = 1;    /* moves right of the action icon */
	int action = -1;  /* action icon row in imgActions, -1 = none */
	int scroll = 0;   /* first visible character of a long message */
	int segW, i, n, viewW;
	uint8_t eType;

	String_ctor(&text);

	viewW = cv->viewRect[2];
	Graphics_setColor(g, (int)0xff000000);
	Graphics_fillRect(g, 0, 0, viewW, 0x14);
	Graphics_setColor(g, (int)0xff88713e);
	Graphics_drawRect(g, 0, 0, viewW - 1, 0x13);
	segW = cv->imgHudBarSegment.width;
	for (i = 0; i < cv->viewRect[2]; i += segW)
		Canvas_drawRegion(cv, &cv->imgHudBarSegment, 0, 0, segW, 0x14, 0, i, 0, 0, 0, 0);

	Canvas_drawImageSimple(cv, &hud->imgTopLeft, 0, 0x14, 5);
	Canvas_drawImageSimple(cv, &hud->imgTopRight, cv->viewRect[2], 0x14, 9);
	Canvas_drawImageSimple(cv, &hud->imgTopCenter, cv->viewRect[2] / 2, 0x14, 0x11);
	if (hud->imgTopLeft.width + hud->imgTopRight.width + hud->imgTopCenter.width < cv->viewRect[2]) {
		/* two-tone rule under the bar between the ornaments */
		int leftEnd = cv->viewRect[2] / 2 - (hud->imgTopCenter.width + 1) / 2;
		int rightStart = cv->screenRect[2] / 2 + hud->imgTopCenter.width / 2;
		int rightEnd = cv->viewRect[2] - hud->imgTopRight.width;

		Graphics_setColor(g, (int)0xffeecc5a);
		Graphics_drawLine(g, hud->imgTopLeft.width, 0x14, leftEnd, 0x14);
		Graphics_drawLine(g, rightStart, 0x14, rightEnd, 0x14);
		Graphics_setColor(g, (int)0xff865e03);
		Graphics_drawLine(g, hud->imgTopLeft.width, 0x15, leftEnd, 0x15);
		Graphics_drawLine(g, rightStart, 0x15, rightEnd, 0x15);
	}

	if (app->player->equippedRing != 0) {
		Canvas_drawImageSimple(cv, &hud->imgTopRingBox, 0, 0x14, 5);
		Canvas_drawRegion(cv, &cv->imgIconsRings, 0, app->player->equippedRing * 18 - 18, 18, 18, 0,
		                  hud->imgTopRingBox.width / 2, hud->imgTopRingBox.height / 2 + 0x13,
		                  ANCHOR_HCENTER | ANCHOR_VCENTER, 0, 0);
	}

	/* message expired (150 ms grace) */
	if (hud->numMessages > 0 && app->time - hud->msgTime > hud->msgDuration + 150)
		Hud_shiftMsgs(hud);

	/* a monster facing us has noticed the player */
	if (cv->state != 2 && facing != NULL && facing->def->eType == 2 && app->player->isFamiliar == 0
	    && (facing->monster->flags & 0x7000) != 0) {
		String_setLength(&hud->msgBuffer, 0);
		Text_getString(cv, 0, 0x5a, &hud->msgBuffer);
		Hud_drawNotification(hud, &hud->msgBuffer);
	}

	if (hud->numMessages > 0) {
		int asNotification, elapsed, excess;

		String_assign(&text, &hud->messages[0]);
		asNotification = (hud->msgFlags[0] >> 1) & 1;
		elapsed = app->time - hud->msgTime;
		if (elapsed > hud->msgDuration)
			goto done;
		/* after 750 ms a too-long message scrolls one char per 100 ms */
		excess = text.len - hud->maxChars;
		if (excess > 0 && elapsed > 750) {
			scroll = (elapsed - 750) / 100;
			if (scroll > excess)
				scroll = excess;
		}
		if (asNotification)
			goto notify;
		goto drawText;
	}

	if (hud->forcedMsg.len > 0) {
		String_assign(&text, &hud->forcedMsg);
		goto drawText;
	}
	if (hud->str1228.len > 0) {
		String_assign(&text, &hud->str1228);
		goto drawText;
	}

	/* hint for the facing entity */
	if (cv->state == 2 || facing == NULL)
		goto done;
	eType = facing->def->eType;
	if (eType == 0)
		goto done;
	if (eType == 0xc && (facing->info & 0x80000) == 0)
		goto done;

	String_assign(&text, &hud->msgBuffer);
	String_setLength(&text, 0);
	String_appendString(&text, &facing->name);
	TextUtil_dehyphenate(app->textUtil, &text);

	if (eType == 3) {
		/* item: pick-up icon when adjacent */
		if (Entity_distSqFrom(facing, cv->viewX, cv->viewY) > ModData_s32(&g_mod, MOD_TILE_DIST_SQ))
			goto drawText;
		action = 1;
		goto icon;
	}
	if (eType == 2) {
		/* monster / NPC */
		if (facing->def->eSubType == 2 && (facing->info & 0x10000000u) == 0) {
			String_setLength(&text, 0);
			Text_getString(cv, 0, 0xa7, &text);
			TextUtil_dehyphenate(app->textUtil, &text);
			goto drawText;
		}
		String_setLength(&text, 0);
		if ((facing->monster->flags & 0x200) != 0) {
			Canvas_resetEvents(cv);
			Canvas_addTextArgStr(cv, &facing->name);
			Text_getString(cv, 0, 0x26, &text);
		} else {
			String *name = &facing->name;
			if ((facing->info & 0x1000000u) == 0 && !String_notEquals(name, &facing->def->name))
				name = &facing->def->longName; /* unnamed: use the long type name */
			String_appendString(&text, name);
			action = 0;
		}
		TextUtil_dehyphenate(app->textUtil, &text);
		goto maybeIcon;
	}
	if (eType == 5 || eType == 0xe) {
		/* door / lever: use icon when adjacent */
		if (Entity_distSqFrom(facing, cv->viewX, cv->viewY) <= ModData_s32(&g_mod, MOD_TILE_DIST_SQ))
			action = 2;
		if (facing->def->eType == 5 && facing->def->parm == 1)
			goto notify;
		goto maybeIcon;
	}
	if (eType == 0xc)
		goto notify;
	if (eType == 7) {
		uint8_t sub = facing->def->eSubType;
		uint32_t sprInfo;
		int frame, tile;

		if (sub == 2)
			goto notify;
		sprInfo = app->render->sprites[(facing->info & 0xffff) - 1].info;
		frame = (sprInfo >> 8) & 0xf;
		tile = sprInfo & 0xff;
		if (sub == 1) {
			if (frame != 0) {
				String_setLength(&text, 0);
				Text_getString(cv, 0, 0xa8, &text);
				TextUtil_dehyphenate(app->textUtil, &text);
				goto drawText;
			}
			action = 2;
			if (tile != 0x90 && tile != 0x8e && (facing->info & 0x80000) == 0) {
				String_setLength(&text, 0);
				Text_getString(cv, 0, 0xa7, &text);
				TextUtil_dehyphenate(app->textUtil, &text);
			}
			goto icon;
		}
		if (sub == 3 || sub == 6 || sub == 5 || tile == 0x77 || tile == 0xb4) {
			action = 2;
			goto icon;
		}
		goto drawText;
	}
	if (eType == 10) {
		action = 0;
		goto icon;
	}
	goto drawText;

notify:
	if (text.len > 0) {
		Hud_drawNotification(hud, &text);
		goto done;
	}
maybeIcon:
	if (action == -1)
		goto drawText;
icon:
	if (app->player->isFamiliar == 0) {
		Canvas_drawRegion(cv, &hud->imgActions, 0, action << 4, 0x10, 0x10, 0, textX, 3, 5, 0, 0);
		textX += 0x10;
	}
drawText:
	n = text.len;
	if (n * 9 + textX + 9 > cv->viewRect[2])
		n = (cv->viewRect[2] - textX) / 7 - 1;
	Canvas_drawStringRange(cv, &text, textX, 5, 0, scroll, n, 0);
done:
	if (cv->state != 0xc)
		Hud_drawTargetHealth(hud);
	String_dtor(&text, 0);
}

/* 0x0001280c
 * Speech bubble with the timed lines (Hud.lines), typed out at 50 ms per
 * character. Who speaks decides where the bubble sits (layout box 6..9). The
 * two extra arguments are unused. */
void Hud_drawSpeechBubble(Hud *hud, int unused, int force)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	Player *player = app->player;
	Combat *combat;
	int16_t *wandBox = &hud->layout[0x18];
	int16_t *playerBox = &hud->layout[0x1c];
	int16_t *noWandBox = &hud->layout[0x20];
	int16_t *npcBox = &hud->layout[0x24];
	int16_t *box;
	Image *tail;
	int x, y, tailX, tailY, tailSy, boxTop, i;

	if (cv->state == 7 || hud->numLines == 0)
		return;
	if (app->time > hud->lineTimes[0] + 2000) {
		Hud_shiftSubtitles(hud);
		if (hud->numLines == 0)
			return;
	}
	Hud_calcMaxLineLen(hud);

	combat = app->combat;
	if (player->ce.weapon != 0 || combat->raiseFlag != 0) {
		if (hud->bubbleMode != 0)
			box = npcBox;
		else if ((player->weapons & 1) == 0)
			box = noWandBox;
		else
			box = wandBox;
	} else {
		if (cv->state == 4) {
			/* hold the bubble while the player's attack animation plays */
			if (combat->attacker == NULL && combat->state != 2 && combat->toggle != 0
			    && app->time < combat->shotEnd)
				return;
		} else if (cv->state == 0xc) {
			if ((player->weapons & 1) == 0)
				return;
			box = wandBox;
			goto draw;
		}
		hud->layout[0x1c] = (int16_t)(cv->viewRect[2] / 2 - (hud->maxLineLen * 7 + 9));
		box = playerBox;
	}

draw:
	y = cv->viewRect[3] + box[1] - 0xc;
	x = cv->screenCenterX + box[0] + box[2] / 2;
	Graphics_setColor(cv->graphics, box == npcBox ? (int)0xff800000 : (int)0xff6633cc);
	boxTop = (y - (hud->numLines * 12 - 12)) - 2;
	Graphics_fillRect(cv->graphics, x, boxTop, hud->maxLineLen * 7 + 4, hud->numLines * 12 + 2);
	Graphics_setColor(cv->graphics, -1);
	Graphics_drawRect(cv->graphics, x, boxTop, hud->maxLineLen * 7 + 4, hud->numLines * 12 + 2);

	/* bubble tail */
	tail = &hud->imgWandChat;
	tailY = y + 0xc;
	tailSy = 0;
	if (box == playerBox) {
		tailX = x + hud->maxLineLen * 7 - 0xc;
	} else if (box == wandBox) {
		tailX = x + 8;
	} else if (box == npcBox) {
		tail = &cv->imgNpcChat;
		tailX = x + 8;
		tailSy = 9;
	} else {
		tailX = x + 0x18;
		tailY = y - hud->numLines * 12 + 2;
		tailSy = 9;
	}
	Canvas_drawRegion(cv, tail, 0, tailSy, 0xf, 9, 0, tailX, tailY, 5, 0, 0);

	x += 1;
	for (i = 0; i < hud->numLines; i++) {
		Canvas_drawStringRange(cv, &hud->lines[i], x, y, 0, 0, (app->time - hud->lineTimes[i]) / 50, 0);
		y -= 12;
	}
}

/* 0x0000dbac
 * Bottom bar: HP gauge and number, armor value and icon, ammo count and
 * weapon icon, the wand (shaking while it talks) and the speech bubble. */
void Hud_drawStatusBar(Hud *hud)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	Player *player = app->player;
	CombatEntity *ce = &player->ce;
	const int16_t *L = hud->layout;
	int cx = cv->screenCenterX;
	int bottom = cv->viewRect[1] + cv->viewRect[3];
	int hp, maxHp, armor, maxArmor, shown, elapsed, ratio, fill, xx, flags;
	int hpFlash = 0, armorFlash = 0;

	hp = CombatEntity_getHealth(ce);
	maxHp = CombatEntity_getMaxHealth(ce);
	armor = CombatEntity_getArmorPoints(ce);
	maxArmor = CombatEntity_getMaxArmorPoints(ce);

	/* animate the HP number/gauge toward the real value over 250 ms */
	if (hud->lastHealth > maxHp) {
		hud->hpAnim1 = maxHp;
		hud->lastHealth = maxHp;
	}
	if (hud->hpAnim1 != hp) {
		hud->hpAnim1 = hp;
		hud->hpAnim2 = app->time;
	}
	elapsed = app->time - hud->hpAnim2;
	if (elapsed > 250) {
		hud->lastHealth = hp;
		shown = hp;
	} else {
		shown = hud->lastHealth - elapsed * (hud->lastHealth - hud->hpAnim1) / 250;
	}
	/* flash while changing, and blink when at a quarter or less */
	if (hud->hpAnim1 != shown || (shown <= maxHp >> 2 && ((app->time / 491) & 1) != 0))
		hpFlash = 1;
	if (armor <= maxArmor >> 2 && (int32_t)((uint32_t)app->time << 22) < 0)
		armorFlash = 1;

	/* gauge: 8.8 fill ratio scaled to the gauge height, rounded */
	ratio = (shown << 16) / (maxHp << 8);
	fill = (0x80 + ((ratio * (L[7] << 8)) >> 8)) >> 8;
	Graphics_setColor(cv->graphics, (int)0xff333333);
	Graphics_fillRect(cv->graphics, cx + L[4], bottom + L[5], L[6], L[7] - fill);
	Graphics_setColor(cv->graphics, hpFlash ? (int)0xffff882f : (int)0xffcc0000);
	Graphics_fillRect(cv->graphics, cx + L[4], bottom + L[5] + L[7] - fill, L[6], fill);

	Canvas_drawImageSimple(cv, &hud->imgBottomCenter, cx, cv->viewRect[3], 0x12);
	if (cv->viewRect[2] > 0xc6) {
		for (xx = cx - hud->imgBottomCenter.width / 2; xx >= 0; xx -= hud->imgBottomLeft.width)
			Canvas_drawImageSimple(cv, &hud->imgBottomLeft, xx, cv->viewRect[3], 10);
		for (xx = cx + hud->imgBottomCenter.width / 2; xx < cv->viewRect[0] + cv->viewRect[2];
		     xx += hud->imgBottomRight.width)
			Canvas_drawImageSimple(cv, &hud->imgBottomRight, xx, cv->viewRect[3], 6);
	}

	if (player->weapons & 1) {
		/* the wand jitters while its speech line is new, or while a
		 * style-1 dialog is up (400 ms of every second) */
		int dx = 0, dy = 0, shake;

		if (hud->numLines > 0 && app->time < hud->lineTimes[hud->numLines - 1] + 500)
			shake = 1;
		else
			shake = cv->state == 7 && cv->dialogStyle == 1 && app->time % 1000 < 400;
		if (shake) {
			dx = 2 - (int)(((uint32_t)Random_nextInt(&app->random) & 0x7fff) % 3);
			dy = 1 - (int)(((uint32_t)Random_nextInt(&app->random) & 0x7fff) % 3);
		}
		Canvas_drawImage(cv, &hud->imgWand, cx + L[0x18] + L[0x1a] / 2 + dx,
		                 bottom + L[0x19] + L[0x1b] / 2 + dy, ANCHOR_HCENTER | ANCHOR_VCENTER, 0);
	}

	/* HP number (font style 9 = colour cycle + large while flashing) */
	String_setLength(&hud->str1248, 0);
	hud_appendInt(&hud->str1248, shown);
	xx = cx + L[0] + L[2] / 2;
	Canvas_drawStringRange(cv, &hud->str1248, hpFlash ? xx + 1 : xx - 1, bottom + L[1] + L[3] / 2,
	                       ANCHOR_HCENTER | ANCHOR_VCENTER, 0, -1, hpFlash ? 9 : 0);

	/* armor */
	String_setLength(&hud->str1258, 0);
	hud_appendInt(&hud->str1258, ce->armor);
	flags = 0;
	if (armorFlash) {
		cv->textColor = 0xc800;
		flags = 0x10;
	}
	Canvas_drawStringRange(cv, &hud->str1258, cx + L[8] + L[10] / 2, bottom + L[9] + L[11] / 2,
	                       ANCHOR_HCENTER | ANCHOR_VCENTER, 0, -1, flags);
	Canvas_drawRegion(cv, &hud->imgBottombarIcon, 0, ce->armorType * 0x14, 0xe, 0x14, 0,
	                  cx + L[0xc] + L[0xe] / 2, bottom + L[0xd] + L[0xf] / 2,
	                  ANCHOR_HCENTER | ANCHOR_VCENTER, 0, 0);

	if (player->weapons != 0) {
		int weapon = ce->weapon;
		WeaponInfo *w = &app->combat->weapons[weapon];
		int icon;

		String_setLength(&hud->str1268, 0);
		if (w->ammoType != 0) {
			int n = player->ammo[w->ammoType];
			if (player->statusEffects[0].turns > 0)
				n += player->statusEffects[0].turns;
			hud_appendInt(&hud->str1268, n);
		} else {
			Text_getString(cv, 0, 0xa9, &hud->str1268);
		}
		Canvas_drawStringRange(cv, &hud->str1268, cx + L[0x10] + L[0x12] / 2, bottom + L[0x11] + L[0x13] / 2,
		                       ANCHOR_HCENTER | ANCHOR_VCENTER, 0, 2, 0);

		/* weapon/ammo icon row in Bottombar_Icon */
		if (w->ammoType == 1)
			icon = 4;
		else if (w->ammoType == 2)
			icon = 5;
		else if (w->ammoType == 3 || w->ammoType == 4 || w->ammoType == 5)
			icon = 6;
		else if (weapon == 4)
			icon = 9;
		else if (weapon == 1)
			icon = 3;
		else if ((arm_bit(weapon) & 0xc) != 0)
			icon = 7;
		else if (w->ammoType == 0)
			icon = 8;
		else
			icon = 6;
		Canvas_drawRegion(cv, &hud->imgBottombarIcon, 0, icon * 0x14, 0xe, 0x14, 0,
		                  cx + L[0x14] + L[0x16] / 2, bottom + L[0x15] + L[0x17] / 2,
		                  ANCHOR_HCENTER | ANCHOR_VCENTER, 0, 0);
	}

	Hud_drawSpeechBubble(hud, 0, 0);
}

/* 0x0000cb04 */
void Hud_draw(Hud *hud)
{
	Hud_drawTopBar(hud);
	Hud_drawOverlays(hud);
	Hud_drawStatusBar(hud);
}

/* 0x0000e71c
 * Cinematic captions: caption2 at the top in the large font, caption1 above
 * the bottom of the view. */
void Hud_drawCinematicText(Hud *hud)
{
	Canvas *cv = g_app->canvas;

	if (hud->caption2.len > 0)
		Canvas_drawStringRange(cv, &hud->caption2, cv->screenCenterX, 0xf, ANCHOR_HCENTER, 0, -1, 1);
	if (hud->caption1.len > 0)
		Canvas_drawStringRange(cv, &hud->caption1, cv->screenCenterX, cv->viewRect[1] + cv->viewRect[3] - 0x20,
		                       ANCHOR_HCENTER, 0, -1, 0);
}

/* 0x0000f3d4
 * Status-effect icons and the arrow showing where damage came from. */
void Hud_drawOverlays(Hud *hud)
{
	Canvas *cv = g_app->canvas;

	if (cv->state != 0xc)
		Player_drawBuffs(g_app->player);

	if (g_app->time < hud->damageDirUntil && hud->damageFlash >= 0) {
		int x, y, sy = 0;

		if (hud->damageDir == 0)
			return;
		if (hud->damageDir == 1) {        /* from the right */
			sy = 0x24;
			x = cv->viewRect[2] - 0x14;
			y = cv->viewRect[3] >> 1;
		} else if (hud->damageDir == 3) { /* from the left */
			sy = 0x12;
			x = 0x14;
			y = cv->viewRect[3] >> 1;
		} else {                          /* from behind */
			x = cv->viewRect[2] >> 1;
			y = cv->viewRect[3] - 0x28;
		}
		Canvas_drawRegion(cv, &hud->imgAttackArrows, 0, sy, 0x12, 0x12, 0, x, y,
		                  ANCHOR_HCENTER | ANCHOR_VCENTER, 0, 0);
	} else if (hud->damageDirUntil != 0) {
		hud->damageDirUntil = 0;
	}
}

/* Labels of the controls overlay: text id into the Canvas scratch string. */
static String *hud_helpText(int id, int dehyphenate)
{
	Canvas *cv = g_app->canvas;

	String_setLength(&cv->scratch, 0);
	Text_getString(cv, 0, id, &cv->scratch);
	if (dehyphenate)
		TextUtil_dehyphenate(g_app->textUtil, &cv->scratch);
	return &cv->scratch;
}

/* 0x0000f4fc
 * Tutorial overlay naming the HUD elements (strings 0:0xaa..0xb3) with
 * pointer anchors from UI_Chatanchors. Layout is fixed for the 176-wide
 * screen like the original. */
void Hud_drawControlsHelp(Hud *hud)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	Graphics *g = cv->graphics;
	Image *anchors = &hud->imgChatAnchors;
	const int labelBg = (int)0xff001d59;
	int bottom = cv->viewRect[1] + cv->viewRect[3];
	Entity *facing;

	/* centre box: title and "press a key" line */
	Graphics_setColor(g, (int)0xff000000);
	Graphics_fillRect(g, cv->screenCenterX - 0x28, cv->screenCenterY - 0xe, 0x50, 0x1a);
	Graphics_setColor(g, -1);
	Graphics_drawRect(g, cv->screenCenterX - 0x28, cv->screenCenterY - 0xe, 0x50, 0x1a);
	Canvas_drawStringRange(cv, hud_helpText(0xaa, 0), cv->screenCenterX, cv->screenCenterY,
	                       ANCHOR_HCENTER | ANCHOR_BOTTOM, 0, -1, 0);
	Canvas_drawStringRange(cv, hud_helpText(0xab, 1), cv->screenCenterX, cv->screenCenterY,
	                       ANCHOR_HCENTER, 0, -1, 0);

	/* the anchor arrows are recoloured black -> label colour (blend mode 10) */
	Render_setBlendColors(app->render, (int)0xff000000, labelBg);

	Canvas_drawLabelBox(cv, hud_helpText(0xac, 0), 0, bottom - 0x46, labelBg, -1);
	Canvas_drawRegion(cv, anchors, 9, 9, 9, 9, 0, 4, bottom - 0x36, 0, 10, 0);

	Canvas_drawLabelBox(cv, hud_helpText(0xad, 1), 0x30, bottom - 0x34, labelBg, -1);
	Canvas_drawRegion(cv, anchors, 9, 9, 9, 9, 0, 0x45, bottom - 0x24, 0, 10, 0);

	Canvas_drawLabelBox(cv, hud_helpText(0xae, 1), 0x5c, bottom - 0x34, labelBg, -1);
	Canvas_drawRegion(cv, anchors, 9, 9, 9, 9, 0, 0x70, bottom - 0x24, 0, 10, 0);

	Canvas_drawLabelBox(cv, hud_helpText(0xaf, 0), 100, bottom - 0x4c, labelBg, -1);
	Canvas_drawRegion(cv, anchors, 9, 0, 9, 9, 0, 0x94, bottom - 0x48, 0, 10, 0);
	Graphics_drawLine(g, 0x9d, bottom - 0x44, 0x9d, bottom - 0x1c);

	if (app->player->equippedRing != 0) {
		Canvas_drawLabelBox(cv, hud_helpText(0xb0, 0), 0x10, 0x37, labelBg, -1);
		Canvas_drawRegion(cv, anchors, 0, 0, 9, 9, 0, 0x22, 0x38, ANCHOR_BOTTOM, 10, 0);
		Graphics_drawLine(g, 0x26, 0x1c, 0x26, 0x2e);
		Graphics_drawLine(g, 0x14, 0x1c, 0x26, 0x1c);
	}

	facing = app->player->facingEntity;
	if (facing != NULL && facing->def->eType != 0
	    && (facing->def->eType != 0xc || (facing->info & 0x80000) != 0)) {
		Canvas_drawLabelBox(cv, hud_helpText(0xb1, 1), 0x3c, 0x1c, labelBg, -1);
		Canvas_drawRegion(cv, anchors, 0, 0, 9, 9, 0, 0x49, 0x1d, ANCHOR_BOTTOM, 10, 0);
		Graphics_drawLine(g, 0x4d, 0x14, 0x4d, 0xf);
	}

	if (app->player->numStatusEffects > 0) {
		Graphics_setColor(g, labelBg);
		Graphics_fillRect(g, 0x74, 0, 0x36, 0x1c);
		Graphics_setColor(g, -1);
		Graphics_drawRect(g, 0x74, 0, 0x36, 0x1c);
		Canvas_drawStringRange(cv, hud_helpText(0xb2, 1), 0x76, 2, 0, 0, -1, 0);
		Canvas_drawStringRange(cv, hud_helpText(0xb3, 1), 0x76, 0xf, 0, 0, -1, 0);
		Canvas_drawRegion(cv, anchors, 9, 9, 9, 9, 0, 0x8c, 0x1c, 0, 10, 0);
	}
}
