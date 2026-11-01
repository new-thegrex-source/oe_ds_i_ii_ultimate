/*
 * Game persistence: the "Config" file, the "SaveData" file (player records and
 * per-map world snapshots at fixed slot offsets), the save/load of lerp sprites
 * and script threads, and the moving-line (pushable block) positions.
 *
 * Every record is written and read exactly like the phone game did it: little-
 * endian ints and shorts and single bytes, through the core OutputStream /
 * Stream_read* helpers, so a save made by the original has the same layout.
 *
 * File layouts (all fields in order):
 *   Config:  int 0x5d, u8 vibrate, u8 help, u8 fileDebug, int deaths,
 *            int volume, int animFrames, int helpShown, int itemHelp,
 *            int ringHelp, 12 x {int sel, int scroll}.
 *   SaveData slot 0/1 (400 bytes): short mapId, int x, y, angle, x2, y2,
 *            Player_saveState, zero padding up to 400.
 *   SaveData slot 3+ : Game_saveWorldState (see that function).
 */

#include <stdio.h>

#include "game.h"

/* Strings in the module's data area (never copied into the source). */
#define MOD_STR_FILEIO_ERROR   0x30484u
#define MOD_STR_SAVING_GAME    0x30448u
#define MOD_STR_LEVEL_NAME_0   0x30398u
#define MOD_STR_LEVEL_NAME_1   0x30390u
#define MOD_STR_LEVEL_NAME_2   0x30384u
#define MOD_STR_LEVEL_NAME_3   0x30378u
#define MOD_STR_LEVEL_NAME_4   0x3036cu
#define MOD_STR_LEVEL_NAME_5   0x30360u
#define MOD_STR_LEVEL_NAME_6   0x30358u
#define MOD_STR_LEVEL_NAME_7   0x30348u
#define MOD_STR_LEVEL_NAME_8   0x30338u
#define MOD_STR_LEVEL_NAME_9   0x30330u
#define MOD_STR_LEVEL_NAME_10  0x30328u

#define CONFIG_VERSION      0x5d
#define PLAYER_RECORD_SIZE  400
#define NUM_SAVED_MENUS     12
#define NUM_LERP_SPRITES    16
#define NUM_SCRIPT_THREADS  16
#define LEVEL_NAME_SIZE     0x18
#define FILE_ERROR_NOFILE   0x100   /* error code set when "SaveData" cannot be opened */

/* Map ids (resource ids of the .bsp maps). */
#define MAP_FIRST           6000
#define MAP_LAST_SAVED      0x1778  /* maps above this (menu map, item map) keep no world state */
#define MAP_SLOT_BASE       0x176b  /* slot = mapId - 0x176b (map 6000 -> slot 5) */

/* Line.flags: which side of its tile a moving (block) line faces. */
#define LINE_FACE_N   0x800
#define LINE_FACE_S   0x1000
#define LINE_FACE_E   0x2000
#define LINE_FACE_W   0x4000
#define LINE_SEEN     0x80          /* automap */
#define SPRITE_SEEN   0x8000000     /* automap */
#define TILE_SEEN     0x10          /* mapFlags automap bit */
#define TILE_SOLID    0x01

/* Tile-event word fields (Render.tileEvents[]). */
#define EVENT_FIRST_CMD(ev) (((ev) & 0xfffffu) >> 10)
#define EVENT_NUM_CMDS(ev)  (((ev) & 0x7ffffffu) >> 20)
#define EVENT_STATE(ev)     (((ev) & 0x7fffffffu) >> 27)
#define EVENT_STATE_MASK    0x78000000u

/* Commands that are never recorded as "consumed" in a world save. */
#define OP_CONTAINER_ITEM   0x2f
#define OP_FIRE_PIT_DEST    0x3a

/* ---- stream helpers ---------------------------------------------------- */

static int rdInt(File *f) { return Stream_readInt(f); }
static int rdShort(File *f) { return Stream_readShort(f); }
static int rdByte(File *f) { return Stream_readByte(f); }   /* signed */
static int rdUByte(File *f) { return Stream_readUByte(f); }

/* The original seeks its read Files with 0x244b4 ("InputStream_seek", really
 * an IFile seek on the File). */
static void fileSeek(File *f, int whence, int off)
{
	InputStream_seek(f, whence, off);
}

static void setFileError(Game *game, int code)
{
	App_setErrorCode(g_app, code);
	App_setErrorStr(game->app, (char *)ModData_str(&g_mod, MOD_STR_FILEIO_ERROR));
}

/*
 * Which faces of tile t are closed by moving (block) lines: 4 north, 8 south,
 * 2 east, 1 west; 0xf = tile fully boxed in. Shared by Game_clearEnclosedTiles
 * and Game_loadMovableBlocks, which test the same geometry inline.
 */
