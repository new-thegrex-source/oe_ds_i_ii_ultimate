/*
 * Canvas: the top-level game state machine.
 *
 * Canvas_run is called once per tick. It advances the scripts, replays queued
 * keys (canvas_input.c) and then runs the handler of the current state
 * (Canvas.state, changed only through Canvas_setState). Most states draw
 * through the UI helpers (Canvas_draw*); the exploring/combat states drive the
 * 3D view through Canvas_updateMovement (canvas_move.c) and Render.
 *
 * Also here: startup/shutdown, map loading and the save-state step.
 */

#include "oe_vm.h"
#include "game.h"

/* Strings in the executable's data area. */
#define MOD_STR_SAVE_NOTHING   0x46418u /* "Save state didn't save anything." */
#define MOD_STR_RENDER_AVG     0x463f4u /* "Render Avg: " */
#define MOD_STR_HORIZ_AVG      0x463e4u /* " Horiz Avg: " */
#define MOD_STR_TOTAL_AVG      0x463d4u /* " Total Avg: " */
#define MOD_STR_MS             0x463d0u /* "ms" */
/* Compass labels for the debug location overlay, by octant (angle >> 7). */
#define MOD_STR_DIR_E          0x46440u
#define MOD_STR_DIR_NE         0x4644cu
#define MOD_STR_DIR_N          0x4643cu
#define MOD_STR_DIR_NW         0x46450u
#define MOD_STR_DIR_W          0x46448u
#define MOD_STR_DIR_SW         0x46458u
#define MOD_STR_DIR_S          0x46444u
#define MOD_STR_DIR_SE         0x46454u
#define MOD_STR_DIR_NONE       0x4645cu /* "**" */
/* Debug configuration keys (App_debugCheckpoint, a stub returning the default). */
#define MOD_CFG_VIEW_SIZE      0x4634cu /* "App-ViewSize" */
#define MOD_CFG_FRAMES         0x4635cu /* "App-Frames" */
#define MOD_CFG_STARTUP_MAP    0x46378u /* "App-StartupMap" */
#define MOD_CFG_SKIP_INTRO     0x46368u /* "App-SkipIntro" */
#define MOD_CFG_EXCL_FEEDBACK  0x46388u /* "App-ExclusiveFeedback" */
#define MOD_CFG_FORCE_INTERP   0x463a0u /* "App-ForceInterpolation" */
#define MOD_CFG_TELL_A_FRIEND  0x463b8u /* "App-TellAFriend" */
#define MOD_CFG_LOAD_SAVE_GAME 0x46408u /* "loadSaveGame" */

/* Canvas states (Canvas.state). */
enum {
	ST_LOGOS = 0,
	ST_IDLE = 1,
	ST_MENU = 2,
	ST_PLAYING = 3,
	ST_COMBAT = 4,
	ST_AUTOMAP = 5,
	ST_LOADING = 6,
	ST_DIALOG = 7,
	ST_INTRO = 8,
	ST_BENCHMARK = 9,
	ST_BENCHMARK_END = 10,
	ST_TRAVELMAP = 0xb,
	ST_DYING = 0xc,
	ST_ENDING = 0xd,
	ST_CREDITS = 0xe,
	ST_SAVING = 0xf,
	ST_ERROR = 0x10,
	ST_CINEMATIC = 0x11,
	ST_FADE = 0x12,
	ST_LOCKPICK = 0x13,
	ST_QUICK_WEAPON = 0x14,
	ST_QUICK_ITEM = 0x15,
	ST_LAST = 0x16,
};

/* Canvas.saveType bits for the saving state. */
enum {
	SAVE_PLAYER = 0x01,
	SAVE_WORLD = 0x02,
	SAVE_THEN_MAIN = 0x04,    /* back to the main menu */
	SAVE_THEN_TRAVEL = 0x08,  /* level change: travel map / loading */
	SAVE_THEN_MENU = 0x10,    /* MenuSystem menu 1 */
	SAVE_THEN_EXIT = 0x20,    /* quit the applet */
	SAVE_THEN_MENU16 = 0x40,  /* MenuSystem menu 0x16 */
};

/* Canvas.fadeFlags bits. */
#define FADE_CHANGE_MAP 2

/* Script event flags for Game_executeTile. */
#define EV_SPECIAL 0xf000

/* Player status effect slots. */
#define SE_TINT   4
#define SE_FROZEN 9

/* Map ids 1..7 are the overworld levels reached through the travel map. */
#define IS_TRAVEL_MAP(id) ((id) >= 1 && (id) <= 7)

#define COLOR_BLACK ((int)0xff000000)

static const char *modStr(uint32_t off)
{
	return ModData_str(&g_mod, off);
}

static int debugConfig(Applet *app, uint32_t keyOff, int def)
{
	return App_debugCheckpoint(app, (char *)modStr(keyOff), def);
}

/* Replace the scratch string with a dehyphenated text-table entry. */
static void loadScratchText(Canvas *canvas, int group, int id)
{
	String_setLength(&canvas->scratch, 0);
	Text_getString(canvas, group, id, &canvas->scratch);
	TextUtil_dehyphenate(g_app->textUtil, &canvas->scratch);
}

