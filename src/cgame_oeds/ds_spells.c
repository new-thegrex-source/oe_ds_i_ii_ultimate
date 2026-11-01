/*
 * The wand spells of Orcs & Elves DS (its touch screen's spell circle, DS 0x02068070..0x02069f04),
 * on the PC: Q opens the circle over the 3D view, the mouse (or the arrows and SELECT) picks.
 *
 * The DS rules (oeds-re/README.md, "Wand spells"):
 *   - four spells, learned through the scripts (Player_give type 11, Player.dsSpells): 0 force,
 *     1 fire, 2 lightning, 3 stun; each costs wand energy (Player.ammo[1], the crystals' charges);
 *   - their icons sit up, down, left and right on the circle; a spell is picked by its icon (learned
 *     and affordable), then its sequence of four icons (fixed per spell, ending with its own) is put
 *     in: the next icon blinks, a wrong one ends the cast ("invalid spell sequence");
 *   - after the fourth, the spell's name ("LIGHTNING!") shows for 1.2 s, then the wand fires with
 *     the spell's weapon in place of its own (DS 0x02069d6c: Combat weapon 0 := the spell's, then
 *     the attack ahead);
 *   - effects (DS Combat_playerAttackFrame / Combat_finishPlayerAttack): fire splashes the tiles
 *     around the target (sides half, corners a quarter), force knocks what stands around it away
 *     (3 tiles from the sides, 2 from the corners), stun stops the monster for 6 turns.
 * Not done here: the DS's own art (UI_Shapes), its sparkle trail and spell sprites, the fire's
 * burning (the port has no burning monsters yet).
 */
#include "oe_vm.h"
#include "game.h"
#include "ds_local.h"

/* Canvas.state */
#define CS_PLAYING	3
#define CS_COMBAT	4

/* DS 0x0206bd00: the icons (index = spell): its name (CodeStrings) and sequence */
static const struct {
	int			name;			/* CodeStrings: "FORCE!" ... */
	int			seq[4];			/* the icons to put in */
	unsigned	color;
} spells[4] = {
	{ 210, { 1, 3, 2, 0 }, 0xffb070ffu },		/* force: up */
	{ 212, { 0, 3, 2, 1 }, 0xffff7020u },		/* fire: down */
	{ 209, { 3, 1, 0, 2 }, 0xffffff40u },		/* lightning: left */
	{ 211, { 2, 0, 1, 3 }, 0xff60e0ffu },		/* stun: right */
};

/* DS 0x020696a4: the spells' weapons (damage, range 0..2, energy cost); the projectile is the wand's */
static const struct {
	int dmg, cost;
} spellWeapons[4] = {
	{ 80, 80 }, { 100, 80 }, { 80, 35 }, { 50, 80 }
};

enum { PHASE_CLOSED, PHASE_PICK, PHASE_SEQUENCE, PHASE_CAST };

static struct {
	int		phase;
	int		spell;				/* picked */
	int		hover;				/* the icon under the pointer / chosen with the arrows, -1 none */
	int		step;				/* sequence: icons put in */
	int		path[4];			/* the icons put in */
	int		blinkTime;			/* the next icon blinks every 300 ms */
	int		castTime;			/* PHASE_CAST: when the name appeared */
	int		buttons;			/* the pointer's buttons last tick */
	int		pointerOn;
	int		px, py;				/* the pointer, game screen pixels */
	int		lastPx, lastPy;
	int		cx, cy, r, ir;		/* the circle on the screen, the icons' radius */
	/* the cast: weapon 0 is the spell's until the attack is over */
	int		casting;			/* the spell + 1 */
	Weapon	wand;
} sp;

static int learned(Player *pl, int s)
{
	return (pl->dsSpells & (1 << s)) != 0;
}

static int affordable(Player *pl, int s)
{
	return pl->ammo[1] >= spellWeapons[s].cost;
}

static void iconPos(int icon, int *x, int *y)
{
	/* DS 0x02068d18: up, down, left, right */
	*x = sp.cx + (icon == 2 ? -sp.r : icon == 3 ? sp.r : 0);
	*y = sp.cy + (icon == 0 ? -sp.r : icon == 1 ? sp.r : 0);
}

