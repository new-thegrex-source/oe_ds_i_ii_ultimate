/*
 * Render: sprites (original Render::drawSprite 0x20914, Render::renderSprite
 * 0x21a10) and the wall probe Render_tileTouchesWall.
 *
 * Render_drawSprite picks a sprite's animation frame (monster idle/attack/
 * pain cycles, torches, flames ...), applies bobbing / jitter / waves, and
 * then puts one or more bitshapes on screen with Render_renderSprite
 * (multi-part boss, shadows, glows, overlays). Ported from the disassembly;
 * tile numbers are sprite ids from the .bar mappings.
 *
 * Render_renderSprite fills the temporary segment Render.seg: billboards
 * become a view-facing segment drawn by Render_drawLineSeg, wall-aligned
 * sprites (Sprite.info bits 19..22) a world line drawn by Render_drawLine.
 */

#include "render_local.h"
#include "oe_hw.h"

/* Tables in the executable's data area (read from the user's .mod). */
#define MOD_BOB_TABLE       0x2fc2cu /* int8[4]: small position bob/jitter steps */
#define MOD_BOSS_WIGGLE     0x2fc30u /* int8[13]: wiggle of the boss's arm part (tile 0xb9) */
#define MOD_STR_BAD_MEDIA   0x2fd80u

/* Sprite.info bits */
#define SPRITE_HIDDEN       0x10000u
#define SPRITE_WALL_TEX     0x40000u     /* drawn with a wall texture (Render.wallBase) */
#define SPRITE_WALLALIGNED  0x780000u    /* bits 19..22: aligned to a wall instead of a billboard */
#define SPRITE_FLIP         0x1000000u
#define SPRITE_FRAME_MASK   0xf00u

/* Where the original objects lived inside the Applet (one 0x1b6cd0-byte
 * object). A few animations are desynchronised by hashing the low bits of an
 * Entity's or Sprite's address; the port rebuilds those addresses relative to
 * the Applet, assuming the phone's heap block started on a 256-byte boundary. */
#define PHONE_RENDER_OFS    0x16d4u
#define PHONE_GAME_OFS      0x1a7cc8u
#define PHONE_SPRITES_OFS   0xfd50u      /* Render.sprites, 0x2c bytes each */
#define PHONE_ENTITIES_OFS  0x8ecu       /* Game.entities, 0x24 bytes each */

static uint32_t phoneEntityAddr(Entity *ent)
{
	Game *game = g_app->game;
	if (ent >= game->entities && ent < game->entities + 400)
		return PHONE_GAME_OFS + PHONE_ENTITIES_OFS + (uint32_t)(ent - game->entities) * 0x24u;
	return (uint32_t)(uintptr_t)ent;
}

static uint32_t phoneSpriteAddr(Render *render, Sprite *sprite)
{
	if (sprite >= render->sprites && sprite < render->sprites + 1000)
		return PHONE_RENDER_OFS + PHONE_SPRITES_OFS + (uint32_t)(sprite - render->sprites) * 0x2cu;
	return (uint32_t)(uintptr_t)sprite;
}

/* int8 bob step, index taken modulo 4 */
static int bob(int i)
{
	return ModData_s8(&g_mod, MOD_BOB_TABLE + ((uint32_t)i & 3u));
}

static int spriteMedia(Render *render, int tile)
{
	return render->spriteBase[tile];
}

/* Animation frame for a sprite whose entity has a monster (0x209ee..0x20bac).
 * May rewrite sprite->info, the monster's frame timer and *mode. */