/* 0x0003e7dc */
int Canvas_startup(Canvas *canvas)
{
	Applet *app = g_app;
	int frames, w, vw, sh, avail;
	int16_t renderH;

	canvas->screenRect[0] = 0;
	canvas->screenRect[1] = 0;
	canvas->screenRect[2] = (int16_t)app->cxScreen;
	canvas->screenRect[3] = (int16_t)app->cyScreen;
	canvas->usableHeight = (int16_t)app->cyScreen;
	canvas->mapStringGroup = -1;

	if (!Text_loadStringGroup(canvas, 0) || !Text_loadStringGroup(canvas, 3))
		return 0;

	canvas->render = app->render;
	canvas->player = app->player;
	canvas->game = app->game;
	canvas->entityDefs = app->entityDefs;
	canvas->combat = app->combat;
	canvas->hud = app->hud;
	canvas->menuSystem = app->menuSystem;
	canvas->particles = app->particles;
	Graphics_init(&canvas->graphicsCtx);
	canvas->graphics = &canvas->graphicsCtx;

	canvas->weaponCycleKey = 0;
	canvas->vibrateEnabled = 1;
	canvas->fcd4 = 0;
	canvas->frameSkip = 0;
	canvas->waitTime = 0;
	canvas->showMonsters = 0;
	canvas->showLocation = 0;
	canvas->showDebugVar = 0;
	canvas->startupMap = 1;
	canvas->numKeys = 0;
	canvas->logoIdx = 0;
	canvas->logoTime = 0;
	canvas->oldState = -1;
	canvas->pendingEffect = 0;
	canvas->dialogItem = NULL;
	canvas->dialogViewLines = 0;
	canvas->showCodes = 0;
	canvas->prevMapId = 0;
	canvas->mapId = 0;
	canvas->automapFlag = 0;
	canvas->automapFirstFrame = 0;
	canvas->loadType = 0;
	canvas->saveType = 0;
	canvas->profileSums[0] = 0;
	canvas->softKeyRightX = 0;
	canvas->softKeyLeftX = 0;
	canvas->renderParam = -1;
	canvas->textColor = 0;
	canvas->moveCount = 0;
	canvas->numHelpMsgs = 0;
	canvas->destZ = 0x24;
	canvas->vibrateEndTime = 0;
	canvas->viewZ = 0x24;
	canvas->f2cc = 0xc;
	canvas->viewRect[2] = 0;
	canvas->viewRect[3] = 0;

	/* the screen width is kept even */
	if (canvas->screenRect[2] & 1)
		canvas->screenRect[2]--;

	{
		int16_t size = (int16_t)debugConfig(app, MOD_CFG_VIEW_SIZE, 0);
		if (size != 0) {
			canvas->viewRect[2] = size;
		} else {
			canvas->viewRect[2] = canvas->screenRect[2];
			size = canvas->screenRect[3];
		}
		canvas->viewRect[3] = size;
	}
	canvas->dialogPortrait = -1;

	/* characters per line (7 px font) for the various text boxes */
	w = canvas->screenRect[2];
	canvas->textWidths[2] = (w - 2) / 7;
	canvas->textWidths[3] = (w - 9) / 7;
	canvas->textWidths[1] = (w - 23) / 7;
	canvas->textWidths[0] = canvas->textWidths[1];
	canvas->textWidths[4] = canvas->textWidths[3];
	vw = canvas->viewRect[2];
	canvas->textWidths[7] = vw / 7;
	canvas->textWidths[5] = (vw - 23) / 7;
	canvas->textWidths[6] = (vw - 53) / 7;
	canvas->largeScreen = vw >= 0xb0;

	/* 3D view: 20 px HUD bar above, 15 px below (35 total); keep a 20 px
	 * status bar on tall screens and an even render height (port: unless the options turned it
	 * off; then the HUD and the weapon sit at the screen's bottom edge) */
	sh = canvas->screenRect[3];
	renderH = (int16_t)(canvas->viewRect[3] - 35);
	if (sh < 0x94 || !Options_softkeyBar()) {
		canvas->usableHeight = (int16_t)sh;
	} else {
		int gap = sh - canvas->viewRect[3];
		if (gap < 20)
			renderH = (int16_t)(renderH - (20 - gap));
		canvas->hasStatusBar = 1;
		canvas->usableHeight = (int16_t)(sh - 20);
	}
	if (renderH & 1)
		renderH = (int16_t)(renderH - 1);
	canvas->viewRect[0] = (int16_t)((w - vw) / 2);
	canvas->viewRect[3] = (int16_t)(renderH + 35);
	avail = canvas->hasStatusBar ? canvas->usableHeight : sh;
	canvas->viewRect[1] = (int16_t)((avail - canvas->viewRect[3]) / 2);
	canvas->screenCenterX = vw / 2;
	canvas->screenCenterY = canvas->viewRect[3] / 2;
	canvas->renderRect[0] = canvas->viewRect[0];
	canvas->renderRect[1] = (int16_t)(canvas->viewRect[1] + 20);
	canvas->renderRect[2] = (int16_t)vw;
	canvas->renderRect[3] = renderH;
	Render_setScreenRect(app->render, canvas->renderRect);
	if (canvas->viewRect[1] + canvas->viewRect[3] == canvas->usableHeight - 1)
		canvas->usableHeight = (int16_t)(canvas->usableHeight - 1);

	if (!Hud_startup(canvas->hud, canvas->largeScreen))
		return 0;

	frames = debugConfig(app, MOD_CFG_FRAMES, 6);
	if (frames < 2)
		frames = 2;
	else if (frames > 0x40)
		frames = 0x40;
	Canvas_setAnimFrames(canvas, frames);
	canvas->startupMap = debugConfig(app, MOD_CFG_STARTUP_MAP, 1);
	canvas->skipIntro = debugConfig(app, MOD_CFG_SKIP_INTRO, 0) != 0;
	canvas->exclusiveFeedback = debugConfig(app, MOD_CFG_EXCL_FEEDBACK, 1) != 0;
	canvas->forceInterpolation = debugConfig(app, MOD_CFG_FORCE_INTERP, 1) != 0;
	canvas->tellAFriend = debugConfig(app, MOD_CFG_TELL_A_FRIEND, 1) != 0;

	Image_load(app, 0x21, &canvas->imgLogos, 1);
	Image_load(app, 0xb, &canvas->imgFont, 1);
	Image_load(app, 0xc, &canvas->imgFontLarge, 1);
	Image_load(app, 0x27, &canvas->imgScrollStory, 1);
	Image_load(app, 0, &canvas->imgAutomapCursor, 1);
	Image_load(app, 0x2d, &canvas->imgNpcChat, 1);
	Image_load(app, 0x2e, &canvas->imgNpcCorners, 1);
	Image_load(app, 0x31, &canvas->imgWandCorners, 1);
	Image_load(app, 0x30, &canvas->imgPlayerCorners, 1);
	Image_load(app, 0x2f, &canvas->imgPlayerChat, 1);
	Image_load(app, 1, &canvas->imgBgScroll, 1);
	Image_load(app, 5, &canvas->imgBgScrollTears, 1);
	Image_load(app, 3, &canvas->imgBgScrollStains, 1);
	Image_load(app, 2, &canvas->imgBgScrollBarter, 1);
	Image_load(app, 6, &canvas->imgBgScrollTearsBarter, 1);
	Image_load(app, 4, &canvas->imgBgScrollStainsBarter, 1);
	Image_load(app, 0x1d, &canvas->imgIcons, 1);
	Image_load(app, 0x1f, &canvas->imgIconsRings, 1);
	Image_load(app, 0x1e, &canvas->imgIconsBuffs, 1);
	Image_load(app, 0x11, &canvas->imgHudBarSegment, 1);

	if (App_testFileSystem(app, 20000, 3))
		return 1;

	/* not enough room for save files */
	canvas->fcd4 = 1;
	loadScratchText(canvas, 0, 0x28);
	return 0;
}

