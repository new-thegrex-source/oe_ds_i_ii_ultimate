/*
 * Orcs & Elves II (BREW, EN, EA 2007): the game module's side of the engine
 * interface (see cgame_common/oe_game.c and oe_public.h).
 *
 * What oe2-re's main.c did as the BREW shell now lives in the engine (timer,
 * key repeat, suspend/resume, screenshots); the parts that know the game's
 * structures stay here: creating the applet, the world hash and the OE_PIPE
 * status line used by the QA bots and golden traces (re/tools/qa in oe2-re),
 * and the developer commands.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "oe_vm.h"
#include "oe_export.h"

#define TICK_MS 40
#define FAST_TICK_MS 16 /* --fast: about one tick per 60 Hz frame */

static Applet *app;

int OeGame_info(int what)
{
	switch (what) {
	case OE_INFO_TICK_MS: return TICK_MS;
	case OE_INFO_FAST_TICK_MS: return FAST_TICK_MS;
	case OE_INFO_SCREEN_WIDTH: return 176;
	case OE_INFO_SCREEN_HEIGHT: return 220;
	case OE_INFO_PAUSE_SOUND_ON_SUSPEND: return 0; /* oe2-re let sounds play on while minimised */
	}
	return 0;
}

int OeGame_init(int screenW, int screenH)
{
	if (Brew_init("Orcs & Elves II", screenW, screenH) != 0)
		return -1;
	app = App_create(screenW, screenH);
	if (!app) {
		Oe_print("Engine startup failed.\n");
		Brew_shutdown();
		return -1;
	}
	Applet_HandleEvent(app, EVT_APP_START, 0, 0);
	return 0;
}

void OeGame_shutdown(void)
{
	if (app) {
		Applet_HandleEvent(app, EVT_APP_STOP, 0, 0);
		App_destroy(app);
		app = NULL;
	}
	Brew_shutdown();
}

int OeGame_event(int evt, int wParam, int dwParam)
{
	return app ? Applet_HandleEvent(app, evt, wParam, dwParam) : 0;
}

void OeGame_tick(void)
{
	if (app)
		Applet_Tick(app);
}

/* ---------------------------------------------------------------- QA (OE_PIPE) */

/* FNV-1a over the state the game rules act on (oe2-re main.c world_hash): the random pool, every sprite's
 * position and flags, and every entity's info, parameter, tile link and health. */
static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n--)
		h = (h ^ *b++) * 16777619u;
	return h;
}

static uint32_t world_hash(void)
{
	Applet *a = g_app;
	Render *r = a->render;
	Game *g = a->game;
	uint32_t h = 2166136261u;
	h = fnv(h, a->random.pool, sizeof(a->random.pool));
	h = fnv(h, &a->random.index, sizeof(a->random.index));
	for (int i = 0; i < r->numSprites && i < 800; i++) {
		const Sprite *s = &r->sprites[i];
		int32_t v[4];
		v[0] = s->x; v[1] = s->y; v[2] = s->z; v[3] = (int32_t)s->info;
		h = fnv(h, v, sizeof(v));
	}
	for (int i = 0; i < g->numEntities && i < 450; i++) {
		const Entity *e = &g->entities[i];
		int32_t v[4];
		v[0] = (int32_t)e->info; v[1] = e->param; v[2] = e->linkIndex;
		v[3] = e->monster ? CombatEntity_getHealth(&e->monster->ce) : 0;
		h = fnv(h, v, sizeof(v));
	}
	return h;
}

