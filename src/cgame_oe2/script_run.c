/*
 * Tile-event script threads: allocation, the frame stack, the command loop
 * that walks a tile event's byte-code and hands each command to the
 * interpreter (ScriptThread_executeCommand in script.c / script_ext.c),
 * suspension/resumption, and thread save/load.
 *
 * Map event data (all in Render, loaded with the map):
 *   eventTiles[]  sorted tile indexes (y*32 + x), tileEvents[] the parallel event words
 *   byteCodes[]   {opcode, arg, flags} triples
 *   mapFlags[]    bit 0x10 = tile has events
 *
 * Event word:   bits 9..19 first command, 20..26 command count,
 *               27..29 current state, bit 30 = skipped when the caller passes 0x400.
 * Command flags: bits 16..23 state mask (0x10000 << state), 0x100 executed,
 *               0x200 one-shot (cleared after running), 0x800 special trigger class,
 *               remaining low bits = trigger mask matched against the caller's flags.
 *
 * Interpreter return codes: 0 not run, 1 ok, 2 yield (thread suspended),
 * 3 abort the event, 4 restart the event.
 */

#include "game.h"

enum {
	SCRIPT_NOT_RUN = 0,
	SCRIPT_OK = 1,
	SCRIPT_YIELD = 2,
	SCRIPT_ABORT = 3,
	SCRIPT_RESTART = 4,
};

#define MAX_SCRIPT_THREADS 16
#define MAX_SCRIPT_FRAMES 10

/* ScriptFrame.frameFlags */
#define FRAME_NESTED     0x01 /* pushed with the "nested" argument */
#define FRAME_SUSPENDED  0x02
#define FRAME_NO_RESUME  0x04 /* set by TRIGGER (op 3) while its nested tile runs: do not resume this frame from the child */

/* Map flag: the tile owns an event. */
#define TILEFLAG_HAS_EVENT 0x10

/* Opcode of the per-tile container records read by Game_openContainer. */
#define OP_CONTAINER_ITEM 0x2f

/* Event-word helpers. */
static int event_firstCommand(uint32_t ev) { return (int)((ev & 0xfffff) >> 9); }
static int event_numCommands(uint32_t ev) { return (int)((ev & 0x7ffffff) >> 20); }
static int event_state(uint32_t ev) { return (int)((ev & 0x3fffffff) >> 27); }

/* 0x000076a4 */
void ScriptThread_pushFrame(ScriptThread *thread, uint32_t event, uint32_t flags, int nested)
{
	if (thread->numFrames == MAX_SCRIPT_FRAMES)
		return;

	ScriptFrame *frame = &thread->frames[thread->numFrames++];
	frame->cmdIndex = 0xff; /* ScriptThread_run pre-increments to 0 */
	frame->frameFlags = nested ? FRAME_NESTED : 0;
	frame->event = event;
	frame->flags = flags;
}

/* 0x00007630 */
ScriptThread *Game_allocScriptThread(Game *game)
{
	for (int i = 0; i < MAX_SCRIPT_THREADS; i++) {
		ScriptThread *thread = &game->scriptThreads[i];
		if (thread->inuse == 0) {
			ScriptThread_reset_24184(thread);
			thread->inuse = 1;
			thread->index = i & 0xff;
			game->numScriptThreads++;
			return thread;
		}
	}
	return NULL;
}

/* 0x0001b9e8 */
void Game_freeScriptThread(Game *game, ScriptThread *thread)
{
	ScriptThread_reset_34fec(thread);
	game->numScriptThreads--;
}

/* 0x0001bc68 */
int Game_getScriptThreadIndex(ScriptThread *thread)
{
	return (int)(thread - g_app->game->scriptThreads);
}

/* 0x00024184 */
void ScriptThread_reset_24184(ScriptThread *thr)
{
	thr->numFrames = 0;
	thr->unpauseTime = 0;
	thr->state = SCRIPT_YIELD;
}

/* 0x00034fec */
void ScriptThread_reset_34fec(ScriptThread *thread)
{
	thread->inuse = 0;
	ScriptThread_reset_24184(thread);
}

/* 0x000079d4  Wake a thread whose wait time has passed. `t` is the thread's
 * unpauseTime, which is its first member, so it doubles as the thread pointer. */
int Timer_expired(int *t, int now)
{
	if (*t == 0 || *t > now)
		return 0;
	*t = 0;
	ScriptThread_run((ScriptThread *)(void *)t);
	return 1;
}