/* 0x0003c3f8 */
void Canvas_shutdown(Canvas *canvas)
{
	Applet *app = g_app;
	int i;

	App_freeImage(app, &canvas->imgLogos);
	App_freeImage(app, &canvas->imgFont);
	App_freeImage(app, &canvas->imgFontLarge);
	App_freeImage(app, &canvas->imgScrollStory);
	App_freeImage(app, &canvas->imgAutomapCursor);
	App_freeImage(app, &canvas->imgNpcChat);
	App_freeImage(app, &canvas->imgNpcCorners);
	App_freeImage(app, &canvas->imgWandCorners);
	App_freeImage(app, &canvas->imgPlayerCorners);
	App_freeImage(app, &canvas->imgPlayerChat);
	App_freeImage(app, &canvas->imgBgScroll);
	App_freeImage(app, &canvas->imgBgScrollTears);
	App_freeImage(app, &canvas->imgBgScrollStains);
	App_freeImage(app, &canvas->imgBgScrollBarter);
	App_freeImage(app, &canvas->imgBgScrollTearsBarter);
	App_freeImage(app, &canvas->imgBgScrollStainsBarter);
	App_freeImage(app, &canvas->imgIcons);
	App_freeImage(app, &canvas->imgIconsRings);
	App_freeImage(app, &canvas->imgIconsBuffs);
	App_freeImage(app, &canvas->imgHudBarSegment);
	App_freeImage(app, &canvas->imgStoryPages);
	App_freeImage(app, &canvas->imgTravelMap);
	App_freeImage(app, &canvas->imgMapNode);
	for (i = 0; i < 20; i++)
		Canvas_unloadStringGroup(canvas, i);
	String_release(&canvas->scratch);
	String_release(&canvas->dialogText);
	String_release(&canvas->lockEntered);
	String_release(&canvas->lockString);
	String_release(&canvas->loadingText);
	String_release(&canvas->softKeyLeft);
	String_release(&canvas->softKeyRight);
	for (i = 0; i < 6; i++)
		String_release(&canvas->textArgs[i]);
	for (i = 0; i < 2; i++)
		String_release(&canvas->msgRing[i]);
}

/* 0x00024a80 Buffer a key press until the next tick (at most 4). */
void Canvas_queueKey(Canvas *canvas, short key)
{
	if (canvas->numKeys != 4)
		canvas->keyQueue[canvas->numKeys++] = key;
}

/* 0x000357a8 Replay the queued keys; a handler returning 0 keeps the rest. */
void Canvas_runInputEvents(Canvas *canvas)
{
	Applet *app = g_app;
	int i, n;

	if (canvas->waitTime) {
		/* input is blocked for a moment (e.g. after a dialog) */
		if (app->time > canvas->waitTime) {
			canvas->waitTime = 0;
			if (canvas->state != ST_CINEMATIC)
				Canvas_drawSoftKeys_10c68(canvas, &canvas->softKeyLeft, &canvas->softKeyRight, 1);
		}
		canvas->numKeys = 0;
		return;
	}

	for (i = 0; i < canvas->numKeys; i++) {
		if (!Canvas_handleEvent(canvas, (uint16_t)canvas->keyQueue[i]))
			break;
	}
	for (n = 0; i < canvas->numKeys; i++, n++)
		canvas->keyQueue[n] = canvas->keyQueue[i];
	canvas->numKeys = n;
}

/* 0x00034fe0 Forget the %1..%6 arguments for the next text lookup. */
void Canvas_resetEvents(Canvas *canvas)
{
	canvas->numTextArgs = 0;
}

/* 0x000354d8 The applet was resumed: restore soft keys and music. */
void Canvas_resume(Canvas *canvas)
{
	Applet *app = g_app;

	Canvas_refreshSoftKeys(canvas);
	switch (canvas->state) {
	case ST_MENU: {
		int menu = canvas->menuSystem->menu;
		if (menu >= 3 && menu <= 0x11)
			Sound_play(app->sndSys, 0x56, 3, 5, -1);
		else if (menu == 1 || menu == 0x12)
			Sound_play(app->sndSys, 0x54, 2, 5, -1);
		break;
	}
	case ST_ENDING:
	case ST_INTRO:
		Sound_play(app->sndSys, 0x53, 3, 5, -1);
		break;
	case ST_CREDITS:
		Sound_play(app->sndSys, 0x56, 2, 5, -1);
		break;
	case ST_AUTOMAP:
		canvas->automapFirstFrame = 0;
		break;
	default:
		break;
	}
}

/* 0x0000c0c0 */
void Canvas_openMainMenu(void)
{
	Sound_requestStopAll(g_app->sndSys);
	MenuSystem_setMenu(g_app->menuSystem, 1);
}

/* 0x0000c0fc Start the level after the intro story. */
void Canvas_restartLevel(Canvas *canvas)
{
	App_freeImage(g_app, &canvas->imgStoryPages);
	Canvas_loadMapEx(canvas, canvas->startupMap, 0);
}

/* 0x00007a98 Leave the game for the main menu (load == 0) or start up. */
void Canvas_backToMain(Canvas *canvas, int load)
{
	Applet *app = g_app;

	canvas->mapId = 0;
	if (load == 0) {
		loadScratchText(canvas, 0, 0x2f);
		Graphics_setColor(canvas->graphics, COLOR_BLACK);
		Graphics_fillRect(canvas->graphics, 0, 0, canvas->viewRect[2], canvas->viewRect[3]);
		Canvas_drawString(canvas, &canvas->scratch, canvas->screenCenterX, canvas->screenCenterY - 0x18, 0x11);
		Player_reset(app->player, 1);
		Render_unloadMap(app->render);
		Game_unloadMap(app->game);
		MenuSystem_setMenu(app->menuSystem, 3);
		return;
	}

	if (canvas->skipIntro == 0) {
		MenuSystem_setMenu(app->menuSystem, 0x14);
		return;
	}
	/* debug start-up: straight into the saved game or the start map */
	Player_reset(app->player, 1);
	Render_unloadMap(app->render);
	Game_unloadMap(app->game);
	if (App_hasSaveData(app) && debugConfig(app, MOD_CFG_LOAD_SAVE_GAME, 0)) {
		Canvas_startLoading(canvas, 1, 0, 0x45);
		return;
	}
	Canvas_loadMapEx(canvas, canvas->startupMap, 0);
}

/* 0x00026cf8 */
void Canvas_loadMap(Canvas *canvas, int mapId)
{
	Canvas_loadMapEx(canvas, mapId, 0);
}

/* 0x00026d00 Change level: save first when leaving an overworld level. */
void Canvas_loadMapEx(Canvas *canvas, int mapId, int force)
{
	Applet *app = g_app;

	Applet_resetCounters(app, 0);
	canvas->prevMapId = canvas->mapId;
	canvas->mapId = mapId;
	Sound_requestStopAll(app->sndSys);

	if (force == 0 && app->game->loadType == 0 && IS_TRAVEL_MAP(canvas->prevMapId)) {
		/* "Saving Game": the saving state continues to the travel map */
		loadScratchText(canvas, 3, 0x11e);
		canvas->saveType = SAVE_PLAYER | SAVE_WORLD | SAVE_THEN_TRAVEL;
		Canvas_setState(canvas, ST_SAVING);
		return;
	}
	loadScratchText(canvas, 0, 0x2f);
	Canvas_setState(canvas, IS_TRAVEL_MAP(canvas->mapId) ? ST_TRAVELMAP : ST_LOADING);
}

