/*
 * Game: startup/shutdown, map entity spawning, player spawn, map change and
 * the persistent state (Config file, SD_0/SD_1 player records, SD_<n> world
 * snapshots).
 *
 * Save files are written exactly like the phone game wrote them (little-endian
 * ints/shorts, single bytes for bools), so the layout of every record below
 * follows the original byte for byte.
 */

#include "game.h"

/* Save-file chunk magics (little-endian ASCII) */
#define MAGIC_CONFIG_HEAD 0x47464348 /* "HCFG" */
#define MAGIC_CONFIG_TAIL 0x47464354 /* "TCFG" */
#define MAGIC_PLAYER_HEAD 0x524c5048 /* "HPLR" */
#define MAGIC_PLAYER_TAIL 0x524c5054 /* "TPLR" */
#define MAGIC_WORLD_HEAD  0x444c5748 /* "HWLD" */
#define MAGIC_WORLD_TAIL  0x444c5754 /* "TWLD" */
#define CONFIG_VERSION    0x15

#define MAX_ENTITIES      450
#define MAX_MONSTERS      100
#define NUM_SCRIPT_THREADS 16
#define NUM_LERP_SPRITES  16
#define NUM_DROP_SLOTS    8
#define NUM_SAVED_MENUS   13

/* Tile-event word fields (Render.tileEvents[]) */
#define EVENT_FIRST_CMD(ev) (((ev) & 0xfffff) >> 9)
#define EVENT_NUM_CMDS(ev)  (((ev) & 0x7ffffff) >> 20)
#define EVENT_STATE(ev)     (((ev) & 0x3fffffff) >> 27)

/* Script opcodes the camera loader scans for */
#define OP_PASSWORD     0x0a
#define OP_CONTAINER    0x2f
#define OP_CAMERA_DELAY 0x35
#define OP_CAMERA_KEY   0x36
#define OP_CAMERA_LOOP  0x38

/* Builds "SD_<n>" into a String the same way the original did. */
static void saveFileName(String *name, int n)
{
	String_setLength(name, 0);
	String_appendCStr_76f0(name, "SD_");
	String_appendInt(name, n);
}

/* Last little-endian int of a fully loaded file (the chunk's tail magic). */
static int streamTrailerInt(InputStream *stream)
{
	const uint8_t *data = (const uint8_t *)InputStream_getBuffer(stream);
	int end = InputStream_getSize(stream) - 4;
	if (end < 0)
		return 0; /* shorter than one int: cannot hold the tail magic */
	return (int)((uint32_t)data[end] | (uint32_t)data[end + 1] << 8 |
	             (uint32_t)data[end + 2] << 16 | (uint32_t)data[end + 3] << 24);
}

/* 0x0003e23c */
int Game_startup(Game *game)
{
	int i;

	game->difficulty = 1;
	game->activeMonsters = NULL;
	game->combatMonsters = NULL;
	game->inactiveMonsters = NULL;
	String_setLength(&game->lockCombination, 0);
	game->arrived = 0;
	game->disableAI = 0;
	game->alertEntity = NULL;
	game->activeSprites = 0;
	game->activePropagators = 0;
	game->f9be8 = 0;
	for (i = 0; i < NUM_SCRIPT_THREADS; i++)
		ScriptThread_reset_34fec(&game->scriptThreads[i]);

	/* String ids of the level names (group 3?) used by the travel map. */
	game->levelNames[0] = 0xc0;
	game->levelNames[1] = 0xc1;
	game->levelNames[2] = 0xc2;
	game->levelNames[3] = 0xc3;
	game->levelNames[4] = 0xc4;
	game->levelNames[5] = 0xc5;
	game->levelNames[6] = 0xc6;
	game->levelNames[7] = 0x10d;
	game->numLevels = 9;
	/* The original stores a halfword here; the upper half is never touched. */
	game->fadc0 = (game->fadc0 & ~0xffff) | 0x65;
	game->skipLevel = 0;
	game->debugVarsLoaded = 0;
	game->spawnOverride = 0;
	return 1;
}

/* 0x0003c368 */
void Game_shutdown(Game *game)
{
	int i;

	for (i = 0; i < MAX_ENTITIES; i++)
		String_release(&game->entities[i].name);
	String_release(&game->lockCombination);
}

/* 0x0004387c */
void Game_unloadMap(Game *game)
{
	int i;

	game->activeSprites = 0;
	game->activePropagators = 0;
	game->goldPaid = 0;
	g_app->player->facingEntity = NULL;
	g_app->combat->target = NULL;
	g_app->combat->attacker = NULL;
	g_app->hud->targetEnt = NULL;

	for (i = 0; i < 48; i++) {
		if (game->gsprites[i].flags & 1)
			Game_gspriteDestroy(game, &game->gsprites[i]);
	}

	game->inactiveMonsters = NULL;
	game->activeMonsters = NULL;
	game->combatMonsters = NULL;
	String_setLength(&game->lockCombination, 0);

	for (i = 0; i < 1024; i++)
		game->entityGrid[i] = NULL;
	for (i = 0; i < game->numEntities; i++)
		Entity_reset(&game->entities[i]);

	game->numCameraKeys = 0;
	game->numCameras = 0;
}

/* 0x00031a54 */
void Game_deleteSaveFiles(void)
{
	String name;
	int i;

	String_ctor(&name);
	for (i = 2; i < 10; i++) {
		saveFileName(&name, i);
		IFILEMGR_Remove(g_app->fileMgr, String_cstr(&name));
	}
	saveFileName(&name, 0);
	IFILEMGR_Remove(g_app->fileMgr, String_cstr(&name));
	saveFileName(&name, 1);
	IFILEMGR_Remove(g_app->fileMgr, String_cstr(&name));
	String_dtor(&name, 0);
}