/* Status line printed before every tick (same format as oe2-re's main.c pipe_status) */
static void pipe_status(int tick)
{
	Applet *a = g_app;
	Canvas *c = a->canvas;
	Hud *h = a->hud;
	Player *p = a->player;
	Entity *f = p->facingEntity;
	char line[1024];
	snprintf(line, sizeof(line), "S %d st %d map %d x %d y %d ang %d hp %d/%d xp %d lvl %d gold %d menu %d sel %d "
	         "nitems %d fam %d lock %d %d face '%.*s' msg '%.*s' w %08x\n",
	         tick, c->state, c->mapId, c->destX >> 6, c->destY >> 6, c->destAngle & 0x3ff,
	         CombatEntity_getHealth(&p->ce), CombatEntity_getMaxHealth(&p->ce), p->currentXP, p->level, p->gold,
	         a->menuSystem->menu, a->menuSystem->selectedIndex, a->menuSystem->numItems, p->isFamiliar,
	         c->lockSlot, c->lockCorrectHits + 10 * c->lockDone,
	         f && f->def ? f->def->name.len : 0, f && f->def ? f->def->name.buf : "",
	         h->numMessages > 0 ? h->messages[0].len : 0, h->numMessages > 0 ? h->messages[0].buf : "",
	         (unsigned)world_hash());
	trap_PipeWrite(line);
}

static int argi(int n)
{
	char buf[64];
	trap_Argv(n, buf, sizeof(buf));
	return atoi(buf);
}

#define MAP_RES_BASE 1999 /* map N is resource 1999 + N */

/* "oemap [N]" / "devoemap [N]": go to map N (1-based) the way the game changes levels (Canvas_loadMapEx);
 * devoemap skips the save on leaving an overworld level and turns on god mode. Only from a level of a started
 * game (mapId 0 is the main menu: no player set up yet). */
static void cmd_oemap(int dev)
{
	Canvas *cv = g_app->canvas;
	int mapId;

	if (trap_Argc() < 2) {
		Oe_print("map %d; usage: %s <1..>\n", cv->mapId, dev ? "devoemap" : "oemap");
		return;
	}
	mapId = argi(1);
	if (mapId < 1 || trap_ResSize(MAP_RES_BASE + mapId) <= 0) {
		Oe_print("oemap: no map %d in the .bar\n", mapId);
		return;
	}
	/* in the in-game menu, playing, in combat, on the automap or in a dialog (not one over lock-picking:
	 * leaving that goes back to the lock) */
	if (cv->mapId < 1 || cv->state < 2 || cv->state > 7 || cv->state == 6 || (cv->state == 7 && cv->oldState == 0x13)) {
		Oe_print("oemap: start or load a game first\n");
		return;
	}
	if (dev)
		g_app->player->god = 1;
	Canvas_loadMapEx(cv, mapId, dev);
}

/* Developer and QA commands (oe2-re's pipe_tick). The engine handles quit, shot and key names itself. */
int OeGame_consoleCommand(void)
{
	char cmd[64];
	if (!app)
		return 0;
	trap_Argv(0, cmd, sizeof(cmd));
	if (!strcmp(cmd, "pipestatus")) {
		pipe_status(argi(1));
	} else if (!strcmp(cmd, "god")) {
		g_app->player->god = 1;
	} else if (!strcmp(cmd, "noclip")) {
		g_app->player->noclip = !g_app->player->noclip;
	} else if (!strcmp(cmd, "oe_export")) {
		/* textures for a mod (cgame_common/oe_export.c): wtexels, stexels, ftexels.bin, render_load.c */
		static const oeExportGame_t oe2 = { 1004, 1003, 1005, { 0, 0, 0, 0 }, { 0, 1007, 1006, 1008 } };
		Export_textures(&oe2);
	} else if (!strcmp(cmd, "oemap") || !strcmp(cmd, "devoemap")) {
		cmd_oemap(cmd[0] == 'd');
	} else if (!strcmp(cmd, "tp") && trap_Argc() >= 3) {
		Canvas *cv = g_app->canvas;
		int tx = argi(1), ty = argi(2);
		cv->viewX = cv->destX = tx * 64 + 32;
		cv->viewY = cv->destY = ty * 64 + 32;
		cv->viewAngle = cv->destAngle;
		Game_unlinkEntityById(g_app->player);
		Canvas_finishRotation(cv, 1);
		Game_relinkEntity(g_app->player);
	} else {
		return 0;
	}
	return 1;
}