/* 0x0002730c Load canvas->mapId and drop the player in. Returns 0 on failure. */
int Canvas_doLoadMap(Canvas *canvas)
{
	Applet *app = g_app;
	int noSave;

	Render_unloadMap(app->render);
	Game_unloadMap(app->game);
	if (!Render_beginLoadMap(app->render, canvas->mapId))
		return 0;
	if (canvas->mapId <= 7 && canvas->mapId > app->player->highestMap)
		app->player->highestMap = canvas->mapId;
	if (!Render_beginLoadMapData(app->render))
		return 0;

	Game_spawnEntities(app->game);
	canvas->hud->numMessages = 0;
	String_setLength(&canvas->hud->forcedMsg, 0);
	String_setLength(&canvas->hud->str1228, 0);
	Hud_shuffleBarks(canvas->hud);
	Player_selectWeapon(app->player, app->player->ce.weapon);
	canvas->frameSkip = 1;
	ParticleSystem_freeAll(canvas->particles);
	Player_initForLevel(app->player);
	app->game->levelStartTime = App_getUpTimeMS();
	Game_loadWorld(app->game);
	noSave = 1 - App_hasSaveData(app);
	Game_spawnPlayer(app->game, 0, 0, 0);
	canvas->f2c8 = 0;
	canvas->moveCount = 0;

	if (app->game->loadedFromSave == 0) {
		if (noSave)
			Game_saveState(app->game, canvas->mapId, canvas->mapId, canvas->destX, canvas->destY,
			               canvas->destAngle, canvas->destX, canvas->destY, SAVE_PLAYER | SAVE_WORLD);
		Game_saveLevelSnapshot(app->game);
	}
	app->player->tempWeapon = 0;

	/* level start scripts */
	Game_executeTile(app->game, 0, 0, EV_SPECIAL, 1);
	if (app->player->gameCompleted)
		Game_executeTile(app->game, 3, 0, EV_SPECIAL, 1);
	canvas->nextAmbientSound = app->playTime + 8000;
	canvas->nextRandomEvent = app->playTime + 12000;

	if (app->game->loadedFromSave == 0) {
		Canvas *cv = app->canvas;
		cv->prevX = cv->destX;
		cv->prevY = cv->destY;
		Game_executeTile(app->game, app->canvas->viewX >> 6, app->canvas->viewY >> 6, 0x40f, 1);
		Canvas_finishRotation(app->canvas, 0);
	}
	Canvas_finishRotation(canvas, 0);
	Game_checkBobAfterWarp(app->game, 0, 0, 0);
	Canvas_revealAutomap(canvas);
	app->game->loadedFromSave = 0;
	app->game->f9c14 = 0;
	app->game->loadType = 0;
	Canvas_requestState12(canvas, 1000, 0x21);
	Canvas_dequeueHelpDialog(canvas, 0);

	canvas->f2cc = 0xc;
	if (canvas->mapId >= 7) {
		canvas->f2cc = 4;
	} else {
		int highest = app->player->highestMap;
		if (highest == 6)
			canvas->f2cc = 5;
		else if (highest >= 4)
			canvas->f2cc = 6;
		else if (highest >= 2)
			canvas->f2cc = 7;
	}

	app->game->paused = 0;
	app->playTime = App_getUpTimeMS();
	if (canvas->state == ST_LOADING)
		Canvas_setState(canvas, ST_PLAYING);
	return 1;
}

/* 0x00036eb0 Show `strTable:strId` and save according to saveType. */
void Canvas_saveState(Canvas *canvas, int saveType, int strTable, int strId)
{
	canvas->saveType = saveType;
	String_setLength(&canvas->loadingText, 0);
	Text_getString(canvas, strTable, strId, &canvas->loadingText);
	TextUtil_dehyphenate(g_app->textUtil, &canvas->loadingText);
	Canvas_setState(canvas, ST_SAVING);
}

/* 0x00039c34 Frames per tile step / quarter turn. */
void Canvas_setAnimFrames(Canvas *canvas, int n)
{
	canvas->animFrames = n;
	canvas->moveStep = (n + 63) / n;
	canvas->turnStep = (n + 255) / n;
}

/* 0x0003d784 Start a fade (state 0x12) of `duration` ms. */
void Canvas_requestState12(Canvas *cv, int duration, int flags)
{
	cv->fadeFlags = (uint32_t)flags;
	cv->fadeDuration = duration;
	cv->fadeStart = -1;
	if (cv->state == ST_CINEMATIC || (flags & 0x20))
		return;
	Canvas_setState(cv, ST_FADE);
}

/* 0x0003d920 Screen shake after `delayMs` and optional phone vibration. */
void Canvas_startShake(Canvas *cv, int delayMs, int intensity, int vibrateMs)
{
	Applet *app = g_app;

	if (intensity) {
		app->render->shakeEndTime = App_getUpTimeMS() + delayMs;
		app->render->shakeIntensity = intensity;
	}
	if (vibrateMs && cv->vibrateEnabled) {
		cv->vibrateEndTime = App_getUpTimeMS() + vibrateMs;
		ISOUND_Vibrate(vibrateMs & 0xffff);
	}
}

/* 0x0003d9cc Start the render benchmark (spin in place, state 9). */
void Canvas_enterState9(Canvas *cv, int menuMode)
{
	cv->snapView = 1;
	cv->profiling = 1;
	cv->profileSums[0] = 1;
	cv->profileSums[1] = 0;
	cv->profileSums[2] = 0;
	cv->profileSums[3] = 0;
	if (menuMode) {
		g_app->menuSystem->animPeriod = 4;
		return;
	}
	cv->turnStep = 4;
	cv->destAngle = cv->viewAngle;
	Canvas_setState(cv, ST_BENCHMARK);
}