/* 0x0001a120 */
void Game_doChangeMap(Game *game)
{
	Applet *app = g_app;
	uint32_t pending;
	int autosave = 0;
	int i;

	if (game->pendingChangeMap == 0)
		return;

	app->player->critCounter = 0;
	app->player->lastTarget = NULL;
	pending = game->pendingChangeMap;
	/* Bits 8..30 carry a spawn position for the new map (Game_spawnPlayer). */
	game->spawnOverride = (pending & 0x7fffffff) >> 8;

	/* Bit 31: leaving this level completes it; first completion autosaves. */
	if (pending & 0x80000000u) {
		uint32_t bit = arm_bit(app->canvas->mapId - 1);
		if ((app->player->completedLevels & bit) == 0) {
			autosave = 1;
			app->player->completedLevels |= bit;
		}
	}

	/* Outside map 6, re-arm every eType 7 / sub 9 entity (clear 0x40000000). */
	if (app->canvas->mapId != 6) {
		for (i = 0; i < game->numEntities; i++) {
			EntityDef *def = game->entities[i].def;
			if (def != NULL && def->eType == 7 && def->eSubType == 9)
				game->entities[i].info &= ~0x40000000u;
		}
	}

	if (autosave) {
		app->menu->mapId = (int16_t)((pending & 0xff) + 1);
		String_setLength(&app->canvas->scratch, 0);
		Text_getString(app->canvas, 3, 0x11e, &app->canvas->scratch); /* "Saving Game" */
		app->canvas->saveType = 0x13;
		Canvas_setState(app->canvas, 0xf);
	} else {
		Canvas_loadMap(app->canvas, (pending & 0xff) + 1);
	}
	game->pendingChangeMap = 0;
}

/* 0x0003d030 */
void Game_spawnPlayer(Game *game, int unused, int keepPos, int arg)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;

	/* Only a game restored from a save can resume in familiar mode. */
	if (game->loadedFromSave != 0)
		keepPos = app->player->isFamiliar;

	if (game->loadedFromSave == 0 || keepPos == 0) {
		int tx, ty, dir, angle;
		uint32_t packed = (uint32_t)game->spawnOverride;

		if (packed == 0) {
			int tile = app->render->playerTile;
			dir = app->render->spawnDir;
			ty = tile / 32;
			tx = tile - ty * 32;
		} else {
			/* x bits 0-4, y bits 5-9, dir bits 10-17 */
			tx = packed & 0x1f;
			dir = (packed & 0x3ffff) >> 10;
			ty = (packed & 0x3ff) >> 5;
			game->spawnOverride = 0;
		}

		switch (dir) {
		case 0: angle = 0x200; break;
		case 1: angle = 0; break;
		case 3: angle = 0x300; break;
		default: angle = 0x100; break;
		}

		cv->destX = cv->viewX = tx * 64 + 32;
		cv->destY = cv->viewY = ty * 64 + 32;
		cv->viewZ = 0x24;
		cv->destAngle = angle;
		cv->viewAngle = angle;
		app->player->ce.x = cv->viewX;
		app->player->ce.y = cv->viewY;
		Game_relinkEntity(app->player);
		app->render->f58 = 4;
		game->turnTime = app->time;
	} else {
		/* Resume as the familiar: the body stays at the current view, the
		 * camera goes back to the familiar's saved spot. */
		int x = cv->savedViewX;
		int y = cv->savedViewY;
		int angle = cv->savedViewAngle;

		app->player->ce.x = cv->viewX;
		app->player->ce.y = app->canvas->viewY;
		app->player->isFamiliar = 0;
		Game_relinkEntity(app->player);
		Player_setFamiliar(app->player, 1, 0, 0);
		cv = app->canvas;
		cv->destX = cv->viewX = x;
		cv->destY = cv->viewY = y;
		cv->destZ = cv->viewZ = 0xf;
		cv->destAngle = cv->viewAngle = angle;
		Game_relinkEntity(app->player);
	}
}

/* Allocates the next entity slot, or NULL when the table is full. */
static Entity *nextEntity(Game *game)
{
	if (game->numEntities == MAX_ENTITIES)
		return NULL;
	return &game->entities[game->numEntities++];
}