static int iconAt(int x, int y)
{
	int i, ix, iy;
	for (i = 0; i < 4; i++) {
		iconPos(i, &ix, &iy);
		if ((x - ix) * (x - ix) + (y - iy) * (y - iy) <= sp.ir * sp.ir * 2)
			return i;
	}
	return -1;
}

static void layout(Canvas *cv)
{
	int h = cv->softKeyBarY > 0 ? cv->softKeyBarY : cv->screenH;
	int size = cv->screenW < h ? cv->screenW : h;

	sp.cx = cv->viewCenterX;
	sp.cy = cv->viewCenterY;
	sp.r = size * 3 / 10;
	sp.ir = size / 14 > 6 ? size / 14 : 6;
}

/* developer: "castspell N" casts spell N as if its sequence was just put in */
void DsSpells_debugCast(Canvas *cv, int s)
{
	if (s < 0 || s > 3 || cv->state != CS_PLAYING)
		return;
	layout(cv);
	sp.spell = s;
	sp.step = 4;
	memcpy(sp.path, spells[s].seq, sizeof sp.path);
	sp.phase = PHASE_CAST;
	sp.castTime = cv->app->tickTime;
}

int DsSpells_open(void)
{
	return sp.phase != PHASE_CLOSED;
}

static void closeCircle(Canvas *cv)
{
	sp.phase = PHASE_CLOSED;
	/* the view without the circle */
	Canvas_renderView(cv, cv->viewX, cv->viewY, cv->viewAngle);
	Hud_draw(cv->hud);
	Canvas_drawPlayingSoftKeys(cv);
}

/* Q in play: the circle (the wand in hand), if a spell is known */
void DsSpells_toggle(Canvas *cv)
{
	Player *pl = cv->player;
	int i, any = 0, can = 0;

	if (sp.phase != PHASE_CLOSED) {
		closeCircle(cv);
		return;
	}
	if (cv->state != CS_PLAYING || !(pl->weapons & 1))
		return;
	for (i = 0; i < 4; i++) {
		any |= learned(pl, i);
		can |= learned(pl, i) && affordable(pl, i);
	}
	if (!any)
		return;
	if (!can) {
		Hud_addMessage(cv->hud, (char *)DsStrings_get("CodeStrings", 27), 0);	/* "Not enough energy..." */
		return;
	}
	Player_selectWeapon(pl, 0);
	memset(&sp.path, 0, sizeof sp.path);
	sp.phase = PHASE_PICK;
	sp.hover = -1;
	sp.step = 0;
	sp.buttons = ~0;			/* (a click held from before is not a pick) */
	layout(cv);
	if (!pl->dsSpellHelp) {
		/* the first time: how it works (DS CodeStrings 28, without the touch screen) */
		pl->dsSpellHelp = 1;
		Canvas_showDialog(cv, NULL, (char *)DsStrings_get("CodeStrings", 28), 1, 0, 0);
	}
}

/* an icon put in (DS 0x02069180) */
static void putIcon(Canvas *cv, int icon)
{
	Player *pl = cv->player;

	if (icon < 0)
		return;
	if (sp.phase == PHASE_PICK) {
		if (!learned(pl, icon) || !affordable(pl, icon))
			return;
		sp.spell = icon;
		sp.phase = PHASE_SEQUENCE;
		sp.step = 0;
		sp.blinkTime = cv->app->tickTime;
		return;
	}
	if (sp.phase != PHASE_SEQUENCE)
		return;
	if (icon != spells[sp.spell].seq[sp.step]) {
		Hud_addMessage(cv->hud, (char *)DsStrings_get("CodeStrings", 26), 0);	/* "invalid spell sequence" */
		sp.phase = PHASE_PICK;
		sp.step = 0;
		return;
	}
	sp.path[sp.step++] = icon;
	sp.blinkTime = cv->app->tickTime;
	if (sp.step == 4) {
		sp.phase = PHASE_CAST;
		sp.castTime = cv->app->tickTime;
	}
}