static unsigned enclosedMask(Render *render, int t)
{
	int ty = t / 32, tx = t % 32;
	int x0 = tx * 64, y0 = ty * 64;
	int cx = x0 + 32, cy = y0 + 32;
	int n = (int8_t)render->numMovingLines;
	unsigned mask = 0;

	for (int i = 0; i < n; i++) {
		const Line *ln = render->movingLines[i].line;
		uint32_t fl = ln->flags;
		int my = (ln->y1 + ln->y2) >> 1;
		int mx = (ln->x1 + ln->x2) >> 1;

		if ((fl & LINE_FACE_N) && mx == cx && cy <= my && my <= y0 + 64)
			mask |= 4;
		else if ((fl & LINE_FACE_S) && mx == cx && y0 <= my && my <= cy)
			mask |= 8;
		else if ((fl & LINE_FACE_E) && x0 <= mx && mx <= cx && my == cy)
			mask |= 2;
		else if ((fl & LINE_FACE_W) && mx <= x0 + 64 && cx <= mx && my == cy)
			mask |= 1;
	}
	return mask;
}

/* Tile a moving line belongs to: its midpoint moved half a tile behind its face. */
static void movingLineTile(const Line *ln, int *outX, int *outY)
{
	int mx = (ln->x1 + ln->x2) >> 1;
	int my = (ln->y1 + ln->y2) >> 1;
	uint32_t fl = ln->flags;

	if (fl & LINE_FACE_N)
		my -= 0x20;
	else if (fl & LINE_FACE_S)
		my += 0x20;
	else if (fl & LINE_FACE_E)
		mx += 0x20;
	else if (fl & LINE_FACE_W)
		mx -= 0x20;
	*outX = mx;
	*outY = my;
}

/* ---- startup ------------------------------------------------------------- */

/* 0x00027f58 */
int Game_startup(Game *game)
{
	static const uint32_t levelNameOfs[] = {
		MOD_STR_LEVEL_NAME_0, MOD_STR_LEVEL_NAME_1, MOD_STR_LEVEL_NAME_2, MOD_STR_LEVEL_NAME_3,
		MOD_STR_LEVEL_NAME_4, MOD_STR_LEVEL_NAME_5, MOD_STR_LEVEL_NAME_6, MOD_STR_LEVEL_NAME_7,
		MOD_STR_LEVEL_NAME_8, MOD_STR_LEVEL_NAME_9, MOD_STR_LEVEL_NAME_10,
	};
	Applet *app = game->app;

	game->canvas = app->canvas;
	game->render = app->render;
	game->player = app->player;
	game->activeMonsters = NULL;
	game->combatMonsters = NULL;
	game->inactiveMonsters = NULL;
	game->password[0] = '\0';
	game->disableAI = 0;
	game->alertMonster = NULL;
	game->numActiveGSprites = 0;
	game->numMovingGSprites = 0;
	game->suppressDialog = 0;

	for (int i = 0; i < 400; i++)
		game->entities[i].app = game->app;
	for (int i = 0; i < NUM_SCRIPT_THREADS; i++) {
		game->threads[i].app = game->app;
		game->threads[i].inUse = 0;
	}

	/* Level names for the travel map / level select (11 slots of 0x18). */
	game->numLevelNames = 0;
	for (size_t i = 0; i < sizeof levelNameOfs / sizeof levelNameOfs[0]; i++) {
		char *dst = game->levelNames + game->numLevelNames * LEVEL_NAME_SIZE;
		snprintf(dst, LEVEL_NAME_SIZE, "%s", ModData_str(&g_mod, levelNameOfs[i]));
		game->numLevelNames++;
	}

	game->skipLoadWorld = 0;
	game->debugVarsLoaded = 0;
	game->autoShowUntouched = 0;
	return 1;
}

/* 0x0001a400 */
void Game_initDebugVars(Game *game)
{
	if (game->debugVarsLoaded)
		return;
	game->debugVarsLoaded = 1;

	/* App_getConfigInt is a stub in this build: every key returns its default. */
	game->player->helpEnabled = (uint8_t)App_getConfigInt(game->app, "help", game->player->helpEnabled);
	game->disableAI = App_getConfigInt(game->app, "disableAI", 0) != 0;
	if (App_getConfigInt(game->app, "equip", 0))
		Player_equipForMap(game->player, game->canvas->mapId);
	game->player->noclip = (uint8_t)App_getConfigInt(game->app, "noclip", 0);
	game->player->god = (uint8_t)App_getConfigInt(game->app, "god", 0);
	game->canvas->showMonsters = (uint8_t)App_getConfigInt(game->app, "showMonsters", 0);
	game->canvas->showNodes = (uint8_t)App_getConfigInt(game->app, "showNodes", 0);
	game->canvas->showLocation = (uint8_t)App_getConfigInt(game->app, "showLocation", 0);
	game->canvas->showSaveSize = (uint8_t)App_getConfigInt(game->app, "showSaveSize", 0);
	game->canvas->showCodes = (uint8_t)App_getConfigInt(game->app, "showCodes", 0);
	game->player->invisible = (uint8_t)App_getConfigInt(game->app, "invis", 0);
	game->player->ce.weapon = (uint8_t)App_getConfigInt(game->app, "weapon", game->player->ce.weapon);
}

