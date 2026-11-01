/*
 * Orcs & Elves (BREW, 1049 build): the game module's side of the engine
 * interface (see cgame_common/oe_game.c and oe_public.h).
 *
 * What oe-re's main.c did as the BREW shell now lives in the engine (timer,
 * key repeat, suspend/resume, screenshots); the parts that know the game's
 * structures stay here: creating the applet, the world hash and the OE_PIPE
 * status line used by the QA bots and golden traces (re/tools/qa in oe-re),
 * and the developer commands.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "ds_local.h"
#include "oe_vm.h"
#include "oe_export.h"

#define TICK_MS 15
#define FAST_TICK_MS 5 /* --fast: three times the phone's rate */

static Applet *app;

int OeGame_info(int what)
{
	switch (what) {
	case OE_INFO_TICK_MS: return TICK_MS;
	case OE_INFO_FAST_TICK_MS: return FAST_TICK_MS;
	case OE_INFO_SCREEN_WIDTH: return 176;
	case OE_INFO_SCREEN_HEIGHT: return 220;
	/* An interruption silenced the phone; here the sounds pause and carry on afterwards. The channels stay
	 * "playing", so no sound is reported as finished while minimised. EVT_APP_SUSPEND has already cleared the
	 * loop flags (Sound_clearChannelFlags, as the original did), so a looping track plays to its end after the
	 * restore and is not repeated. */
	case OE_INFO_PAUSE_SOUND_ON_SUSPEND: return 1;
	}
	return 0;
}

int OeGame_init(int screenW, int screenH)
{
	if (Brew_init("Orcs & Elves DS", screenW, screenH) != 0)
		return -1;
	app = App_create(screenW, screenH);
	if (!app) {
		Oe_print("Engine startup failed.\n");
		Brew_shutdown();
		return -1;
	}
	App_handleEvent(app, EVT_APP_START, 0, 0);
	return 0;
}

void OeGame_shutdown(void)
{
	if (app) {
		App_handleEvent(app, EVT_APP_STOP, 0, 0);
		App_destroy(app);
		app = NULL;
	}
	Brew_shutdown();
}

int OeGame_event(int evt, int wParam, int dwParam)
{
	return app ? App_handleEvent(app, evt, wParam, dwParam) : 0;
}

void OeGame_tick(void)
{
	if (app)
		App_tick(app);
}

/* ---------------------------------------------------------------- QA (OE_PIPE) */

/* FNV-1a over the state the game rules act on: the random pool, every sprite's position and flags, and every
 * entity's info, parameter, tile link and health. One changed draw, step or hit anywhere changes it, which is
 * what the golden traces (re/tools/qa/golden.py) pin down. Pointers are left out (they differ per run). */
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
	for (int i = 0; i <= r->numSprites && i < 1000; i++) { /* 1-based */
		const Sprite *s = &r->sprites[i];
		int32_t v[4];
		v[0] = s->x; v[1] = s->y; v[2] = s->z; v[3] = (int32_t)s->info;
		h = fnv(h, v, sizeof(v));
	}
	for (int i = 0; i < g->numEntities && i < 400; i++) {
		const Entity *e = &g->entities[i];
		int32_t v[4];
		v[0] = (int32_t)e->info; v[1] = e->param; v[2] = e->linkIndex; v[3] = e->monster ? e->monster->ce.health : 0;
		h = fnv(h, v, sizeof(v));
	}
	return h;
}

/* Status line printed before every tick (same format as oe-re's main.c pipe_status):
 *   S <tick> st <Canvas.state> map <mapId> x <tileX> y <tileY> ang <destAngle 0..1023, 0=east 256=north>
 *   hp <hp>/<max> xp <xp> lvl <level> gold <gold> menu <MenuSystem.menuId> sel <selected>
 *   nitems <numItems> wand <has the wand> lock <digits typed> <lockDone>
 *   face '<name of the faced entity's def>' msg '<first HUD message>' w <world_hash, 8 hex digits> */
static void pipe_status(int tick)
{
	Applet *a = g_app;
	Canvas *c = a->canvas;
	Hud *h = a->hud;
	Player *p = a->player;
	Entity *f = p->facing;
	const char *face = f && f->def && f->def->name ? f->def->name : "";
	const char *msg = h->numMessages > 0 ? h->messages : "";  /* 5 x 0x40 buffers, first one */
	char line[1024];
	snprintf(line, sizeof(line), "S %d st %d map %d x %d y %d ang %d hp %d/%d xp %d lvl %d gold %d menu %d sel %d "
	         "nitems %d wand %d lock %d %d face '%s' msg '%.*s' w %08x\n",
	         tick, c->state, c->mapId, c->destX >> 6, c->destY >> 6, c->destAngle & 0x3ff,
	         p->ce.health, p->ce.maxHealth, p->xp, p->level, p->gold,
	         a->menuSystem->menuId, a->menuSystem->selected, a->menuSystem->numItems, (int)(p->weapons & 1),
	         (int)strnlen(c->lockTyped, sizeof(c->lockTyped)), c->lockDone, face, 0x40, msg, (unsigned)world_hash());
	trap_PipeWrite(line);
}

/* "ents": one line per live monster / NPC entity (QA): index, def name, linked tile, health, Entity.info,
 * goal type/param, monster flags. Sent through the pipe; the engine writes it to the file the bot asked for. */
