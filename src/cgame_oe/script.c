/*
 * Tile-event script threads, cinematic cameras and lerp sprites.
 *
 * Map events live in Render (loaded with the map):
 *   tileEvents[]  sorted event words, one per tile with events
 *   commands[]    {opcode, arg1, arg2} records
 *   mapFlags[]    bit 8 = the tile owns an event
 *
 * Event word:   bits 0..9 tile (y*32 + x), 10..19 first command, 20..26 command
 *               count, 27..30 current state, bit 31 = skipped when the caller's
 *               trigger flags contain 0x400.
 * Command arg2: low bits trigger mask (ANDed with the frame's flags), 0x100 set
 *               once the command ran, 0x200 one-shot (arg2 cleared after it ran),
 *               bits 16..24 state mask (0x10000 << (state - 1)).
 *
 * Interpreter results: 0 not run, 1 ok, 2 yield (thread suspended), 3 abort the
 * event, 4 restart the event.
 *
 * Cameras are keyframed Catmull-Rom splines (op 0x36 keys); lerp sprites move a
 * map sprite linearly and wake the script thread that started them.
 */

#include "game.h"

/* 4x4 Catmull-Rom basis, 16.16 (already halved), rows t^3, t^2, t, 1. */
#define MOD_CAMERA_SPLINE_BASIS   0x2c748u /* 16 x s32 */
#define MOD_STR_MAX_LERP_SPRITES  0x304b0u

enum {
	SCRIPT_NOT_RUN = 0,
	SCRIPT_OK = 1,
	SCRIPT_YIELD = 2,
	SCRIPT_ABORT = 3,
	SCRIPT_RESTART = 4,
};

#define MAX_SCRIPT_THREADS 16
#define MAX_SCRIPT_FRAMES  8
#define MAX_LERP_SPRITES   16

/* ScriptFrame.frameFlags */
#define FRAME_NESTED    0x01
#define FRAME_SUSPENDED 0x02 /* resume this frame when the child frame finishes */
#define FRAME_NO_RESUME 0x04

/* ScriptThread.state value for a suspended thread */
#define THREAD_PAUSED 2

#define TILEFLAG_HAS_EVENT 0x08

/* Camera key opcodes scanned by Game_loadCameras */
#define OP_CAMERA_DURATION 0x35
#define OP_CAMERA_KEY      0x36
#define OP_CAMERA_LOOP     0x38

/* Entity.info / Sprite.info bits used here */
#define SPRITE_INFO_HIDDEN    0x10000u
#define SPRITE_INFO_FRAME     0xf00u
#define ENT_INFO_MOVING       0x400000u
#define ENT_INFO_LERPING      0x8000000u

/* LerpSprite.flags */
#define LERP_NO_WAKE 0x01

static inline uint32_t event_firstCommand(uint32_t ev) { return (ev & 0xfffff) >> 10; }
static inline uint32_t event_numCommands(uint32_t ev) { return (ev & 0x7ffffff) >> 20; }
static inline uint32_t event_state(uint32_t ev) { return (ev & 0x7fffffff) >> 27; }

/* ---------------------------------------------------------------- threads */

/* 0x0000567c */
void *ScriptThread_pushFrame(ScriptThread *thread, int event, int flags, int nested)
{
	if (thread->numFrames == MAX_SCRIPT_FRAMES)
		return thread; /* the original returned the thread itself when full */

	ScriptFrame *frame = &thread->frames[(int8_t)thread->numFrames++];
	frame->cmdIndex = 0xff; /* ScriptThread_run pre-increments to 0 */
	frame->frameFlags = 0;
	if (nested)
		frame->frameFlags = FRAME_NESTED;
	frame->event = (uint32_t)event;
	frame->flags = (uint32_t)flags;
	return &frame->cmdIndex;
}

/* 0x00005810
 * Resume a thread waiting on op 0x25 (WAIT) once its time has come. */
int ScriptThread_checkWait(ScriptThread *thread, int now)
{
	if (thread->waitTime == 0 || now < thread->waitTime)
		return 0;
	thread->waitTime = 0;
	ScriptThread_run(thread);
	return 1;
}

/* 0x00010414
 * Run the commands of the frame's event from frame->cmdIndex on. */