/* ---- map change ---------------------------------------------------------- */

/* 0x000117c4 */
void Game_processMapChange(Game *game)
{
	uint32_t pending = game->pendingMapChange;
	int saveFirst = 0;

	if (pending == 0)
		return;

	game->player->missStreak = 0;
	game->player->lastTarget = NULL;
	/* bits 0-7 map - 6000, bits 8-30 packed spawn (x | y<<5 | dir<<10), bit 31 save on first leave */
	game->spawnParam = (pending & 0x7fffffffu) >> 8;

	if (pending & 0x80000000u) {
		/* Bit of the map being left (shift >= 32 gives 0, like the ARM lsl). */
		unsigned shift = (uint16_t)(game->canvas->mapId - MAP_FIRST) & 0xff;
		uint32_t bit = shift < 32 ? 1u << shift : 0;
		if ((game->player->visitedMaps & bit) == 0) {
			game->player->visitedMaps |= bit;
			saveFirst = 1;
		}
	}

	if (saveFirst) {
		/* Save, then the saving state loads the destination stored in the Menu. */
		game->app->menu->f20 = (int16_t)((pending & 0xff) + MAP_FIRST);
		strcpy(game->canvas->printMsg, ModData_str(&g_mod, MOD_STR_SAVING_GAME));
		game->canvas->saveType = 0x13;
		Canvas_setState(game->canvas, 0x12);
	} else {
		Canvas_loadMap(game->canvas, (int)(pending & 0xff) + MAP_FIRST, 0);
	}
	game->pendingMapChange = 0;
}

/* ---- Config -------------------------------------------------------------- */

/* 0x000146d0 */
int Game_hasSaveData(Game *game)
{
	File file;
	int ok = 0;

	File_ctor(&file, game->app);
	int cfgMissing = IFILEMGR_Test(game->app->fileMgr, "Config");
	int saveMissing = IFILEMGR_Test(game->app->fileMgr, "SaveData");
	if (saveMissing + cfgMissing == 0 && File_openRead(&file, "Config"))
		ok = rdInt(&file) == CONFIG_VERSION;
	File_dtor(&file, 0);
	return ok;
}

/* 0x00018aa8 */
void Game_loadConfig(Game *game)
{
	File file;

	File_ctor(&file, game->app);
	if (File_openRead(&file, "Config")) {
		if (rdInt(&file) == CONFIG_VERSION) {
			Menu *menu;

			game->canvas->vibrate = (uint8_t)rdUByte(&file);
			game->player->helpEnabled = (uint8_t)rdUByte(&file);
			game->app->fileDebug = (uint8_t)rdUByte(&file);
			game->player->deaths = rdInt(&file);
			game->app->sound->volume = rdInt(&file);
			Canvas_setAnimFrames(game->canvas, rdInt(&file));
			game->player->helpShown = (uint32_t)rdInt(&file);
			game->player->itemHelp = (uint32_t)rdInt(&file);
			game->player->ringHelp = (uint32_t)rdInt(&file);
			menu = game->app->menu;
			for (int i = 0; i < NUM_SAVED_MENUS; i++) {
				menu->indexes[i * 2] = rdInt(&file);
				menu->indexes[i * 2 + 1] = rdInt(&file);
			}
			if (file.error != 0)
				setFileError(game, file.error);
		} else {
			/* Old or foreign config: drop it and the save that goes with it. */
			App_removeFile(game->app, "Config");
			App_removeFile(game->app, "SaveData");
		}
	}
	File_dtor(&file, 0);
}

/* 0x000232e8 */
void Game_saveConfig(Game *game)
{
	OutputStream os;

	OutputStream_ctor(&os, game->app);
	if (!File_open(&os, "Config", _OFM_CREATE)) {
		setFileError(game, os.error);
	} else {
		Menu *menu = game->app->menu;

		OutStream_writeInt(&os, CONFIG_VERSION);
		OutStream_writeByte(&os, game->canvas->vibrate);
		OutStream_writeByte(&os, game->player->helpEnabled);
		OutStream_writeByte(&os, game->app->fileDebug);
		OutStream_writeInt(&os, game->player->deaths);
		OutStream_writeInt(&os, game->app->sound->volume);
		OutStream_writeInt(&os, game->canvas->animFrames);
		OutStream_writeInt(&os, (int)game->player->helpShown);
		OutStream_writeInt(&os, (int)game->player->itemHelp);
		OutStream_writeInt(&os, (int)game->player->ringHelp);
		for (int i = 0; i < NUM_SAVED_MENUS; i++) {
			OutStream_writeInt(&os, menu->indexes[i * 2]);
			OutStream_writeInt(&os, menu->indexes[i * 2 + 1]);
		}
		if (os.error != 0) {
			setFileError(game, os.error);
			App_removeFile(game->app, "Config");
		}
	}
	OutputStream_dtor(&os, 0);
}