/* keys while the circle is up: arrows choose an icon, SELECT puts it in, CLR / soft keys / Q close */
int DsSpells_key(Canvas *cv, int avk)
{
	switch (avk) {
	case AVK_UP: case AVK_2:		sp.hover = 0; break;
	case AVK_DOWN: case AVK_8:		sp.hover = 1; break;
	case AVK_LEFT: case AVK_4:		sp.hover = 2; break;
	case AVK_RIGHT: case AVK_6:		sp.hover = 3; break;
	case AVK_SELECT: case AVK_5:	putIcon(cv, sp.hover); break;
	case AVK_SOFT3:
	case AVK_SOFT1:
	case AVK_CLR:
		if (sp.phase != PHASE_CAST)
			closeCircle(cv);
		break;
	}
	return 1;
}

static void fireSpell(Canvas *cv)
{
	Combat *combat = cv->combat;
	Weapon *w = &combat->weapons[0];
	int s = sp.spell;

	/* DS 0x02069d6c: the wand fires the spell's weapon (in hand again: a dialog may have put it away) */
	Player_selectWeapon(cv->player, 0);
	sp.wand = *w;
	sp.casting = s + 1;
	w->dmgMin = w->dmgMax = spellWeapons[s].dmg;
	w->rangeMin = 0;
	w->rangeMax = 2;
	w->accuracy = 0x100;
	w->ammoType = 1;
	w->ammoUsage = spellWeapons[s].cost;
	w->numShots = 1;
	Canvas_attackAhead(cv);
	if (cv->state != CS_COMBAT)
		DsSpells_attackDone(combat);	/* (it did not fire) */
}

/* the end of the player's attack (Combat_finishPlayerAttack): a spell's effect, the wand back */
void DsSpells_attackDone(Combat *combat)
{
	int s = sp.casting - 1, tx, ty, i;
	Entity *target = combat->target;

	if (!sp.casting)
		return;
	combat->weapons[0] = sp.wand;
	sp.casting = 0;
	tx = combat->targetX >> 6;
	ty = combat->targetY >> 6;
	if (s == 1) {
		/* fire: the tiles around (DS Combat_playerAttackFrame, effect 0x1d) */
		static const int d[8][3] = { { -1, 0, 0x8000 }, { 1, 0, 0x8000 }, { 0, -1, 0x8000 }, { 0, 1, 0x8000 },
			{ -1, 1, 0x4000 }, { 1, 1, 0x4000 }, { 1, -1, 0x4000 }, { -1, -1, 0x4000 } };
		/* (never the player's own tile: the DS's splash spares the player) */
		for (i = 0; i < 8; i++) {
			if (tx + d[i][0] != (g_app->canvas->viewX >> 6) || ty + d[i][1] != (g_app->canvas->viewY >> 6))
				Combat_damageTile(combat, tx + d[i][0], ty + d[i][1], d[i][2]);
		}
	} else if (s == 0) {
		/* force: what stands around is pushed away (effect 0x1c, DS 0x0201f654) */
		static const int d[8][3] = { { -1, 0, 3 }, { 1, 0, 3 }, { 0, -1, 3 }, { 0, 1, 3 },
			{ -1, 1, 2 }, { 1, 1, 2 }, { 1, -1, 2 }, { -1, -1, 2 } };
		for (i = 0; i < 8; i++) {
			Entity *e = Game_findEntityAt(combat->game, (tx + d[i][0]) * 64 + 32, (ty + d[i][1]) * 64 + 32, 4);
			if (e && e->monster)
				Entity_knockback(e, tx * 64 + 32, ty * 64 + 32, d[i][2]);
		}
	} else if (s == 3 && target && target->monster && combat->targetType == 2) {
		/* stun: 6 turns without acting (effect 0x1f: monster flag 0x2000, +6) */
		MonsterGoal *goal = &target->monster->goal;
		if (goal->type != 9) {
			MonsterGoal_reset(goal);
			goal->type = 9;
		}
		goal->param = (int16_t)(goal->param + 6);
	}
}

static void drawIcon(Canvas *cv, int i, int lit, int dim)
{
	Applet *app = cv->app;
	int x, y, r = sp.ir;
	unsigned c = spells[i].color;

	iconPos(i, &x, &y);
	if (dim)
		c = 0xff404040u;
	Gfx_setColor(app, (int)(lit ? 0xffffffffu : 0xff101010u));
	Gfx_fillRect(app, x - r - 2, y - r - 2, 2 * r + 5, 2 * r + 5);
	Gfx_setColor(app, (int)c);
	Gfx_fillRect(app, x - r, y - r, 2 * r + 1, 2 * r + 1);
	if (i == sp.hover) {
		Gfx_setColor(app, (int)0xffffffffu);
		Gfx_drawRect(app, x - r - 4, y - r - 4, 2 * r + 8, 2 * r + 8);
	}
}