int ScriptThread_runEvent(ScriptThread *thread, void *framePtr)
{
	ScriptFrame *frame = framePtr;
	uint32_t ev = frame->event;
	uint32_t trigger = frame->flags;
	int ranAny = 0;

	/* bit 31 events are skipped for 0x400 (move/turn/use) triggers */
	if ((ev & 0x80000000u) && (trigger & 0x400))
		return SCRIPT_NOT_RUN;

	uint32_t state = event_state(ev);
	uint32_t stateMask = 0;
	if (state != 0)
		stateMask = 0x10000u << (state - 1);
	if ((trigger & 0x1ffffff) >> 16)
		stateMask |= trigger & 0x1ff0000;

	Render *render = thread->app->render;
	Command *cmds = &render->commands[event_firstCommand(ev)];
	int end = (int)event_numCommands(ev) * 3;

	/* i walks words (3 per command) like the original */
	for (int i = frame->cmdIndex * 3; i < end; i += 3) {
		Command *cmd = &cmds[i / 3];
		int op = cmd->op;
		int arg = cmd->arg1;
		uint32_t cmdFlags = (uint32_t)cmd->arg2;

		if (stateMask == 0) {
			if ((cmdFlags & 0x1ffffff) >> 16)
				continue;
		} else if ((stateMask & cmdFlags) == 0) {
			continue;
		}
		if ((frame->flags & cmdFlags) == 0)
			continue;

		cmd->arg2 = (int32_t)(cmdFlags | 0x100);
		int result = ScriptThread_executeCommand(thread, frame, op, arg);
		if (result == SCRIPT_NOT_RUN)
			continue;
		if (result == SCRIPT_ABORT)
			return SCRIPT_ABORT;
		if (cmdFlags & 0x200)
			cmd->arg2 = 0; /* one-shot */
		if (result == SCRIPT_YIELD) {
			frame->cmdIndex = (uint8_t)(i / 3);
			return SCRIPT_YIELD;
		}
		if (result == SCRIPT_RESTART) {
			/* the loop increment follows: a restart resumes at command 1 */
			frame->cmdIndex = 0;
			i = 0;
		}
		ranAny = 1;
	}
	return ranAny ? SCRIPT_OK : SCRIPT_NOT_RUN;
}

/* 0x00010528 */
int ScriptThread_executeTile(ScriptThread *thread, int tx, int ty, int flags, int extra)
{
	Player_checkCanaries(thread->app->player);

	Game *game = thread->app->game;
	if (game->scriptsDisabled || tx < 0 || tx >= 32 || ty < 0 || ty >= 32) {
		thread->state = 0;
		return 0;
	}

	game->yieldFlag = 0;
	int tile = ty * 32 + tx;
	Render *render = thread->app->render;
	int ev = -1;
	if (render->mapFlags[tile] & TILEFLAG_HAS_EVENT)
		ev = Render_findTileEvent(render, tile);
	if (ev == -1) {
		thread->state = 0;
		return 0;
	}
	ScriptThread_pushFrame(thread, (int)thread->app->render->tileEvents[ev], flags, extra);
	return ScriptThread_run(thread);
}

/* 0x000179f4 */
void ScriptThread_init(ScriptThread *thr)
{
	thr->numFrames = 0;
	thr->waitTime = 0;
	thr->state = THREAD_PAUSED;
}

/* 0x0001fd80
 * Push the tile's event as a suspended frame without running it (the caller
 * resumes it later). */
int ScriptThread_initTileEvent(ScriptThread *thread, int tx, int ty, int a, int b)
{
	Game *game = thread->app->game;
	if (game->scriptsDisabled || tx < 0 || tx >= 32 || ty < 0 || ty >= 32) {
		thread->state = 0;
		return 0;
	}

	game->yieldFlag = 0;
	int tile = ty * 32 + tx;
	Render *render = thread->app->render;
	int ev = -1;
	if (render->mapFlags[tile] & TILEFLAG_HAS_EVENT)
		ev = Render_findTileEvent(render, tile);
	if (ev == -1) {
		thread->state = 0;
		return 0;
	}
	ScriptThread_pushFrame(thread, (int)thread->app->render->tileEvents[ev], a, b);
	thread->frames[(int8_t)thread->numFrames - 1].frameFlags |= FRAME_SUSPENDED;
	return 2;
}

/* 0x0002286c
 * Run (or resume) the top frame. A finished frame is popped; if its parent was
 * suspended waiting for it (and not marked no-resume), the parent resumes. */