static int monsterFrame(Render *render, Sprite *sprite, Entity *ent, int frame, int *mode)
{
	EntityMonster *mon = ent->monster;
	EntityDef *def = ent->def;
	uint32_t info = sprite->info;
	int now = render->app->tickTime;

	if (frame == 6 && now > mon->frameTime) {
		/* pain frame over */
		mon->frameTime = 0;
		sprite->info &= ~SPRITE_FRAME_MASK;
		frame = 0;
	} else if (frame == 7 && now > mon->frameTime) {
		/* start of the death: frame 8 */
		mon->frameTime = now;
		sprite->info = (sprite->info & ~SPRITE_FRAME_MASK) + 0x800;
		frame = 8;
	} else {
		int sub = def->eSubType;
		if (sub == 7 && mon->goal.type == 6) {
			frame = 2;
		} else if (sub == 0xf && mon->goal.type == 0xb) {
			frame = 4;
		} else if ((frame == 2 || frame == 4) && now > mon->frameTime) {
			/* attack wind-up done: 2 -> 3, 4 -> 5 */
			mon->frameTime = 0;
			frame = (frame == 4) ? 5 : 3;
			sprite->info = (sprite->info & ~SPRITE_FRAME_MASK) | (uint32_t)frame << 8;
		} else if (frame == 0 || (frame == 1 && !(ent->info & 0x8000000))) {
			/* idle/walk toggle every ~4 s */
			if (now > mon->frameTime) {
				frame = (frame == 0);
				sprite->info = (info & ~SPRITE_FRAME_MASK) | (uint32_t)frame << 8;
				mon->frameTime = (int)((phoneEntityAddr(ent) & 0xff) << 3) + 4000;
				if (def->eSubType == 2 && !(ent->info & 0x20000000))
					mon->frameTime *= 3;
				mon->frameTime += render->app->tickTime;
			}
		} else if (sub == 0xf) {
			if (frame == 3) {
				if (mon->frameTime != 0) {
					if (now > mon->frameTime) {
						sprite->info = info & ~SPRITE_FRAME_MASK;
						frame = 0;
					} else {
						frame = 2;
					}
				}
			} else if (frame == 5) {
				if (mon->frameTime == 0)
					mon->frameTime = now;
				now = render->app->tickTime;
				if (now >= mon->frameTime + 400) {
					if (now < mon->frameTime + 600) {
						frame = 4;
					} else {
						sprite->info &= ~SPRITE_FRAME_MASK;
						frame = 0;
					}
				}
			}
		}
	}

	if (def->eSubType == 0xa) {
		if (frame == 3 || frame == 5 || frame == 6 || frame == 7)
			*mode = 6;
		else if (frame == 2 || frame == 4)
			*mode = 5;
		else if (frame == 8)
			*mode = 0;
	}
	return frame;
}

/* Animation frame of a sprite without a monster (0x20bd4..0x20c64). */
static int staticFrame(Render *render, Sprite *sprite, Entity *ent, int tile)
{
	uint32_t hash = (phoneSpriteAddr(render, sprite) & 0xff) << 3;
	uint32_t t = (uint32_t)render->frameTime;

	if (tile == 0xcb)
		return (int)((((uint32_t)sprite->x ^ (uint32_t)sprite->y) + hash + t) & 0xfff) >> 8;
	if ((unsigned)(tile - 0x7d) <= 2)
		return (int)((t & 0x1ff) >> 7);
	if (tile == 0xd0)
		return ((t + hash) & 0xfff) >> 8 ? 1 : 0;
	if (ent != NULL)
		return (int)(((t + (phoneEntityAddr(ent) << 3)) >> 8) & 3);
	return (int)(((t + hash) >> 8) & 3);
}