static void drawCircle(Canvas *cv)
{
	Applet *app = cv->app;
	Player *pl = cv->player;
	int i, k, now = app->tickTime;
	int blinkOn = ((now - sp.blinkTime) / 300) % 2 == 0;

	/* the ring */
	Gfx_setColor(app, (int)0xffd0b060u);
	for (k = 0; k < 64; k++) {
		int a0 = k * 16, a1 = (k + 1) * 16;
		int *sinT = cv->render->sinTable;
		Gfx_drawLine(app, sp.cx + ((sinT[(a0 + 0x100) & 0x3ff] * sp.r) >> 16), sp.cy - ((sinT[a0 & 0x3ff] * sp.r) >> 16),
		             sp.cx + ((sinT[(a1 + 0x100) & 0x3ff] * sp.r) >> 16), sp.cy - ((sinT[a1 & 0x3ff] * sp.r) >> 16));
	}
	/* the stroke put in so far */
	Gfx_setColor(app, (int)0xffffffffu);
	for (i = 1; i < sp.step; i++) {
		int x0, y0, x1, y1;
		iconPos(sp.path[i - 1], &x0, &y0);
		iconPos(sp.path[i], &x1, &y1);
		Gfx_drawLine(app, x0, y0, x1, y1);
	}
	for (i = 0; i < 4; i++) {
		/* (all four are always there: a sequence uses them all; for picking, the spells not learned or
		 * not affordable are dark) */
		int lit = 0, dim = sp.phase == PHASE_PICK && (!learned(pl, i) || !affordable(pl, i));
		if (sp.phase == PHASE_SEQUENCE) {
			int j;
			for (j = 0; j < sp.step; j++)
				lit |= sp.path[j] == i;
			if (spells[sp.spell].seq[sp.step] == i && blinkOn)
				lit = 1;		/* the next one blinks */
		}
		drawIcon(cv, i, lit, dim);
	}
	if (sp.phase == PHASE_CAST)
		Canvas_drawString(cv, (char *)DsStrings_get("CodeStrings", spells[sp.spell].name), sp.cx, sp.cy - 4, 0x11, 0);
}

/* every tick in play: the pointer, the cast's timing, the circle drawn over the view */
void DsSpells_frame(Canvas *cv)
{
	int xyb[3], scale = Brew_screenScale(), buttons;

	if (sp.phase == PHASE_CLOSED)
		return;
	if (cv->state != CS_PLAYING) {
		sp.phase = PHASE_CLOSED;
		return;
	}
	layout(cv);
	sp.pointerOn = trap_Pointer(xyb, 1);
	buttons = sp.pointerOn ? xyb[2] : 0;
	if (sp.pointerOn) {
		int icon;
		if (scale < 1)
			scale = 1;
		sp.px = xyb[0] / scale - cv->originX;
		sp.py = xyb[1] / scale - cv->originY;
		icon = iconAt(sp.px, sp.py);
		/* (the pointer chooses only when it moves or clicks: the arrows choose too) */
		if (icon >= 0 && (sp.px != sp.lastPx || sp.py != sp.lastPy || (buttons & 1)))
			sp.hover = icon;
		sp.lastPx = sp.px;
		sp.lastPy = sp.py;
		if ((buttons & 1) && !(sp.buttons & 1))
			putIcon(cv, icon);
		else if ((buttons & 2) && !(sp.buttons & 2) && sp.phase != PHASE_CAST) {
			sp.buttons = buttons;
			closeCircle(cv);
			return;
		}
	}
	sp.buttons = buttons;

	if (sp.phase == PHASE_CAST && cv->app->tickTime - sp.castTime > 1200) {
		/* DS 0x02069844: after the name, menu action 0x2a fires */
		sp.phase = PHASE_CLOSED;
		Canvas_renderView(cv, cv->viewX, cv->viewY, cv->viewAngle);
		Hud_draw(cv->hud);
		fireSpell(cv);
		return;
	}
	Canvas_renderView(cv, cv->viewX, cv->viewY, cv->viewAngle);
	Hud_draw(cv->hud);
	drawCircle(cv);
}
