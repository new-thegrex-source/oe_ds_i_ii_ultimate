/*
 * Render_drawSprite: per-sprite animation, special effects and the calls that
 * put a sprite's bitshape(s) on screen (original Render::drawSprite, 0x32630).
 *
 * The original is one 7 KB function. It picks the animation frame (monster
 * idle/walk/attack/pain cycles, torches, bobbing pickups, ...), applies
 * position wobble / bob / jitter, and then draws one or more billboards with
 * Render_drawSpriteAt (shadows, glows, overlay effects, two-part sprites).
 * Ported from the disassembly; tile numbers are sprite ids from the .bar.
 */

#include "render_local.h"
#include "oe_hw.h"

/* Tables in the executable's data area (read from the user's .mod). */
#define MOD_JITTER_TABLE   0x47168u /* int8[4]: tiny position jitter, used *2 */
#define MOD_DIST_SQ_TABLE  0x45ebc  /* int32[15]: (64*k)^2, k = 1..15 (tile distance -> world dist^2) */
#define MOD_FLAME_MODES    0x46194u /* uint8[3][4]: glow render mode for flame tiles 0xe..0x10 by phase */

/* sprite ids (tile numbers) of effect shapes drawn on top of others */
#define SPR_SECOND_HALF    0x76   /* second part of sprite 0x74 */
#define SPR_ALT_99         0x98   /* sprite 0x99 when the entity lacks flag 0x4000000 */
#define SPR_BASE_8D        0x8d   /* base drawn under 0x94 / 0x79 */
#define SPR_DROP_SHADOW    0xe9   /* flying monster shadow */
#define SPR_STATUS_FX      0xea   /* monster status-effect overlay (effect bit 8) */
#define SPR_GLOW_FX        0xeb   /* glow of 0xd/0xe monsters (flag 8) */
#define SPR_ITEM_SPARKLE   0xfe   /* sparkle over items (Entity.param & 1) */
#define SPR_FLEE_MARK      0xff   /* monster goal 4 marker */
#define SPR_OVERLAY_88     0xc1   /* drawn over sprite 0x88 */

/* Entity.info / Sprite.info bits seen here */
#define SPRITE_HIDDEN      0x10000u
#define SPRITE_WALL_TEX    0x40000u    /* sprite uses a wall texture (wallRemap) */
#define SPRITE_FLIP        0x1000000u
#define SPRITE_WALLALIGNED 0x780000u
#define SPRITE_FRAME_MASK  0xf00u

static int jitter(int idx)
{
	return ModData_s8(&g_mod, MOD_JITTER_TABLE + (uint32_t)(idx & 3));
}

/* Render_sinWave with a 32-bit millisecond time sign-extended to 64 bits. */
static int wave(Render *r, int t, int amplitude, int freq, int phase)
{
	return Render_sinWave(r, t, t >> 31, amplitude, freq, phase);
}

/* Animation frame of an ordinary sprite (the part of 0x32630 between 0x32874
 * and 0x32c10). May rewrite sprite->info, monster timers, *z and *scale. */