/* Multi-part boss (tile 0xb9), 0x20f62..0x2124c. */
static void drawBoss(Render *render, Sprite *sprite, Entity *ent, int z, int frame, int mode, int scale, int extra)
{
	Applet *app = render->app;
	int sinA = render->sinTable[render->viewAngle & 0x3ff];
	int cosA = render->sinTable[(render->viewAngle + 0x100) & 0x3ff];
	int bx = sprite->x - (rshl(cosA, 4) >> 16);
	int by = sprite->y + (rshl(sinA, 4) >> 16);
	int t, ax, az, f;

	Render_renderSprite(render, bx + 0x1f, by, z, spriteMedia(render, 0xb8), frame,
	                    (int)(sprite->info | 0x100000), mode, scale, extra);
	Render_renderSprite(render, bx - 0x1f, by, z, spriteMedia(render, 0xb8), frame,
	                    (int)(sprite->info | 0x5100000), mode, scale, extra);

	t = app->tickTime;
	ax = bob((t / 0x14cb) % 4) + bx + 0x10;
	az = bob((t / 0x1163) % 4) + z + 0x1a;
	Render_renderSprite(render, ax, by, az, spriteMedia(render, 0xbc), frame,
	                    (int)(sprite->info | 0x5080000), mode, scale, extra);
	Render_renderSprite(render, bx + 0x3a, by, z, spriteMedia(render, 0xb9), frame,
	                    (int)(sprite->info | 0x5080000), mode, scale, extra);
	Render_renderSprite(render, bx, by, z, spriteMedia(render, 0xbd), frame,
	                    (int)(sprite->info | 0x5080000), mode, scale, extra);

	t = app->tickTime;
	ax = bob((t / 0x1a93) % 4) + bx + 0x2e;
	az = bob((t / 0x137b) % 4) + sprite->z + 0xf;
	Render_renderSprite(render, ax, by, az, spriteMedia(render, 0xbb), frame,
	                    (int)(sprite->info | 0x5080000), mode, scale, extra);

	/* the arm: wiggles while Entity.param is 0, frame 1 when it is 1 */
	ax = bx - 0xb;
	az = sprite->z + 0x28;
	if (ent->param == 0) {
		t = app->tickTime;
		ax += ModData_s8(&g_mod, MOD_BOSS_WIGGLE + (uint32_t)((t / 0x595) % 13));
		az += ModData_s8(&g_mod, MOD_BOSS_WIGGLE + (uint32_t)((t / 0xae7) % 13));
		frame = 0;
	} else if (ent->param == 1) {
		frame = 1;
	}
	Render_renderSprite(render, ax, by, az, spriteMedia(render, 0xba), frame,
	                    (int)(sprite->info | 0x5080000), mode, scale, extra);

	/* angry player: a flash on the arm for 3 x 256 ms of every 4 s */
	if (app->player->mood >= 0x20)
		return;
	t = app->tickTime;
	f = (int)(((uint32_t)t & 0x3ff) >> 8);
	if (((uint32_t)t & 0xfff) >> 10)
		return;
	if (f >= 3)
		return;
	Render_renderSprite(render, ax, by, az, spriteMedia(render, 0xe8), f,
	                    (int)(sprite->info | 0x4080000), 7, scale, extra);
}

