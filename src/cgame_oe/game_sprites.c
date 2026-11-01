/*
 * Game: transient "game sprites" (GameSprite, 24 slots at Game+0x6dc0) and
 * animated doors.
 *
 * A game sprite borrows one of the 24 custom sprite slots the map loader
 * reserved in Render (Render.customSprites). It can animate through three
 * frames, move linearly from start to dest, follow the view, and ramp its
 * scale; it is destroyed (sprite hidden) once its lifetime runs out.
 * Doors use them for the sliding/sinking door panel.
 */

#include "game.h"

/* Error text when every slot is taken (App_setErrorStr). */
#define MOD_STR_MAX_CUSTOM_SPRITES 0x30458u

#define NUM_GSPRITES 24

/* GameSprite.flags */
#define GS_INUSE      0x01
#define GS_ANIM       0x02
#define GS_LERP       0x04
#define GS_FOLLOWVIEW 0x08
#define GS_SCALE      0x10
#define GS_NORELINK   0x20

/* GameSprite.type */
#define GS_TYPE_EFFECT     0
#define GS_TYPE_DOOR_OPEN  1 /* data = Line */
#define GS_TYPE_DOOR_CLOSE 2 /* data = Line; clears the line's 0x40000 flag when done */

/* Sprite.info bits */
#define SPRITE_HIDDEN     0x10000u
#define SPRITE_WALLTEX    0x40000u    /* tile is a wall texture */
#define SPRITE_SIDE_MASK  0x780000u   /* wall-side orientation bits */
#define SPRITE_SIDE_X     0x80000u
#define SPRITE_SIDE_Y     0x400000u
#define SPRITE_BRIGHT     0x2000000u  /* ? effect sprites */
#define SPRITE_NOCLIP     0x4000000u  /* ? set on door panels */
#define SPRITE_ANIMATED   0x80000000u

/* Line.flags bits */
#define LINE_OPEN     0x40u
#define LINE_AXIS_X   0x100u
#define LINE_AXIS_Y   0x200u
#define LINE_LOCKED   0x400u
#define LINE_OPENED   0x40000u /* opened by script op 0xf; cleared when a closing panel finishes */

/* Entity.info bits */
#define ENT_LINKED 0x4000000u

static inline void relinkSprite(Render *render, Sprite *sprite, int x, int y)
{
	/* Render_relinkSpriteAt takes the sprite as a pointer to its x field. */
	Render_relinkSpriteAt(render, &sprite->x, x, y);
}

