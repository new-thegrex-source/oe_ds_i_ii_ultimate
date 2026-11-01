/*
 * Combat.cpp (part 3): the first-person weapon / hands overlay drawn over the
 * 3D view every frame (Canvas_renderView -> Combat_drawWeapon).
 *
 * Bitshape sprite ids: 1 + weapon for the weapon itself; 5 torch, 7 bow
 * string, 8 hammer arm, 0xb staff glow are extra layers.
 */
#include "game.h"

#define MOD_WEAPON_HAND_POS 0x4613cu /* 11 x {s16 idleX, idleY, swingX, swingY}: weapon screen offsets */
#define MOD_WEAPON_GLOW     0x46174u /* u8 [weapon*4 + frame]: blend mode of the wand glow layer */
#define MOD_FAMILIAR_BOB    0x45eacu /* 4 x s32: eye-height bob while walking as the familiar */

static int handPos(int weapon, int field) { return ModData_s16(&g_mod, MOD_WEAPON_HAND_POS + weapon * 8 + field * 2); }

static void drawSprite(int sprite, int frame, int x, int y, int mode, int blend, int flip)
{
	Render_draw2DSprite(g_app->render, sprite, frame, x, y, mode, blend, 0x100, flip);
}

/* weapon space (x 0..64 around the centre, y up from the bottom) to the
 * renderer's view (device pixels: screenCenterX is in game pixels) */
static int screenX(int scale, int wx) { return g_app->canvas->screenCenterX * Brew_screenScale() + ((scale * ((wx - 0x20) << 8) + 0xff00) >> 16); }
static int screenY(int scale, int wy) { return g_app->render->height - ((scale * ((0x40 - wy) << 8) + 0xff00) >> 16); }