/* 0x00026584 */
void Game_spawnEntities(Game *game)
{
	Applet *app = g_app;
	Combat *combat;
	EntityDef *blockerDef, *blockerDefAlt, *blockerDefLow;
	Entity *ent;
	int mapId = app->canvas->mapId;
	int highestMap = app->player->highestMap;
	int64_t memBefore, memAfter;
	int i;

	for (i = 0; i < 4; i++)
		game->trackedSlots[i] = -1;
	game->watchEntity = NULL;
	game->alertEntity = NULL;
	memBefore = App_getFreeMemory(app);
	game->entityMem[0] = (int32_t)memBefore;
	game->entityMem[1] = (int32_t)(memBefore >> 32);
	game->numMonsters = 0;
	game->numLerpSprites = 0;
	for (i = 0; i < NUM_LERP_SPRITES; i++)
		game->lerpSprites[i].hSprite = 0;
	game->numScriptThreads = 0;
	for (i = 0; i < NUM_SCRIPT_THREADS; i++)
		ScriptThread_reset_34fec(&game->scriptThreads[i]);

	/* Weapon slot 0 (the "default" weapon) is copied from a per-progress entry. */
	combat = app->combat;
	if (highestMap - 1 <= 6 && mapId != 7)
		combat->weapons[0] = combat->weapons[highestMap + 34];
	else
		combat->weapons[0] = combat->weapons[41];

	/* Entity 0: the world; 1 and 2: the player (two defs). */
	game->numEntities = 1;
	game->entities[0].def = EntityDefs_findType(app->entityDefs, 0, 0);
	String_assign(&game->entities[0].name, &game->entities[0].def->name);

	ent = &game->entities[game->numEntities++];
	ent->def = EntityDefs_findType(app->entityDefs, 1, 0);
	String_assign(&ent->name, &ent->def->name);
	ent->info = 0x40000;

	ent = &game->entities[game->numEntities++];
	ent->def = EntityDefs_findType(app->entityDefs, 1, 1);
	String_assign(&ent->name, &ent->def->name);

	blockerDef = EntityDefs_findType(app->entityDefs, 0xc, 0);
	blockerDefAlt = EntityDefs_findType(app->entityDefs, 0xc, 1);
	blockerDefLow = EntityDefs_findType(app->entityDefs, 0xd, 0);

	/* Map sprites -> entities */
	for (i = 0; i < app->render->numMapSprites; i++) {
		Render *render;
		Sprite *spr;
		EntityDef *def;
		uint32_t info;
		int tile, x, y;

		Canvas_drawLoading(app->canvas);
		render = app->render;
		spr = &render->sprites[i];
		info = spr->info;

		if (info & 0x8000000) {
			/* Flagged "no entity": just clear the flag. */
			spr->info = info & ~0x8000000u;
			continue;
		}

		tile = info & 0xff;
		if (info & 0x40000)
			tile += 0x101;
		x = spr->x;
		y = spr->y;
		def = EntityDef_findByTile(app->entityDefs, tile);

		if (def == NULL || def->eType != 2) {
			uint32_t wall;
			info = spr->info;
			/* Wall-mounted sprites: move the link point onto the tile in front. */
			wall = info & 0x780000;
			if (wall != 0x600000 && wall != 0x180000 && wall != 0) {
				if (info & 0x200000)
					x -= 0x40;
				else if (info & 0x100000)
					y -= 0x40;
				else if (info & 0x80000)
					y += 0x20;
				else if (info & 0x400000)
					x += 0x20;
			}

			if (def == NULL) {
				/* No definition: only solid decorations get a blocker entity. */
				if ((info & 0x20000) == 0)
					continue;
				ent = nextEntity(game);
				if (ent == NULL)
					return;
				ent->info = (i + 1) & 0xffff;
				if (tile == 0xa6 || tile == 0xa8 || tile == 0xc2) {
					ent->def = blockerDefLow;
					if (spr->info & 0x40000000)
						spr->info &= ~0x40000000u;
				} else if (spr->info & 0x40000000) {
					ent->def = blockerDefAlt;
					spr->info &= ~0x40000000u;
				} else {
					ent->def = blockerDef;
				}
				String_assign(&ent->name, &ent->def->name);
				spr->ent = ent;
				if ((spr->info & 0x10000) == 0)
					Game_linkEntity(game, ent, x >> 6, y >> 6);
				continue;
			}
		}

		ent = nextEntity(game);
		if (ent == NULL)
			return;
		ent->def = def;
		ent->info = (i + 1) & 0xffff;

		if (def->eType == 2) {
			EntityMonster *mon;
			if (game->numMonsters == MAX_MONSTERS)
				return;
			mon = &game->monsters[game->numMonsters++];
			ent->monster = mon;
			EntityMonster_reset(mon);
			/* The sprite's facing bits become the monster's facing flags. */
			info = spr->info;
			if (info & 0x80000)
				mon->flags |= 0x1000;
			else if (info & 0x100000)
				mon->flags |= 0x2000;
			else if (info & 0x200000)
				mon->flags |= 0x3000;
			else if (info & 0x400000)
				mon->flags |= 0x4000;
			spr->info &= 0xff87ffffu;
			ent->info |= 0x80000;
			Game_deactivate(game, ent);
		} else if (def->eType == 7 &&
		           (def->eSubType == 1 || def->eSubType == 6 || def->eSubType == 4)) {
			spr->info |= 0x80000000u;
		}
		Entity_initspawn(ent);
		spr->ent = ent;
		if ((spr->info & 0x10000) == 0)
			Game_linkEntity(game, ent, x >> 6, y >> 6);
	}

	/* Eight hidden sprites reserved for dropped items. */
	game->firstDropEntity = game->numEntities;
	for (i = 0; i < NUM_DROP_SLOTS; i++) {
		ent = nextEntity(game);
		if (ent == NULL)
			return;
		ent->info = (app->render->firstDropSprite + i + 1) & 0xffff;
		app->render->dropSprites[i]->info |= 0x10000;
	}

	/* Lines with an entity definition (doors, switches, ...). */
	game->firstLineEntity = game->numEntities;
	for (i = 0; i < app->render->numLines; i++) {
		Render *render = app->render;
		Line *line = &render->lines[i];
		EntityDef *def = EntityDef_findByTile(app->entityDefs, line->texture + 0x101);
		int x, y;

		if (def == NULL)
			continue;
		ent = nextEntity(game);
		if (ent == NULL)
			return;
		ent->def = def;
		ent->info = ((i + 1) & 0xffff) | 0x200000;
		Entity_initspawn(ent);

		/* Link at the line's midpoint, nudged one unit to its front side. */
		x = line->x1 + (line->x2 - line->x1) / 2;
		y = line->y1 + (line->y2 - line->y1) / 2;
		if (line->flags & 0x800)
			y -= 1;
		else if (line->flags & 0x2000)
			x += 1;
		else if (line->flags & 0x1000)
			y += 1;
		else if (line->flags & 0x4000)
			x -= 1;
		Game_linkEntity(game, ent, x >> 6, y >> 6);
	}
	game->lastEntity = game->numEntities - 1;

	/* Solid tiles get the world entity unless a line entity already sits there. */
	for (i = 0; i < 1024; i++) {
		Entity *e;
		if ((app->render->mapFlags[i] & 1) == 0)
			continue;
		for (e = game->entityGrid[i]; e != NULL; e = e->nextOnTile) {
			if (e->info & 0x200000)
				break;
		}
		if (e == NULL)
			Game_linkWorldEntity(game, i % 32, i / 32);
	}

	memAfter = App_getFreeMemory(app);
	memBefore = (int64_t)(((uint64_t)(uint32_t)game->entityMem[1] << 32) | (uint32_t)game->entityMem[0]);
	memBefore -= memAfter;
	game->entityMem[0] = (int32_t)memBefore;
	game->entityMem[1] = (int32_t)(memBefore >> 32);

	Game_loadCameras(game);
	Game_initDebugVars(game);
}