/* 0x0003b038 */
void Canvas_setState(Canvas *canvas, int state)
{
	Applet *app;
	int old;

	if (state < 0 || state > ST_LAST)
		return;
	app = g_app;

	/* leaving the current state */
	old = canvas->state;
	if (old == ST_AUTOMAP) {
		Graphics_setColor(canvas->graphics, COLOR_BLACK);
		Graphics_fillRect(canvas->graphics, 0, 0, canvas->screenRect[2], canvas->screenRect[3]);
		if (state == ST_DIALOG || state == ST_LOCKPICK) {
			/* these draw over the 3D view: put it back */
			canvas->render->dbgForceDrawer = 0;
			Canvas_renderView(canvas, canvas->viewX, canvas->viewY, canvas->viewAngle);
			Hud_draw(canvas->hud);
		}
		Player_stub(); /* (player, time - automapTime) in the original; empty */
	} else if (old == ST_MENU) {
		Player_stub(); /* (player, time - menu start time) */
	} else if (old == ST_CINEMATIC) {
		uint32_t eff;
		app->game->skippingCinematic = 0;
		eff = (uint32_t)canvas->pendingEffect;
		if (eff) {
			/* restore the screen tint suspended for the cut-scene */
			Player_setStatusEffect(canvas->player, SE_TINT, eff & 0xff, (eff >> 8) & 0xff, (eff >> 16) & 0xff);
			Player_updateStats(canvas->player);
			canvas->pendingEffect = 0;
		}
	} else if (old == ST_DIALOG && state != ST_DIALOG && canvas->oldState == ST_LOCKPICK) {
		/* a dialog opened during lock-picking returns to it */
		state = ST_LOCKPICK;
	}

	canvas->oldState = canvas->state;
	canvas->state = state;

	switch (state) {
	case ST_COMBAT:
		Canvas_clearSoftKeys(canvas);
		canvas->combatDone = 0;
		break;

	case ST_PLAYING:
		app->game->turnTime = app->time;
		if (app->game->monstersTurn == 0 || canvas->oldState == ST_CINEMATIC)
			Canvas_drawPlayingSoftKeys(canvas);
		if (canvas->oldState == ST_COMBAT && Combat_getTargetType(app->combat) == 3)
			Game_executeTile(app->game, 4, 0, EV_SPECIAL, 1);
		break;

	case ST_LOCKPICK:
		canvas->lockCorrectHits = 0;
		canvas->lockSlot = 0;
		canvas->lockDone = 0;
		canvas->numKeys = 0;
		app->player->f270 = app->player->lockpickTries;
		Canvas_clearSoftKeys(canvas);
		break;

	case ST_QUICK_WEAPON:
	case ST_QUICK_ITEM:
		Canvas_clearSoftKeys(canvas);
		String_setLength(&app->hud->forcedMsg, 0);
		loadScratchText(canvas, 0, state == ST_QUICK_WEAPON ? 0x2d : 0x2e);
		String_appendString(&app->hud->forcedMsg, &canvas->scratch);
		break;

	case ST_DIALOG:
		String_setLength(&canvas->softKeyLeft, 0);
		String_setLength(&canvas->softKeyRight, 0);
		Text_getString(canvas, 0, 0x2b, &canvas->softKeyRight);
		TextUtil_dehyphenate(app->textUtil, &canvas->softKeyRight);
		if (!(canvas->dialogFlags & 1)) {
			Text_getString(canvas, 0, 0xce, &canvas->softKeyLeft);
			TextUtil_dehyphenate(app->textUtil, &canvas->softKeyLeft);
			Canvas_drawSoftKeys_10c68(canvas, &canvas->softKeyLeft, &canvas->softKeyRight, 1);
		} else {
			String empty;
			String_ctor(&empty);
			Canvas_drawSoftKeys_10c68(canvas, &empty, &canvas->softKeyRight, 1);
			String_dtor(&empty, 0);
		}
		canvas->numKeys = 0;
		break;

	case ST_DYING:
		Canvas_clearSoftKeys(canvas);
		canvas->dyingTime = app->time;
		Hud_draw(canvas->hud);
		break;

	case ST_ENDING:
		Canvas_clearSoftKeys(canvas);
		Canvas_initScrollScreen13(canvas);
		break;

	case ST_CREDITS:
		Canvas_initCredits(canvas);
		break;

	case ST_INTRO:
		Canvas_clearSoftKeys(canvas);
		Canvas_initScrollScreen8(canvas);
		break;

	case ST_TRAVELMAP:
		Canvas_clearSoftKeys(canvas);
		Graphics_setColor(canvas->graphics, COLOR_BLACK);
		Graphics_fillRect(canvas->graphics, 0, 0, canvas->screenRect[2], canvas->screenRect[3]);
		Canvas_initTravelMap(canvas);
		break;

	case ST_LOADING:
	case ST_SAVING:
		canvas->drawLoading = 1;
		Canvas_clearSoftKeys(canvas);
		break;

	case ST_AUTOMAP:
		canvas->automapFlag = app->game->activePropagators > 0;
		canvas->automapFirstFrame = 0;
		canvas->automapTime = app->time;
		Canvas_drawPlayingSoftKeys(canvas);
		break;

	case ST_MENU:
		canvas->menuSystem->startTime = app->time;
		if (canvas->oldState == ST_PLAYING)
			MenuSystem_playOpenSound(canvas->menuSystem);
		if (canvas->oldState != ST_MENU)
			canvas->numKeys = 0;
		break;

	case ST_CINEMATIC: {
		Player *player;
		canvas->hud->numMessages = 0;
		String_setLength(&canvas->hud->caption1, 0);
		String_setLength(&canvas->hud->caption2, 0);
		canvas->hud->numLines = 0;
		player = app->player;
		if (player->statusEffects[SE_TINT].turns > 0) {
			/* suspend the screen tint during the cut-scene (restored on exit) */
			StatusEffect *se = &player->statusEffects[SE_TINT];
			canvas->pendingEffect = se->amount;
			canvas->pendingEffect |= se->stacks << 8;
			canvas->pendingEffect |= se->turns << 16;
			Player_removeBuff(app->player, SE_TINT);
			Player_updateStats(app->player);
		}
		break;
	}

	default:
		break;
	}
}

/* 0x0000a008 State 4: a combat exchange is running. */
void Canvas_combatState(Canvas *canvas)
{
	Applet *app = g_app;

	Game_updateMonsterLerps(app->game);
	if (!FreeMove_update(canvas, 0))   /* port: free movement looks around */
		Canvas_updateMovement(canvas, 0, 0);
	ParticleSystem_update(app->particles, 1);

	if (canvas->combatDone) {
		/* wait for the monsters to finish moving, then hand control back */
		if (app->game->monstersMoving == 0) {
			if (app->combat->attacker == NULL)
				Game_advanceTurn(app->game);
			if (!Game_isCinematicActive(app->game)) {
				Canvas_setState(canvas, ST_PLAYING);
				if ((app->combat->savedAngle & 0x3ff) != (canvas->destAngle & 0x3ff))
					Canvas_finishRotation(canvas, 1);
			} else {
				Canvas_setState(canvas, ST_CINEMATIC);
				ScriptThread_run(app->game->activeCamera->thread);
			}
		}
	} else if (!Combat_runFrame(app->combat)) {
		Entity *attacker;

		if (canvas->state == ST_DYING) {
			while (app->game->combatMonsters)
				Game_removeFromCombat(app->game->combatMonsters);
			return;
		}

		attacker = app->combat->attacker;
		if (attacker == NULL) {
			/* the player's attack finished */
			Game_touchTile(app->game, canvas->destX, canvas->destY, 0);
			canvas->combatDone = 1;
		} else if (canvas->moveCount == 0) {
			MonsterGoal *goal = &attacker->monster->goal;
			Entity *next;

			if (goal->flags & 8) {
				MonsterGoal_reset(goal);
				goal->type = 5;
				goal->param = 1;
				Entity_aiThink(attacker);
			}
			/* next monster in the attack queue that can still attack */
			next = attacker->monster->nextAttacker;
			if (next) {
				for (; next; next = next->monster->nextAttacker) {
					if (Entity_aiIsAttackValid(next)) {
						Entity *nxt = attacker->monster->nextAttacker;
						Combat_start(app->combat, nxt, nxt->monster->target, 0, 0);
						break;
					}
					Game_removeFromCombat(next);
				}
			} else {
				app->game->combatMonsters = NULL;
				if (app->game->monstersMoving) {
					Canvas_setState(canvas, ST_PLAYING);
				} else {
					Game_checkBobAfterWarp(app->game, 0, 0, 0);
					Canvas_drawPlayingSoftKeys(canvas);
					canvas->combatDone = 1;
				}
			}
		}
	}

	Game_updateMovers(app->game);
	if (!Game_isCinematicActive(app->game)) {
		Hud_draw(app->hud);
		Hud_drawSpeechBubble(app->hud, 0, 0);
	}
}