int ScriptThread_run(ScriptThread *thread)
{
	if (thread == NULL || (int8_t)thread->numFrames <= 0)
		return 0; /* port guard: the original dereferenced these unchecked */

	Player_checkCanaries(thread->app->player);

	int depth = (int8_t)thread->numFrames;
	ScriptFrame *top = &thread->frames[depth - 1];
	top->cmdIndex++;
	thread->state = (uint8_t)ScriptThread_runEvent(thread, top);

	if (thread->state == THREAD_PAUSED) {
		top->frameFlags |= FRAME_SUSPENDED;
		Player_checkCanaries(thread->app->player);
		return 2;
	}

	thread->numFrames--;
	depth = (int8_t)thread->numFrames;
	if (depth > 0) {
		ScriptFrame *parent = &thread->frames[depth - 1];
		if ((parent->frameFlags & FRAME_SUSPENDED) && !(parent->frameFlags & FRAME_NO_RESUME)) {
			parent->frameFlags &= (uint8_t)~FRAME_SUSPENDED;
			thread->state = (uint8_t)ScriptThread_run(thread);
		}
	}
	Player_checkCanaries(thread->app->player);
	return (int8_t)thread->state;
}

/* 0x00006da8
 * Drop one frame from a thread (dialog owner closed without resuming). */
void Counter_dec(void *threadPtr)
{
	ScriptThread *thread = threadPtr;
	if ((int8_t)thread->numFrames > 0)
		thread->numFrames--;
}

/* 0x00005634 */
void *Game_allocScriptThread(Game *game)
{
	for (int i = 0; i < MAX_SCRIPT_THREADS; i++) {
		ScriptThread *thread = &game->threads[i];
		if (thread->inUse == 0) {
			ScriptThread_init(thread);
			thread->inUse = 1;
			thread->index = (uint8_t)i;
			game->numThreads++;
			return thread;
		}
	}
	return NULL;
}

/* 0x00012940 */
void Game_freeScriptThread(Game *game, void *threadPtr)
{
	ScriptThread *thread = threadPtr;
	thread->inUse = 0;
	game->numThreads--;
}

/* 0x000105d8
 * Run a tile's event in a fresh thread; the thread stays alive only if the
 * event yielded. */
int Game_executeTile(Game *game, int tx, int ty, int flags, int extra)
{
	if (game->scriptsDisabled || tx < 0 || tx >= 32 || ty < 0 || ty >= 32 ||
	    !(game->render->mapFlags[ty * 32 + tx] & TILEFLAG_HAS_EVENT))
		return 0;

	ScriptThread *thread = Game_allocScriptThread(game);
	if (thread == NULL)
		return 0; /* port guard: the original passed NULL on */
	int result = ScriptThread_executeTile(thread, tx, ty, flags, extra);
	if (result != 2)
		Game_freeScriptThread(game, thread);
	return result;
}

/* 0x00022814
 * Wake waiting threads; free every thread that is no longer suspended. */
void Game_runScriptThreads(Game *game)
{
	if (game->numThreads == 0)
		return;

	for (int i = 0; i < MAX_SCRIPT_THREADS; i++) {
		ScriptThread *thread = &game->threads[i];
		if (thread->inUse == 0)
			continue;
		ScriptThread_checkWait(thread, game->app->tickTime);
		if (thread->state != THREAD_PAUSED || thread->numFrames == 0)
			Game_freeScriptThread(game, thread);
	}
}

/* ---------------------------------------------------------------- cameras */

/* 0x00001860
 * Keys are authored in tiles: convert them to world units (tile centre) and
 * chain them, optionally as a loop. */
void Camera_initKeys(Camera *cam, int loop)
{
	cam->keyDone = 0;
	for (int i = 0; i < cam->numKeys; i++) {
		CameraKey *key = &cam->keys[i];

		key->x = key->x * 64 + 32;
		key->y = key->y * 64 + 32;

		if (i < cam->numKeys - 1)
			key->next = &cam->keys[i + 1];
		else
			key->next = loop ? cam->keys : NULL;

		if (i >= 1)
			key->prev = &cam->keys[i - 1];
		else
			key->prev = loop ? &cam->keys[cam->numKeys - 1] : NULL;
	}
}

/* 0x00001b44
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

/* 0x00001ba4
 * Catmull-Rom segment between p1 and p2; t is 16.16 in [0, 1). The original
 * accumulates 64-bit coefficients one control point at a time. */