/* 0x0002017c */
void Game_deleteSaveFiles(Game *game)
{
	IFILEMGR_Remove(game->app->fileMgr, "Config");
	IFILEMGR_Remove(game->app->fileMgr, "SaveData");
}

/* ---- SaveData ------------------------------------------------------------ */

/* 0x0001cbe8 */
int Game_openSaveData(Game *game, void *stream)
{
	OutputStream *os = stream;
	/* IFILEMGR_Test returns SUCCESS (0) when the file exists. */
	int isNew = IFILEMGR_Test(game->app->fileMgr, "SaveData") != SUCCESS;

	if (isNew) {
		IFile *f = IFILEMGR_OpenFile(game->app->fileMgr, "SaveData", _OFM_CREATE);
		if (f == NULL)
			return 0;
		IFILE_Release(f);
	}
	if (!File_open(os, "SaveData", _OFM_READWRITE))
		return 0;
	if (isNew) {
		/* Grow a fresh file to its full size so every slot can be sought to. */
		OutputStream_seek(os, 0, SaveFile_totalSize());
		OutputStream_seek(os, 0, 0);
	}
	return 1;
}

/* 0x00023638 */
int Game_savePlayerState(Game *game, void *stream, short mapId, int x, int y, int angle, int x2, int y2)
{
	OutputStream *os = stream;
	int start = OutputStream_getPosition(os);

	OutStream_writeShort(os, mapId);
	OutStream_writeInt(os, x);
	OutStream_writeInt(os, y);
	OutStream_writeInt(os, angle);
	OutStream_writeInt(os, x2);
	OutStream_writeInt(os, y2);
	if (Player_saveState(game->player, os)) {
		int used = OutputStream_getPosition(os) - start;
		if (used < PLAYER_RECORD_SIZE) {
			OutputStream_seek(os, 2, PLAYER_RECORD_SIZE - used); /* relative: pad the record */
			return 1;
		}
	}
	return 0;
}

/* 0x00019fcc */
int Game_loadPlayerState(Game *game, void *stream)
{
	File *file = stream;
	Canvas *cv = game->canvas;
	int start = File_tell(file);
	int v;

	game->respawnMap = (int16_t)rdShort(file);
	v = rdInt(file);
	cv->viewX = v;
	cv->destX = v;
	v = rdInt(file);
	cv->viewY = v;
	cv->destY = v;
	v = rdInt(file);
	cv->viewAngle = v;
	cv->destAngle = v;
	cv->prevX = rdInt(file);
	cv->prevY = rdInt(file);

	if (!Player_loadState(game->player, file))
		return 0;
	int used = File_tell(file) - start;
	if (used > PLAYER_RECORD_SIZE - 1)
		return 0;
	fileSeek(file, 2, PLAYER_RECORD_SIZE - (File_tell(file) - start));
	return 1;
}

/* 0x0001a748 */
int Game_loadState(Game *game, int type)
{
	File file;
	int ok = 0;

	File_ctor(&file, game->app);
	if (!File_openRead(&file, "SaveData")) {
		setFileError(game, FILE_ERROR_NOFILE);
	} else {
		App_dumpCounters(game->app);
		ok = 1;
		if (type == 1) {
			fileSeek(&file, 0, SaveFile_recordOffset(0));
			ok = Game_loadPlayerState(game, &file);
		} else if (type == 2) {
			fileSeek(&file, 0, SaveFile_recordOffset(1));
			ok = Game_loadPlayerState(game, &file);
		}
		if (ok) {
			Canvas *cv = game->canvas;

			game->activeLoadType = type;
			game->player->nextLevelXP = Player_xpForLevel(game->player, game->player->level);
			if (cv->viewX != 0 && cv->viewY != 0) {
				int dir = Dir_fromAngle(cv->viewAngle & 0x3ff);
				game->spawnParam = (uint32_t)dir << 10 |
				                   (((uint32_t)cv->viewY & 0x7ff) >> 6) << 5 |
				                   ((uint32_t)cv->viewX & 0x7ff) >> 6;
			}
			Canvas_loadMap(game->canvas, (uint16_t)game->respawnMap, 0);
			game->isSaved = 0;
			game->isLoaded = 1;
			ok = 1;
		}
	}
	File_dtor(&file, 0);
	return ok;
}

/* 0x000238fc */
void Game_saveState(Game *game, int lastMap, int mapId, int x, int y, int angle, int x2, int y2, int saveType)
{
	OutputStream os;

	Game_saveConfig(game);
	OutputStream_ctor(&os, game->app);
	if (Game_openSaveData(game, &os)) {
		int ok = 1;
		if (saveType & 1)
			ok = Game_savePlayerState(game, &os, (short)mapId, x, y, angle, x2, y2);
		/* World state of the map being left, in that map's slot (signed compare). */
		if (ok && (saveType & 2) && lastMap - MAP_SLOT_BASE <= 13) {
			OutputStream_seek(&os, 0, SaveFile_recordOffset(lastMap - MAP_SLOT_BASE));
			Game_saveWorldState(game, &os, 1);
			/* (the original then read the stream position, unused) */
		}
	}
	OutputStream_dtor(&os, 0);
}