/* 0x000258a4 */
void Game_loadCameras(Game *game)
{
	Render *render = g_app->render;
	int ev, i;

	game->activeCamera = NULL;
	game->cinematicActive = 0;
	game->numCameras = 0;
	game->numCameraKeys = 0;

	/* Pass 1: count events that hold camera keys, and the keys themselves. */
	for (ev = 0; ev < g_app->render->numTileEvents; ev++) {
		uint32_t event = render->tileEvents[ev];
		int first = EVENT_FIRST_CMD(event) * 3;
		int keys = 0;
		for (i = 0; i < (int)EVENT_NUM_CMDS(event) * 3; i += 3) {
			if (render->byteCodes[first + i] == OP_CAMERA_KEY) {
				keys++;
				game->numCameraKeys++;
			}
		}
		if (keys != 0)
			game->numCameras++;
	}

	if (game->numCameras == 0 || game->numCameras > 32)
		return;

	/* Pass 2: build one Camera per event; rewrite each key command's arg to
	 * (camera | keyIndex << 8) so the interpreter can find it later. */
	game->numCameras = 0;
	game->numCameraKeys = 0;
	for (ev = 0; ev < g_app->render->numTileEvents; ev++) {
		uint32_t event = g_app->render->tileEvents[ev];
		int first = EVENT_FIRST_CMD(event) * 3;
		int loop = 0;
		int duration = 1000;
		int camIdx = game->numCameras;
		int keys = 0;

		if (camIdx >= 32)
			return;

		for (i = 0; i < (int)EVENT_NUM_CMDS(event) * 3; i += 3) {
			Render *r = g_app->render;
			int op = r->byteCodes[first + i];

			if (op == OP_CAMERA_KEY) {
				uint32_t arg = (uint32_t)r->byteCodes[first + i + 1];
				CameraKey *key;
				uint8_t flags;

				if (keys == 0)
					game->cameras[camIdx].firstKey = game->numCameraKeys;
				flags = (arg & 0x80000000u) ? 1 : 0;
				if (arg & 0x40000000u)
					flags |= 2;
				if (arg & 0x20000000u)
					flags |= 4;
				key = &game->cameraKeys[game->numCameraKeys];
				key->x = arg & 0x1f;
				key->y = (arg & 0x3ff) >> 5;
				key->z = (arg & 0x3ffff) >> 10;
				key->duration = duration;
				if (arg & 0x10000000u)
					flags |= 8;
				key->angle = Dir_toAngle(g_app->textUtil, (arg & 0x7fffff) >> 18);
				key->flags = flags;
				r->byteCodes[first + i + 1] = game->numCameras | keys << 8;
				keys++;
				game->numCameraKeys++;
			} else if (op == OP_CAMERA_DELAY) {
				duration = r->byteCodes[first + i + 1];
			} else if (op == OP_CAMERA_LOOP) {
				loop = r->byteCodes[first + i + 1] != 0;
			}
		}

		if (keys != 0) {
			game->cameras[camIdx].numKeys = keys;
			Camera_initKeys(&game->cameras[camIdx], loop);
			game->numCameras++;
		}
	}
}

/* 0x0002890c */
void Game_initDebugVars(Game *game)
{
	Applet *app = g_app;

	if (game->debugVarsLoaded != 0)
		return;
	game->debugVarsLoaded = 1;

	app->player->enableHelp = App_debugCheckpoint(app, "help", app->player->enableHelp != 0) != 0;
	game->disableAI = App_debugCheckpoint(app, "disableAI", 0) != 0;
	if (App_debugCheckpoint(app, "equip", 0) != 0)
		Player_equipForMap(app->player, app->canvas->mapId);
	app->player->noclip = App_debugCheckpoint(app, "noclip", 0) != 0;
	app->player->god = App_debugCheckpoint(app, "god", 0) != 0;
	app->canvas->showMonsters = App_debugCheckpoint(app, "showMonsters", 0) != 0;
	app->canvas->showNodes = App_debugCheckpoint(app, "showNodes", 0) != 0;
	app->canvas->showLocation = App_debugCheckpoint(app, "showLocation", 0) != 0;
	app->canvas->showDebugVar = App_debugCheckpoint(app, "showSaveSize", 0) != 0;
	app->canvas->showCodes = App_debugCheckpoint(app, "showCodes", 0) != 0;
	app->player->ce.weapon = App_debugCheckpoint(app, "weapon", app->player->ce.weapon) & 0xff;
}

/* ---- Config ---- */

/* 0x00025b60
 * Config layout: int 'HCFG', int version 0x15, u8 difficulty, u8 vibrate,
 * u8 weaponCycleKey, u8 enableHelp, int totalDeaths, int volume, int animFrames,
 * int helpBitmask, int invHelpBitmask, int spellHelpBitmask,
 * 13 x {int selected, int scroll} menu indexes, int 'TCFG'. */
int Game_loadConfig(Game *x)
{
	Applet *app = g_app;
	InputStream stream;
	String name;
	int ok = 0;
	int i;

	if (!App_isConfigValid()) {
		Game_deleteSaveFiles();
		return 0;
	}

	InputStream_ctor(&stream);
	String_ctorCStr(&name, "Config");
	i = FileStream_open(&stream, &name);
	String_dtor(&name, 0);
	if (i == 1) {
		if (Stream_readInt(&stream) == MAGIC_CONFIG_HEAD &&
		    streamTrailerInt(&stream) == MAGIC_CONFIG_TAIL &&
		    Stream_readInt(&stream) == CONFIG_VERSION) {
			app->game->difficulty = (uint8_t)Stream_readUByte(&stream);
			app->canvas->vibrateEnabled = Stream_readBool(&stream);
			app->canvas->weaponCycleKey = Stream_readBool(&stream);
			app->player->enableHelp = Stream_readBool(&stream);
			app->player->totalDeaths = Stream_readInt(&stream);
			app->sndSys->volume = Stream_readInt(&stream);
			Canvas_setAnimFrames(app->canvas, Stream_readInt(&stream));
			app->player->helpBitmask = (uint32_t)Stream_readInt(&stream);
			app->player->invHelpBitmask = (uint32_t)Stream_readInt(&stream);
			app->player->spellHelpBitmask = (uint32_t)Stream_readInt(&stream);
			for (i = 0; i < NUM_SAVED_MENUS; i++) {
				app->menu->savedIndexes[i * 2] = Stream_readInt(&stream);
				app->menu->savedIndexes[i * 2 + 1] = Stream_readInt(&stream);
			}
			ok = 1;
		} else {
			Game_deleteSaveFiles();
		}
	}
	InputStream_dtor(&stream, 0);
	return ok;
}