/* 0x00017db4  Run the commands of the frame's event from frame->cmdIndex on.
 * The original also passes bit 30 of the event word to the interpreter as a
 * fifth (stack) argument, which the interpreter never reads. */
int ScriptThread_runEvent(ScriptThread *thread, ScriptFrame *frame)
{
	Render *render = g_app->render;
	uint32_t ev = frame->event;
	int ranSomething = 0;

	/* Bit-30 events are skipped for callers that pass 0x400. */
	if ((ev & 0x40000000) && (frame->flags & 0x400))
		return SCRIPT_NOT_RUN;

	int base = event_firstCommand(ev) * 3;
	int end = event_numCommands(ev) * 3;

	/* Commands run only in the event's current state, or in any state the
	 * caller explicitly asks for. */
	uint32_t stateMask = 0x10000u << event_state(ev);
	if (frame->flags & 0xff0000)
		stateMask |= frame->flags & 0xff0000;

	for (int i = frame->cmdIndex * 3; i < end; i += 3) {
		int *cmd = &render->byteCodes[base + i];
		uint32_t cmdFlags = (uint32_t)cmd[2];

		if (stateMask == 0) {
			if (cmdFlags & 0xff0000)
				continue;
		} else if ((cmdFlags & stateMask) == 0) {
			continue;
		}

		if (frame->flags & 0x800) {
			if ((cmdFlags & 0x40000800) == 0)
				continue;
		} else if (cmdFlags & 0x800) {
			continue;
		}

		uint32_t triggers = cmdFlags & 0xbffff7ffu;
		if ((frame->flags & triggers) == 0)
			continue;

		cmd[2] = (int)(cmdFlags | 0x100); /* executed */
		int ret = ScriptThread_executeCommand(thread, frame, cmd[0], cmd[1]);
		if (ret == SCRIPT_NOT_RUN)
			continue;
		if (ret == SCRIPT_ABORT)
			return SCRIPT_ABORT;
		if (triggers & 0x200)
			cmd[2] = 0; /* one-shot command */
		if (ret == SCRIPT_YIELD) {
			frame->cmdIndex = (uint8_t)(i / 3);
			return SCRIPT_YIELD;
		}
		ranSomething = 1;
		if (ret == SCRIPT_RESTART) {
			/* The original resets to 0 before the loop's += 3, so a restart
			 * resumes at the event's second command. */
			frame->cmdIndex = 0;
			i = 0;
		}
	}
	return ranSomething ? SCRIPT_OK : SCRIPT_NOT_RUN;
}

/* 0x00035974  Continue the top frame; when it finishes, pop it and resume a
 * suspended parent frame. */
int ScriptThread_run(ScriptThread *thread)
{
	ScriptFrame *top = &thread->frames[thread->numFrames - 1];

	top->cmdIndex++;
	thread->state = ScriptThread_runEvent(thread, top);
	if (thread->state == SCRIPT_YIELD) {
		top->frameFlags |= FRAME_SUSPENDED;
		return SCRIPT_YIELD;
	}

	thread->numFrames--;
	if (thread->numFrames > 0) {
		ScriptFrame *parent = &thread->frames[thread->numFrames - 1];
		if ((parent->frameFlags & FRAME_SUSPENDED) && !(parent->frameFlags & FRAME_NO_RESUME)) {
			parent->frameFlags &= ~FRAME_SUSPENDED;
			thread->state = ScriptThread_run(thread);
		}
	}
	return thread->state;
}

/* 0x00017f90 */
int ScriptThread_executeTile(ScriptThread *thread, int tx, int ty, int flags, int extra)
{
	Game *game = g_app->game;
	Render *render = g_app->render;

	if (game->blockTileEvents != 0 || tx < 0 || tx >= 32 || ty < 0 || ty >= 32) {
		thread->state = 0;
		return 0;
	}

	game->threadWaiting = 0;
	int tile = tx + ty * 32;
	if (render->mapFlags[tile] & TILEFLAG_HAS_EVENT) {
		int ev = Render_findTileEvent(render, tile);
		if (ev != -1) {
			ScriptThread_pushFrame(thread, render->tileEvents[ev], (uint32_t)flags, extra);
			return ScriptThread_run(thread);
		}
	}
	thread->state = 0;
	return 0;
}

/* 0x00017f70 */
int ScriptThread_executeTile0(ScriptThread *thread, int tx, int ty, int flags)
{
	return ScriptThread_executeTile(thread, tx, ty, flags, 0);
}