/* 0x00020914 */
void Render_drawSprite(Render *render, void *spritePtr)
{
	Sprite *sprite = (Sprite *)spritePtr;
	Applet *app = render->app;
	Entity *ent;
	EntityMonster *mon;
	EntityDef *def;
	uint32_t info, entryInfo;
	int tile, x, y, z, frame, mode, scale, extra, flags;

	if (sprite->info & SPRITE_HIDDEN)
		return;

	/* a monster that gets drawn wakes up (port: not by a free camera) */
	ent = sprite->ent;
	if (ent != NULL && ent->monster != NULL && (ent->info & 0x400a0000) == 0 && !Hw_freeView() &&
	    app->player->noclip == 0 && render->game->disableAI == 0 && !(ent->monster->flags & 1))
		Game_activateEx(render->game, ent, 1, 1);

	entryInfo = sprite->info;
	tile = (int)(entryInfo & 0xff);
	x = sprite->x;
	y = sprite->y;
	frame = (int)((entryInfo >> 8) & 0xf);
	mode = sprite->renderMode;
	z = sprite->z;
	extra = 0;
	scale = rshl(sprite->scale, 10);

	ent = sprite->ent;
	if (ent != NULL && (mon = ent->monster) != NULL) {
		int petrified = mon->petrifyTurns;
		if (petrified) {
			extra = 4;  /* stone: grey */
			mode = 0;
		}
		if (app->tickTime < mon->flashUntil && (ent->def->eSubType != 0xd || petrified))
			extra |= 8; /* hit flash */
	}

	/* ---- animation frame ---- */
	if (!(extra & 4) && !(entryInfo & 0x80000000u)) {
		if (ent == NULL)
			frame = staticFrame(render, sprite, NULL, tile);
		else if (ent->monster != NULL)
			frame = monsterFrame(render, sprite, ent, frame, &mode);
		else if (ent->def->eType == 3)
			frame = ((app->tickTime + (int)((phoneEntityAddr(ent) & 0xff) << 8)) / 6000) % 2;
		else
			frame = staticFrame(render, sprite, ent, tile);
	}
	if (ent != NULL && ent->def != NULL && ent->monster != NULL && ent->def->eSubType == 6 &&
	    (ent->info & 0x800000)) {
		if (frame == 0 || frame == 1)
			frame = 9;
		else if (frame == 6)
			frame = 4;
	}

	/* ---- per-tile movement ---- */
	if (tile == 0x38 && (frame == 0 || frame == 1)) {
		int t = app->tickTime;
		x += bob(t / 100) * 2;
		y += bob(t / 150) * 2;
		frame = (t / 800) % 2;
	} else if (tile == 0x43) {
		int now = (int)GETUPTIMEMS();
		int a = Render_sinWave(render, now, 0x19999, 0x8000, 0xc);
		int b = Render_sinWave(render, now, 0x19999, 0x8000, 0x7f);
		int c = Render_sinWave(render, now, 0x19999, 0x8000, 0x181);
		x += a;
		y += c;
		z += b;
		frame = (app->tickTime / 1600) % 2;
	} else if (ent != NULL && ent->def->eType == 2 && ent->def->eSubType == 7 && ent->monster != NULL &&
	           ent->monster->goal.type == 6 && ent->monster->petrifyTurns == 0) {
		x += 2 - (Random_nextByte(&app->random) & 3);
		y += 2 - (Random_nextByte(&app->random) & 3);
	}

	/* render mode 0xd: the frame selects a translucency level */
	if (mode == 0xd) {
		if (frame == 0)
			mode = 0;
		else if (frame == 1)
			mode = 6;
		else if (frame == 2)
			mode = 5;
		else if (frame == 3)
			mode = 4;
		else
			return;
	}

	/* ---- floor wobble / drunk height ---- */
	if (render->wobble != 0) {
		ent = sprite->ent;
		if (ent == NULL)
			goto dispatch;
		def = ent->def;
		{
			uint32_t typeBit = def->eType < 32 ? 1u << def->eType : 0;
			if ((typeBit & 0xf188) == 0 && (sprite->info & SPRITE_WALLALIGNED) == 0 &&
			    (ent->monster == NULL || (ent->info & 0x40000000) || def->eSubType == 2 || def->eSubType == 3)) {
				if (def->eType == 6 && def->eSubType == 4)
					goto dispatch;
				if (tile == 0x75)
					z += render->floorZ + render->wobbleOfs - 5;
				else if ((ent->info & 0x40000000) || def->eSubType == 3)
					z += (render->floorZ >> 2) + render->wobbleOfs;
				else
					z += (render->floorZ >> 1) + render->wobbleOfs;
			}
		}
	} else {
		if (render->drunkLevel != 0 && (sprite->info & SPRITE_WALLALIGNED) == 0)
			z += render->floorZ;
		if (sprite->ent == NULL)
			goto dispatch;
	}
	ent = sprite->ent;
	if (ent->def->eType == 3 && (ent->def->eSubType == 1 || ent->def->eSubType == 5))
		z += Render_sinWave(render, (int)GETUPTIMEMS(), 0x20000, 0x3333, (int)(phoneEntityAddr(ent) & 0x3ff));

dispatch:
	ent = sprite->ent;
	info = sprite->info;

	if (info & SPRITE_WALL_TEX) {
		Render_renderSprite(render, x, y, z, render->wallBase[tile], frame, (int)info, mode, scale, extra);
		return;
	}
	if (tile == 0x74) {
		/* drawn as tile 0x76; 0x74 itself on top while the entity is linked */
		Render_renderSprite(render, x, y, z, spriteMedia(render, 0x76), frame, (int)info, mode, scale, extra);
		if (ent == NULL || !(ent->info & 0x4000000))
			return;
		Render_renderSprite(render, x, y, z, spriteMedia(render, 0x74), frame, (int)sprite->info, mode, scale, extra);
		return;
	}
	if (tile == 0xb9) {
		if (ent != NULL)
			drawBoss(render, sprite, ent, z, frame, mode, scale, extra);
		return;
	}
	if (tile == 0x91) {
		Render_renderSprite(render, x, y, z, spriteMedia(render, 0x91), frame, (int)info, mode, scale, extra);
		if (ent == NULL || !(ent->param & 0x100))
			return;
		Render_renderSprite(render, x, y, z, spriteMedia(render, 0x92), frame, (int)sprite->info, 7, scale, extra);
		return;
	}
	if (ent != NULL && ent->def->eType == 3) {
		Render_renderSprite(render, x, y, z, spriteMedia(render, tile), frame, (int)info, mode, scale, extra);
		if (ent->param == 0)
			return;
		Render_renderSprite(render, x, y, z, spriteMedia(render, 0xfe), frame, (int)sprite->info, 0, scale, extra);
		return;
	}
	if ((tile == 0x94 || tile == 0x8e || tile == 0x93) && (info & SPRITE_WALLALIGNED) == 0) {
		Render_renderSprite(render, x, y, z, spriteMedia(render, 0x8d), frame, (int)info, 0, scale, extra);
		Render_renderSprite(render, x, y, z + 0xe, spriteMedia(render, tile), frame, (int)sprite->info, 0, scale, extra);
		return;
	}

	flags = (int)info;
	if (tile == 0x3b) {
		if (ent == NULL)
			goto plainEffects;
		mon = ent->monster;
		if ((mon != NULL && mon->goal.type == 0xb) || frame == 7) {
			x += 1 - (int)((unsigned)Random_nextByte(&app->random) % 3);
			y += 1 - (int)((unsigned)Random_nextByte(&app->random) % 3);
		}
		goto draw;
	}

	if (ent != NULL && ent->def->eType == 2) {
		/* ---- monsters ---- */
		int sub;
		def = ent->def;
		mon = ent->monster;
		if (frame == 1) {
			sub = def->eSubType;
			if (sub == 0 || sub == 1 || sub == 0xb || sub == 4 || sub == 5 || sub == 7 || sub == 0xc || sub == 0xd)
				flags ^= (int)SPRITE_FLIP; /* walk frame alternates facing */
			else
				goto notLunger;
		}
		if (def->eSubType == 0xb) {
			int sinA = render->sinTable[render->viewAngle & 0x3ff];
			int cosA = render->sinTable[(render->viewAngle + 0x100) & 0x3ff];
			if (frame == 4) {
				/* lunge towards the viewer */
				x -= rshl(cosA, 3) >> 16;
				y += rshl(sinA, 3) >> 16;
			} else if (frame == 5) {
				x -= rshl(cosA, 4) >> 16;
				y += rshl(sinA, 4) >> 16;
			} else if (frame == 8) {
				int elapsed = app->tickTime - mon->frameTime;
				if (elapsed < 0x32)
					mode = 6;
				else if (elapsed < 0x64)
					mode = 5;
				else if (elapsed < 0x96)
					mode = 4;
				else {
					sprite->info = info | SPRITE_HIDDEN;
					return;
				}
			}
			if (mon->petrifyTurns == 0) {
				int t = app->tickTime;
				x += bob(t / 500) * 2;
				y += bob(t / 750) * 2;
				z += Render_sinWave(render, (int)GETUPTIMEMS(), 0x20000, 0x4000, (int)(phoneEntityAddr(ent) & 0x3ff));
			}
		} else {
notLunger:
			sub = def->eSubType;
			if (sub == 8) {
				if (def->parm == 2) {
					/* rising out of the ground over 750 ms (Entity.param = start time) */
					int now = app->tickTime;
					int start = ent->param;
					if (now < start + 750) {
						int elapsed = now - start;
						z = rshl(elapsed, 6) / 750 - 0x20;
						x += 1 - (int)((unsigned)Random_nextByte(&app->random) % 3);
						y += 1 - (int)((unsigned)Random_nextByte(&app->random) % 3);
						if (elapsed < 325)
							mode = 4;
						else if (elapsed < 525)
							mode = 5;
						else
							mode = 6;
					} else if (start != 0) {
						ent->param = 0;
						z = 0x20;
						sprite->z = 0x20;
					}
				}
			} else if (sub == 5 && mon->petrifyTurns == 0) {
				int t = app->tickTime;
				x += bob(t / 500) * 2;
				y += bob(t / 750) * 2;
				z += Render_sinWave(render, (int)GETUPTIMEMS(), 0x20000, 0x4000, (int)(phoneEntityAddr(ent) & 0x3ff));
			}
		}

		/* status overlays: shadow-like glow under the monster */
		if ((ent->info & 0x10000000) && mode != 4 && mon->petrifyTurns == 0) {
			int glowMode = ((render->frameTime / 50) & 1) ? 7 : 8;
			Render_renderSprite(render, x, y, z, spriteMedia(render, 0xe9), 0, flags, glowMode, 0x14000, extra & ~8);
			mode = 0xf;
		} else if ((ent->info & 0x1000000) && mode != 4 && mon->petrifyTurns == 0) {
			int glowMode = ((render->frameTime / 100) & 1) ? 7 : 8;
			Render_renderSprite(render, x, y, z, spriteMedia(render, 0xe9), 0, flags, glowMode, 0x14000, (extra & ~8) | 4);
			mode = 0xc;
		}
		/* fleeing marker */
		if (mon->goal.type == 4)
			Render_renderSprite(render, x, y, z + 10, spriteMedia(render, 0xff), 0, flags & ~0x20000000, 0, scale, extra);
		goto draw;
	}

	if (tile == 0xe || tile == 0xc) {
		z += Render_sinWave(render, (int)GETUPTIMEMS(), 0x20000, 0x4000, 0);
		frame = 0;
		goto draw;
	}

plainEffects:
	if ((unsigned)(tile - 0x7d) <= 2) {
		z += Render_sinWave(render, (int)GETUPTIMEMS(), 0x20000, 0x4000, 0);
	} else if (tile == 0xeb) {
		extra |= 1;
		scale = rshl(scale, 1);
	}

draw:
	Render_renderSprite(render, x, y, z, spriteMedia(render, tile), frame, flags, mode, scale, extra);

	/* ---- overlays drawn after the sprite ---- */
	if (tile == 0x88) {
		Render_renderSprite(render, x, y, z, spriteMedia(render, 0xc1), frame, flags, 7, scale, extra);
		return;
	}
	if (tile == 0x39) {
		mon = ent != NULL ? ent->monster : NULL;
		if (mon != NULL && (mon->flags & 8) && mon->petrifyTurns == 0 && frame < 7) {
			if (app->tickTime < mon->flashUntil) {
				if (frame == 0) {
					flags &= ~(int)SPRITE_FLIP;
					frame = 6;
				} else if (frame == 1) {
					flags |= (int)SPRITE_FLIP;
					frame = 6;
				}
			}
			Render_renderSprite(render, x, y, z, spriteMedia(render, 0x39), frame + 9, flags, 7, scale, extra);
			return;
		}
		goto flames;
	}
	if (tile == 0x3b) {
		int goal;
		if (ent == NULL)
			goto flames;
		mon = ent->monster;
		goal = mon != NULL ? mon->goal.type : 0;
		if (goal == 9) {
			int f = (app->tickTime >> 8) % 3;
			Render_renderSprite(render, x, y, z + 0x20, spriteMedia(render, 0xf3), f, flags, 7, scale, extra);
			return;
		}
		if (frame == 4) {
			if (goal != 0xb)
				return;
			Render_renderSprite(render, x, y, z, spriteMedia(render, 0x3b), 9, flags, 7, scale, extra);
		} else if (frame == 2) {
			Render_renderSprite(render, x, y, z, spriteMedia(render, 0x3b), 0xc, flags, 7, scale, extra);
		} else if (frame == 3) {
			int m = ((render->frameTime / 100) & 1) ? 7 : 8;
			Render_renderSprite(render, x, y, z, spriteMedia(render, 0x3b), 0xd, flags, m, scale, extra);
		} else if (frame == 5) {
			Render_renderSprite(render, x, y, z, spriteMedia(render, 0x3b), 10, flags, 7, scale, extra);
		}
		return;
	}
	if (tile == 0xe || tile == 0xc) {
		Render_renderSprite(render, x, y, z, spriteMedia(render, tile), 1, flags, 7, scale, extra);
		return;
	}

flames:
	if ((unsigned)(tile - 0x7d) <= 2) {
		int phase;
		Render_renderSprite(render, x, y, z, spriteMedia(render, tile), 4, flags, 7, scale, extra);
		phase = ((render->frameTime / 3) >> 5) % 0x18;
		if (phase >= 4)
			return;
		if (phase & 1) {
			scale = scale / 2 + scale / 4;
			z += 6;
		}
		Render_renderSprite(render, x, y, z, spriteMedia(render, 0x79), 0, flags, 7, scale, extra);
	}
}