/* 0x000363d8 */
void Game_saveConfig(Game *game)
{
	Applet *app = g_app;
	OutputStream os;
	String name;
	int i, opened;

	OutputStream_ctor(&os);
	String_ctorCStr(&name, "Config");
	opened = File_open(&os, &name, _OFM_CREATE);
	String_dtor(&name, 0);
	if (opened == 1) {
		OutStream_writeInt(&os, MAGIC_CONFIG_HEAD);
		OutStream_writeInt(&os, CONFIG_VERSION);
		OutStream_writeBool(&os, (int8_t)app->game->difficulty);
		OutStream_writeByte(&os, app->canvas->vibrateEnabled);
		OutStream_writeByte(&os, app->canvas->weaponCycleKey);
		OutStream_writeByte(&os, app->player->enableHelp);
		OutStream_writeInt(&os, app->player->totalDeaths);
		OutStream_writeInt(&os, app->sndSys->volume);
		OutStream_writeInt(&os, app->canvas->animFrames);
		OutStream_writeInt(&os, (int)app->player->helpBitmask);
		OutStream_writeInt(&os, (int)app->player->invHelpBitmask);
		OutStream_writeInt(&os, (int)app->player->spellHelpBitmask);
		for (i = 0; i < NUM_SAVED_MENUS; i++) {
			OutStream_writeInt(&os, app->menu->savedIndexes[i * 2]);
			OutStream_writeInt(&os, app->menu->savedIndexes[i * 2 + 1]);
		}
		OutStream_writeInt(&os, MAGIC_CONFIG_TAIL);
		if (os.error == 0)
			Resource_dispose(&os);
	}
	OutputStream_dtor(&os, 0);
}

/* ---- player record (SD_0 = last save, SD_1 = level-start snapshot) ---- */

/* 0x00036810
 * Layout: int 'HPLR', short mapId, u8 f260, u8 isFamiliar,
 * [familiar: int savedViewX, int savedViewY, int savedViewAngle, short savedViewZ],
 * int x, int y, int angle, int x2, int y2, int gotoX, int gotoY, int gotoAngle,
 * Player_saveState record, int 'TPLR'. */
int Game_savePlayerState(Game *game, OutputStream *os, short mapId, int x, int y, int angle, int x2, int y2)
{
	Applet *app = g_app;

	OutStream_writeInt(os, MAGIC_PLAYER_HEAD);
	OutStream_writeShort(os, mapId);
	OutStream_writeBool(os, app->player->f260 != 0);
	OutStream_writeBool(os, app->player->isFamiliar != 0);
	if (app->player->isFamiliar != 0) {
		OutStream_writeInt(os, app->canvas->savedViewX);
		OutStream_writeInt(os, app->canvas->savedViewY);
		OutStream_writeInt(os, app->canvas->savedViewAngle);
		OutStream_writeShort(os, app->canvas->savedViewZ);
	}
	OutStream_writeInt(os, x);
	OutStream_writeInt(os, y);
	OutStream_writeInt(os, angle);
	OutStream_writeInt(os, x2);
	OutStream_writeInt(os, y2);
	OutStream_writeInt(os, app->canvas->gotoX);
	OutStream_writeInt(os, app->canvas->gotoY);
	OutStream_writeInt(os, app->canvas->gotoAngle);
	if (Player_saveState(app->player, os) == 0)
		return 0;
	OutStream_writeInt(os, MAGIC_PLAYER_TAIL);
	return os->error == 0;
}

/* 0x00028038 */
int Game_loadPlayerHeader(Game *game, InputStream *stream)
{
	Applet *app = g_app;
	Canvas *cv;

	if (Stream_readInt(stream) != MAGIC_PLAYER_HEAD)
		return 0;
	if (streamTrailerInt(stream) != MAGIC_PLAYER_TAIL)
		return 0;

	game->savedMapId = Stream_readShort(stream);
	app->player->f260 = Stream_readUByte(stream) == 1;
	app->player->isFamiliar = Stream_readUByte(stream) == 1;
	cv = app->canvas;
	if (!app->player->isFamiliar) {
		cv->viewX = Stream_readInt(stream);
		cv->viewY = Stream_readInt(stream);
		cv->viewAngle = Stream_readInt(stream);
	} else {
		/* Note the original reads the familiar block back in a different
		 * order than it was written: the first triple (the saved body
		 * position) becomes the view, the following x/y/angle become the
		 * saved position. */
		cv->viewX = cv->destX = Stream_readInt(stream);
		cv->viewY = cv->destY = Stream_readInt(stream);
		cv->viewAngle = cv->destAngle = Stream_readInt(stream);
		cv->savedViewZ = (int16_t)Stream_readShort(stream);
		cv->savedViewX = Stream_readInt(stream);
		cv->savedViewY = Stream_readInt(stream);
		cv->savedViewAngle = Stream_readInt(stream);
	}
	cv->viewZ = 0x24;
	cv->destZ = 0x24;
	cv->prevX = Stream_readInt(stream);
	cv->prevY = Stream_readInt(stream);
	cv->gotoX = Stream_readInt(stream);
	cv->gotoY = Stream_readInt(stream);
	cv->gotoAngle = Stream_readInt(stream);
	game->spawnOverride = 0;
	return Player_loadState(app->player, stream) != 0;
}

/* 0x00028dac  slot 1 = "SD_0" (saved game), slot 2 = "SD_1" (level start);
 * any other slot just restarts the saved map with the current player. */
int Game_loadSaveGame(Game *game, int slot)
{
	Applet *app = g_app;
	Canvas *cv;

	Applet_resetCounters(app, 0);
	if (slot == 1 || slot == 2) {
		InputStream stream;
		String name;
		int ok = 0;

		InputStream_ctor(&stream);
		String_ctor(&name);
		saveFileName(&name, slot == 1 ? 0 : 1);
		if (FileStream_open(&stream, &name) != 0)
			ok = Game_loadPlayerHeader(game, &stream);
		String_dtor(&name, 0);
		InputStream_dtor(&stream, 0);
		if (!ok)
			return 0;
	}

	game->loadType = slot;
	app->player->nextLevelXP = Player_xpForLevel(app->player, app->player->level);
	cv = app->canvas;
	if (cv->viewX != 0 && cv->viewY != 0) {
		int dir = Dir_fromAngle(app->textUtil, cv->viewAngle & 0x3ff);
		cv = app->canvas;
		game->spawnOverride = ((cv->viewX & 0x7ff) >> 6) |
		                      ((cv->viewY & 0x7ff) >> 6) << 5 |
		                      dir << 10;
	}
	Canvas_loadMapEx(app->canvas, game->savedMapId, 0);
	game->f9c14 = 0;
	game->loadedFromSave = 1;
	return 1;
}

/* ---- per-level world state (SD_2..SD_9) ---- */