/* 0x00030ffc  Queue a tile's event on a thread without running it (used while
 * a dialog is open); the frame starts out suspended. */
int ScriptThread_initTileEvent(ScriptThread *thread, int tx, int ty, int a, int b)
{
	Game *game = g_app->game;
	Render *render = g_app->render;

	if (game->blockTileEvents != 0 || tx < 0 || tx >= 32 || ty < 0 || ty >= 32) {
		thread->state = 0;
		return 0;
	}

	game->threadWaiting = 0;
	int tile = tx + ty * 32;
	int ev;
	if ((render->mapFlags[tile] & TILEFLAG_HAS_EVENT) == 0 ||
	    (ev = Render_findTileEvent(render, tile)) == -1) {
		thread->state = 0;
		return 0;
	}

	ScriptThread_pushFrame(thread, render->tileEvents[ev], (uint32_t)a, b);
	thread->frames[thread->numFrames - 1].frameFlags |= FRAME_SUSPENDED;
	return SCRIPT_YIELD;
}

/* 0x00018098  Run a tile's event on a fresh thread; the thread is kept only
 * if the script yielded. */
int Game_executeTile(Game *game, int tx, int ty, int flags, int extra)
{
	if (game->blockTileEvents != 0 || tx < 0 || tx >= 32 || ty < 0 || ty >= 32 ||
	    (g_app->render->mapFlags[tx + ty * 32] & TILEFLAG_HAS_EVENT) == 0)
		return 0;

	ScriptThread *thread = Game_allocScriptThread(game);
	int ret;
	/* All 16 threads busy: the original wrote through the NULL thread (undefined, near address 0); the port
	 * drops the event as if the tile had none. */
	if (thread == NULL)
		return 0;
	if (g_app->canvas->state == 7) {
		/* A dialog is up: queue the event to run once it closes. */
		ret = ScriptThread_initTileEvent(thread, tx, ty, flags, extra);
		thread->unpauseTime = -1;
	} else {
		ret = ScriptThread_executeTile(thread, tx, ty, flags, extra);
	}
	if (ret != SCRIPT_YIELD)
		Game_freeScriptThread(game, thread);
	return ret;
}

/* 0x00018078 */
int Game_executeTile0(Game *game, int tx, int ty, int flags)
{
	return Game_executeTile(game, tx, ty, flags, 0);
}

/* 0x00035890  Resume waiting threads and free finished ones. Threads only
 * advance in the playing (3) and cinematic (0x12) canvas states. */
void Game_runScriptThreads(Game *game)
{
	if (game->numScriptThreads == 0)
		return;

	for (int i = 0; i < MAX_SCRIPT_THREADS; i++) {
		ScriptThread *thread = &game->scriptThreads[i];
		if (thread->inuse == 0)
			continue;

		if (thread->state == SCRIPT_YIELD) {
			int cs = g_app->canvas->state;
			if (cs == 3 || cs == 0x12) {
				if (thread->unpauseTime == -1) {
					/* Was waiting for a dialog/menu to close. */
					thread->unpauseTime = 0;
					ScriptThread_run(thread);
				} else {
					Timer_expired(&thread->unpauseTime, g_app->time);
				}
				if (thread->state != SCRIPT_YIELD) {
					Game_freeScriptThread(game, thread);
					continue;
				}
			}
			if (thread->numFrames != 0)
				continue;
		}
		Game_freeScriptThread(game, thread);
	}
}

/* 0x00024790  Is any live thread running an event whose first frame carries
 * trigger flag 0x10000000? */
int Game_hasThreadFlag(Game *game)
{
	if (game->numScriptThreads <= 0)
		return 0;
	for (int i = 0; i < MAX_SCRIPT_THREADS; i++) {
		ScriptThread *thread = &game->scriptThreads[i];
		if (thread->inuse != 0 && (thread->frames[0].flags & 0x10000000))
			return 1;
	}
	return 0;
}

/* 0x0000a3f8  Sum of the counts in the container records (opcode 0x2f, kind 4)
 * of the event on the entity's tile. */