/* 0x0002fc00 States 3/9: exploring. */
int Canvas_updatePlaying(Canvas *canvas)
{
	Applet *app = g_app;
	Game *game;
	Entity *head;

	if (canvas->snapView)
		return Canvas_updateCamera(canvas);

	/* while frozen the turn passes on its own every 1.5 s */
	if (app->player->statusEffects[SE_FROZEN].turns > 0 && app->time - app->game->turnTime > 1500) {
		Game_touchTile(app->game, canvas->destX, canvas->destY, 0);
		Game_advanceTurn(app->game);
	}

	if (CombatEntity_getHealth(&app->player->ce) <= 0) {
		Player_died(app->player);
		return 0;
	}

	game = app->game;
	if (game->activePropagators == 0 && game->monstersTurn)
		Game_endPlayerTurn(game);
	Game_updateMovers(app->game);
	if (canvas->viewX != canvas->destX || canvas->viewY != canvas->destY || canvas->viewAngle != canvas->destAngle)
		ParticleSystem_freeAll(canvas->particles);
	if (!FreeMove_update(canvas, 1))   /* port: free movement (freemove.c) */
		Canvas_updateMovement(canvas, 0, 0);
	ParticleSystem_update(canvas->particles, 1);

	if (canvas->hideHud) {
		/* controls help over a black screen */
		Graphics_setColor(canvas->graphics, COLOR_BLACK);
		Graphics_fillRect(canvas->graphics, 0, 0, canvas->viewRect[2], canvas->viewRect[3]);
		Hud_draw(canvas->hud);
		Hud_drawControlsHelp(canvas->hud);
	} else if (!Game_isCinematicActive(app->game)) {
		Hud_draw(canvas->hud);
	}

	/* now and then a random awake monster makes a noise */
	head = app->game->activeMonsters;
	if (head && app->playTime > canvas->nextAmbientSound) {
		Entity *ent = head;
		int n = 0, count = 0, snd;
		do {
			ent = ent->monster->nextOnList;
			n++;
			if (ent == app->game->activeMonsters) {
				/* counted the ring: walk on a random number of steps */
				count = n;
				n = (int)(0u - (uint32_t)Random_nextInt(&app->random)) % count;
			}
		} while (n != 0);
		canvas->nextAmbientSound = (int)((uint32_t)Random_nextInt(&app->random) * 64u - (uint32_t)count * 1000u);
		if (canvas->nextAmbientSound < 0)
			canvas->nextAmbientSound = 0;
		canvas->nextAmbientSound += app->playTime + 6000;
		snd = EntityMonster_getSound(ent->monster, 0);
		if (snd != -1)
			Sound_play(app->sndSys, snd, 0, 1, -1);
	}

	/* random map event script */
	if (app->playTime > canvas->nextRandomEvent) {
		int delay = (int)((uint32_t)Random_nextInt(&app->random) * 100u);
		canvas->nextRandomEvent = delay;
		if (delay < 6000)
			canvas->nextRandomEvent = 6000;
		canvas->nextRandomEvent += app->playTime;
		Game_executeTile0(app->game, 1, 0, EV_SPECIAL);
	}

	if (!Game_isCinematicActive(app->game) && canvas->state == ST_PLAYING)
		Canvas_dequeueHelpDialog(canvas, 0);
	return 0;
}

/* Append "<sum/frames>.<hundredths>ms" for the benchmark overlay. */
static void appendAverage(Canvas *canvas, int frames, int sum)
{
	String_appendInt(&canvas->scratch, sum / frames);
	String_appendChar(&canvas->scratch, '.');
	String_appendInt(&canvas->scratch, sum * 100 / frames - sum / frames * 100);
	String_appendCStr_76f0(&canvas->scratch, (char *)modStr(MOD_STR_MS));
}

/* 0x0003219c Benchmark / snap-view camera: step towards the destination and render. */
int Canvas_updateCamera(Canvas *canvas)
{
	Applet *app = g_app;

	if (canvas->profiling) {
		/* benchmark: spin in place */
		canvas->viewAngle = (canvas->viewAngle + canvas->turnStep) & 0x3ff;
	} else {
		if (canvas->viewX == canvas->destX && canvas->viewY == canvas->destY && canvas->viewAngle == canvas->destAngle)
			return 0;
		if (canvas->viewX < canvas->destX) {
			canvas->viewX += canvas->moveStep;
			if (canvas->viewX > canvas->destX)
				canvas->viewX = canvas->destX;
		} else if (canvas->viewX > canvas->destX) {
			canvas->viewX -= canvas->moveStep;
			if (canvas->viewX < canvas->destX)
				canvas->viewX = canvas->destX;
		}
		if (canvas->viewY < canvas->destY) {
			canvas->viewY += canvas->moveStep;
			if (canvas->viewY > canvas->destY)
				canvas->viewY = canvas->destY;
		} else if (canvas->viewY > canvas->destY) {
			canvas->viewY -= canvas->moveStep;
			if (canvas->viewY < canvas->destY)
				canvas->viewY = canvas->destY;
		}
		if (canvas->viewZ < canvas->destZ)
			canvas->viewZ++;
		else if (canvas->viewZ > canvas->destZ)
			canvas->viewZ--;
		if (canvas->viewAngle < canvas->destAngle) {
			canvas->viewAngle += canvas->turnStep;
			if (canvas->viewAngle > canvas->destAngle)
				canvas->viewAngle = canvas->destAngle;
		} else if (canvas->viewAngle > canvas->destAngle) {
			canvas->viewAngle -= canvas->turnStep;
			if (canvas->viewAngle < canvas->destAngle)
				canvas->viewAngle = canvas->destAngle;
		}
	}

	canvas->benchTimes[12] = app->time;
	Render_render(app->render, canvas->viewX, canvas->viewY, canvas->viewZ, canvas->viewAngle);

	if (canvas->profiling) {
		int frames = canvas->profileSums[0];

		String_setLength(&canvas->scratch, 0);
		String_appendCStr_76f0(&canvas->scratch, (char *)modStr(MOD_STR_RENDER_AVG));
		appendAverage(canvas, frames, canvas->profileSums[1]);
		Canvas_drawString(canvas, &canvas->scratch, 0, 0x14, 0);

		String_setLength(&canvas->scratch, 0);
		String_appendCStr_76f0(&canvas->scratch, (char *)modStr(MOD_STR_HORIZ_AVG));
		appendAverage(canvas, canvas->profileSums[0], canvas->profileSums[2]);
		Canvas_drawString(canvas, &canvas->scratch, 0, 0x20, 0);

		String_setLength(&canvas->scratch, 0);
		String_appendCStr_76f0(&canvas->scratch, (char *)modStr(MOD_STR_TOTAL_AVG));
		appendAverage(canvas, canvas->profileSums[0], canvas->profileSums[3]);
		Canvas_drawString(canvas, &canvas->scratch, 0, 0x2c, 0);
	}
	canvas->benchTimes[7] = App_getUpTimeMS();
	return 0;
}