/* 0x00036578  short count, then per entity: short handle (index | flag bits)
 * followed by Entity_saveState's record. */
int Game_saveEntityStates(Game *game, OutputStream *os)
{
	int16_t handles[MAX_ENTITIES];
	int count = 0;
	int i;

	for (i = 0; i < game->numEntities; i++) {
		int handle;
		if (Entity_getSaveHandle(&game->entities[i], &handle) != 0)
			handles[count++] = (int16_t)handle;
	}
	OutStream_writeShort(os, (int16_t)count);
	for (i = 0; i < count; i++) {
		int handle = handles[i];
		OutStream_writeShort(os, handle);
		Entity_saveState(&game->entities[handle & 0x3ff], os, handle);
	}
	return os->error;
}

/* 0x00025d40 */
void Game_loadEntityStates(Game *game, InputStream *stream)
{
	int count = Stream_readShort(stream);
	int i;

	for (i = 0; i < count; i++) {
		int handle = Stream_readShort(stream);
		Entity_loadState(&game->entities[handle & 0x3ff], stream, handle);
	}
}

/* 0x00036998  int travelTime, int elapsed, short hSprite, short sprite x/y/z,
 * short src x/y/z, short dst x/y/z, u8 srcScale, u8 dstScale, u8 flags,
 * u8 script-thread index. */
void LerpSprite_saveState(LerpSprite *ls, OutputStream *os)
{
	Applet *app = g_app;
	Sprite *spr;

	if (ls->hSprite == 0)
		return;
	spr = &app->render->sprites[ls->hSprite - 1];
	OutStream_writeInt(os, ls->travelTime);
	OutStream_writeInt(os, app->playTime - ls->startTime);
	OutStream_writeShort(os, (int16_t)ls->hSprite);
	OutStream_writeShort(os, (int16_t)spr->x);
	OutStream_writeShort(os, (int16_t)spr->y);
	OutStream_writeShort(os, (int16_t)spr->z);
	OutStream_writeShort(os, (int16_t)ls->srcX);
	OutStream_writeShort(os, (int16_t)ls->srcY);
	OutStream_writeShort(os, (int16_t)ls->srcZ);
	OutStream_writeShort(os, (int16_t)ls->dstX);
	OutStream_writeShort(os, (int16_t)ls->dstY);
	OutStream_writeShort(os, (int16_t)ls->dstZ);
	OutStream_writeBool(os, (int8_t)ls->srcScale);
	OutStream_writeBool(os, (int8_t)ls->dstScale);
	OutStream_writeBool(os, (int8_t)ls->flags);
	OutStream_writeBool(os, (int8_t)Game_getScriptThreadIndex(ls->thread));
}

/* 0x00028b20 */
void LerpSprite_loadState(LerpSprite *ls, InputStream *stream)
{
	Applet *app = g_app;
	Sprite *spr;

	ls->travelTime = Stream_readInt(stream);
	ls->startTime = app->playTime - Stream_readInt(stream);
	ls->hSprite = Stream_readShort(stream);
	spr = &app->render->sprites[ls->hSprite - 1];
	spr->x = Stream_readShort(stream);
	spr->y = Stream_readShort(stream);
	spr->z = Stream_readShort(stream);
	Render_relinkSprite(app->render, spr);
	ls->srcX = Stream_readShort(stream);
	ls->srcY = Stream_readShort(stream);
	ls->srcZ = Stream_readShort(stream);
	ls->dstX = Stream_readShort(stream);
	ls->dstY = Stream_readShort(stream);
	ls->dstZ = Stream_readShort(stream);
	ls->srcScale = Stream_readUByte(stream);
	ls->dstScale = Stream_readUByte(stream);
	ls->flags = Stream_readUByte(stream);
	ls->thread = &app->game->scriptThreads[Stream_readUByte(stream)];
	LerpSprite_calcDist(ls);
}

/* True for door lines whose open state is saved (flags 0x20 and 8). */
static int isSavedDoorLine(const Line *line)
{
	return (line->flags & 0x20) != 0 && (line->flags & 8) != 0;
}

/* True for an event command whose state is saved: a consumed one-shot
 * (flags word cleared), except container data (0x2f) and passwords (0xa). */
static int isConsumedCommand(const Render *render, int code)
{
	int op = render->byteCodes[code];
	return op != OP_CONTAINER && op != OP_PASSWORD && render->byteCodes[code + 2] == 0;
}

/* 0x00037530
 * Layout: int 'HWLD', int mapHash, u8 automapFlag, int levelTime,
 * entity states (Game_saveEntityStates),
 * ceil(numLines/8) bytes: line flag 0x80 (seen on automap), bit i = line 8k+i,
 * door bytes: bit per door line (flags 0x20 & 8) = open (0x40), 8 doors/byte,
 * ceil(numMapSprites/4) bytes: per sprite 2 bits {hidden 0x10000, 0x8000000},
 * 128 bytes: tile flag 0x20 (automap visited) per tile,
 * per tile event: u8 state, u8 n, n x u8 consumed-command index,
 * u8 numLerpSprites + records, u8 numScriptThreads + 16 thread records,
 * int dropIndex, int moves, int xpGained, u8 numNotebookIndexes, u8 questComplete,
 * u8 questFailed, 8 x short notebookIndexes, u8 floorHeight, u8 ceilHeight,
 * u8 mapFlagsByte, u8 floorUniform, u8 ceilUniform, u8 bobEnable, short goldPaid,
 * short watchEntity index (-1 none), int 'TWLD'. */