static int pickFrame(Render *r, Sprite *sprite, int tileKey, int frame, int *z, int *scale)
{
	Applet *app = g_app;
	Entity *ent = sprite->ent;
	/* position hash used to desynchronise animations of identical sprites */
	int hash = sprite->x + sprite->y + sprite->z;

	if (ent != NULL && ent->monster != NULL) {
		EntityMonster *mon = ent->monster;
		EntityDef *def = ent->def;

		/* monster flag 0x8000 on subtype 8: short two-step "spawn" anim */
		if ((mon->flags & 0x8000) && def->eSubType == 8) {
			frame = (r->frameTime - mon->frameTime) / 250;
			if (frame > 1)
				frame = 1;
			if (frame == 0)
				frame = 7;
			else
				mon->flags &= ~0x8000;
			return frame;
		}
		/* pain frame times out */
		if (frame == 6 && app->time > mon->frameTime) {
			mon->frameTime = 0;
			sprite->info &= ~SPRITE_FRAME_MASK;
			return 0;
		}
		if (def->eSubType == 6 && mon->goal.type == 6)
			return 4;
		if ((def->eSubType == 0xd || def->eSubType == 0xe) && frame == 4 &&
		    app->time < mon->frameTime - 100)
			return 2;
		/* attack frames 2/4 advance to 3/5 when their time is up */
		if ((frame == 2 || frame == 4) && app->time > mon->frameTime) {
			mon->frameTime = 0;
			frame = (frame == 4) ? 5 : 3;
			sprite->info = (sprite->info & ~SPRITE_FRAME_MASK) | (uint32_t)frame << 8;
			return frame;
		}
		/* idle/walk toggle */
		if (frame == 0 || (frame == 1 && !(ent->info & 0x8000000))) {
			if (app->time > mon->frameTime) {
				frame = 1 - frame;
				sprite->info = (sprite->info & ~SPRITE_FRAME_MASK) | (uint32_t)frame << 8;
				mon->frameTime = 4000 + ((uint16_t)ent->linkIndex & 0xff) * 8;
				if (def->eSubType == 2 && !(sprite->ent->info & 0x10000000))
					mon->frameTime *= 3;
				mon->frameTime += app->time;
				int sub = sprite->ent->def->eSubType;
				if (sub != 5 && sub != 0xe && sub != 0xd && (Random_nextByte(&app->random) & 1))
					sprite->info ^= SPRITE_FLIP;
			}
		}
		return frame;
	}

	if (ent != NULL && ent->def->eType == 3) {
		if (ent->info & 0x8000000)
			return frame;
		if (app->canvas->state == 4) {
			if (frame == 3)
				*z += 10;
			return frame;
		}
		int t = (app->time + ((uint16_t)ent->linkIndex & 0xff) * 256) / 3000;
		sprite->info &= ~SPRITE_FRAME_MASK;
		if (t & 1)
			sprite->info |= SPRITE_FLIP;
		else
			sprite->info &= ~SPRITE_FLIP;
		return 0;
	}

	if (tileKey == 0x89)
		return ((r->frameTime + (hash & 0xff) * 256) / 1000) & 3;
	if (tileKey == 0x78)
		return (r->frameTime >> 8) % 3 + 1;
	if (tileKey == 0x97) {
		*scale -= (*scale >> 2) + (*scale >> 3);
		return Render_getFlickerFrame(r, r->frameTime + (hash & 0xff) * 256) - 1;
	}
	if (ent != NULL && tileKey == 0x72)
		return (r->frameTime >> 7) % 6;

	/* default: 4-frame loop, 256 ms per frame */
	uint32_t seed = ent != NULL ? (uint16_t)ent->linkIndex : (uint32_t)hash;
	return (int)((((uint32_t)r->frameTime + seed * 8) << 22) >> 30);
}