/* 0x00013248 */
void *Game_gspriteAllocAnim(Game *game, int tile, int x, int y, int z)
{
	GameSprite *gs = Game_gspriteAlloc(game, GS_TYPE_EFFECT, tile, 0, GS_ANIM);
	if (gs == NULL)
		return NULL; /* the original carried on with a null slot (error already raised) */

	/* three 150 ms frames */
	gs->frameDuration = 150;
	gs->lifetime = 450;
	gs->lerpDuration = 450;
	gs->start[0] = gs->sprite->x = x;
	gs->start[1] = gs->sprite->y = y;
	gs->start[2] = gs->sprite->z = z;
	gs->dest[0] = gs->start[0];
	gs->dest[1] = gs->start[1];
	gs->dest[2] = gs->start[2];

	switch (tile) {
	case 0xf6:
		gs->sprite->renderMode = 7;
		break;
	case 0xec: {
		/* grows from 1/4 to 1.5x just above the floor */
		gs->sprite->renderMode = 7;
		gs->startScale = 0x10;
		gs->destScale = 0x60;
		gs->lifetime = 400;
		gs->scaleRate = 200;
		gs->lerpDuration = 400;
		int fz = game->render->floorZ + 5;
		gs->dest[2] = fz;
		gs->start[2] = fz;
		gs->sprite->z = fz;
		gs->flags |= GS_SCALE;
		break;
	}
	case 0xf4: {
		/* grows to 1.5x while sinking to z 0x10 */
		gs->sprite->info |= SPRITE_BRIGHT;
		gs->sprite->renderMode = 7;
		gs->flags |= GS_LERP | GS_FOLLOWVIEW | GS_SCALE;
		gs->startScale = 0x40;
		gs->destScale = 0x60;
		int dur = gs->lerpDuration;
		gs->scaleRate = 32000 / dur;
		gs->dest[2] = 0x10;
		gs->vel[2] = ((0x10 - gs->start[2]) * 1000) / dur;
		break;
	}
	case 0xf5:
		gs->sprite->renderMode = 0xf;
		break;
	case 0xc0:
	case 0xc6:
		gs->sprite->renderMode = 7;
		gs->lifetime = 600;
		gs->frameDuration = 150;
		gs->lerpDuration = 600;
		break;
	case 0xeb: {
		/* puff: grows 0.5x -> 2.5x while drifting 15 units down */
		gs->sprite->renderMode = 7;
		gs->startScale = 0x20;
		gs->destScale = 0xa0;
		int fz = game->render->floorZ + 0x20;
		gs->dest[2] = fz;
		gs->start[2] = fz;
		gs->sprite->z = fz;
		int startZ = gs->start[2];
		gs->lifetime = 400;
		int destZ = startZ - 15;
		gs->dest[2] = destZ;
		gs->lerpDuration = 400;
		gs->scaleRate = ((gs->destScale - gs->startScale) * 1000) / 400;
		gs->vel[2] = ((destZ - startZ) * 1000) / 400;
		gs->flags |= GS_LERP | GS_SCALE;
		break;
	}
	case 0xf7:
	case 0xf8:
	case 0xf9:
	case 0xfa:
	case 0xfb:
		gs->sprite->info |= SPRITE_BRIGHT;
		break;
	default:
		break;
	}

	relinkSprite(game->render, gs->sprite, -1, -1);
	return gs;
}

/* 0x000133f8 */
void *Game_gspriteAlloc(Game *game, int type, int tile, int frame, int flags)
{
	int i;
	for (i = 0; i < NUM_GSPRITES; i++) {
		if (!(game->gsprites[i].flags & GS_INUSE))
			break;
	}
	if (i == NUM_GSPRITES) {
		App_setErrorStr(game->app, (char *)ModData_str(&g_mod, MOD_STR_MAX_CUSTOM_SPRITES));
		App_setErrorCode(g_app, 1);
		return NULL;
	}

	GameSprite *gs = &game->gsprites[i];
	memset(gs->dest, 0, sizeof gs->dest);
	memset(gs->start, 0, sizeof gs->start);
	memset(gs->vel, 0, sizeof gs->vel);
	gs->frameDuration = 250;
	gs->lerpDuration = 0;
	gs->lifetime = 0;
	gs->type = (uint8_t)type;
	gs->time = game->app->tickTime;
	gs->data = NULL;
	gs->sprite = game->render->customSprites[i];
	gs->sprite->renderMode = 0;
	gs->sprite->scale = 0x40;
	gs->destScale = 0x40;
	gs->startScale = 0x40;
	gs->scaleRate = 0;
	gs->flags = (uint8_t)flags | GS_INUSE;
	gs->sprite->info = (uint32_t)(tile | frame << 8);
	if (gs->flags & GS_ANIM)
		gs->sprite->info |= SPRITE_ANIMATED;
	if (gs->flags & GS_LERP)
		game->numMovingGSprites++;
	game->numActiveGSprites++;
	return gs;
}

/* 0x00013518 */
void Game_gspriteDestroy(Game *game, void *gsArg)
{
	GameSprite *gs = gsArg;

	gs->sprite->info |= SPRITE_HIDDEN;
	gs->flags &= ~GS_INUSE;
	if (gs->type == GS_TYPE_DOOR_OPEN) {
		Canvas_updateFacing(game->canvas);
	} else if (gs->type == GS_TYPE_DOOR_CLOSE) {
		/* data is the door Line here (Game_useDoor) */
		Line *line = gs->data;
		line->flags &= ~LINE_OPENED;
		Canvas_updateFacing(game->canvas);
	}
}