/* 0x000234ec */
void Game_saveLevelSnapshot(Game *game)
{
	OutputStream os;

	OutputStream_ctor(&os, game->app);
	if (Game_openSaveData(game, &os)) {
		Canvas *cv = game->canvas;
		OutputStream_seek(&os, 0, SaveFile_recordOffset(1));
		if (Game_savePlayerState(game, &os, (short)cv->mapId, cv->viewX, cv->viewY, cv->viewAngle,
		                         cv->viewX, cv->viewY)) {
			OutputStream_seek(&os, 0, SaveFile_recordOffset(3));
			Game_saveWorldState(game, &os, 1);
			/* (the original compared the position with SaveFile_maxMapRecordSize, result unused) */
		}
	}
	OutputStream_dtor(&os, 0);
}

/* 0x00006ae8 */
int Game_calcSaveSize(Game *game)
{
	OutputStream os;

	/* A stream with no file that only counts bytes. */
	OutputStream_ctor(&os, game->app);
	os.noWrite = 1;
	Game_saveWorldState(game, &os, 0);
	OutputStream_flush(&os);
	OutputStream_dtor(&os, 0);
	return os.written;
}

/* 0x0001bc50 */
void Game_loadWorld(Game *game)
{
	File file;

	if (game->canvas->mapId > MAP_LAST_SAVED)
		return;
	if (game->skipLoadWorld) {
		game->skipLoadWorld = 0;
		return;
	}
	File_ctor(&file, game->app);
	if (File_openRead(&file, "SaveData")) {
		int slot = game->activeLoadType == 2 ? 3 : game->canvas->mapId - MAP_SLOT_BASE;
		fileSeek(&file, 0, SaveFile_recordOffset(slot));
		Game_loadWorldState(game, &file);
	}
	File_dtor(&file, 0);
}

/* ---- entities ------------------------------------------------------------ */

/* 0x00023438 */
void Game_saveEntityStates(Game *game, void *os)
{
	int16_t handles[400];
	int16_t count = 0;

	for (int i = 0; i < game->numEntities; i++) {
		if (Entity_getSaveHandle(&game->entities[i], &handles[count]))
			count++;
	}
	OutStream_writeShort(os, count);
	for (int i = 0; i < count; i++) {
		OutStream_writeShort(os, handles[i]);
		Entity_saveState(&game->entities[handles[i] & 0x3ff], os, handles[i]);
	}
}

/* 0x00018bd4 */
void Game_loadEntityStates(Game *game, void *stream)
{
	File *file = stream;
	int count = rdShort(file);

	for (int i = 0; i < count; i++) {
		int handle = rdShort(file); /* entity index | state flags (Entity_getSaveHandle) */
		Entity_loadState(&game->entities[handle & 0x3ff], file, handle);
	}
}

/* ---- moving lines (pushable blocks) -------------------------------------- */

/* 0x00006fa4 */
void Game_clearEnclosedTiles(Game *game)
{
	/* Tiles boxed in by block faces on all four sides lose their entity list
	 * and their solid flag (the blocks are about to be moved by the load). */
	for (int t = 0; t < 1024; t++) {
		if (enclosedMask(game->render, t) == 0xf) {
			game->tileLists[t] = NULL;
			game->render->mapFlags[t] &= ~TILE_SOLID;
		}
	}
}

/* 0x0002357c */
void Game_saveMovingLines(Game *game, void *os)
{
	uint8_t written[1024];
	Render *render = game->render;
	int n;

	Game_snapMovingWalls(game, 1);
	memset(written, 0, sizeof written);
	n = (int8_t)render->numMovingLines;
	for (int i = 0; i < n; i++) {
		int x, y;
		movingLineTile(render->movingLines[i].line, &x, &y);
		int t = (y >> 6) * 32 + (x >> 6);
		if (t < 0 || t >= 1024)
			continue; /* never happens on valid maps; skipped like Game_loadMovableBlocks */
		if (written[t])
			continue;
		written[t] = 1;
		/* one (tileX, tileY) pair per block */
		OutStream_writeBool(os, (int8_t)(x >> 6));
		OutStream_writeBool(os, (int8_t)(y >> 6));
	}
}

