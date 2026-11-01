/*
 * Player: the active-modifier box drawn at the right edge of the 3D view
 * (one row per modifier: value, icon, turns left).
 */
#include "game.h"

/* "more rows" marker drawn under the box (a font glyph) */
#define MOD_STR_MORE_MARK 0x46498u /* "\x7f" */

/* 0x0000e330 */
void Player_drawBuffs(Player *player)
{
	Applet *app = g_app;
	Canvas *cv;
	int count = player->numBuffs;
	int shown, rows, boxH, more = 0;
	int right, x, y, top, i;

	if (count == 0)
		return;

	cv = app->canvas;
	right = cv->viewRect[0] + cv->viewRect[2];
	x = right - 0x29;
	top = app->hud->imgTopCenter.height;
	y = top + 0x18;

	/* modifier 0 (familiar energy) is only listed while possessing */
	if (player->statusEffects[0].turns > 0 && player->isFamiliar == 0) {
		count--;
		if (count == 0)
			return;
	}

	rows = count > 6 ? 6 : count;
	boxH = rows * 13 + 2;
	if (count > 6) {
		more = 1;
		boxH += 6;
	}

	Graphics_setColor(cv->graphics, 0);
	Graphics_fillRect(app->canvas->graphics, x, top + 0x16, 0x28, boxH);
	Graphics_setColor(app->canvas->graphics, (int)0xffaaaaaau);
	Graphics_drawRect(app->canvas->graphics, x, top + 0x16, 0x28, boxH);

	shown = 0;
	for (i = 0; i < 12 && shown < 6; i++) {
		Buff *b = &player->buffs[i];

		if (b->turns == 0)
			continue;
		if (i == 0 && player->isFamiliar == 0)
			continue;
		shown++;
		Player_drawStatusEffect(player, i, b->amount, b->turns, x, y);
		y += 13;
	}

	if (more) {
		String mark;

		String_ctorCStr(&mark, (char *)ModData_str(&g_mod, MOD_STR_MORE_MARK));
		Canvas_drawStringFlags(app->canvas, &mark, x + 0x14, y - 7, 0x10, 0);
		String_dtor(&mark, 0);
	}
}

/* 0x00011028  one row: "+amount" (not for on/off modifiers), icon, turns */
void Player_drawStatusEffect(Player *player, int effect, int amount, int turns, int x, int y)
{
	Applet *app = g_app;
	String text;
	int iconFlags = 0;
	int age, fade;

	String_ctor(&text);
	if (effect != 3 && effect != 2 && effect != 0 && effect != 9) {
		if (amount >= 0)
			String_appendCStr_76f0(&text, "+");
		String_appendInt(&text, amount);
	}

	/* blink the icon during the last 10 turns */
	if (turns < 10)
		iconFlags = ((app->time / 250) & 1) == 0 ? 3 : 0;

	Canvas_drawStringRange(app->canvas, &text, x + 0x16, y + 1, 9, 0, -1, 0);
	Canvas_drawSmallIcon(app->canvas, effect, x + 0x25, y, 9, iconFlags, 0);

	/* the turn counter shows right after a turn, then fades out (100 ms steps) */
	age = (app->time - player->turnTime) / 100;
	if (age < 3)
		fade = 0;
	else if (age < 4)
		fade = 0x20;
	else if (age < 5)
		fade = 0x40;
	else if (age < 6)
		fade = 0x80;
	else
		fade = 0xff;

	if (fade != 0xff) {
		String_setLength(&text, 0);
		String_appendInt(&text, turns);
		Canvas_drawStringRange(app->canvas, &text, x + 0x1f, y + 1, 0x11, 0, -1, fade);
	}
	String_dtor(&text, 0);
}