static void pipe_ents(void)
{
	Game *g = g_app->game;
	char line[512];
	for (int i = 0; i < g->numEntities; i++) {
		Entity *e = &g->entities[i];
		int n;
		if (!e->def || (e->def->eType != 2 && e->def->eType != 3))
			continue;
		n = snprintf(line, sizeof(line), "%d %s tile %d %d info %#x", i, e->def->name ? e->def->name : "?",
		             e->linkIndex % 32, e->linkIndex / 32, e->info);
		if (e->monster)
			snprintf(line + n, sizeof(line) - n, " hp %d/%d goal %d param %d at %d %d flags %#x",
			         e->monster->ce.health, e->monster->ce.maxHealth, e->monster->goal.type, e->monster->goal.param,
			         e->monster->goal.x, e->monster->goal.y, e->monster->flags);
		strncat(line, "\n", sizeof(line) - strlen(line) - 1);
		trap_PipeWrite(line);
	}
}

static int argi(int n)
{
	char buf[64];
	trap_Argv(n, buf, sizeof(buf));
	return atoi(buf);
}

#define MAP_FIRST 6000 /* m01.bsp, resource 6000 */
#define MAP_MENU 6012  /* DS map_menu (BREW 6009 ui.bsp), the main-menu background */

/* "oemap [N]" / "devoemap [N]": go to level N (1 = m01, or the resource id 6000...) the way the game's own
 * debug menu does (Canvas_loadMap); devoemap skips the autosave on leaving and turns on god mode. Only from a
 * level of a started game: the main menu has no player set up yet. */
static void cmd_oemap(int dev)
{
	Canvas *cv = g_app->canvas;
	char arg[32];
	int mapId;

	if (trap_Argc() < 2) {
		Oe_print("map %d (oemap %d); usage: %s <1..> or <%d..>\n", cv->mapId, cv->mapId - MAP_FIRST + 1,
		         dev ? "devoemap" : "oemap", MAP_FIRST);
		return;
	}
	trap_Argv(1, arg, sizeof(arg));
	mapId = atoi(arg);
	if (mapId > 0 && mapId < MAP_FIRST)
		mapId += MAP_FIRST - 1;
	if (mapId < MAP_FIRST || mapId == MAP_MENU || trap_ResSize(mapId) <= 0) {
		Oe_print("oemap: no map %s in the .bar\n", arg);
		return;
	}
	/* in the in-game menu (the debug menu's own state), playing, in combat, on the automap or in a dialog */
	if (cv->mapId < MAP_FIRST || cv->mapId == MAP_MENU || cv->state < 2 || cv->state > 7 || cv->state == 6) {
		Oe_print("oemap: start or load a game first\n");
		return;
	}
	if (dev)
		g_app->player->god = 1;
	Canvas_loadMap(cv, mapId, dev);
}

/* Developer and QA commands (oe-re's pipe_tick). The engine handles quit, shot and key names itself. */
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
		/* textures for a mod (cgame_common/oe_export.c): one resource each, render_load.c */
		static const oeExportGame_t oe = { 5007, 5005, 5006, { 0, 7512, 9000, 7000 }, { 0, 0, 0, 0 } };
		Export_textures(&oe);
	} else if (!strcmp(cmd, "oemap") || !strcmp(cmd, "devoemap")) {
		cmd_oemap(cmd[0] == 'd');
	} else if (!strcmp(cmd, "ents")) {
		pipe_ents();
	} else if (!strcmp(cmd, "raise") && trap_Argc() >= 3) {
		/* "raise C V": entity V dies (if alive) and monster C raises its corpse through the game's own
		 * Combat_raiseDead, as a necromancer's spell would (regression test for raised shadow names). */
		Game *g = g_app->game;
		int c = argi(1), v = argi(2);
		if (c >= 0 && v >= 0 && c != v && c < g->numEntities && v < g->numEntities && g->entities[c].monster &&
		    g->entities[v].monster) {
			if (g->entities[v].monster->ce.health > 0)
				Entity_died(&g->entities[v], 0); /* not by the player: no XP or kill stats */
			Combat_raiseDead(&g->entities[c], v);
		}
	} else if (!strcmp(cmd, "give") && trap_Argc() >= 4) {
		/* "give TYPE INDEX AMOUNT": Player_give (11 2 1: the lightning spell; 3 1 99: the wand's energy) */
		Player_give(g_app->player, argi(1), argi(2), argi(3), 0);
	} else if (!strcmp(cmd, "castspell") && trap_Argc() >= 2) {
		DsSpells_debugCast(g_app->canvas, argi(1));
	} else if (!strcmp(cmd, "maxmap") && trap_Argc() >= 2) {
		/* "maxmap N": the furthest map reached (6000 + n; the wand's energy grows with it) */
		g_app->player->maxMapReached = (uint16_t)argi(1);
	} else if (!strcmp(cmd, "tp") && trap_Argc() >= 3) {
		Canvas *cv = g_app->canvas;
		int tx = argi(1), ty = argi(2);
		cv->viewX = cv->destX = tx * 64 + 32;
		cv->viewY = cv->destY = ty * 64 + 32;
		cv->viewAngle = cv->destAngle;
		Game_unlinkPlayerEntity(g_app->player);
		Canvas_finishRotation(cv);
		Game_relinkEntity(g_app->player);
	} else {
		return 0;
	}
	return 1;
}
