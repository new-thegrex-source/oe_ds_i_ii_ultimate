/*
 * Game: cinematic cameras, lerp sprites (script-driven sprite movers),
 * transient game sprites (effects, sliding doors, wall decals), player view
 * vectors and small helpers.
 *
 * Cameras follow keyframed Catmull-Rom splines (the MayaCamera of the later
 * id/Fountainhead games). Lerp sprites move map sprites linearly over time and
 * resume the script thread that started them when they arrive.
 */

#include "game.h"

/* 4x4 Catmull-Rom basis (16.16, already halved), rows = t^3, t^2, t, 1. */
#define MOD_CAMERA_SPLINE_BASIS 0x45d8cu /* 16 x s32 */

/* Tile size in world units and the world position of a tile centre. */
#define TILE_SHIFT 6
#define TILE_CENTER(t) ((t) * 64 + 32)

/* Sprite.info bits used here */
#define SPRITE_INFO_HIDDEN      0x10000u
#define SPRITE_INFO_FRAME_MASK  0xf00u
#define SPRITE_INFO_FLIP_X      0x1000000u
#define SPRITE_INFO_FULLBRIGHT  0x2000000u /* ? set on effect sprites */
#define SPRITE_INFO_AUTOMAP     0x8000000u /* seen on the automap */
#define SPRITE_INFO_FLIP_Y      0x20000000u
#define SPRITE_INFO_ANIMATED    0x80000000u

/* Entity.info bits */
#define ENT_INFO_LERPING        0x8000000u
#define ENT_INFO_MOVING         0x400000u

/* GameSprite.flags */
#define GS_INUSE      0x01
#define GS_ANIM       0x02
#define GS_LERP       0x04
#define GS_FOLLOWVIEW 0x08
#define GS_SCALE      0x10
#define GS_NORELINK   0x20
#define GS_KEEPDRAWN  0x40 /* don't expire until drawn in the current frame */

/* GameSprite.type */
#define GS_TYPE_EFFECT     0
#define GS_TYPE_ENTITY     1 /* ? data = Entity */
#define GS_TYPE_LINE       2 /* data = Line, clears its 0x40000 flag when done */
#define GS_TYPE_ANIMMODE   3 /* render mode follows the anim frame */
#define GS_TYPE_DOOR_OPEN  4 /* door sinking/opening; becomes type 5 when done */
#define GS_TYPE_DOOR_SLIDE 5 /* door sliding into the wall */

/* Wrap-around 32-bit helpers matching ARM integer behaviour. */
static inline int32_t shl32(int32_t v, int n) { return (int32_t)((uint32_t)v << n); }
static inline int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

static inline Sprite *spriteFromHandle(int handle)
{
	/* lerp sprites and entities index sprites from 1 */
	return &g_app->render->sprites[handle - 1];
}

/* 0x0000216c */
void Camera_initKeys(Camera *cam, int loop)
{
	Game *game = g_app->game;
	CameraKey *first = &game->cameraKeys[cam->firstKey];

	cam->keyDone = 0;
	for (int i = 0; i < cam->numKeys; i++) {
		CameraKey *key = &first[i];

		/* keys are authored in tiles; convert to world units (tile centre) */
		key->x = TILE_CENTER(key->x);
		key->y = TILE_CENTER(key->y);

		if (i < cam->numKeys - 1)
			key->next = &first[i + 1];
		else
			key->next = loop ? first : NULL;

		if (i > 0)
			key->prev = &first[i - 1];
		else
			key->prev = loop ? &first[cam->numKeys - 1] : NULL;
	}
}

/* 0x0000227c
 * Square root of the 64-bit value (hi:lo) << 8 by Newton iteration starting at
 * 0x100: four unrolled steps, then up to twelve more until it converges. */