/* 0x00013558 */
void Game_gspriteUpdateAll(Game *game)
{
	if (game->numActiveGSprites == 0)
		return;

	game->numActiveGSprites = 0;
	game->numMovingGSprites = 0;
	for (int i = 0; i < NUM_GSPRITES; i++) {
		GameSprite *gs = &game->gsprites[i];
		if (!(gs->flags & GS_INUSE))
			continue;

		game->numActiveGSprites++;
		int elapsed = game->app->tickTime - gs->time;
		if (gs->lifetime > 0 && elapsed >= gs->lifetime) {
			Game_gspriteDestroy(game, gs);
			continue;
		}

		if (gs->flags & GS_ANIM) {
			/* frame = (elapsed / frameDuration) as a byte, mod 3 */
			uint32_t frame = (uint32_t)((elapsed / gs->frameDuration) & 0xff) % 3u;
			gs->sprite->info = (gs->sprite->info & ~0xf00u) | (frame << 8);
			if (gs->type == 4)
				gs->sprite->renderMode = (uint8_t)(frame + 7);
		}

		if (gs->flags & GS_FOLLOWVIEW) {
			Canvas *cv = game->canvas;
			Sprite *s = gs->sprite;
			int oldTx = s->x >> 6;
			int oldTy = s->y >> 6;
			s->x = cv->viewX + cv->stepX;
			s->y = cv->viewY + cv->stepY;
			if (gs->flags & GS_LERP) {
				if (elapsed <= gs->lerpDuration) {
					s->z = (elapsed * gs->vel[2]) / 1000 + cv->viewZ + (0x24 - gs->start[2]);
				} else {
					s->z = cv->viewZ + (0x24 - gs->dest[2]);
					gs->flags &= ~GS_LERP;
				}
			} else {
				s->z = cv->viewZ;
			}
			if (!(gs->flags & GS_NORELINK) && ((s->x >> 6) != oldTx || (s->y >> 6) != oldTy))
				relinkSprite(game->render, s, -1, -1);
		} else if (gs->flags & GS_LERP) {
			game->numMovingGSprites++;
			Sprite *s = gs->sprite;
			if (elapsed < gs->lerpDuration) {
				s->x = (elapsed * gs->vel[0]) / 1000 + gs->start[0];
				s->y = (elapsed * gs->vel[1]) / 1000 + gs->start[1];
				s->z = (elapsed * gs->vel[2]) / 1000 + gs->start[2];
				/* relinked every step while moving (no tile-change test here) */
				if (!(gs->flags & GS_NORELINK))
					relinkSprite(game->render, s, -1, -1);
			} else {
				/* arrived: snap, no relink */
				s->x = gs->dest[0];
				s->y = gs->dest[1];
				s->z = gs->dest[2];
				gs->flags &= ~GS_LERP;
			}
		}

		if (gs->flags & GS_SCALE) {
			if (elapsed > gs->lerpDuration) {
				gs->sprite->scale = gs->destScale;
				gs->flags &= ~GS_SCALE;
			} else {
				gs->sprite->scale = (uint8_t)((elapsed * gs->scaleRate) / 1000 + gs->startScale);
			}
		}
	}
}