int Game_saveWorldState(Game *game, OutputStream *os, int brief)
{
	Applet *app = g_app;
	Render *render = app->render;
	int numLines, numSprites, numEvents;
	int i, bit;
	uint8_t bits;

	OutStream_writeInt(os, MAGIC_WORLD_HEAD);
	OutStream_writeInt(os, render->mapHash);
	OutStream_writeByte(os, game->automapFlag);
	app->playTime = (int)GETUPTIMEMS();
	OutStream_writeInt(os, app->playTime - game->levelStartTime);
	Game_saveEntityStates(game, os);

	/* Lines marked on the automap */
	numLines = render->numLines;
	for (i = 0; i < numLines; i += 8) {
		bits = 0;
		for (bit = 0; bit < 8 && i + bit != numLines; bit++) {
			if (render->lines[i + bit].flags & 0x80)
				bits |= 1 << bit;
		}
		OutStream_writeBool(os, (int8_t)bits);
	}

	/* Door open states, eight doors per byte */
	numLines = render->numLines;
	i = 0;
	while (i < numLines) {
		bits = 0;
		bit = 0;
		while (bit < 8 && i != numLines) {
			const Line *line = &render->lines[i];
			if (isSavedDoorLine(line)) {
				if (line->flags & 0x40)
					bits |= 1 << bit;
				bit++;
			}
			i++;
		}
		OutStream_writeBool(os, (int8_t)bits);
	}

	/* Map sprites, four per byte (may read a few sprites past the map's) */
	numSprites = render->numMapSprites;
	i = 0;
	while (i < numSprites) {
		bits = 0;
		for (bit = 0; bit < 8; bit += 2) {
			uint32_t info = render->sprites[i++].info;
			if (info & 0x10000)
				bits |= 1 << bit;
			if (info & 0x8000000)
				bits |= 1 << (bit + 1);
		}
		OutStream_writeBool(os, (int8_t)bits);
	}

	/* Visited tiles */
	for (i = 0; i < 1024; i += 8) {
		bits = 0;
		for (bit = 0; bit < 8; bit++) {
			if (render->mapFlags[i + bit] & 0x20)
				bits |= 1 << bit;
		}
		OutStream_writeBool(os, (int8_t)bits);
	}

	/* Tile events: state and consumed one-shot commands */
	numEvents = render->numTileEvents;
	for (i = 0; i < numEvents; i++) {
		uint32_t event = render->tileEvents[i];
		int first = EVENT_FIRST_CMD(event) * 3;
		int count = EVENT_NUM_CMDS(event) * 3;
		int c;
		int8_t consumed = 0;

		OutStream_writeBool(os, (int)EVENT_STATE(event));
		for (c = 0; c < count; c += 3) {
			if (isConsumedCommand(render, first + c))
				consumed++;
		}
		OutStream_writeBool(os, consumed);
		for (c = 0; c < count; c += 3) {
			if (isConsumedCommand(render, first + c))
				OutStream_writeBool(os, (int8_t)(c / 3));
		}
	}

	OutStream_writeBool(os, (int8_t)game->numLerpSprites);
	for (i = 0; i < NUM_LERP_SPRITES; i++) {
		if (game->lerpSprites[i].hSprite != 0)
			LerpSprite_saveState(&game->lerpSprites[i], os);
	}
	OutStream_writeBool(os, (int8_t)game->numScriptThreads);
	for (i = 0; i < NUM_SCRIPT_THREADS; i++)
		ScriptThread_saveState(&game->scriptThreads[i], os);

	OutStream_writeInt(os, game->dropIndex);
	OutStream_writeInt(os, app->player->moves);
	OutStream_writeInt(os, app->player->xpGained);
	OutStream_writeBool(os, (int8_t)app->player->numNotebookIndexes);
	OutStream_writeBool(os, (int8_t)app->player->questComplete);
	OutStream_writeBool(os, (int8_t)app->player->questFailed);
	for (i = 0; i < 8; i++)
		OutStream_writeShort(os, app->player->notebookIndexes[i]);
	OutStream_writeBool(os, (int8_t)app->render->floorHeight);
	OutStream_writeBool(os, (int8_t)app->render->ceilHeight);
	OutStream_writeBool(os, (int8_t)app->render->mapFlagsByte);
	OutStream_writeByte(os, app->render->floorUniform);
	OutStream_writeByte(os, app->render->ceilUniform);
	OutStream_writeByte(os, app->render->bobEnable);
	OutStream_writeShort(os, (int16_t)game->goldPaid);
	OutStream_writeShort(os, game->watchEntity == NULL ? -1
	                         : (int16_t)Game_getEntityIndex(game->watchEntity));
	OutStream_writeInt(os, MAGIC_WORLD_TAIL);
	return os->error;
}

