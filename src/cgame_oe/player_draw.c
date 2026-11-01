/*
 * Player HUD drawing: the active-effects box at the right edge of the 3D view.
 */
#include "game.h"

#define MOD_STR_MORE_EFFECTS 0x2e6a8u
#define MOD_STR_SIGNED_INT   0x2e6a4u

#define BUFF_BOX_W    0x28
#define BUFF_ROW_H    13
#define BUFF_MAX_ROWS 6

static char *modStr(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

/* 0x0000b9bc  one row: "%+d" amount, 12x12 icon (blinking when < 10 turns
 * remain), remaining turns fading in over 300..600 ms after a turn */
void Player_drawStatusEffect(Player *player, int effect, int amount, int turns, int x, int y)
{
	Applet *app = player->app;
	char text[8];
	int iconMode = 0;
	int since, fade;

	if (effect == 4 || effect == 3 || effect == 0 || effect == 5)
		text[0] = '\0'; /* effects without a strength */
	else
		App_snprintf(text, sizeof text, modStr(MOD_STR_SIGNED_INT), amount);

	if (turns < 10 && ((app->tickTime / 250) & 1) == 0)
		iconMode = 6;

	Canvas_drawStringRange(player->canvas, text, x + 0x16, y + 1, 9, 0, -1, 0);
	Canvas_drawSmallIcon(player->canvas, effect, x + 0x25, y, 9, iconMode, 0);

	since = (app->tickTime - player->buffChangeTime) / 100;
	if (since < 3)
		fade = 0;
	else if (since < 4)
		fade = 0x20;
	else if (since < 5)
		fade = 0x40;
	else if (since < 6)
		fade = 0x80;
	else
		fade = 0xff;
	if (fade == 0xff)
		return;

	App_snprintf(text, sizeof text, "%d", turns);
	Canvas_drawStringRange(player->canvas, text, x + 0x1f, y + 1, 0x11, 0, -1, fade);
}

/* 0x00009f44 */
void Player_drawBuffs(Player *player)
{
	Applet *app = player->app;
	Canvas *cv = player->canvas;
	Hud *hud = app->hud;
	int count = player->numBuffs;
	int right, x, top, y, h, more, drawn, i;

	if (count == 0)
		return;

	right = cv->originX + cv->viewW;
	x = right - 0x29;
	top = hud->topBarH + hud->topCenter.height;
	y = top + 4;
	more = count > BUFF_MAX_ROWS;
	if (more)
		count = BUFF_MAX_ROWS;
	h = count * BUFF_ROW_H + (more ? 8 : 2);

	Gfx_setColor(app, 0);
	Gfx_fillRect(app, x, top + 2, BUFF_BOX_W, h);
	Gfx_setColor(app, (int)0xffaaaaaau);
	Gfx_drawRect(app, x, top + 2, BUFF_BOX_W, h);

	drawn = 0;
	for (i = 0; i < 10 && drawn < BUFF_MAX_ROWS; i++) {
		int turns = player->buffs[i].turns;

		if (turns == 0)
			continue;
		drawn++;
		Player_drawStatusEffect(player, i, (int8_t)player->buffs[i].amount, turns, x, y);
		y += BUFF_ROW_H;
	}
	if (more)
		Canvas_drawString(cv, modStr(MOD_STR_MORE_EFFECTS), right - 0x15, y - 7, 0x10, 0);
}