int64_t Game_FixedSqrt(Game *self, int lo, int hi)
{
	if (hi == 0 && lo == 0)
		return 0;

	int64_t value = (int64_t)((((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo) << 8);
	int64_t x = 0x100;

	for (int i = 0; i < 4; i++)
		x = (value / x + x) >> 1;

	for (int i = 0; i < 12; i++) {
		int64_t next = (value / x + x) >> 1;
		if (next == x)
			break;
		x = next;
	}
	return x;
}

/* 0x000024c8
 * Spline through angles (0..0x3ff): unwrap each control point relative to the
 * previous one so the curve takes the short way round. */
int Camera_splineAngle(Camera *cam, int t, int a0, int a1, int a2, int a3)
{
	if (a1 - a0 > 0x200)
		a1 -= 0x400;
	else if (a1 - a0 < -0x200)
		a1 += 0x400;

	if (a2 - a1 > 0x200)
		a2 -= 0x400;
	else if (a2 - a1 < -0x200)
		a2 += 0x400;

	if (a3 - a2 > 0x200)
		a3 -= 0x400;
	else if (a3 - a2 < -0x200)
		a3 += 0x400;

	int angle = Camera_spline(cam, t, a0, a1, a2, a3);
	if (angle > 0x200)
		angle -= 0x400;
	return angle;
}

/* 0x00002544
 * Catmull-Rom segment between p1 and p2; t is 16.16 in [0,1). */
int Camera_spline(Camera *cam, int t, int p0, int p1, int p2, int p3)
{
	int32_t powers[4];
	int64_t coef[4] = {0, 0, 0, 0};
	int32_t basis[16];
	int32_t pts[4];

	powers[2] = t;
	powers[1] = (int32_t)(((int64_t)t * t) >> 16);
	powers[0] = (int32_t)(((int64_t)powers[1] * t) >> 16);
	powers[3] = 0x10000;

	for (int i = 0; i < 16; i++)
		basis[i] = ModData_s32(&g_mod, MOD_CAMERA_SPLINE_BASIS + i * 4);

	pts[0] = shl32(p0, 16);
	pts[1] = shl32(p1, 16);
	pts[2] = shl32(p2, 16);
	pts[3] = shl32(p3, 16);

	/* coefficients accumulate one control point at a time, like the original */
	for (int p = 0; p < 4; p++)
		for (int row = 0; row < 4; row++)
			coef[row] += (int64_t)pts[p] * basis[row * 4 + p];
	for (int row = 0; row < 4; row++)
		coef[row] >>= 16;

	int64_t sum = 0;
	for (int row = 0; row < 4; row++)
		sum += (int64_t)((uint64_t)coef[row] * (uint64_t)(int64_t)powers[row]);

	return (int)(sum >> 32);
}

/* 0x00002ee8 */
int Camera_apply(Camera *cam)
{
	Render_render(g_app->render, cam->x, cam->y, cam->z, cam->angle);
	return Canvas_updateFade(g_app->canvas);
}

/* 0x0000359c
 * Jump straight to the end of keyframe `key` and resume the waiting script. */
void Camera_snapToKey(Camera *cam, int key)
{
	if (cam->curKey == key && cam->keyDone)
		return;

	cam->curKey = key;
	CameraKey *k = &g_app->game->cameraKeys[cam->firstKey + key];
	if (k->next)
		k = k->next;

	cam->x = k->x;
	cam->y = k->y;
	cam->z = k->z;
	cam->angle = k->angle;
	ScriptThread_run(cam->thread);
	cam->keyDone = 1;
}

/* 0x00003648
 * Cache the player's view position and direction vectors (the familiar's
 * owner stays at the saved view while the familiar walks around). */
void Game_updatePlayerVars(Game *game)
{
	Canvas *cv = g_app->canvas;
	int32_t *sinTable = g_app->render->sinTable;
	uint32_t angle;

	if (!g_app->player->isFamiliar) {
		game->viewX = cv->viewX;
		game->viewY = cv->viewY;
		game->viewZ = cv->viewAngle; /* +0xbbcc holds the current angle */
		game->destX = cv->destX;
		game->destY = cv->destY;
		angle = (uint32_t)cv->destAngle;
	} else {
		game->viewX = game->destX = cv->savedViewX;
		game->viewY = game->destY = cv->savedViewY;
		angle = (uint32_t)cv->savedViewAngle;
		game->viewZ = (int32_t)angle;
	}
	game->viewAngle = (int32_t)angle;

	int32_t s = sinTable[angle & 0x3ff];
	int32_t c = sinTable[(angle + 0x100) & 0x3ff];
	game->viewSin = s;
	game->viewCos = c;
	game->viewStepY = mul32(s, -0x40) >> 16;
	game->viewStepX = shl32(c, 6) >> 16;
	game->viewRightStepX = shl32(sinTable[(angle + 0x200) & 0x3ff], 6) >> 16;
	game->viewRightStepY = mul32(c, -0x40) >> 16;
}

/* 0x00003798
 * Interpolate the camera along keyframe `key` at `time` ms into it. When the
 * key's duration has passed, resume the script thread waiting on it. */
void Camera_update(Camera *cam, int key, int time)
{
	if (cam->curKey == key && cam->keyDone)
		return;

	cam->keyDone = 0;
	cam->curKey = key;

	CameraKey *k = &g_app->game->cameraKeys[cam->firstKey + key];
	if (time >= k->duration) {
		if (cam->thread)
			ScriptThread_run(cam->thread);
		cam->keyDone = 1;
		return;
	}

	CameraKey *prev = k->prev ? k->prev : k;
	CameraKey *next = k;
	CameraKey *next2 = k;
	int t = 0;

	if (k->next) {
		next = k->next;
		next2 = next->next ? next->next : next;

		/* t = time / duration in 16.16, computed as a 64-bit divide */
		int64_t num = (int64_t)shl32(time, 16) * 65536;
		int64_t den = (int64_t)shl32(k->duration, 16);
		t = (int)(num / den);
	}

	cam->x = Camera_spline(cam, t, prev->x, k->x, next->x, next2->x);
	cam->y = Camera_spline(cam, t, prev->y, k->y, next->y, next2->y);
	cam->z = Camera_spline(cam, t, prev->z, k->z, next->z, next2->z);
	cam->angle = Camera_splineAngle(cam, t, prev->angle, k->angle, next->angle, next2->angle);
}

/* 0x000072fc
 * Find the mover already driving `sprite` for this thread, or claim a free one. */
LerpSprite *Game_allocLerpSprite(Game *game, ScriptThread *thread, int sprite)
{
	int handle = sprite + 1;

	for (int i = 0; i < 16; i++) {
		LerpSprite *ls = &game->lerpSprites[i];
		if (ls->hSprite == handle && ls->thread == thread)
			return ls;
		if (ls->hSprite == 0) {
			ls->thread = thread;
			ls->hSprite = handle;
			game->numLerpSprites++;
			return ls;
		}
	}
	return NULL;
}

/* 0x00008b1c */
void LerpSprite_calcDist(LerpSprite *ls)
{
	int dx = ls->dstX - ls->srcX;
	int dy = ls->dstY - ls->srcY;
	int distSq256 = shl32(mul32(dy, dy) + mul32(dx, dx), 8);

	/* the original passes Applet.textUtil as the unused first argument */
	ls->dist = (int32_t)(Game_FixedSqrt(g_app->game, distSq256, distSq256 >> 31) >> 8);
}

/* 0x00009638
 * Used on the dialog owner thread when a dialog is closed. */
void Counter_dec(ScriptThread *thread)
{
	if (thread->numFrames > 0)
		thread->numFrames--;
}

/* 0x0001b9b0 */
void Game_freeLerpSprite(Game *game, LerpSprite *ls)
{
	ls->hSprite = 0;
	game->numLerpSprites--;
}

/* 0x0001c908
 * Script op: mark walls and sprites inside the tile rect as seen on the
 * automap. */
void Game_revealAutomapRect(Game *game, int x, int y, int w, int h)
{
	Render *r = g_app->render;
	int x2 = x + w;
	int y2 = y + h;

	int numLines = r->numLines;
	for (int i = 0; i < numLines; i++) {
		Line *ln = &r->lines[i];
		uint32_t flags = ln->flags;
		if (flags & 0x20)
			continue; /* never shown on the automap */

		int mx = (ln->x1 + ((ln->x2 - ln->x1) >> 1)) >> TILE_SHIFT;
		int my = (ln->y1 + ((ln->y2 - ln->y1) >> 1)) >> TILE_SHIFT;
		if (mx >= x && mx < x2 && my >= y && my < y2)
			ln->flags = flags | 0x80;
	}

	int numSprites = r->numMapSprites;
	for (int i = 0; i < numSprites; i++) {
		Sprite *spr = &r->sprites[i];
		int sx = spr->x >> TILE_SHIFT;
		int sy = spr->y >> TILE_SHIFT;
		if (sx >= x && sx < x2 && sy >= y && sy < y2)
			spr->info |= SPRITE_INFO_AUTOMAP;
	}

	/* The original's tile loop computes the tile index once before the loops
	 * and never advances it, so only the rect's first tile (x,y) is updated. */
	if (w > 0 && h > 0) {
		uint16_t *tile = &r->mapFlags[x + y * 32];
		if (*tile & 8)
			*tile = (uint16_t)((*tile & 0xff) | 0x20);
	}
}

/* 0x0001cb40
 * One-shot animated effect sprite (3 frames, 150 ms each, gone after 450 ms),
 * with per-tile render settings. */
GameSprite *Game_gspriteAllocAnim(Game *game, int tileNum, int x, int y, int z)
{
	GameSprite *gs = Game_gspriteAlloc(game, GS_TYPE_EFFECT, tileNum, 0, GS_ANIM);
	if (!gs)
		return NULL; /* the original would dereference NULL here */

	gs->duration = 150;
	gs->f10 = 450;
	gs->travelTime = 450;
	gs->start[0] = gs->sprite->x = x;
	gs->start[1] = gs->sprite->y = y;
	gs->start[2] = gs->sprite->z = z;
	Vec3_copy(gs->dest, gs->start);

	switch (tileNum) {
	case 0xc0:
	case 0xc6:
		gs->sprite->renderMode = 4;
		gs->f10 = 600;
		gs->duration = 150;
		gs->travelTime = 600;
		break;
	case 0xf2:
	case 0xf3:
	case 0xf4:
		gs->sprite->renderMode = 4;
		break;
	case 0xf5:
	case 0xf6:
	case 0xf7:
	case 0xf8:
	case 0xf9:
	case 0xfd:
		gs->sprite->renderMode = 2;
		gs->sprite->info |= SPRITE_INFO_FULLBRIGHT;
		break;
	case 0xfa:
		gs->sprite->info |= SPRITE_INFO_FULLBRIGHT;
		break;
	case 0xfb:
		/* follows the view, rises to z 16 and grows from 1.0 to 1.5 */
		gs->sprite->info |= SPRITE_INFO_FULLBRIGHT;
		gs->sprite->renderMode = 4;
		gs->flags |= GS_LERP | GS_FOLLOWVIEW | GS_SCALE;
		gs->startScale = 0x40;
		gs->destScale = 0x60;
		gs->scaleStep = 32000 / gs->travelTime;
		gs->dest[2] = 0x10;
		gs->vel[2] = (0x10 - gs->start[2]) * 1000 / gs->travelTime;
		break;
	default:
		break;
	}

	Render_relinkSprite(g_app->render, gs->sprite);
	return gs;
}

/* 0x0001cd18
 * Claim one of the 48 game sprites (each owns Render.customSprites[i]). */
GameSprite *Game_gspriteAlloc(Game *game, int type, int tileNum, int n2, int flags)
{
	int i = 0;
	while (i < 48 && (game->gsprites[i].flags & GS_INUSE))
		i++;
	if (i == 48)
		return NULL;

	GameSprite *gs = &game->gsprites[i];
	Vec3_zero(gs->dest);
	Vec3_zero(gs->start);
	Vec3_zero(gs->vel);
	gs->duration = 250;
	gs->travelTime = 0;
	gs->f10 = 0;
	gs->type = (uint8_t)type;
	gs->data = NULL;
	gs->time = g_app->time;
	gs->sprite = g_app->render->customSprites[i];
	gs->sprite->renderMode = 0;
	gs->sprite->scale = 0x40;
	gs->destScale = 0x40;
	gs->startScale = 0x40;
	gs->scaleStep = 0;
	gs->flags = (uint8_t)(flags | GS_INUSE);
	gs->sprite->info = (uint32_t)(tileNum | (n2 << 8));
	if (gs->flags & GS_ANIM)
		gs->sprite->info |= SPRITE_INFO_ANIMATED;
	if (gs->flags & GS_LERP)
		game->activePropagators++;
	game->activeSprites++;
	return gs;
}

/* 0x0001ce60
 * Expire a game sprite. Door sprites chain: an opening door (type 4) turns
 * into a 1 s slide (type 5) that finally unlinks the door tile. */
void Game_gspriteDestroy(Game *game, GameSprite *gs)
{
	gs->sprite->info |= SPRITE_INFO_HIDDEN;
	gs->flags &= ~GS_INUSE;

	switch (gs->type) {
	case GS_TYPE_ENTITY:
		break;
	case GS_TYPE_LINE: {
		Line *line = (Line *)gs->data;
		line->flags &= ~0x40000u;
		break;
	}
	case GS_TYPE_DOOR_OPEN: {
		Line *line = (Line *)gs->data;
		gs->type = GS_TYPE_DOOR_SLIDE;
		gs->flags |= GS_INUSE;
		gs->sprite->info &= ~SPRITE_INFO_HIDDEN;
		Vec3_copy(gs->start, gs->dest);
		Vec3_zero(gs->vel);
		gs->travelTime = 1000;
		gs->f10 = 1000;
		gs->time = g_app->time;
		if (line->flags & 0x100) {
			gs->dest[0] += 0x40;
			gs->vel[0] = 0x40;
		} else {
			gs->dest[1] -= 0x40;
			gs->vel[1] = -0x40;
		}
		return;
	}
	case GS_TYPE_DOOR_SLIDE: {
		Line *line = (Line *)gs->data;
		Game_unlinkPlayerFromTile(game,
			((line->x2 >> 6) + (line->x1 >> 6)) >> 1,
			((line->y1 >> 6) + (line->y2 >> 6)) >> 1);
		break;
	}
	default:
		return;
	}
	Canvas_updateFacing(g_app->canvas);
}

/* 0x0001cf9c
 * Per-frame update of all game sprites: expiry, animation frame, movement
 * (absolute or following the view) and scaling. */
void Game_gspriteUpdateAll(Game *game)
{
	Canvas *cv = g_app->canvas;

	if (game->activeSprites == 0)
		return;

	game->activeSprites = 0;
	game->activePropagators = 0;

	for (int i = 0; i < 48; i++) {
		GameSprite *gs = &game->gsprites[i];
		if (!(gs->flags & GS_INUSE))
			continue;

		game->activeSprites++;
		int elapsed = g_app->time - gs->time;

		if (gs->f10 > 0 && elapsed >= gs->f10) {
			if (!(gs->flags & GS_KEEPDRAWN) || gs->sprite->frameCount != g_app->render->frameCount)
				Game_gspriteDestroy(game, gs);
			continue;
		}

		if (gs->flags & GS_ANIM) {
			int frame = (elapsed / gs->duration) % 3 & 0xff;
			gs->sprite->info = (gs->sprite->info & ~SPRITE_INFO_FRAME_MASK) | ((uint32_t)frame << 8);
			if (gs->type == GS_TYPE_ANIMMODE)
				gs->sprite->renderMode = frame + 4;
		}

		if (gs->flags & GS_FOLLOWVIEW) {
			Sprite *spr = gs->sprite;
			int oldTileX = spr->x >> TILE_SHIFT;
			int oldTileY = spr->y >> TILE_SHIFT;

			spr->x = cv->viewX + cv->viewStepX;
			spr->y = cv->viewY + cv->viewStepY;
			if (!(gs->flags & GS_LERP)) {
				spr->z = cv->viewZ;
			} else if (elapsed > gs->travelTime) {
				spr->z = cv->viewZ + (0x24 - gs->dest[2]);
				gs->flags &= ~GS_LERP;
			} else {
				spr->z = mul32(elapsed, gs->vel[2]) / 1000 + cv->viewZ + (0x24 - gs->start[2]);
			}

			if (!(gs->flags & GS_NORELINK) &&
			    (oldTileX != spr->x >> TILE_SHIFT || oldTileY != spr->y >> TILE_SHIFT))
				Render_relinkSprite(g_app->render, spr);
		} else if (gs->flags & GS_LERP) {
			game->activePropagators++;
			if (elapsed >= gs->travelTime) {
				/* arrived (the original does not relink on arrival) */
				gs->sprite->x = gs->dest[0];
				gs->sprite->y = gs->dest[1];
				gs->sprite->z = gs->dest[2];
				gs->flags &= ~GS_LERP;
			} else {
				gs->sprite->x = mul32(elapsed, gs->vel[0]) / 1000 + gs->start[0];
				gs->sprite->y = mul32(elapsed, gs->vel[1]) / 1000 + gs->start[1];
				gs->sprite->z = mul32(elapsed, gs->vel[2]) / 1000 + gs->start[2];
				if (!(gs->flags & GS_NORELINK))
					Render_relinkSprite(g_app->render, gs->sprite);
			}
		}

		if (gs->flags & GS_SCALE) {
			if (elapsed > gs->travelTime) {
				gs->sprite->scale = gs->destScale;
				gs->flags &= ~GS_SCALE;
			} else {
				gs->sprite->scale = (mul32(elapsed, gs->scaleStep) / 1000 + gs->startScale) & 0xff;
			}
		}
	}
}

/* 0x000246e4 */
int Game_isCinematicActive(Game *game)
{
	return game->cinematicActive != 0 && game->activeCamera != NULL;
}

/* 0x0003c6b4
 * Fast-forward the running cinematic: finish all movers, expire the camera
 * thread's wait and snap the camera key, until the script ends the skip. */
void Game_skipCinematic(Game *game)
{
	game->skippingCinematic = 1;
	do {
		Game_snapLerpSprites(game);
		Timer_expired(&game->activeCamera->thread->unpauseTime, g_app->playTime + 0x40000000);
		Camera_snapToKey(game->activeCamera, game->cameraKeyIdx);
	} while (game->skippingCinematic);

	int state = g_app->canvas->state;
	if (state != 0x11)
		Canvas_updateMovement(g_app->canvas, state, 0); /* 3rd arg is stale r2 in the original */
}

/* Move a mover-controlled entity's monster state to (x,y) and keep the entity
 * linked to the tile its sprite now occupies. Returns 1 if it changed tile. */
static int relinkSpriteEntity(Game *game, Sprite *spr)
{
	Entity *ent = spr->ent;
	int link = ent->linkIndex;
	int linkY = link / 32;
	int linkX = link - linkY * 32;

	if ((spr->x >> TILE_SHIFT) == linkX && (spr->y >> TILE_SHIFT) == linkY)
		return 0;

	Game_unlinkEntity_43710(game, ent);
	Game_linkEntity(game, spr->ent, spr->x >> TILE_SHIFT, spr->y >> TILE_SHIFT);
	return 1;
}

/* 0x0003c73c
 * Finish every lerp sprite immediately, then resume each distinct waiting
 * (non-async) thread once. */
void Game_snapLerpSprites(Game *game)
{
	ScriptThread *waiting[16];
	int numWaiting = 0;

	if (game->numLerpSprites == 0)
		return;

	for (int i = 0; i < 16; i++) {
		LerpSprite *ls = &game->lerpSprites[i];
		if (ls->hSprite == 0)
			continue;

		Sprite *spr = spriteFromHandle(ls->hSprite);
		spr->x = ls->dstX;
		spr->y = ls->dstY;
		spr->z = ls->dstZ;
		spr->scale = ls->dstScale;
		Render_relinkSprite(g_app->render, spr);

		if (spr->ent && !(spr->info & SPRITE_INFO_HIDDEN)) {
			EntityMonster *mon = spr->ent->monster;
			if (mon) {
				mon->ce.x = mon->destX = ls->dstX;
				mon->ce.y = mon->destY = ls->dstY;
			}
			relinkSpriteEntity(game, spr);
		}

		if (!(ls->flags & 1)) {
			int j = 0;
			while (j < numWaiting && waiting[j] != ls->thread)
				j++;
			if (j == numWaiting)
				waiting[numWaiting++] = ls->thread;
		}
		Game_freeLerpSprite(game, ls);
	}

	for (int j = 0; j < numWaiting; j++)
		ScriptThread_run(waiting[j]);
}

/* 0x0003cba4
 * Decal on one face of a wall next to tile (tx,ty); lasts 30 s. */
GameSprite *Game_spawnWallSprite(Game *game, int tx, int ty, int side, int a, int b)
{
	int x = TILE_CENTER(tx);
	int y;

	switch (side) {
	case 0x80000: /* north face */
		y = ty * 64 + 1;
		break;
	case 0x100000: /* south face */
		y = ty * 64 + 63;
		break;
	case 0x200000: /* west face */
		x = tx * 64 + 1;
		y = TILE_CENTER(ty);
		break;
	case 0x400000: /* east face */
		x = tx * 64 + 63;
		y = TILE_CENTER(ty);
		break;
	default:
		return NULL;
	}

	GameSprite *gs = Game_gspriteAlloc(g_app->game, GS_TYPE_EFFECT, a, b, GS_KEEPDRAWN);
	if (!gs)
		return NULL; /* the original would dereference NULL here */

	gs->sprite->x = x;
	gs->sprite->y = y;
	gs->sprite->z = 0x20;
	gs->sprite->info |= (uint32_t)side | 0x84800000u;
	if (Random_nextByte(&g_app->random) & 1)
		gs->sprite->info ^= SPRITE_INFO_FLIP_X;
	if (Random_nextByte(&g_app->random) & 1)
		gs->sprite->info ^= SPRITE_INFO_FLIP_Y;
	gs->sprite->renderMode = 0xb;
	gs->f10 = 30000;
	gs->travelTime = 30000;
	Render_relinkSprite(g_app->render, gs->sprite);
	return gs;
}

/* 0x0003d4fc
 * Put a decal on every wall face around tile (tx,ty) (neighbour has map
 * flag 4 = solid wall). */
void Game_spawnWallSpritesAround(Game *game, int tx, int ty, int tile, int arg)
{
	uint16_t *flags = g_app->render->mapFlags;
	int idx = tx + ty * 32;

	if (tx > 0 && (flags[idx - 1] & 4))
		Game_spawnWallSprite(game, tx, ty, 0x200000, tile, arg);
	if (tx < 31 && (flags[idx + 1] & 4))
		Game_spawnWallSprite(game, tx, ty, 0x400000, tile, arg);
	if (ty > 0 && (flags[idx - 32] & 4))
		Game_spawnWallSprite(game, tx, ty, 0x80000, tile, arg);
	if (ty < 31 && (flags[idx + 32] & 4))
		Game_spawnWallSprite(game, tx, ty, 0x100000, tile, arg);
}

/* Linear interpolation used by the movers: t is 0..256 (8 bits). */
static inline int moverLerp(int from, int to, int t)
{
	return from + (mul32(t, shl32(to - from, 8)) >> 16);
}

/* 0x00043e70
 * Advance one lerp sprite. Returns bit 0 when it finished (and was freed),
 * bit 1 when its entity changed tile. */
int Game_updateMover(Game *game, LerpSprite *mv)
{
	int result = 0;

	if (mv->hSprite == 0)
		return 0;

	Sprite *spr = spriteFromHandle(mv->hSprite);
	int elapsed = g_app->playTime - mv->startTime;

	if (elapsed >= mv->travelTime) {
		spr->x = mv->dstX;
		spr->y = mv->dstY;
		spr->z = mv->dstZ;
		spr->scale = mv->dstScale;
		Render_relinkSprite(g_app->render, spr);

		if (spr->ent) {
			spr->ent->info &= ~ENT_INFO_LERPING;
			if (!(spr->info & SPRITE_INFO_HIDDEN)) {
				EntityMonster *mon = spr->ent->monster;
				if (mon) {
					mon->ce.x = mon->destX = mv->dstX;
					mon->ce.y = mon->destY = mv->dstY;
					spr->ent->info &= ~ENT_INFO_MOVING;
					spr->ent->monster->frameTime = g_app->time;
				}
				if (relinkSpriteEntity(game, spr))
					result = 2;
			}
			EntityDef *def = spr->ent->def;
			if (def->eType == 7 && def->eSubType == 4)
				spr->info |= SPRITE_INFO_ANIMATED;
		}
		Game_freeLerpSprite(game, mv);
		return result | 1;
	}

	int t = 0;
	if (mv->travelTime != 0)
		t = shl32(elapsed, 16) / shl32(mv->travelTime, 8);

	spr->x = moverLerp(mv->srcX, mv->dstX, t);
	spr->y = moverLerp(mv->srcY, mv->dstY, t);
	spr->z = moverLerp(mv->srcZ, mv->dstZ, t);
	spr->scale = moverLerp(mv->srcScale, mv->dstScale, t) & 0xff;
	Render_relinkSprite(g_app->render, spr);

	Entity *ent = spr->ent;
	if (ent && !(spr->info & SPRITE_INFO_HIDDEN)) {
		uint32_t info = spr->info;
		int tileX = spr->x >> TILE_SHIFT;
		int tileY = spr->y >> TILE_SHIFT;
		int link = ent->linkIndex;
		int linkY = link / 32;
		int linkX = link - linkY * 32;

		if (!(info & SPRITE_INFO_ANIMATED)) {
			/* walk cycle from the distance travelled: 4 phases per 0x1000 */
			int curFrame = (int)((info & SPRITE_INFO_FRAME_MASK) >> 8);
			uint32_t phase = ((uint32_t)mul32(t, mv->dist) << 18) >> 30;
			int walk = 0;

			if (ent->def->eType == 2) {
				if (curFrame == 0 || curFrame == 1) {
					info = (info & ~SPRITE_INFO_FRAME_MASK) | ((phase != 0 && phase != 2) ? 0x100u : 0);
					walk = 1;
				}
			} else if (ent->def->eType == 3) {
				info = (info & ~SPRITE_INFO_FRAME_MASK) | ((phase == 0 || phase == 2) ? 0x100u : 0x200u);
				walk = 1;
			}
			if (walk) {
				spr->info = info;
				if (phase > 1)
					info |= SPRITE_INFO_FLIP_X;
				else
					info &= ~SPRITE_INFO_FLIP_X;
				spr->info = info;
			}
		}

		if (tileX != linkX || tileY != linkY) {
			Game_unlinkEntity_43710(game, ent);
			Game_linkEntity(game, spr->ent, spr->x >> TILE_SHIFT, spr->y >> TILE_SHIFT);
			result = 2;
			EntityMonster *mon = spr->ent->monster;
			if (mon) {
				mon->ce.x = mon->destX = TILE_CENTER(tileX);
				mon->ce.y = mon->destY = TILE_CENTER(tileY);
				spr->ent->info &= ~ENT_INFO_MOVING;
			}
		}
	}

	/* tile 0xaa: two-frame bobbing sprite (frame 1 low, frame 0 raised by 3) */
	if ((spr->info & 0xff) == 0xaa) {
		uint32_t phase = ((uint32_t)mul32(t, mv->dist) << 19) >> 30;
		uint32_t info = spr->info & ~SPRITE_INFO_FRAME_MASK;
		if (phase <= 1)
			info |= SPRITE_INFO_ANIMATED | 0x100;
		else
			info |= SPRITE_INFO_ANIMATED;
		spr->info = info;
		spr->z = phase > 1 ? mv->dstZ + 3 : mv->dstZ;
	}
	return result;
}

/* 0x00044278
 * Advance all lerp sprites; resume each distinct thread whose (non-async)
 * mover finished, and refresh the facing entity if anything changed tile. */
void Game_updateMovers(Game *game)
{
	ScriptThread *waiting[16];
	int numWaiting = 0;
	int changedTile = 0;

	if (game->numLerpSprites == 0)
		return;

	for (int i = 0; i < 16; i++) {
		LerpSprite *ls = &game->lerpSprites[i];
		ScriptThread *thread = ls->thread;
		int flags = ls->flags;
		int res = Game_updateMover(game, ls);

		if ((res & 1) && !(flags & 1)) {
			int j = 0;
			while (j < numWaiting && waiting[j] != thread)
				j++;
			if (j == numWaiting)
				waiting[numWaiting++] = thread;
		}
		if (res & 2)
			changedTile = 1;
	}

	for (int j = 0; j < numWaiting; j++)
		ScriptThread_run(waiting[j]);
	if (changedTile)
		Canvas_updateFacing(g_app->canvas);
}

/* 0x00044c30
 * Four-slot set of door lines to close when the player warps (-1 = free):
 * add puts v in the first free slot, remove clears the slot holding v. */
void Game_setTrackedSlot(Game *game, int v, int add)
{
	for (int i = 0; i < 4; i++) {
		if (add) {
			if (game->trackedSlots[i] == -1) {
				game->trackedSlots[i] = v;
				return;
			}
		} else if (game->trackedSlots[i] == v) {
			game->trackedSlots[i] = -1;
			return;
		}
	}
}