/* 0x00032630 */
void Render_drawSprite(Render *render, Sprite *sprite)
{
	Applet *app = g_app;
	Canvas *cv;
	Entity *ent;
	uint32_t info = sprite->info;

	if (info & SPRITE_HIDDEN)
		return;

	int tile = info & 0xff;                 /* sprite id */
	int tileKey = tile;                     /* id used for special cases; wall textures are offset */
	if (info & SPRITE_WALL_TEX)
		tileKey = tile + 0x101;

	ent = sprite->ent;
	if (ent != NULL && tileKey == 0x8a && !Hw_freeView())
		ent->info |= 0x20000000;

	/* a visible monster wakes up (port: not seen by a free camera) */
	if (app->player->isFamiliar == 0 && !Hw_freeView()) {
		ent = sprite->ent;
		if (ent != NULL && ent->monster != NULL) {
			EntityMonster *mon = ent->monster;
			if ((ent->info & 0x200a0000) == 0 && app->player->noclip == 0 &&
			    app->game->disableAI == 0 && (mon->flags & 1) == 0 && (mon->flags & 0x7000) == 0)
				Game_activateEx(app->game, ent, 1, 1);
		}
	}

	int x = sprite->x, y = sprite->y, z = sprite->z;
	int extra = 0;                          /* effect flags handed to Render_drawSpriteAt */
	info = sprite->info;
	int frame = (info >> 8) & 0xf;
	int mode = sprite->renderMode;
	int scale = rshl(sprite->scale, 10);
	sprite->frameCount = render->frameCount;

	/* sprite 0x82 standing on the camera: pushed ahead along the view step */
	cv = app->canvas;
	if (tile == 0x82 && cv->viewX == x && cv->viewY == y) {
		y += (cv->viewStepY >> 6) * 18;
		if (sprite->scale != 0x40)
			scale = 0x10000;
		x += (cv->viewStepX >> 6) * 18;
		z -= 0x20;
	}

	/* monster status effects override the render mode */
	ent = sprite->ent;
	if (ent != NULL && ent->monster != NULL) {
		EntityMonster *mon = ent->monster;
		int effects = (int16_t)mon->monsterEffects;
		int subType = ent->def->eSubType;
		if (effects & 4)
			scale = rshl(sprite->scale - (sprite->scale >> 2), 10);
		if (effects & 2) {
			extra = 0x10;
			mode = 0;
		} else if (effects & 8) {
			extra = 0x40;
			mode = 0;
		} else if (effects & 1) {
			extra = 0x20;
			mode = 0;
		}
		if ((subType == 0xe || subType == 0xd) && mode == 0xa) {
			sprite->renderMode = 0;
			extra |= 0x10;
			mode = 0;
		} else if (mon->flags & 0x200) {
			extra |= 4;
			mode = 0;
		}
	}

	if ((extra & 0x14) == 0 && !(info & 0x80000000u))
		frame = pickFrame(render, sprite, tileKey, frame, &z, &scale);

	/* subtype-5 monsters with entity flag 0x800000 use their alternate frames */
	ent = sprite->ent;
	if (ent != NULL && ent->def != NULL && ent->monster != NULL && ent->def->eSubType == 5 &&
	    (ent->info & 0x800000) && tile != 0x95) {
		if (frame == 0 || frame == 1)
			frame = 9;
		else if (frame == 6)
			frame = 4;
	}

	if (!(extra & 0x10) && tileKey == 0x38 && (frame == 0 || frame == 1)) {
		/* shivering idle */
		int t = app->time;
		x += jitter(t / 100) * 2;
		y += jitter(t / 150) * 2;
		frame = (t / 800) & 1;
	} else if (tileKey == 0x43 || tileKey == 0x49) {
		/* floating wisps: two mirrored copies orbit, then the main one */
		int amp = (tileKey == 0x49) ? 0x10000 : 0x19999;
		int freq = (tileKey == 0x49) ? 0x4ccc : 0x8000;
		int t = App_getUpTimeMS();
		int dx = wave(render, t, amp, freq, 0xc);
		int dy = wave(render, t, amp, freq, 0x7f);
		int dz = wave(render, t, amp, freq, 0x181);
		frame = (t / 1600) & 1;
		mode = 1;
		extra |= 1;
		Render_drawSpriteAt(render, x - dx, y - dy, z - dz, render->spriteRemap[tile], frame,
		                    (int)sprite->info, mode, scale, extra);
		Render_drawSpriteAt(render, x + dx, y + dy, z + dz, render->spriteRemap[tile], frame,
		                    (int)sprite->info, mode, scale, extra);
		t += 100;
		dx = wave(render, t, amp, freq, 0xc);
		dy = wave(render, t, amp, freq, 0x7f);
		dz = wave(render, t, amp, freq, 0x181);
		y += dz;
		z += dy;
		x += dx;
		mode = 2;
	} else if (ent != NULL && ent->def->eType == 2 && ent->def->eSubType == 6 && ent->monster != NULL &&
	           ent->monster->goal.type == 6 && !(ent->monster->monsterEffects & 2)) {
		/* raging monster shakes */
		x += 2 - (Random_nextInt(&app->random) & 3);
		y += 2 - (Random_nextInt(&app->random) & 3);
	}

	/* render mode 9: blend level follows the frame (0..3) */
	if (mode == 9) {
		switch (frame) {
		case 0: mode = 0; break;
		case 1: mode = 3; break;
		case 2: mode = 2; break;
		case 3: mode = 1; break;
		default: return;
		}
	}

	/* floor bob / drunk floor wobble */
	if (render->bobEnable) {
		ent = sprite->ent;
		if (ent != NULL) {
			EntityDef *def = ent->def;
			int et = def->eType;
			int typeExempt = et < 32 && ((0x7188u >> et) & 1);
			if (!typeExempt && !(sprite->info & SPRITE_WALLALIGNED)) {
				int skip = ent->monster != NULL && !(ent->info & 0x20000000) && def->eSubType != 2;
				if (!skip && !(et == 6 && def->eSubType == 4)) {
					if (tileKey == 0x75)
						z = render->effFloor + render->bobOffset + z - 5;
					else if (ent->info & 0x20000000)
						z += render->bobOffset + (render->effFloor >> 2);
					else
						z += render->bobOffset + (render->effFloor >> 1);
				}
			}
		}
	} else if (render->drunkLevel != 0 && !(sprite->info & SPRITE_WALLALIGNED)) {
		z += render->effFloor;
	}

	/* entity flag 0x80000000: random shake (param bits 16..20) or a slow float */
	ent = sprite->ent;
	if (ent != NULL && (ent->info & 0x80000000u)) {
		int shake = ((uint32_t)ent->param << 11) >> 27;
		if (shake != 0) {
			x += Random_nextInt(&app->random) % (shake + 1);
			y += Random_nextInt(&app->random) % (shake + 1);
		} else {
			int fast = ent->def->parm == 2;
			int period = fast ? 50 : 2000;
			int amp = rdiv64(rlsl64((int64_t)(fast ? 15 : 10) << 16, 16), 0xa0000);
			int freq = rdiv64(rlsl64(rshl(ent->linkIndex % 4 + 5, 16), 16), 0xf0000);
			int t = App_getUpTimeMS();
			x += wave(render, t, amp, freq, 0xc);
			y += wave(render, t, amp, freq, 0x7f);
			if (!(sprite->ent->info & 0x8000000)) {
				int phase = (app->time + ((uint16_t)sprite->ent->linkIndex & 0xff) * 256) / period;
				frame = 0;
				sprite->info &= ~SPRITE_FRAME_MASK;
				if (phase & 1)
					sprite->info |= SPRITE_FLIP;
				else
					sprite->info &= ~SPRITE_FLIP;
			}
		}
	}

	if (tileKey == 0x46 && render->drunkTarget > 0) {
		int amp = 0x20000;
		int freq = rdiv64(rlsl64(rshl(render->drunkTarget / 20 + 2, 16), 16), 0xf0000);
		int t = App_getUpTimeMS();
		x += wave(render, t, amp, freq, 0xc);
		y += wave(render, t, amp, freq, 0x7f);
	} else if (tileKey == 0xaa) {
		extra |= 1;
	}

	ent = sprite->ent;
	if (ent != NULL && ent->def->eType == 3 && ent->def->eSubType == 1) {
		int t = App_getUpTimeMS();
		z += wave(render, t, 0x20000, 0x3333, (uint16_t)sprite->ent->linkIndex & 0x3ff);
	}

	info = sprite->info;
	if (info & SPRITE_WALL_TEX) {
		Render_drawSpriteAt(render, x, y, z, render->wallRemap[tile], frame, (int)info, mode, scale, extra);
		return;
	}

	ent = sprite->ent;
	/* original dereferences sprite->ent unconditionally for 0x74/0x93/0x99 */
	uint32_t entInfo = ent != NULL ? ent->info : 0;

	if (tileKey == 0x74) {
		Render_drawSpriteAt(render, x, y, z, render->spriteRemap[SPR_SECOND_HALF], frame, (int)info, mode,
		                    scale, extra);
		if (!(sprite->ent->info & 0x4000000))
			return;
		Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], frame, (int)sprite->info, mode,
		                    scale, extra);
		return;
	}
	if (tileKey == 0x7b) {
		int t = App_getUpTimeMS();
		z += wave(render, t, 0x19999, 0x8000, 0x7f);
		Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], 0, (int)sprite->info, mode, scale,
		                    extra);
		Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], frame + 1, (int)sprite->info, 4,
		                    scale, extra);
		return;
	}
	if (tileKey == 0x78) {
		int m = mode;
		if (frame != 0) {
			Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], 0, (int)info, mode, scale, extra);
			m = 4;
		}
		Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], frame, (int)sprite->info, m, scale,
		                    extra);
		return;
	}
	if (tileKey == 0x93) {
		Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], (entInfo & 0x4000000) ? 0 : 1,
		                    (int)info, mode, scale, extra);
		return;
	}
	if (tileKey == 0x10 || tileKey == 0xe || tileKey == 0xf) {
		/* flames: glow pass with a cycling blend, then the flame itself */
		int t = App_getUpTimeMS();
		int phase = ((t + (sprite->x ^ sprite->y)) / 200) & 3;
		int glowMode = ModData_u8(&g_mod, MOD_FLAME_MODES + (uint32_t)((tileKey - 0xe) * 4 + phase));
		z -= 8;
		Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], 4, (int)sprite->info, glowMode,
		                    0xa000, extra | 1);
		Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], phase, (int)sprite->info, mode,
		                    0xa000, extra);
		return;
	}
	if (tileKey == 0xde || tileKey == 0xdd || tileKey == 0xdc) {
		Render_drawSpriteAt(render, x, y, z + 4, render->spriteRemap[tile], frame, (int)info, mode, 0xc000,
		                    extra);
		return;
	}
	if (tileKey == 0x70) {
		Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], frame, (int)info, 0, scale >> 1,
		                    extra);
		return;
	}
	if (tileKey == 0x99) {
		int shape = (entInfo & 0x4000000) ? render->spriteRemap[tile] : render->spriteRemap[SPR_ALT_99];
		Render_drawSpriteAt(render, x, y, z, shape, frame, (int)info, mode, scale, extra);
		return;
	}
	if (ent != NULL && ent->def->eType == 3) {
		/* item: optional sparkle over it */
		Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], frame, (int)info, mode, scale, extra);
		if (!(sprite->ent->param & 1))
			return;
		int sub = sprite->ent->def->eSubType;
		int dz = (sub == 2 || sub == 5) ? 0xe : 0;
		Render_drawSpriteAt(render, x, y, z + dz, render->spriteRemap[SPR_ITEM_SPARKLE], frame,
		                    (int)(sprite->info & ~SPRITE_FLIP), 0, scale, extra);
		return;
	}
	if (tileKey == 0x94 && !(info & SPRITE_WALLALIGNED)) {
		Render_drawSpriteAt(render, x, y, z, render->spriteRemap[SPR_BASE_8D], frame, (int)info, 0, scale,
		                    extra);
		Render_drawSpriteAt(render, x, y, z + 0xe, render->spriteRemap[tile], frame, (int)sprite->info, 0,
		                    scale, extra);
		return;
	}
	if (tileKey == 0x79 && !(info & SPRITE_WALLALIGNED)) {
		Render_drawSpriteAt(render, x, y, z, render->spriteRemap[SPR_BASE_8D], frame, (int)info, 0, scale,
		                    extra);
		Render_drawSpriteAt(render, x, y, z + 0xa, render->spriteRemap[tile], frame, (int)sprite->info, 0,
		                    0xd000, extra);
		return;
	}

	uint32_t drawFlags = info;

	if (ent != NULL && ent->def->eType == 2) {
		/* ---- monsters ---- */
		EntityMonster *mon = ent->monster;
		int sub;

		if (frame == 1) {
			sub = ent->def->eSubType;
			if (sub == 0 || sub == 1 || sub == 9 || sub == 3 || sub == 4 || sub == 6 || sub == 0xf ||
			    sub == 0xb)
				drawFlags ^= SPRITE_FLIP;
		}

		/* sleeping/facing-away monster (flags 0x7000 = direction it looks):
		 * wakes when the player is on its side and close enough */
		int dir = (int16_t)mon->flags & 0x7000;
		if (dir != 0) {
			int stillAsleep = 1;
			Player *pl = app->player;
			if (dir < 0x3000) {
				int d = y - pl->ce.y;
				if (d < 0 && dir == 0x1000)
					stillAsleep = 0;
				else if (d > 0 && dir == 0x2000)
					stillAsleep = 0;
			} else {
				int d = x - pl->ce.x;
				if (d > 0 && dir == 0x3000)
					stillAsleep = 0;
				else if (d < 0 && dir == 0x4000)
					stillAsleep = 0;
			}
			int distSq = Entity_distSqFrom(ent, app->canvas->viewX, app->canvas->viewY);
			if (ModData_s32(&g_mod, MOD_DIST_SQ_TABLE + 3 * 4) < distSq || app->player->noclip != 0)
				stillAsleep = 0;
			if (stillAsleep)
				sprite->ent->monster->flags &= ~0x7000;
			else
				frame = 9;
		}

		ent = sprite->ent;
		EntityDef *def = ent->def;
		sub = def->eSubType;
		if (sub == 9) {
			if (frame == 4) {
				x -= (int)((uint32_t)render->viewCos << 3) >> 16;
				y += (int)((uint32_t)render->viewSin << 3) >> 16;
			} else if (frame == 5) {
				x -= (int)((uint32_t)render->viewCos << 4) >> 16;
				y += (int)((uint32_t)render->viewSin << 4) >> 16;
			} else if (frame == 8) {
				/* dying: fade out over 150 ms, then hide */
				int64_t dt = (int32_t)(app->time - ent->monster->frameTime);
				if (dt < 50)
					mode = 3;
				else if (dt < 100)
					mode = 2;
				else if (dt >= 150) {
					sprite->info |= SPRITE_HIDDEN;
					return;
				} else
					mode = 1;
			}
			if (!(sprite->ent->monster->monsterEffects & 2)) {
				int t = app->time;
				x += jitter(t / 500) * 2;
				y += jitter(t / 750) * 2;
				int up = App_getUpTimeMS();
				z += wave(render, up, 0x20000, 0x4000, (uint16_t)sprite->ent->linkIndex & 0x3ff);
			}
		} else if (sub == 7 && def->parm == 2) {
			/* rising out of the ground for 750 ms (start time in Entity.param) */
			int start = ent->param;
			if (app->time < start + 750) {
				int e = app->time - start;
				z = (e * 64) / 750 - 0x20;
				x += 1 - Random_nextInt(&app->random) % 3;
				y += 1 - Random_nextInt(&app->random) % 3;
				if (e < 0x145)
					mode = 1;
				else if (e < 0x20d)
					mode = 2;
				else
					mode = 3;
			} else if (start != 0) {
				ent->param = 0;
				z = 0x20;
				sprite->z = 0x20;
			}
		} else if (sub == 4) {
			int t = app->time;
			x += jitter(t / 500) * 2;
			y += jitter(t / 750) * 2;
			int up = App_getUpTimeMS();
			z = z + wave(render, up, 0x20000, 0x4000, (uint16_t)sprite->ent->linkIndex & 0x3ff) + 4;
		} else if (sub == 0xa && frame == 3) {
			y += rmul(render->viewSin, 0x18) >> 16;
			x -= rmul(render->viewCos, 0x18) >> 16;
		}

		/* flying monsters cast a flickering shadow on the floor */
		if ((sprite->ent->info & 0x1000000) && mode != 1) {
			int shadowMode = ((render->frameTime / 100) & 1) ? 4 : 5;
			Render_drawSpriteAt(render, x, y, z, render->spriteRemap[SPR_DROP_SHADOW], 0, (int)drawFlags,
			                    shadowMode, 0x14000, (extra & ~8) | 4);
			mode = 8;
		}

		ent = sprite->ent;
		sub = ent->def->eSubType;
		if (sub == 0xe || sub == 0xd) {
			mon = ent->monster;
			if (mon->flags & 8) {
				int up = App_getUpTimeMS();
				z = z + wave(render, up, 0x20000, 0x5555, (uint16_t)sprite->ent->linkIndex & 0x3ff) + 7;
			}
			mon = sprite->ent->monster;
			if ((frame == 0 || frame == 1) && !(mon->flags & 8) && !(mon->monsterEffects & 0x10)) {
				frame = 2;
				Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], 0xa, (int)drawFlags, 4, scale,
				                    1);
			} else if (frame == 2 || frame == 3) {
				int weapon = Combat_getMonsterAttackField(app->combat, sprite->ent->def, 1);
				int fxFrame = (weapon == sprite->ent->monster->ce.weapon) ? 10 : 9;
				Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], fxFrame, (int)drawFlags, 4,
				                    scale, 1);
			}
		}

		if (sprite->ent->monster->goal.type == 4)
			Render_drawSpriteAt(render, x, y, z + 0xa, render->spriteRemap[SPR_FLEE_MARK], 0,
			                    (int)(drawFlags & ~0x20000000u), 0, scale, extra);
	}

	/* status-effect overlay (monster effect bit 8) */
	ent = sprite->ent;
	if (tile != 0x95 && ent != NULL) {
		EntityDef *def = ent->def;
		int et = def->eType;
		if ((et == 2 || et == 9) && ent->monster != NULL && (ent->monster->monsterEffects & 8)) {
			int dz = 0xa;
			int fxScale = scale;
			if (et == 9 || !(ent->info & 0x40000)) {
				fxScale = 0x8000;
				dz = 0;
			} else if (def->eSubType == 1 && def->parm == 0) {
				dz = 5;
			}
			int fxFrame = (app->time / 250) & 3;
			Render_drawSpriteAt(render, x, y, z + dz, render->spriteRemap[SPR_STATUS_FX], fxFrame, 0, 0,
			                    fxScale, 0);
		}
	}

	/* the sprite itself */
	Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], frame, (int)drawFlags, mode, scale, extra);

	ent = sprite->ent;
	if (ent != NULL && (ent->def->eSubType == 0xe || ent->def->eSubType == 0xd)) {
		if (ent->monster->flags & 8) {
			int glowScale = (int32_t)(rmul64(0x13333, scale) >> 16);
			int up = App_getUpTimeMS();
			Render_drawSpriteAt(render, x, y, z, render->spriteRemap[SPR_GLOW_FX], (up / 200) & 3,
			                    (int)drawFlags, 4, glowScale, 1);
		}
		if (frame == 4) {
			Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], 0xb, (int)drawFlags, 4, scale, 1);
		} else if (frame == 5) {
			int up = App_getUpTimeMS();
			if (!((up / 100) & 1))
				Render_drawSpriteAt(render, x, y, z, render->spriteRemap[tile], 0xc, (int)drawFlags, 4, scale,
				                    1);
		}
	}

	if (tileKey == 0x88)
		Render_drawSpriteAt(render, x, y, z, render->spriteRemap[SPR_OVERLAY_88], frame, (int)drawFlags, 4,
		                    scale, extra);
}