/* 0x00012bb4 */
void Combat_drawWeapon(Combat *combat)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	Player *player = app->player;
	Render *render = app->render;

	if (cv->state == 0xc || player->weapons == 0)
		return;

	int shakeX = render->shakeX;
	int shakeY = render->shakeY;
	int now = (int)GETUPTIMEMS();
	int frame = 0;
	int blend = 0;
	int ox, oy; /* weapon offset in weapon space */

	if (app->hud->damageDirUntil > now && app->hud->damageFlash > 0)
		blend = 8; /* red flash while hurt */

	int w = player->ce.weapon;
	WeaponInfo *wp = &combat->weapons[w];

	if (player->statusEffects[9].turns != 0) {
		/* invisible: drawn see-through, no animation */
		ox = handPos(w, 0);
		oy = handPos(w, 1) - 0x40;
		blend |= 0x10;
		now = 0;
	} else if (cv->state == 4 && combat->attacker == NULL && combat->state != 2 && combat->toggle != 0) {
		/* swinging */
		ox = handPos(w, 2);
		oy = handPos(w, 3) - 0x40;
		frame = 1;
	} else if (w == 4 && player->isFamiliar) {
		/* the familiar's paws bob while it walks or turns */
		int step = 0;
		ox = handPos(4, 2);
		int base = handPos(4, 3) - 0x40;
		if (cv->viewAngle != cv->destAngle)
			step = (Math_abs(cv->destAngle - cv->viewAngle) / 64) & 3;
		else if (cv->destX != cv->viewX)
			step = (Math_abs(cv->destX - cv->viewX) / 16) & 3;
		else if (cv->destY != cv->viewY)
			step = (Math_abs(cv->destY - cv->viewY) / 16) & 3;
		oy = base - step;
		cv->viewZ = ModData_s32(&g_mod, MOD_FAMILIAR_BOB + step * 4) + 0xf;
		frame = step == 0 ? 7 : 6;
	} else {
		ox = handPos(w, 0);
		oy = handPos(w, 1) - 0x40;
	}

	int flip = 0;
	if (player->ce.weapon == 1) {
		flip = player->f264; /* axe alternates hands */
	} else if (arm_bit(player->ce.weapon) & 0xc) {
		/* sword/dagger: the guard pose drops when an NPC or a harmless
		 * creature is in front (beyond arm's reach) */
		int atEase = 0;
		if (cv->state != 4) {
			Entity *fe = player->facingEntity;
			player->fc0 = 1;
			if (fe == NULL)
				goto guard_done;
			if (Entity_distSqFrom(fe, cv->viewX, cv->viewY) > 0x1000) {
				int t = fe->def->eType;
				if (t == 2) {
					if (!(fe->def->eSubType == 2 && !(fe->info & 0x10000000)))
						atEase = 1;
				} else if (t == 3) {
					atEase = 1;
				}
			}
			if (atEase) {
				player->fc0 = 0;
				goto guard_draw;
			}
		}
		if (player->fc0 != 0)
			goto guard_done;
	guard_draw:
		if (frame != 0) {
			oy += 5;
			frame = 3;
		} else {
			oy -= 5;
			frame = 2;
			ox += 0x3c;
		}
	guard_done:;
	}

	if (player->isFamiliar == 0) {
		/* lower the weapon while using things (lock-picking, NPCs, levers) */
		Entity *fe = player->facingEntity;
		combat->raiseFlag = cv->state == 0x13;
		if (!combat->raiseFlag && fe && Entity_distSqFrom(fe, cv->viewX, cv->viewY) <= 0x1000) {
			int t = fe->def->eType;
			if (t == 3 || (t == 7 && fe->def->eSubType == 4 && player->ce.weapon != 1 && fe->param == 1))
				combat->raiseFlag = 1;
		}

		int lower = 0;
		if (combat->raiseFlag) {
			if (combat->bobTimer == -1)
				combat->bobTimer = now - 50;
			int t = now - combat->bobTimer;
			lower = t / 50 * 20;
			if (t >= 250) {
				combat->bobTimer = -now;
				lower = 100;
			}
		} else if (combat->bobTimer < -1) {
			int t = combat->bobTimer + now;
			lower = 100 - t / 50 * 20;
			if (t >= 250) {
				combat->bobTimer = -1;
				lower = 20;
			}
		}
		if (lower < 0)
			lower = 0;
		else if (lower > 100)
			lower = 100;
		oy += lower;
	}

	if (flip)
		ox = -ox;
	int scale = (Render_spriteRefW(render) << 16) / 0x8000;
	int s = Brew_screenScale();
	int wx = ox + shakeX;
	int wy = oy + shakeY;
	int x = screenX(scale, wx);
	int y = screenY(scale, wy);
	if (x < 0)
		x = 0;

	w = player->ce.weapon;
	int mode = 0;
	switch (w) {
	case 7: /* hammer: arm layer, then the head */
		drawSprite(8, frame, x, y, 0, blend, flip);
		if (frame == 0) {
			frame = now / 200 % 4 + 2;
			x = screenX(scale, wx - 0x13);
			y = screenY(scale, wy - 0xd);
		} else if (frame == 1) {
			frame = 6;
		}
		blend &= ~8;
		mode = 4;
		break;

	case 4: /* torch: flame flickers while idle */
		if (frame == 0) {
			drawSprite(5, 0, x, y, 0, blend, flip);
			frame = Render_getFlickerFrame(app->render, (int)GETUPTIMEMS());
		}
		break;

	case 1:
		drawSprite(2, frame, x, y, 0, blend, flip);
		return;

	case 5:
	case 6: /* bows: string layer when loaded */
		if (player->ammo[wp->ammoType] < wp->ammoUsage)
			frame = 1;
		else if (frame == 0)
			drawSprite(7, (now / 250 & 3) + 2, x, y, 4, blend, flip);
		break;

	case 0: { /* bare hands / sling */
		drawSprite(1, frame, x, y, 0, blend, flip);
		int m = cv->f2cc;
		if (m != 0xc) {
			if (frame == 0) {
				drawSprite(player->ce.weapon + 1, 3, x + 10 * s, y - 5 * s, m, blend & ~8, flip);
				return;
			}
			drawSprite(player->ce.weapon + 1, frame + 3, x, y, m, blend & ~8, flip);
		}
		if (frame != 1)
			return;
		drawSprite(player->ce.weapon + 1, 2, x - 5 * s, y, 4, blend & ~8, flip);
		return;
	}

	default:
		if (arm_bit(w) & 0x700) { /* wands 8..10: glowing tip */
			int glow = -1;
			int idle = frame != 1;
			if (idle && player->ammo[wp->ammoType] < wp->ammoUsage)
				frame = 0;
			else
				glow = now / 200 & 3;
			drawSprite(w + 1, frame, x, y, 0, blend, flip);
			if (!idle || glow == -1)
				return;
			int cur = player->ce.weapon;
			int glowMode = ModData_u8(&g_mod, MOD_WEAPON_GLOW + cur * 4 + glow);
			if (cur == 10) {
				drawSprite(0xb, glow + 2, x, y, 0, blend, flip);
				drawSprite(cur + 1, now / 250 % 3 + 6, x, y, 4, blend & ~8, flip);
			} else {
				drawSprite(cur + 1, 6, x, y, glowMode, blend & ~8, flip);
				drawSprite(cur + 1, glow + 2, x, y, 0, blend, flip);
			}
			return;
		}
		break;
	}
	drawSprite(player->ce.weapon + 1, frame, x, y, mode, blend, flip);
}