/* 0x00019c40 */
void Game_loadMovableBlocks(Game *game, void *stream)
{
	File *file = stream;
	Render *render;
	int16_t savedTile[1024];
	int n;

	for (int i = 0; i < 1024; i++)
		savedTile[i] = -1;
	Game_snapMovingWalls(game, 1);
	render = game->render;

	/* Move every block line from its map position to its saved tile. The first
	 * line of each block reads the saved (x, y) bytes; the block's other lines
	 * share its original tile and reuse them. */
	n = (int8_t)render->numMovingLines;
	for (unsigned i = 0; (int)i < n; i = (i + 1) & 0xff) {
		Line *ln = render->movingLines[i].line;
		int x, y;
		movingLineTile(ln, &x, &y);
		int t = (y >> 6) * 32 + (x >> 6);
		if (t < 0 || t >= 1024)
			continue; /* never happens on valid maps (the original indexed its stack array) */
		if (savedTile[t] == -1) {
			int sx = rdByte(file);
			int sy = rdByte(file);
			savedTile[t] = (int16_t)(sy * 32 + sx);
		}
		int dstY = savedTile[t] / 32;
		int dstX = savedTile[t] - dstY * 32;
		int dx = (dstX - (x >> 6)) * 64;
		int dy = (dstY - (y >> 6)) * 64;
		ln->x1 += dx;
		ln->x2 += dx;
		ln->y1 += dy;
		ln->y2 += dy;
		Render_relinkLine(render, (int)i, dstX * 64 + 32, dstY * 64 + 32);
	}

	/* Tiles now boxed in on all four sides become solid world tiles. */
	for (int t = 0; t < 1024; t++) {
		if (enclosedMask(game->render, t) == 0xf && !Game_tileHasWorldEntity(game, t % 32, t / 32))
			Game_linkWorldEntity(game, t % 32, t / 32);
	}
}

/* ---- lerp sprites / script threads --------------------------------------- */

/* 0x000236bc */
void LerpSprite_saveState(LerpSprite *ls, void *os)
{
	Applet *app = g_app;
	const Sprite *spr;
	int threadIdx;

	if (ls->hSprite == 0)
		return;
	spr = Render_spriteAt(app->render, ls->hSprite);
	OutStream_writeInt(os, ls->duration);
	OutStream_writeInt(os, app->gameTime - ls->startTime);
	OutStream_writeShort(os, ls->hSprite);
	OutStream_writeShort(os, (int16_t)spr->x);
	OutStream_writeShort(os, (int16_t)spr->y);
	OutStream_writeShort(os, (int16_t)spr->z);
	OutStream_writeShort(os, ls->srcX);
	OutStream_writeShort(os, ls->srcY);
	OutStream_writeShort(os, ls->srcZ);
	OutStream_writeShort(os, ls->dstX);
	OutStream_writeShort(os, ls->dstY);
	OutStream_writeShort(os, ls->dstZ);
	OutStream_writeBool(os, (int8_t)ls->srcScale);
	OutStream_writeBool(os, (int8_t)ls->dstScale);
	OutStream_writeBool(os, (int8_t)ls->flags);
	/* Owning thread as an index into Game.threads (the original divided the
	 * pointer difference by 0x78; a lerp without a thread wrote garbage). */
	threadIdx = ls->thread ? (int)(ls->thread - app->game->threads) : 0;
	OutStream_writeBool(os, (int8_t)threadIdx);
}

/* 0x0001a55c */
void LerpSprite_loadState(LerpSprite *ls, void *stream)
{
	File *file = stream;
	Applet *app = g_app;
	Sprite *spr;
	int h;

	ls->duration = rdInt(file);
	ls->startTime = app->gameTime - rdInt(file);
	h = rdShort(file);
	ls->hSprite = (int16_t)h;
	spr = Render_spriteAt(app->render, h);
	spr->x = rdShort(file);
	spr->y = rdShort(file);
	spr->z = rdShort(file);
	Render_relinkSpriteAt(app->render, spr, -1, -1);
	ls->srcX = (int16_t)rdShort(file);
	ls->srcY = (int16_t)rdShort(file);
	ls->srcZ = (int16_t)rdShort(file);
	ls->dstX = (int16_t)rdShort(file);
	ls->dstY = (int16_t)rdShort(file);
	ls->dstZ = (int16_t)rdShort(file);
	ls->srcScale = (uint8_t)rdByte(file);
	ls->dstScale = (uint8_t)rdByte(file);
	ls->flags = (uint8_t)rdByte(file);
	ls->thread = &app->game->threads[rdByte(file)];
	LerpSprite_calcDist(ls);
}

/* 0x0002388c */
void ScriptThread_saveState(ScriptThread *thread, void *os)
{
	/* A pending wait is stored relative to now. */
	OutStream_writeInt(os, thread->waitTime ? thread->waitTime - thread->app->tickTime : 0);
	OutStream_writeBool(os, (int8_t)thread->state);
	OutStream_writeBool(os, (int8_t)thread->numFrames);
	for (int i = 0; i < (int8_t)thread->numFrames; i++) {
		const ScriptFrame *fr = &thread->frames[i];
		OutStream_writeInt(os, (int)fr->event);
		OutStream_writeInt(os, (int)fr->flags);
		OutStream_writeBool(os, (int8_t)fr->cmdIndex);
		OutStream_writeBool(os, (int8_t)fr->frameFlags);
	}
}