/* 0x0001dcfc  op 0xf open / 0x10 close; returns 1 if the door changed state */
int Game_useDoor(Game *game, int op, int lineIdx, int animate)
{
	Render *render = game->render;
	Line *line = &render->lines[lineIdx];
	uint32_t flags = line->flags;

	if (flags & LINE_LOCKED)
		return 0;
	if (op == 0xf && (flags & LINE_OPEN))
		return 0;
	if (op == 0x10 && !(flags & LINE_OPEN))
		return 0;

	line->flags = flags ^ LINE_OPEN;

	/* the door's entity leaves its tile while open */
	for (int i = game->firstDoor; i <= game->lastDoor; i++) {
		Entity *ent = &game->entities[i];
		if ((int)(ent->info & 0xffff) - 1 != lineIdx)
			continue;
		if (line->flags & LINE_OPEN) {
			Game_unlinkEntity_2ae8c(game, ent);
		} else {
			int t = ent->linkIndex;
			Game_linkEntity(game, ent, t % 32, t / 32);
		}
		break;
	}

	if (game->canvas->state != 2)
		Sound_play(game->app->sound, (line->flags & LINE_OPEN) ? 0x13c7 : 0x13c8, 0, 2, -1);

	if (op == 0xf)
		line->flags |= LINE_OPENED;

	if (!animate)
		return 1;

	/* a panel already moving for this door simply reverses */
	GameSprite *gs = NULL;
	for (int i = 0; i < NUM_GSPRITES; i++) {
		if ((game->gsprites[i].flags & GS_INUSE) && game->gsprites[i].data == line) {
			gs = &game->gsprites[i];
			break;
		}
	}

	if (gs == NULL) {
		gs = Game_gspriteAlloc(game, GS_TYPE_EFFECT, (int8_t)line->texture, 0, GS_ANIM | GS_LERP);
		if (gs == NULL)
			return 1; /* the original would have crashed here (error already raised) */
		gs->data = line;

		uint32_t info = gs->sprite->info;
		gs->sprite->info = info | SPRITE_NOCLIP | SPRITE_WALLTEX;
		info = (info & ~SPRITE_SIDE_MASK) | SPRITE_NOCLIP | SPRITE_WALLTEX;
		gs->sprite->info = info;
		if (line->flags & LINE_AXIS_X)
			gs->sprite->info = info | SPRITE_SIDE_X;
		else if (line->flags & LINE_AXIS_Y)
			gs->sprite->info = info | SPRITE_SIDE_Y;

		gs->lerpDuration = 400;
		gs->lifetime = 400;
		gs->frameDuration = 250;
		gs->type = GS_TYPE_DOOR_OPEN;
		gs->flags |= GS_NORELINK;
		gs->start[0] = (line->x1 + line->x2) >> 1;
		gs->start[1] = (line->y1 + line->y2) >> 1;
		gs->start[2] = 0x20;
		gs->dest[0] = gs->start[0];
		gs->dest[1] = gs->start[1];
		gs->dest[2] = gs->start[2];

		if (game->render->ceilZ <= 0x40) {
			/* low ceiling: the panel rises into it */
			gs->dest[2] = 0x60;
			gs->vel[2] = 64000 / gs->lerpDuration;
		} else {
			/* high ceiling: the panel slides sideways one tile */
			gs->lerpDuration = 250;
			gs->lifetime = 250;
			if (line->flags & LINE_AXIS_X) {
				gs->vel[0] = 0x100;
				gs->dest[0] += 0x40;
			} else if (line->flags & LINE_AXIS_Y) {
				gs->vel[1] = 0x100;
				gs->dest[1] += 0x40;
			}
		}

		if (op == 0x10) {
			/* closing: run the same motion backwards */
			for (int k = 0; k < 3; k++) {
				gs->vel[k] = -gs->vel[k];
				int t = gs->dest[k];
				gs->dest[k] = gs->start[k];
				gs->start[k] = t;
			}
			gs->type = GS_TYPE_DOOR_CLOSE;
		}

		gs->sprite->x = gs->start[0];
		gs->sprite->y = gs->start[1];
		gs->sprite->z = gs->start[2];
		if (gs->type != GS_TYPE_DOOR_CLOSE) {
			relinkSprite(game->render, gs->sprite, -1, -1);
			return 1;
		}
	} else {
		for (int k = 0; k < 3; k++) {
			gs->vel[k] = -gs->vel[k];
			int t = gs->dest[k];
			gs->dest[k] = gs->start[k];
			gs->start[k] = t;
		}
		/* mirror the elapsed time so the panel turns around where it is */
		gs->time = (game->app->tickTime * 2 - gs->time) - gs->lerpDuration;
		gs->flags |= GS_NORELINK;
		if (op == 0xf) {
			gs->type = GS_TYPE_DOOR_OPEN;
			relinkSprite(game->render, gs->sprite, -1, -1);
			return 1;
		}
		gs->type = GS_TYPE_DOOR_CLOSE;
	}

	/* a closing panel is linked where it will end up */
	relinkSprite(game->render, gs->sprite, gs->dest[0], gs->dest[1]);
	return 1;
}