/* 0x00021a10 Put bitshape media `tile` + frame at world (x, y) with height z.
 * flags: Sprite.info-style bits (18 wall texture, 19..22 wall-aligned axis,
 * 24 mirror, 25 effect bit 2, 26 two-sided, 29 mirrored texture, 30 special);
 * mode = blend mode, scale 16.16, extra = Render.segFlags effect bits.
 * Returns 1 when nothing was drawn. */
int Render_renderSprite(Render *render, int x, int y, int z, int tile, int frame, int flags, int mode, int scale, int extra)
{
	Line *seg = &render->seg;
	uint32_t uflags = (uint32_t)flags;
	int aligned = (int)((uflags << 9) >> 28);  /* bits 19..22 */
	int mirror = (uflags & SPRITE_FLIP) != 0;
	int idx, half, left, right;

	if (tile == MEDIA_NONE)
		return 1;	/* DS: a sprite the BREW game has no picture of (the engine draws the DS's) */
	idx = tile + frame;

	if (aligned == 0) {
		/* billboard: reject when behind the near plane (16 units pulled in) */
		seg->x1 = x;
		seg->y1 = y;
		Render_transformPoint(render, &seg->x1);
		seg->x1 -= 0x100000;
		if (seg->x1 < NEAR_PLANE)
			return 1;
	}
	if (render->drunkLevel != 0 && aligned == 0) {
		x += render->drunkOfs[2];
		y += render->drunkOfs[0];
		seg->x1 = x;
		seg->y1 = y;
		Render_transformPoint(render, &seg->x1);
		seg->x1 -= 0x100000;
	}

	/* horizontal extent in world units, relative to the anchor */
	half = scale >> 11;
	seg->z1 = 0;
	if (uflags & SPRITE_WALL_TEX) {
		left = -half;
		right = half;
		seg->flags = LINE_NO_DEPTH;
	} else {
		int ofs = render->spriteMap[idx].ofs;
		left = (rshl(rmul(render->shapeData[ofs + 2], scale), 6) / 0x58 >> 16) - half;
		right = (rshl(rmul(render->shapeData[ofs + 3], scale), 6) / 0x58 >> 16) - half;
		seg->flags = LINE_SPRITE;
	}
	if (mirror) {
		int t = left;
		left = -right;
		right = -t;
	}

	seg->texture = (int16_t)idx;
	render->blendMode = (uint8_t)mode;
	render->segFlags = extra;
	render->curLine = -1;
	render->spriteScale = rdiv64(0x160000000LL, (int64_t)scale);
	render->zOffset = z;
	seg->z2 = right - left;
	if (uflags & 0x40000000)
		seg->flags = 0x40000000;
	if (uflags & 0x4000000)
		seg->flags |= LINE_TWO_SIDED;
	if (uflags & 0x20000000)
		seg->flags |= LINE_MIRROR;
	if (uflags & 0x2000000)
		render->segFlags |= 2;

	if (aligned == 0) {
		seg->x2 = seg->x1;
		if (mirror) {
			int t = seg->z1;
			seg->z1 = seg->z2;
			seg->z2 = t;
		}
		seg->y2 = seg->y1 + rshl(right, 16);
		seg->y1 = seg->y1 + rshl(left, 16);
		RenderHw_billboard(render, x, y, left, right, seg->z1, seg->z2);   /* port: for the GPU as well */
		return Render_drawLineSeg(render, seg);
	}

	/* wall-aligned: a world line along x (bits 19/20) or y (bits 21/22) */
	seg->flags |= LINE_CLIP_CEIL;
	if (uflags & 0x180000) {
		int neg = (uflags & 0x80000) ? mirror : !mirror;
		seg->x1 = neg ? x - left : x + left;
		seg->x2 = neg ? x - right : x + right;
		seg->y1 = y;
		seg->y2 = y;
	} else if (uflags & 0x600000) {
		int neg = (uflags & 0x400000) ? mirror : !mirror;
		seg->y1 = neg ? y - left : y + left;
		seg->y2 = neg ? y - right : y + right;
		seg->x1 = x;
		seg->x2 = x;
	}
	return Render_drawLine(render, seg);
}

/* 0x00006e0c Does any wall face the tile's four edges from the current view?
 * Probes each edge with an invisible (mode 0xff) wall-texture sprite; returns
 * 1 as soon as one would have been drawn. */
int Render_tileTouchesWall(Render *render, int tx, int ty)
{
	int x = tx * 64 + 0x20;
	int y = ty * 64 + 0x20;

	if (Render_renderSprite(render, x, y - 0x20, 0x20, 0, 0, 0x140000, 0xff, 0x10000, 0) == 0)
		return 1;
	if (Render_renderSprite(render, x, y + 0x20, 0x20, 0, 0, 0xc0000, 0xff, 0x10000, 0) == 0)
		return 1;
	if (Render_renderSprite(render, x + 0x20, y, 0x20, 0, 0, 0x240000, 0xff, 0x10000, 0) == 0)
		return 1;
	if (Render_renderSprite(render, x - 0x20, y, 0x20, 0, 0, 0x440000, 0xff, 0x10000, 0) == 0)
		return 1;
	return 0;
}