/* 0x0001a6dc */
void ScriptThread_loadState(ScriptThread *thr, void *stream)
{
	File *file = stream;
	int wait;

	ScriptThread_init(thr);
	wait = rdInt(file);
	thr->waitTime = wait;
	if (wait != 0)
		thr->waitTime = wait + thr->app->tickTime;
	thr->state = (uint8_t)rdByte(file);
	thr->numFrames = (uint8_t)rdByte(file);
	for (int i = 0; i < (int8_t)thr->numFrames; i++) {
		ScriptFrame *fr = &thr->frames[i];
		fr->event = (uint32_t)rdInt(file);
		fr->flags = (uint32_t)rdInt(file);
		fr->cmdIndex = (uint8_t)rdByte(file);
		fr->frameFlags = (uint8_t)rdByte(file);
	}
}

/* ---- world state --------------------------------------------------------- */

/*
 * 0x00023e5c
 * Layout: int mapSaveKey; 8 ints Game.vars; int level time; entity states;
 * line automap bits (1 byte per 8 lines); sprite automap bits (1 byte per 8
 * map sprites); tile automap bits (128 bytes); per tile event: u8 state,
 * u8 n, n x u8 consumed-command index; moving-line tiles; u8 numLerps + the
 * active lerp records; u8 numThreads + 16 thread records; int dropIndex;
 * int levelTurns; int levelXP; u8 notebookCount; 8 shorts notebook; u8 floor
 * height, ceiling height, map flag bits, floor flat, ceiling flat, wobble;
 * [hub map: u8 hub event state, int time left]; short gold paid to the thief.
 */
void Game_saveWorldState(Game *game, void *stream, int brief)
{
	OutputStream *os = stream;
	Render *render = game->render;
	int n;

	OutStream_writeInt(os, render->mapSaveKey);
	for (int i = 0; i < 8; i++)
		OutStream_writeInt(os, game->vars[i]);
	OutStream_writeInt(os, game->app->gameTime - game->levelStartTime);
	Game_saveEntityStates(game, os);

	n = render->numLines;
	for (int i = 0; i < n; i += 8) {
		uint8_t bits = 0;
		for (int b = 0; b < 8 && i + b != n; b++)
			if (render->lines[i + b].flags & LINE_SEEN)
				bits |= (uint8_t)(1 << b);
		OutStream_writeBool(os, (int8_t)bits);
	}

	n = render->numMapSprites;
	for (int i = 0; i < n; i += 8) {
		uint8_t bits = 0;
		for (int b = 0; b < 8 && i + b != n; b++)
			if (render->sprites[i + b].info & SPRITE_SEEN)
				bits |= (uint8_t)(1 << b);
		OutStream_writeBool(os, (int8_t)bits);
	}

	for (int t = 0; t < 1024; t += 8) {
		uint8_t bits = 0;
		for (int b = 0; b < 8; b++)
			if (render->mapFlags[t + b] & TILE_SEEN)
				bits |= (uint8_t)(1 << b);
		OutStream_writeBool(os, (int8_t)bits);
	}

	/* Event states and which one-shot commands have been used up (arg2 == 0). */
	n = render->numTileEvents;
	for (int e = 0; e < n; e++) {
		uint32_t ev = render->tileEvents[e];
		const Command *cmds = &render->commands[EVENT_FIRST_CMD(ev)];
		int numCmds = (int)EVENT_NUM_CMDS(ev);
		int8_t used = 0;

		OutStream_writeBool(os, (int)EVENT_STATE(ev));
		for (int c = 0; c < numCmds; c++)
			if (cmds[c].op != OP_CONTAINER_ITEM && cmds[c].op != OP_FIRE_PIT_DEST && cmds[c].arg2 == 0)
				used++;
		OutStream_writeBool(os, used);
		for (int c = 0; c < numCmds; c++)
			if (cmds[c].op != OP_CONTAINER_ITEM && cmds[c].op != OP_FIRE_PIT_DEST && cmds[c].arg2 == 0)
				OutStream_writeBool(os, (int8_t)c);
	}

	Game_saveMovingLines(game, os);

	OutStream_writeBool(os, (int8_t)game->numLerps);
	for (int i = 0; i < NUM_LERP_SPRITES; i++)
		if (game->lerps[i].hSprite != 0)
			LerpSprite_saveState(&game->lerps[i], os);

	OutStream_writeBool(os, (int8_t)game->numThreads);
	for (int i = 0; i < NUM_SCRIPT_THREADS; i++)
		ScriptThread_saveState(&game->threads[i], os);

	OutStream_writeInt(os, game->dropIndex);
	OutStream_writeInt(os, game->player->levelTurns);
	OutStream_writeInt(os, game->player->levelXP);
	OutStream_writeBool(os, (int8_t)game->player->notebookCount);
	for (int i = 0; i < 8; i++)
		OutStream_writeShort(os, game->player->notebook[i]);
	OutStream_writeBool(os, (int8_t)render->floorHeight);
	OutStream_writeBool(os, (int8_t)render->ceilHeight);
	OutStream_writeBool(os, (int8_t)render->mapFlagBits);
	OutStream_writeByte(os, render->floorFlat);
	OutStream_writeByte(os, render->ceilFlat);
	OutStream_writeByte(os, render->wobble);
	if (render->hubSprite != NULL) {
		OutStream_writeBool(os, (int8_t)game->canvas->hubEventState);
		OutStream_writeInt(os, game->canvas->hubEventTime - game->app->tickTime);
	}
	OutStream_writeShort(os, game->goldPaid);

	if (os->error != 0) {
		setFileError(game, os->error);
		App_removeFile(game->app, "SaveData");
	}
}