/* 0x00032530 Render the 3D view at (x, y) facing `angle`, plus weapon and overlays. */
void Canvas_renderView(Canvas *canvas, int x, int y, int angle)
{
	Applet *app = g_app;

	canvas->benchTimes[12] = app->time;
	canvas->benchTimes[0] = App_getUpTimeMS();
	Render_render(app->render, x, y, canvas->viewZ, angle);
	canvas->benchTimes[1] = App_getUpTimeMS();
	Combat_drawWeapon(app->combat);
	Canvas_updateFade(canvas);

	if (canvas->state == ST_LOCKPICK) {
		/* the lock with its pins (the renderer's view is in device pixels) */
		Render_draw2DSprite(app->render, 0xd, canvas->lockCorrectHits, (canvas->screenCenterX - 0x2c) * Brew_screenScale(),
		                    (canvas->renderRect[1] + 0x14) * Brew_screenScale(), 0, 0, 0x100, 0);
	} else if (canvas->state != ST_CINEMATIC && app->player->statusEffects[SE_TINT].turns > 0) {
		Render_tintScreen(app->render);
	}
}

/* Debug overlay: "tileX tileY DIR". */
static void appendLocation(Canvas *canvas)
{
	static const uint32_t dirNames[8] = {
		MOD_STR_DIR_E, MOD_STR_DIR_NE, MOD_STR_DIR_N, MOD_STR_DIR_NW,
		MOD_STR_DIR_W, MOD_STR_DIR_SW, MOD_STR_DIR_S, MOD_STR_DIR_SE,
	};
	int angle = canvas->viewAngle & 0x3ff;
	uint32_t name = MOD_STR_DIR_NONE;

	String_setLength(&canvas->scratch, 0);
	String_appendInt(&canvas->scratch, canvas->viewX >> 6);
	String_appendChar(&canvas->scratch, ' ');
	String_appendInt(&canvas->scratch, canvas->viewY >> 6);
	String_appendChar(&canvas->scratch, ' ');
	if ((angle & 0x7f) == 0)
		name = dirNames[angle >> 7];
	String_appendCStr_76f0(&canvas->scratch, (char *)modStr(name));
}