/* 0x0002a96c */
int Game_loadWorldState(Game *game, InputStream *stream)
{
	Applet *app = g_app;
	Render *render;
	int numLines, numSprites, numEvents;
	int i, bit, hash, watch;
	uint32_t bits;

	if (Stream_readInt(stream) != MAGIC_WORLD_HEAD ||
	    streamTrailerInt(stream) != MAGIC_WORLD_TAIL) {
		game->spawnOverride = 0;
		return 0;
	}
	hash = Stream_readInt(stream);
	if (app->render->mapHash != hash) {
		/* A zero hash keeps the pending spawn position. */
		if (hash != 0)
			game->spawnOverride = 0;
		return 0;
	}

	game->automapFlag = Stream_readBool(stream);
	app->playTime = (int)GETUPTIMEMS();
	game->levelStartTime = app->playTime - Stream_readInt(stream);
	Game_loadEntityStates(game, stream);
	Canvas_drawLoading(app->canvas);

	render = app->render;
	numLines = render->numLines;
	for (i = 0; i < numLines; i += 8) {
		bits = Stream_readUByte(stream);
		for (bit = 0; bit < 8; bit++) {
			if (bits & (1u << bit))
				render->lines[i + bit].flags |= 0x80;
		}
	}
	Canvas_drawLoading(app->canvas);

	/* Doors saved open: reopen and clear the doorway tile. */
	numLines = render->numLines;
	i = 0;
	while (i < numLines) {
		bits = Stream_readUByte(stream);
		bit = 0;
		while (bit < 8 && i != numLines) {
			Line *line = &render->lines[i];
			if (isSavedDoorLine(line)) {
				if (bits & (1u << bit)) {
					line->flags |= 0x40040;
					Game_unlinkPlayerFromTile(game,
						((line->x2 >> 6) + (line->x1 >> 6)) >> 1,
						((line->y1 >> 6) + (line->y2 >> 6)) >> 1);
				}
				bit++;
			}
			i++;
		}
	}
	Canvas_drawLoading(app->canvas);

	numSprites = render->numMapSprites;
	i = 0;
	while (i < numSprites) {
		bits = Stream_readUByte(stream);
		for (bit = 0; bit < 8; bit += 2) {
			Sprite *spr = &render->sprites[i++];
			if (bits & (1u << bit))
				spr->info |= 0x10000;
			else
				spr->info &= ~0x10000u;
			if (bits & (1u << (bit + 1)))
				spr->info |= 0x8000000;
			else
				spr->info &= ~0x8000000u;
		}
	}
	Canvas_drawLoading(app->canvas);

	for (i = 0; i < 1024; i += 8) {
		bits = Stream_readUByte(stream);
		for (bit = 0; bit < 8; bit++) {
			if (bits & (1u << bit))
				render->mapFlags[i + bit] |= 0x20;
		}
	}
	Canvas_drawLoading(app->canvas);

	numEvents = render->numTileEvents;
	for (i = 0; i < numEvents; i++) {
		uint32_t state = (uint32_t)Stream_readUByte(stream);
		uint32_t event = render->tileEvents[i];
		int n, k;

		render->tileEvents[i] = (event & 0xc7ffffffu) | state << 27;
		n = Stream_readUByte(stream);
		for (k = 0; k < n; k++) {
			int cmd = Stream_readUByte(stream);
			render->byteCodes[EVENT_FIRST_CMD(event) * 3 + cmd * 3 + 2] = 0;
		}
	}
	Canvas_drawLoading(app->canvas);

	game->numLerpSprites = Stream_readUByte(stream);
	for (i = 0; i < NUM_LERP_SPRITES; i++) {
		if (i < game->numLerpSprites)
			LerpSprite_loadState(&game->lerpSprites[i], stream);
		else
			game->lerpSprites[i].hSprite = 0;
	}
	Canvas_drawLoading(app->canvas);

	game->numScriptThreads = Stream_readUByte(stream);
	for (i = 0; i < NUM_SCRIPT_THREADS; i++) {
		ScriptThread_loadState(&game->scriptThreads[i], stream);
		game->scriptThreads[i].inuse = game->scriptThreads[i].numFrames >= 1;
	}
	Canvas_drawLoading(app->canvas);
	Game_updateMovers(game);
	Canvas_drawLoading(app->canvas);

	game->dropIndex = Stream_readInt(stream);
	app->player->moves = Stream_readInt(stream);
	app->player->xpGained = Stream_readInt(stream);
	app->player->numNotebookIndexes = Stream_readUByte(stream);
	app->player->questComplete = (uint8_t)Stream_readUByte(stream);
	app->player->questFailed = (uint8_t)Stream_readUByte(stream);
	for (i = 0; i < 8; i++)
		app->player->notebookIndexes[i] = (int16_t)Stream_readShort(stream);
	app->render->floorHeight = (int16_t)Stream_readUByte2(stream);
	app->render->ceilHeight = (int16_t)Stream_readUByte2(stream);
	app->render->mapFlagsByte = (uint8_t)Stream_readUByte(stream);
	app->render->floorUniform = Stream_readBool(stream);
	app->render->ceilUniform = Stream_readBool(stream);
	app->render->bobEnable = Stream_readBool(stream);
	game->goldPaid = Stream_readShort(stream);
	watch = Stream_readShort(stream);
	if (watch != -1)
		game->watchEntity = &game->entities[watch];

	app->hud->hpAnim1 = 0;
	app->hud->lastHealth = 0;
	app->hud->hpAnim2 = 0;
	Game_resetMonsterPositions(game);
	return 1;
}

/* 0x0002af5c  loads "SD_<map+2>" (or "SD_2" for a level-start restore). */
void Game_loadWorld(Game *game)
{
	Applet *app = g_app;
	InputStream stream;
	String name;

	if (app->canvas->mapId >= 8)
		return;
	if (game->skipLevel != 0) {
		game->skipLevel = 0;
		return;
	}

	InputStream_ctor(&stream);
	String_ctor(&name);
	saveFileName(&name, game->loadType == 2 ? 2 : app->canvas->mapId + 2);
	if (FileStream_open(&stream, &name) != 0 && Game_loadWorldState(game, &stream) == 0)
		IFILEMGR_Remove(app->fileMgr, String_cstr(&name));
	String_dtor(&name, 0);
	InputStream_dtor(&stream, 0);
}

/* 0x000366dc  level-start snapshot: player to "SD_1", world to "SD_2". */
void Game_saveLevelSnapshot(Game *game)
{
	Applet *app = g_app;
	OutputStream os;
	String name;

	OutputStream_ctor(&os);
	String_ctor(&name);
	saveFileName(&name, 1);
	if (File_open(&os, &name, _OFM_CREATE) != 0) {
		Canvas *cv = app->canvas;
		if (Game_savePlayerState(game, &os, (short)cv->mapId, cv->viewX, cv->viewY,
		                         cv->viewAngle, cv->viewX, cv->viewY) != 0) {
			Resource_dispose(&os);
			saveFileName(&name, 2);
			/* The world file is left for OutputStream_dtor to flush/close. */
			if (File_open(&os, &name, _OFM_CREATE) != 0)
				Game_saveWorldState(game, &os, 0);
		}
	}
	String_dtor(&name, 0);
	OutputStream_dtor(&os, 0);
}

/* 0x00036cec  saveType bit 0: player record to "SD_0"; bit 1: world of
 * lastMap to "SD_<lastMap+2>" (maps up to 7 only). */
void Game_saveState(Game *game, int lastMap, int mapId, int x, int y, int angle, int x2, int y2, int saveType)
{
	OutputStream os;
	String name;

	Game_saveConfig(game);
	String_ctor(&name);

	if (saveType & 1) {
		int ok = 0;
		OutputStream_ctor(&os);
		saveFileName(&name, 0);
		if (File_open(&os, &name, _OFM_CREATE) != 0 &&
		    Game_savePlayerState(game, &os, (short)mapId, x, y, angle, x2, y2) != 0) {
			Resource_dispose(&os);
			ok = 1;
		}
		OutputStream_dtor(&os, 0);
		if (!ok) {
			String_dtor(&name, 0);
			return;
		}
	}

	if ((saveType & 2) && lastMap + 2 <= 9) {
		OutputStream_ctor(&os);
		saveFileName(&name, lastMap + 2);
		if (File_open(&os, &name, _OFM_CREATE) != 0) {
			Game_saveWorldState(game, &os, 0);
			Resource_dispose(&os);
		}
		OutputStream_dtor(&os, 0);
	}
	String_dtor(&name, 0);
}