/* 0x0001b894 */
void Game_loadWorldState(Game *game, void *stream)
{
	File *file = stream;
	Render *render = game->render;
	int key = rdInt(file);
	int n;

	if (render->mapSaveKey != key) {
		/* A snapshot of another map version: ignore it and use the map's spawn. */
		if (key != 0)
			game->spawnParam = 0;
		return;
	}

	for (int i = 0; i < 8; i++)
		game->vars[i] = rdInt(file);
	game->app->gameTime = (int32_t)GETUPTIMEMS();
	game->levelStartTime = game->app->gameTime - rdInt(file);

	Game_clearEnclosedTiles(game);
	Game_loadEntityStates(game, file);

	n = render->numLines;
	for (int i = 0; i < n; i += 8) {
		unsigned bits = (unsigned)rdByte(file) & 0xff;
		for (int b = 0; b < 8; b++)
			if (bits & (1u << b))
				render->lines[i + b].flags |= LINE_SEEN;
	}

	n = render->numMapSprites;
	for (int i = 0; i < n; i += 8) {
		unsigned bits = (unsigned)rdByte(file) & 0xff;
		for (int b = 0; b < 8; b++)
			if (bits & (1u << b))
				render->sprites[i + b].info |= SPRITE_SEEN;
	}

	/* 128 automap bytes read back as 32 little-endian ints. */
	for (int t = 0; t < 1024; t += 32) {
		uint32_t bits = (uint32_t)rdInt(file);
		for (int b = 0; b < 32; b++)
			if (bits & (1u << b))
				render->mapFlags[t + b] |= TILE_SEEN;
	}

	n = render->numTileEvents;
	for (int e = 0; e < n; e++) {
		uint32_t ev = render->tileEvents[e];
		int state = rdByte(file);
		render->tileEvents[e] = (uint32_t)state << 27 | (ev & ~EVENT_STATE_MASK);
		int used = rdByte(file);
		for (int c = 0; c < used; c++)
			render->commands[EVENT_FIRST_CMD(ev) + rdByte(file)].arg2 = 0;
	}

	Game_loadMovableBlocks(game, file);

	game->numLerps = (uint8_t)rdByte(file);
	for (int i = 0; i < NUM_LERP_SPRITES; i++) {
		if (i < (int8_t)game->numLerps)
			LerpSprite_loadState(&game->lerps[i], file);
		else
			game->lerps[i].hSprite = 0;
	}

	game->numThreads = (uint8_t)rdByte(file);
	for (int i = 0; i < NUM_SCRIPT_THREADS; i++) {
		ScriptThread_loadState(&game->threads[i], file);
		game->threads[i].inUse = (int8_t)game->threads[i].numFrames > 0;
	}
	Game_updateLerpSprites(game);

	game->dropIndex = rdInt(file);
	game->player->levelTurns = rdInt(file);
	game->player->levelXP = rdInt(file);
	game->player->notebookCount = rdByte(file);
	for (int i = 0; i < 8; i++)
		game->player->notebook[i] = (int16_t)rdShort(file);
	render->floorHeight = (uint8_t)rdByte(file);
	render->ceilHeight = (uint8_t)rdByte(file);
	render->mapFlagBits = (uint8_t)rdByte(file);
	render->floorFlat = (uint8_t)rdUByte(file);
	render->ceilFlat = (uint8_t)rdUByte(file);
	render->wobble = (uint8_t)rdUByte(file);
	if (render->hubSprite != NULL) {
		game->canvas->hubEventState = (uint8_t)rdByte(file);
		game->canvas->hubEventTime = rdInt(file) + game->app->tickTime;
	}
	game->goldPaid = (int16_t)rdShort(file);

	game->app->hud->lastHp = 0;
	game->app->hud->fromHp = 0;
	game->app->hud->hpChangeTime = 0;
	game->canvas->combat->chargeLevel = 0;
	Game_resetMonsterPositions(game);

	if (file->error != 0)
		setFileError(game, file->error);
}