int Camera_spline(Camera *cam, int t, int p0, int p1, int p2, int p3)
{
	int32_t powers[4];
	int32_t basis[16];
	int64_t coef[4] = { 0, 0, 0, 0 };
	int32_t pts[4];

	powers[3] = 0x10000;
	powers[2] = t;
	powers[1] = (int32_t)(((int64_t)t * t) >> 16);
	powers[0] = (int32_t)(((int64_t)powers[1] * t) >> 16);

	for (int i = 0; i < 16; i++)
		basis[i] = ModData_s32(&g_mod, MOD_CAMERA_SPLINE_BASIS + (uint32_t)i * 4);

	pts[0] = (int32_t)((uint32_t)p0 << 16);
	pts[1] = (int32_t)((uint32_t)p1 << 16);
	pts[2] = (int32_t)((uint32_t)p2 << 16);
	pts[3] = (int32_t)((uint32_t)p3 << 16);

	for (int p = 0; p < 4; p++)
		for (int row = 0; row < 4; row++)
			coef[row] += (int64_t)pts[p] * basis[row * 4 + p];
	for (int row = 0; row < 4; row++)
		coef[row] >>= 16;

	/* 64 x 32 multiplies, low 64 bits kept */
	uint64_t sum = 0;
	for (int row = 0; row < 4; row++)
		sum += (uint64_t)coef[row] * (uint64_t)(int64_t)powers[row];

	return (int)((int64_t)sum >> 32);
}

/* 0x00002544 */
void Camera_apply(Camera *cam)
{
	Applet *app = g_app;
	app->render->noViewOffset = 1;
	Render_render(app->render, cam->x, cam->y, cam->z, cam->angle);
	app->render->noViewOffset = 0;
}

/* 0x00002914
 * Jump to the end of keyframe `key` and resume the waiting script. */
void Camera_snapToKey(Camera *cam, int key)
{
	if (cam->curKey == key && cam->keyDone)
		return;

	cam->curKey = key;
	CameraKey *k = &cam->keys[key];
	if (k->next)
		k = k->next;
	cam->x = k->x;
	cam->y = k->y;
	cam->z = k->z;
	cam->angle = k->angle;
	ScriptThread_run(cam->thread);
	cam->keyDone = 1;
}

/* 0x0000297c
 * Interpolate along keyframe `key` at `time` ms into it; when its duration has
 * passed, resume the waiting script. */
void Camera_update(Camera *cam, int key, int time)
{
	if (cam->curKey == key && cam->keyDone)
		return;

	cam->keyDone = 0;
	cam->curKey = key;

	CameraKey *k = &cam->keys[key];
	int duration = k->duration;
	if (time >= duration) {
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
		/* t = time / duration in 16.16 as a 64-bit divide */
		int64_t num = (int64_t)(int32_t)((uint32_t)time << 16) * 0x10000;
		int64_t den = (int32_t)((uint32_t)duration << 16);
		t = (int)(num / den);
	}

	cam->x = Camera_spline(cam, t, prev->x, k->x, next->x, next2->x);
	cam->y = Camera_spline(cam, t, prev->y, k->y, next->y, next2->y);
	cam->z = Camera_spline(cam, t, prev->z, k->z, next->z, next2->z);
	cam->angle = Camera_splineAngle(cam, t, prev->angle, k->angle, next->angle, next2->angle);
}

/* 0x000188dc
 * Build the map's cameras from the event byte-code: every event with op 0x36
 * keys is one camera. Op 0x35 sets the duration of the following keys, op 0x38
 * makes the camera loop. Each key command's arg is rewritten to
 * camera | key << 8 for the interpreter. */