/* 0x00035a14 One tick of the game. */
void Canvas_run(Canvas *canvas)
{
	Applet *app = g_app;
	int state;

	IDISPLAY_Backlight(1);

	if (canvas->profiling) {
		canvas->profileSums[0]++;
		canvas->profileSums[1] += app->render->timeA0 - app->render->time98;
		canvas->profileSums[2] += app->render->time9c - app->render->timeA4;
		canvas->profileSums[3] += app->time - app->lastFrameTime;
	}
	if (canvas->frameSkip > 0) {
		canvas->frameSkip--;
		canvas->numKeys = 0;
	}
	canvas->benchTimes[6] = App_getUpTimeMS();

	if (canvas->state != ST_LOGOS && canvas->state != ST_MENU && app->game->paused == 0)
		app->playTime += app->time - app->lastFrameTime;

	if (canvas->vibrateEndTime != 0 && app->time > canvas->vibrateEndTime) {
		ISOUND_StopVibrate();
		canvas->vibrateEndTime = 0;
	}
	Sound_flushPending(app->sndSys);

	/* scripts run everywhere except the logos and the top-level menu */
	if (canvas->state != ST_LOGOS && !(canvas->state == ST_MENU && app->menuSystem->menu == 0x14)) {
		app->game->traceCount = 0;
		Game_updatePlayerVars(app->game);
		Game_gspriteUpdateAll(app->game);
		Game_runScriptThreads(app->game);
		if (canvas->state != ST_DIALOG && Game_hasThreadFlag(app->game))
			canvas->numKeys = 0;
	}

	Canvas_runInputEvents(canvas);

	state = canvas->state;
	if ((state == ST_SAVING || state == ST_LOADING) && canvas->drawLoading) {
		/* show the loading screen for one frame before doing the work */
		canvas->drawLoading = 0;
		canvas->spinnerFrame = 0;
		Canvas_drawLoading(canvas);
		return;
	}

	switch (state) {
	case ST_LOGOS:
		Canvas_updateLogos(canvas);
		break;

	case ST_MENU:
		Canvas_drawSoftKeys_2b7bc(canvas);
		break;

	case ST_PLAYING:
	case ST_BENCHMARK:
		Canvas_updatePlaying(canvas);
		break;

	case ST_COMBAT:
		Canvas_combatState(canvas);
		break;

	case ST_AUTOMAP:
		Canvas_automapState(canvas);
		break;

	case ST_LOADING:
		if (canvas->loadType == 0) {
			if (!Canvas_doLoadMap(canvas))
				return;
		} else {
			Game_loadSaveGame(app->game, canvas->loadType);
			Hud_addMessageFile(app->hud, 0, 0x2a);
			canvas->loadType = 0;
		}
		break;

	case ST_DIALOG:
		Game_updateMovers(app->game);
		Canvas_updateMovement(canvas, 0, 0);
		if (!Game_isCinematicActive(app->game)) {
			ParticleSystem_update(canvas->particles, 1);
			Hud_draw(canvas->hud);
		} else if (canvas->dialogStyle == 1 || canvas->dialogStyle == 8) {
			Hud_draw(canvas->hud);
		}
		Canvas_drawDialog(canvas);
		break;

	case ST_INTRO:
		Canvas_drawStoryScreen(canvas);
		if (canvas->storyPage >= canvas->storyPageCount)
			Canvas_restartLevel(canvas);
		break;

	case ST_TRAVELMAP:
		Canvas_drawTravelMap(canvas);
		break;

	case ST_DYING:
		Canvas_updateDying(canvas);
		break;

	case ST_ENDING:
		Canvas_drawStoryScreen(canvas);
		if (canvas->storyPage >= canvas->storyPageCount)
			Canvas_openMainMenu();
		break;

	case ST_CREDITS:
		Canvas_drawScrollingText(canvas);
		break;

	case ST_SAVING: {
		uint32_t type = (uint32_t)canvas->saveType;
		Game *game = app->game;

		if (!(type & SAVE_WORLD) && !(type & SAVE_PLAYER)) {
			App_fatalError(app, (char *)modStr(MOD_STR_SAVE_NOTHING));
		} else {
			if (type & SAVE_THEN_TRAVEL) {
				/* level exit: respawn point from the pending change-map target */
				uint32_t spawn = (uint32_t)game->spawnOverride;
				if (spawn == 0) {
					Game_saveState(game, canvas->prevMapId, canvas->mapId, 0, 0, 0, 0, 0, (int)type);
				} else {
					int angle = Dir_toAngle(app->textUtil, (int)((spawn >> 10) & 0xff));
					int x = (int)(spawn & 0x1f) * 64 + 0x20;
					int y = (int)(spawn & 0x3e0) * 2 + 0x20;
					Game_saveState(app->game, canvas->prevMapId, canvas->mapId, x, y, angle, x, y,
					               canvas->saveType);
				}
			} else if (type & SAVE_THEN_MENU) {
				uint32_t spawn = (uint32_t)game->spawnOverride;
				int angle = Dir_toAngle(app->textUtil, (int)((spawn >> 10) & 0xff));
				int x = (int)(spawn & 0x1f) * 64 + 0x20;
				int y = (int)(spawn & 0x3e0) * 2 + 0x20;
				Game_saveState(app->game, canvas->mapId, app->menu->mapId, x, y, angle, x, y, canvas->saveType);
			} else {
				Game_saveState(app->game, canvas->mapId, canvas->mapId, canvas->destX, canvas->destY,
				               canvas->destAngle, canvas->prevX, canvas->prevY, (int)type);
			}
			Hud_addMessageFile(app->hud, 0, 0x29);
		}

		type = (uint32_t)canvas->saveType;
		if (type & SAVE_THEN_MAIN)
			Canvas_backToMain(canvas, 0);
		else if (type & SAVE_THEN_EXIT)
			Applet_shutdown(app);
		else if (type & SAVE_THEN_TRAVEL)
			Canvas_setState(canvas, IS_TRAVEL_MAP(canvas->mapId) ? ST_TRAVELMAP : ST_LOADING);
		else if (type & SAVE_THEN_MENU)
			MenuSystem_setMenu(canvas->menuSystem, 1);
		else if (type & SAVE_THEN_MENU16)
			MenuSystem_setMenu(canvas->menuSystem, 0x16);
		else
			Canvas_setState(canvas, ST_PLAYING);
		canvas->saveType = 0;
		break;
	}

	case ST_ERROR:
		Canvas_drawErrorScreen(canvas);
		break;

	case ST_CINEMATIC: {
		Game *game = app->game;
		Timer_expired(&game->activeCamera->thread->unpauseTime, app->time);
		game = app->game;
		Camera_update(game->activeCamera, game->cameraKeyIdx, App_getUpTimeMS() - game->cameraStartTime);
		Game_updateMovers(app->game);
		Canvas_updateMovement(canvas, 0, 0);
		if (canvas->state == ST_CINEMATIC) {
			if (app->time > app->game->skipUntilTime) {
				/* offer "Skip" on the right soft key */
				String empty;
				loadScratchText(canvas, 0, 0x2b);
				String_ctor(&empty);
				Canvas_drawSoftKeys_10c68(canvas, &empty, &canvas->scratch, 1);
				String_dtor(&empty, 0);
			} else {
				Canvas_clearSoftKeys(canvas);
			}
		}
		break;
	}

	case ST_FADE: {
		/* camera height eases back while the fade runs */
		int zStep = (canvas->animFrames + 11) / canvas->animFrames;
		if (canvas->viewZ < canvas->destZ) {
			canvas->viewZ += zStep;
			if (canvas->viewZ > canvas->destZ)
				canvas->viewZ = canvas->destZ;
		} else if (canvas->viewZ > canvas->destZ) {
			canvas->viewZ -= zStep;
			if (canvas->viewZ < canvas->destZ)
				canvas->viewZ = canvas->destZ;
		}
		Game_updateMovers(canvas->game);
		Canvas_renderView(canvas, canvas->viewX, canvas->viewY, canvas->viewAngle);
		ParticleSystem_update(canvas->particles, 1);
		if (canvas->fadeStart == 0) {
			if (canvas->fadeFlags & FADE_CHANGE_MAP)
				Game_doChangeMap(canvas->game);
			else
				Canvas_setState(canvas, ST_PLAYING);
		}
		Hud_draw(canvas->hud);
		Canvas_drawSoftKeys_10c68(canvas, &canvas->softKeyLeft, &canvas->softKeyRight, 1);
		break;
	}

	case ST_LOCKPICK:
		Canvas_drawSavingProgress(canvas);
		break;

	case ST_QUICK_WEAPON:
	case ST_QUICK_ITEM:
		Canvas_drawQuickMenu(canvas);
		break;

	default: /* ST_IDLE, ST_BENCHMARK_END, 0x16 */
		break;
	}

	/* a monster noticed the player: play its alert sound */
	state = canvas->state;
	if ((state == ST_PLAYING || state == ST_AUTOMAP || state == ST_CINEMATIC) && app->game->alertEntity) {
		int snd = EntityMonster_getRandomSound(app->game->alertEntity->monster, 1);
		if (snd != -1)
			Sound_play(app->sndSys, snd, 0, 3, -1);
		app->game->alertEntity = NULL;
	}
	canvas->benchTimes[7] = App_getUpTimeMS();

	if (canvas->state != ST_MENU) {
		if (canvas->showFPS) {
			int renderMs = canvas->benchTimes[1] - canvas->benchTimes[0];
			int flipMs = canvas->benchTimes[3] - canvas->benchTimes[2];
			int frameMs = app->time - app->lastFrameTime;
			if (canvas->benchTimes[12] == app->time) {
				/* a 3D frame was rendered this tick: latch its timings */
				canvas->benchTimes[0] = 0;
				canvas->benchTimes[8] = renderMs;
				canvas->benchTimes[9] = flipMs;
				canvas->benchTimes[10] = frameMs;
				canvas->benchTimes[1] = 0;
			}
		} else if (canvas->showLocation && canvas->state != ST_AUTOMAP) {
			appendLocation(canvas);
			Canvas_drawString(canvas, &canvas->scratch, 0, 0x34, 0);
		} else if (canvas->showDebugVar) {
			String_setLength(&canvas->scratch, 0);
			String_appendInt(&canvas->scratch, canvas->debugVar);
			Canvas_drawString(canvas, &canvas->scratch, 0, 0x34, 0);
		}
	}

	canvas->benchTimes[2] = App_getUpTimeMS();
	IDISPLAY_Update();
	if (app->deviceBitmap) {
		/* the original filled the device bitmap with raster op 7 after the update */
		AEERect rc = { canvas->screenRect[0], canvas->screenRect[1], canvas->screenRect[2], canvas->screenRect[3] };
		IBITMAP_FillRect(app->deviceBitmap, &rc, 0, 7);
	}
	canvas->benchTimes[3] = App_getUpTimeMS();
	Sound_releasePending(app->sndSys);
}