int Game_tileEventParamSum(Entity *entity)
{
	Render *render = g_app->render;
	Sprite *spr = &render->sprites[(entity->info & 0xffff) - 1];
	int ev = Render_findTileEvent(render, (spr->y >> 6) * 32 + (spr->x >> 6));
	int sum = 0;

	/* The original tests against 0, not -1, so the first event is never summed
	 * and a missing event (-1) reads the word just before tileEvents[], i.e.
	 * the last two eventTiles entries. Reproduced without reading out of bounds. */
	if (ev == 0)
		return 0;

	uint32_t word;
	if (ev < 0)
		word = (uint16_t)render->eventTiles[398] | ((uint32_t)(uint16_t)render->eventTiles[399] << 16);
	else
		word = render->tileEvents[ev];
	int base = event_firstCommand(word) * 3;
	int end = event_numCommands(word) * 3;
	for (int i = 0; i < end; i += 3) {
		const int *cmd = &render->byteCodes[base + i];
		if (cmd[0] == OP_CONTAINER_ITEM && (cmd[1] & 0xff) == 4)
			sum += (int)(((uint32_t)cmd[1] & 0xffffff) >> 8);
	}
	return sum;
}

/* 0x00028ce4 */
void ScriptThread_loadState(ScriptThread *thr, InputStream *stream)
{
	ScriptThread_reset_24184(thr);

	/* Wait times are stored relative to the save moment. */
	thr->unpauseTime = Stream_readInt(stream);
	if (thr->unpauseTime != 0 && thr->unpauseTime != -1)
		thr->unpauseTime += g_app->time;

	thr->state = Stream_readUByte(stream);
	thr->numFrames = Stream_readUByte(stream);
	for (int i = 0; i < thr->numFrames; i++) {
		ScriptFrame *f = &thr->frames[i];
		f->event = (uint32_t)Stream_readInt(stream);
		f->flags = (uint32_t)Stream_readInt(stream);
		f->cmdIndex = (uint8_t)Stream_readUByte(stream);
		f->frameFlags = (uint8_t)Stream_readUByte(stream);
	}
}

/* 0x00036c24 */
void ScriptThread_saveState(ScriptThread *thread, OutputStream *os)
{
	int wait = thread->unpauseTime;
	if (wait != -1 && wait != 0)
		wait -= g_app->time;

	OutStream_writeInt(os, wait);
	OutStream_writeBool(os, (int8_t)thread->state);
	OutStream_writeBool(os, (int8_t)thread->numFrames);
	for (int i = 0; i < thread->numFrames; i++) {
		ScriptFrame *f = &thread->frames[i];
		OutStream_writeInt(os, (int)f->event);
		OutStream_writeInt(os, (int)f->flags);
		OutStream_writeBool(os, (int8_t)f->cmdIndex);
		OutStream_writeBool(os, (int8_t)f->frameFlags);
	}
}

/* 32-bit wrapping multiply, as the ARM `mul` did. */
static int32_t mul32(int32_t a, int32_t b)
{
	return (int32_t)((uint32_t)a * (uint32_t)b);
}

/* 0x0003ce90  Particle/combat effect requested by a script (kind 0..8).
 * `arg` is passed through to the particle presets; for kind 6 it is also the
 * distance (in tiles) ahead of the view where wall decals are spawned. */
void Script_combatEffect(int unused, int kind, int color, int arg)
{
	Combat *combat = g_app->combat;
	int yOff, imgType;

	switch (kind) {
	case 0:
		combat->effectMode = 0;
		Particles_spawnBloodDefault(combat, arg, color, 30);
		break;
	case 1:
		Particles_spawnPreset3(combat, arg, color);
		break;
	case 2:
		Particles_spawnImage1(combat, color);
		break;
	case 3:
		Particles_spawnToTarget(combat, color, arg);
		break;
	case 4:
		Particles_spawnImage2(combat, arg, color);
		break;
	case 5:
		combat->effectMode = 0;
		yOff = 25;
		imgType = -1;
		Particles_spawnBlood(combat, arg, color, 30, yOff, imgType);
		break;
	case 6: {
		combat->effectMode = 1;
		Particles_spawnBlood(combat, arg, color, 30, 0, 0);
		Render *r = g_app->render;
		int dist = arg << 6;
		int x = r->viewX + (mul32(dist, r->viewCos) >> 16);
		int y = r->viewY + (mul32(dist, -r->viewSin) >> 16);
		Game_spawnWallSpritesAround(g_app->game, x >> 6, y >> 6, 0xa1, 0);
		break;
	}
	case 7:
		combat->effectMode = 1;
		Particles_spawnBlood(combat, arg, color, 30, 0, 1);
		break;
	case 8:
		combat->effectMode = 1;
		Particles_spawnBlood(combat, arg, color, 30, 0, 3);
		break;
	default:
		break;
	}
}