void Game_loadCameras(Game *game)
{
	game->activeCamera = NULL;
	game->skippable = 0;
	game->numCameras = 0;
	game->numCameraKeys = 0;

	/* first pass: count */
	Render *render = game->render;
	for (int e = 0; e < render->numTileEvents; e++) {
		uint32_t ev = render->tileEvents[e];
		Command *cmds = &render->commands[event_firstCommand(ev)];
		int keys = 0;
		for (int i = 0; i < (int)event_numCommands(ev); i++) {
			if (cmds[i].op == OP_CAMERA_KEY) {
				game->numCameraKeys++;
				keys++;
			}
		}
		if (keys)
			game->numCameras++;
	}
	if (game->numCameras == 0)
		return;

	game->numCameras = 0;
	game->numCameraKeys = 0;
	for (int e = 0; e < game->render->numTileEvents; e++) {
		uint32_t ev = game->render->tileEvents[e];
		int keys = 0;
		int loop = 0;
		int duration = 1000;
		int camIdx = game->numCameras;
		Camera *cam = &game->cameras[camIdx];

		for (int i = 0; i < (int)event_numCommands(ev); i++) {
			Command *cmd = &game->render->commands[event_firstCommand(ev) + (uint32_t)i];
			if (cmd->op == OP_CAMERA_KEY) {
				if (keys == 0)
					cam->keys = &game->cameraKeys[game->numCameraKeys];
				uint32_t arg = (uint32_t)cmd->arg1;
				CameraKey *key = &game->cameraKeys[game->numCameraKeys];
				key->x = (int32_t)(arg & 0xff);
				key->y = (int32_t)((arg & 0xffff) >> 8);
				key->duration = duration;
				key->z = (int32_t)((arg & 0xffffff) >> 16);
				key->angle = Dir_toAngle((int)(arg >> 24));
				cmd->arg1 = game->numCameras | keys << 8;
				game->numCameraKeys++;
				keys++;
			} else if (cmd->op == OP_CAMERA_DURATION) {
				duration = cmd->arg1;
			} else if (cmd->op == OP_CAMERA_LOOP) {
				loop = cmd->arg1 != 0;
			}
		}
		if (keys) {
			cam->numKeys = keys;
			Camera_initKeys(cam, loop);
			game->numCameras++;
		}
	}
}

/* 0x00017c30 */
int Game_isCinematicActive(Game *game)
{
	return game->skippable && game->activeCamera != NULL;
}

/* ---------------------------------------------------------------- lerp sprites */

/* 0x0000620c */
void LerpSprite_calcDist(LerpSprite *ls)
{
	int dx = ls->dstX - ls->srcX;
	int dy = ls->dstY - ls->srcY;
	/* sqrt((d^2 << 8) << 8) >> 8 */
	ls->dist = (int16_t)(Math_fixedSqrt64((dx * dx + dy * dy) * 0x100, 0) >> 8);
}

/* 0x0000545c */
void *Game_allocLerpSprite(Game *game, void *thread, short sprite)
{
	for (int i = 0; i < MAX_LERP_SPRITES; i++) {
		LerpSprite *ls = &game->lerps[i];
		if (ls->hSprite == 0) {
			ls->hSprite = (int16_t)(sprite + 1);
			ls->thread = thread;
			game->numLerps++;
			return ls;
		}
	}
	App_setErrorStr(game->app, (char *)ModData_str(&g_mod, MOD_STR_MAX_LERP_SPRITES));
	App_setErrorCode(g_app, 1);
	return NULL;
}

/* 0x0001292c */
void Game_freeLerpSprite(Game *game, void *lsPtr)
{
	LerpSprite *ls = lsPtr;
	ls->hSprite = 0;
	game->numLerps--;
}

/* Remember a thread to wake once (the original keeps a 16-entry list). */
static int addWakeThread(ScriptThread **list, int count, ScriptThread *thread)
{
	for (int i = 0; i < count; i++)
		if (list[i] == thread)
			return count;
	list[count] = thread;
	return count + 1;
}

/* Relink the sprite's entity on the tile grid if it changed tile; 1 if it did. */
static int relinkSpriteEntity(Game *game, Sprite *spr)
{
	Entity *ent = spr->ent;
	int tx = spr->x >> 6;
	int ty = spr->y >> 6;
	if (tx == ent->linkIndex % 32 && ty == ent->linkIndex / 32)
		return 0;
	Game_unlinkEntity_2ae8c(game, ent);
	Game_linkEntity(game, spr->ent, spr->x >> 6, spr->y >> 6);
	return 1;
}

/* 0x0002b4cc
 * Interpolate every lerp sprite over its duration (play clock); arrived ones
 * snap to the destination and wake their script threads. */
void Game_updateLerpSprites(Game *game)
{
	if (game->numLerps == 0)
		return;

	ScriptThread *wake[MAX_LERP_SPRITES];
	int numWake = 0;
	int moved = 0;
	Render *render = game->render;

	for (int i = 0; i < MAX_LERP_SPRITES; i++) {
		LerpSprite *ls = &game->lerps[i];
		if (ls->hSprite == 0)
			continue;

		Sprite *spr = Render_spriteAt(render, ls->hSprite);
		int elapsed = game->app->gameTime - ls->startTime;

		if (elapsed < ls->duration) {
			/* 8.8 fraction of the way */
			int frac = (elapsed * 0x10000) / (ls->duration << 8);
			spr->x = ((frac * (ls->dstX - ls->srcX) * 0x100) >> 16) + ls->srcX;
			spr->y = ((frac * (ls->dstY - ls->srcY) * 0x100) >> 16) + ls->srcY;
			spr->z = ((frac * (ls->dstZ - ls->srcZ) * 0x100) >> 16) + ls->srcZ;
			spr->scale = (uint8_t)(((frac * ((int)ls->dstScale - (int)ls->srcScale) * 0x100) >> 16) + ls->srcScale);
			Render_relinkSpriteAt(render, &spr->x, -1, -1);

			Entity *ent = spr->ent;
			if (ent == NULL || (spr->info & SPRITE_INFO_HIDDEN))
				continue;
			int tx = spr->x >> 6;
			int ty = spr->y >> 6;
			int frame = (int)((spr->info & 0xfff) >> 8);
			/* walking monsters alternate frames 0/1 with the distance covered */
			if (ent->monster && (frame == 0 || frame == 1))
				spr->info = (spr->info & ~SPRITE_INFO_FRAME) |
				            ((((uint32_t)frac * (uint16_t)ls->dist & 0x3fff) >> 13) << 8);
			if (tx != ent->linkIndex % 32 || ty != ent->linkIndex / 32) {
				Game_unlinkEntity_2ae8c(game, ent);
				Game_linkEntity(game, spr->ent, spr->x >> 6, spr->y >> 6);
				moved = 1;
				EntityMonster *mon = spr->ent->monster;
				if (mon) {
					mon->ce.x = tx * 64 + 32;
					spr->ent->monster->destX = tx * 64 + 32;
					spr->ent->monster->ce.y = ty * 64 + 32;
					spr->ent->monster->destY = ty * 64 + 32;
					spr->ent->info &= ~ENT_INFO_MOVING;
				}
			}
		} else {
			spr->x = ls->dstX;
			spr->y = ls->dstY;
			spr->z = ls->dstZ;
			spr->scale = ls->dstScale;
			Render_relinkSpriteAt(render, &spr->x, -1, -1);

			Entity *ent = spr->ent;
			if (ent != NULL && !(spr->info & SPRITE_INFO_HIDDEN)) {
				if (ent->monster) {
					ent->monster->ce.x = ls->dstX;
					spr->ent->monster->destX = ls->dstX;
					spr->ent->monster->ce.y = ls->dstY;
					spr->ent->monster->destY = ls->dstY;
					spr->ent->info &= ~ENT_INFO_MOVING;
					spr->ent->monster->frameTime = game->app->tickTime;
				}
				spr->ent->info &= ~ENT_INFO_LERPING;
				if (relinkSpriteEntity(game, spr))
					moved = 1;
			}
			if (!(ls->flags & LERP_NO_WAKE))
				numWake = addWakeThread(wake, numWake, ls->thread);
			Game_freeLerpSprite(game, ls);
		}
	}

	for (int i = 0; i < numWake; i++)
		ScriptThread_run(wake[i]);
	if (moved)
		Canvas_updateFacing(game->canvas);
}

/* 0x00026ee0
 * Jump every lerp sprite to its destination and wake the threads. */
void Game_snapLerpSprites(Game *game)
{
	if (game->numLerps == 0)
		return;

	ScriptThread *wake[MAX_LERP_SPRITES];
	int numWake = 0;
	Render *render = game->render;

	for (int i = 0; i < MAX_LERP_SPRITES; i++) {
		LerpSprite *ls = &game->lerps[i];
		if (ls->hSprite == 0)
			continue;

		Sprite *spr = Render_spriteAt(render, ls->hSprite);
		spr->x = ls->dstX;
		spr->y = ls->dstY;
		spr->z = ls->dstZ;
		spr->scale = ls->dstScale;
		Render_relinkSpriteAt(render, &spr->x, -1, -1);

		Entity *ent = spr->ent;
		if (ent != NULL && !(spr->info & SPRITE_INFO_HIDDEN)) {
			if (ent->monster) {
				ent->monster->ce.x = ls->dstX;
				spr->ent->monster->destX = ls->dstX;
				spr->ent->monster->ce.y = ls->dstY;
				spr->ent->monster->destY = ls->dstY;
			}
			relinkSpriteEntity(game, spr);
		}
		if (!(ls->flags & LERP_NO_WAKE))
			numWake = addWakeThread(wake, numWake, ls->thread);
		Game_freeLerpSprite(game, ls);
	}

	for (int i = 0; i < numWake; i++)
		ScriptThread_run(wake[i]);
}
