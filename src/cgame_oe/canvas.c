/*
 * Canvas: the top-level game state machine (Canvas.cpp in the original).
 *
 * Canvas_run is called by App_tick every 15 ms. It flushes queued sounds,
 * replays the queued keys (canvas_input.c), advances sprites, moving walls and
 * script threads, and then runs the handler of the current state
 * (Canvas.state, changed only through Canvas_setState). The exploring and
 * combat states drive the 3D view through canvas_move.c.
 *
 * Also here: startup/shutdown (view geometry and the .bar settings strings),
 * map loading, the saving step and a few small state helpers.
 */

#include "oe_vm.h"
#include "game.h"

/* ---- strings in the executable's data area (read from the user's .mod) ---- */
#define MOD_STR_INSUFFICIENT_SPACE 0x2d258u
#define MOD_STR_SKIP               0x2d2f8u
#define MOD_STR_RENDER_AVG         0x2d564u /* (canvas_move.c) */
#define MOD_STR_CORRECT_COMBO      0x2d57cu
#define MOD_STR_TRAP_SPRUNG        0x2d594u
#define MOD_STR_SAVING_GAME        0x2d5acu
#define MOD_STR_LOADING_DOTS       0x2d5bcu
#define MOD_STR_BACK               0x2d5c8u
#define MOD_STR_MAKE_OFFER         0x2d5e4u
#define MOD_STR_DETAILS            0x2d5f0u
#define MOD_STR_LOADING            0x2d5f8u
#define MOD_STR_GAME_SAVED         0x2d600u
#define MOD_STR_GAME_LOADED        0x2d60cu
#define MOD_STR_LOADING_GAME       0x2d628u
#define MOD_STR_JUST_A_MOMENT      0x2d820u
#define MOD_STR_PROCESSING         0x2d830u
#define MOD_STR_FMT_ND             0x2d840u
#define MOD_STR_FMT_SP             0x2d84cu
#define MOD_STR_FMT_ML             0x2d85cu
#define MOD_STR_FMT_LI             0x2d86cu
#define MOD_STR_FMT_MS             0x2d87cu
#define MOD_STR_DIR_N              0x2d88cu /* compass labels for the location overlay */
#define MOD_STR_DIR_E              0x2d890u
#define MOD_STR_DIR_S              0x2d894u
#define MOD_STR_DIR_W              0x2d898u
#define MOD_STR_DIR_NE             0x2d89cu
#define MOD_STR_DIR_NW             0x2d8a0u
#define MOD_STR_DIR_SE             0x2d8a4u
#define MOD_STR_DIR_SW             0x2d8a8u
#define MOD_STR_DIR_NONE           0x2d8acu
#define MOD_STR_FMT_LOCATION       0x2d8b0u
#define MOD_STR_FMT_INT            0x2d8bcu

/* Canvas states (Canvas.state, +0x27c). */
enum {
	CS_LOGOS = 0,
	CS_MENU = 2,
	CS_PLAYING = 3,
	CS_COMBAT = 4,
	CS_AUTOMAP = 5,
	CS_LOADING = 6,
	CS_DIALOG = 7,
	CS_LOCK = 8,
	CS_INTRO = 9,
	CS_BENCHMARK = 0xa,
	CS_TRAVELMAP = 0xc,
	CS_DYING = 0xd,
	CS_EPILOGUE = 0xf,
	CS_CREDITS = 0x11,
	CS_SAVING = 0x12,
	CS_ERROR = 0x13,
	CS_CINEMATIC = 0x14,
	CS_FADE = 0x15,
};

/* Canvas.saveType bits for the saving state. */
enum {
	SAVE_PLAYER = 0x01,
	SAVE_WORLD = 0x02,
	SAVE_THEN_MAIN = 0x04,   /* back to the main menu */
	SAVE_THEN_TRAVEL = 0x08, /* level change: travel map or loading */
	SAVE_THEN_MENU = 0x10,   /* MenuSystem menu 10 (level-end statistics) */
};

#define MAP_FIRST     6000   /* m01.bsp */
#define MAP_LAST_TRAVEL 0x1778 /* 6008: last map reached through the travel map */
#define MAP_MENU      0x1779 /* 6009 ui.bsp, the main-menu background */
#define VIEW_EYE_Z    0x24
#define COLOR_BLACK   ((int)0xff000000)
#define COLOR_WHITE   (-1)

static char *modStr(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

/* IDISPLAY_DrawRect(display, rect, RGB_NONE, RGB_NONE, IDF_RECT_FILL): fills with
 * the display's user background colour, which App_handleEvent set to black at
 * EVT_APP_START. Absolute screen coordinates. */
static void displayFillBlack(int x, int y, int w, int h)
{
	AEERect rc;
	IBitmap *screen = Brew_screen();

	if (!screen)
		return;
	rc.x = (int16_t)x;
	rc.y = (int16_t)y;
	rc.dx = (int16_t)w;
	rc.dy = (int16_t)h;
	IBITMAP_FillRect(screen, &rc, 0, AEE_RO_COPY);
}

/* A .bar settings string as a number (ISHELL_LoadResString + WStr_toInt).
 * Returns 0 when the string is missing (*found = 0). */
static int barSetting(int id, int *found)
{
	char buf[36];
	int len = ISHELL_LoadResString(id, buf, 16);

	*found = len != 0;
	if (!len)
		return 0;
	buf[len < 35 ? len : 35] = '\0';
	return atoi(buf); /* WStr_toInt: wstrtostr + atoi; the shim already returns plain text */
}

static uint8_t barFlag(int id)
{
	int found;
	int v = barSetting(id, &found);
	return (uint8_t)(found && v != 0);
}

/* 0x00028358 */
int Canvas_startup(Canvas *cv)
{
	Applet *app = cv->app;
	Hud *hud;
	int found, value, frames, ok;
	int16_t viewSize, h3d, viewH, availH;

	cv->barOpt5 = 0;
	cv->vibrate = 1;
	cv->errorFlag = 0;
	cv->frameSkip = 0;
	cv->ignoreKeysUntil = 0;
	cv->showMonsters = 0;
	cv->showLocation = 0;
	cv->showSaveSize = 0;
	cv->numKeys = 0;
	cv->logoFrame = 0;
	cv->logoTime = 0;
	cv->oldState = -1;
	cv->vibrateUntil = 0;
	cv->hubEventTime = 0;
	cv->dialogItem = NULL;
	cv->linesPerPage = 0;
	cv->showCodes = 0;
	cv->prevMapId = 0;
	cv->mapId = 0;
	cv->usedThisTurn = 0;
	cv->f249 = 0;
	cv->loadType = 0;
	cv->saveType = 0;
	cv->mapLoaded = 0;
	cv->benchFrames = 0;
	cv->screenX = 0;
	cv->screenY = 0;
	cv->screenW = (int16_t)app->cxScreen;
	cv->screenH = (int16_t)app->cyScreen;
	cv->softkeySepR = 0;
	cv->softkeySepL = 0;
	cv->tintKey = -1;
	cv->textColor = 0;
	cv->pushSteps = 0;
	cv->helpCount = 0;
	cv->destZ = VIEW_EYE_Z;
	cv->viewZ = VIEW_EYE_Z;
	cv->glowPalette = -1;
	cv->render = app->render;
	cv->player = app->player;
	cv->game = app->game;
	cv->entityDefs = app->entityDefs;
	cv->combat = app->combat;
	cv->hud = app->hud;
	cv->menuSystem = app->menuSystem;
	cv->particles = app->particles;
	cv->viewW = 0;
	cv->viewH = 0;

	/* string 1: square view size, 0 = whole screen */
	viewSize = (int16_t)barSetting(1, &found);
	if (cv->screenW & 1)
		cv->screenW--;
	if (viewSize == 0) {
		cv->viewW = cv->screenW;
		cv->viewH = cv->screenH;
	} else {
		cv->viewW = viewSize;
		cv->viewH = viewSize;
	}

	/* text columns for the various boxes (7-px font advance) */
	cv->textCols[1] = (cv->viewW - 0x16) / 7;
	cv->textCols[0] = (cv->viewW - 0x1d) / 7;
	cv->textCols[2] = (cv->viewW - 0xb) / 7;
	cv->textCols[3] = (cv->viewW - 9) / 7;
	cv->textCols[4] = (cv->viewW - 0x21) / 7;
	cv->largeScreen = cv->viewW > 175;
	Hud_startup(cv->hud, cv->viewW > 175);

	/* 3D view between the HUD bars, plus a 20-px soft-key bar on tall screens (port: unless the
	 * options turned it off; then the HUD and the weapon sit at the screen's bottom edge) */
	hud = app->hud;
	h3d = (int16_t)(cv->viewH - (uint16_t)hud->bottomBarH - (uint16_t)hud->topBarH);
	if (cv->screenH < 148 || !Options_softkeyBar()) {
		cv->softKeyBarY = cv->screenH;
	} else {
		int gap;
		cv->softkeyBar = 1;
		cv->softKeyBarY = (int16_t)(cv->screenH - 20);
		gap = cv->screenH - cv->viewH;
		if (gap < 20)
			h3d = (int16_t)(h3d - (20 - gap));
	}
	if (h3d & 1)
		h3d = (int16_t)(h3d - 1);
	viewH = (int16_t)((uint16_t)hud->bottomBarH + h3d + (uint16_t)hud->topBarH);
	cv->viewH = viewH;
	cv->originX = (int16_t)((cv->screenW - cv->viewW) / 2);
	availH = cv->softkeyBar ? cv->softKeyBarY : cv->screenH;
	cv->originY = (int16_t)((availH - viewH) / 2);
	cv->viewCenterX = (int16_t)(cv->viewW / 2);
	cv->viewCenterY = (int16_t)(viewH / 2);
	cv->sceneX = cv->originX;
	cv->sceneY = (int16_t)(cv->originY + hud->topBarH);
	cv->sceneW = cv->viewW;
	cv->sceneH = h3d;
	Render_setScreenRect(cv->render, &cv->sceneX);
	if (cv->originY + cv->viewH == cv->softKeyBarY - 1)
		cv->softKeyBarY = (int16_t)(cv->softKeyBarY - 1);

	/* string 2: animation frames per tile move, 2..64 (default 4) */
	value = barSetting(2, &found);
	if (!found)
		frames = 4;
	else if (value < 2)
		frames = 2;
	else if (value > 0x40)
		frames = 0x40;
	else
		frames = value;
	Canvas_setAnimFrames(cv, frames);

	/* string 3: start map; the debug config stub then returns its default */
	value = barSetting(3, &found);
	if (found)
		cv->startMapId = (int16_t)value;
	cv->startMapId = (int16_t)App_getConfigInt(cv->app, "map", MAP_FIRST);

	/* string 4: skip the intro */
	cv->skipIntro = barFlag(4);
	cv->skipIntro = (uint8_t)App_getConfigInt(cv->app, "skipIntro", cv->skipIntro);

	/* strings 5, 7, 9: option flags */
	cv->barOpt5 = barFlag(5);
	cv->barOpt7 = barFlag(7);
	cv->barOpt9 = barFlag(9);

	Image_load(cv->app, 5032, 0, &cv->logos);
	Image_load(cv->app, 5022, 1, &cv->smallFont);
	Image_load(cv->app, 5038, 1, &cv->largeFont);
	Image_load(cv->app, 5112, 1, &cv->storyScroll);
	Image_load(cv->app, 5027, 1, &cv->automapCursor);
	Image_load(cv->app, 5017, 1, &cv->chatHook);
	Image_load(cv->app, 5168, 1, &cv->dialogCorners);
	Image_load(cv->app, 5019, 1, &cv->scrollBg);
	Image_load(cv->app, 5039, 1, &cv->scrollTears);
	Image_load(cv->app, 5166, 1, &cv->scrollStains);
	Image_load(cv->app, 5014, 1, &cv->barterBg);
	Image_load(cv->app, 5036, 1, &cv->barterTears);
	Image_load(cv->app, 5057, 1, &cv->barterStains);
	Image_load(cv->app, 5009, 0, &cv->largeIcons);
	Image_load(cv->app, 5029, 1, &cv->ringIcons);
	Image_load(cv->app, 5136, 0, &cv->buffIcons);
	Image_load(cv->app, 5190, 1, &cv->softkeyDivider);

	ok = App_testFileSystem(cv->app, 35000, 3);
	if (!ok) {
		cv->errorFlag = 1;
		strcpy(cv->printMsg, modStr(MOD_STR_INSUFFICIENT_SPACE));
	}
	return ok != 0;
}

/* 0x00026cf8 */
void Canvas_shutdown(Canvas *cv)
{
	App_freeImage(cv->app, &cv->smallFont);
	App_freeImage(cv->app, &cv->largeFont);
	App_freeImage(cv->app, &cv->storyImage);
	App_freeImage(cv->app, &cv->storyScroll);
	App_freeImage(cv->app, &cv->logos);
	App_freeImage(cv->app, &cv->automapCursor);
	App_freeImage(cv->app, &cv->scrollBg);
	App_freeImage(cv->app, &cv->scrollStains);
	App_freeImage(cv->app, &cv->scrollTears);
	App_freeImage(cv->app, &cv->barterBg);
	App_freeImage(cv->app, &cv->barterStains);
	App_freeImage(cv->app, &cv->barterTears);
	App_freeImage(cv->app, &cv->chatHook);
	App_freeImage(cv->app, &cv->dialogCorners);
	App_freeImage(cv->app, &cv->largeIcons);
	App_freeImage(cv->app, &cv->ringIcons);
	App_freeImage(cv->app, &cv->buffIcons);
	App_freeImage(cv->app, &cv->travelMap);
	App_freeImage(cv->app, &cv->travelNodes);
	App_freeImage(cv->app, &cv->softkeyDivider);
}

/* 0x00025830 A tile move takes n ticks: step = ceil(64/n) units, ceil(256/n) angle units. */
void Canvas_setAnimFrames(Canvas *canvas, int n)
{
	canvas->animFrames = n;
	canvas->moveStep = (n + 0x3f) / n;
	canvas->turnStep = (n + 0xff) / n;
}

/* 0x000260e4 Leave the current state and enter `state`. */
void Canvas_setState(Canvas *canvas, int state)
{
	Applet *app = canvas->app;
	int old = canvas->state;

	/* exit work */
	if (old == CS_AUTOMAP) {
		Gfx_setColor(app, COLOR_BLACK);
		displayFillBlack(0, 0, canvas->screenW, canvas->screenH);
		IDISPLAY_Update();
		if (state == CS_DIALOG || state == CS_LOCK) {
			/* redraw the 3D view under the dialog / lock */
			canvas->render->rNoTexture = 0;
			Canvas_renderView(canvas, canvas->viewX, canvas->viewY, canvas->viewAngle);
			Hud_draw(canvas->hud);
		}
		app = canvas->app;
		Player_addPausedTime(app->player, app->tickTime - canvas->automapStartTime);
	} else if (old == CS_MENU) {
		if (canvas->mapLoaded) {
			displayFillBlack(0, 0, canvas->screenW, canvas->screenH);
			IDISPLAY_Update();
		}
		app = canvas->app;
		Player_addPausedTime(app->player, app->tickTime - canvas->menuSystem->time);
	} else if (old == CS_CINEMATIC) {
		canvas->game->cinematicActive = 0;
	} else if (old == CS_COMBAT && state != CS_COMBAT) {
		canvas->app->combat->chargeLevel = 0;
	}

	canvas->state = state;

	/* entry work */
	switch (state) {
	case CS_COMBAT:
		Canvas_drawSoftKeys(canvas, NULL, NULL);
		canvas->turnDone = 0;
		return;

	case CS_PLAYING:
		canvas->game->phaseTime = canvas->app->tickTime;
		if (canvas->game->monsterTurnMode != 0)
			return;
		Canvas_drawPlayingSoftKeys(canvas);
		return;

	case CS_LOCK: {
		int len;
		canvas->lockTime = 0;
		canvas->lockDone = 0;
		memset(canvas->lockTyped, 0, 5);
		memset(canvas->lockShown, '_', 5);
		len = (int)strlen(canvas->game->password);
		canvas->lockShown[len] = '\0';
		canvas->numKeys = 0;
		Canvas_drawSoftKeys(canvas, NULL, NULL);
		return;
	}

	case CS_DIALOG:
		Canvas_drawSoftKeys(canvas, NULL, modStr(MOD_STR_SKIP));
		canvas->numKeys = 0;
		return;

	case CS_DYING:
		Canvas_drawSoftKeys(canvas, NULL, NULL);
		canvas->stateStartTime = canvas->app->tickTime;
		Hud_draw(canvas->hud);
		return;

	case CS_EPILOGUE:
		Canvas_drawSoftKeys(canvas, NULL, NULL);
		Canvas_initEpilogue(canvas);
		return;

	case CS_CREDITS:
		Canvas_initCredits(canvas);
		return;

	case CS_INTRO:
		Canvas_drawSoftKeys(canvas, NULL, NULL);
		Canvas_initIntro(canvas);
		return;

	case CS_TRAVELMAP:
		Canvas_drawSoftKeys(canvas, NULL, NULL);
		displayFillBlack(0, 0, canvas->screenW, canvas->screenH);
		Canvas_initTravelMap(canvas);
		return;

	case CS_LOADING:
	case CS_SAVING:
		/* Canvas_run shows "Just a moment!" for one tick before doing the work.
		 * (+0xf70 doubles as this flag; the travel map uses it as a path index.) */
		canvas->travelFrom = 1;
		Canvas_drawSoftKeys(canvas, NULL, NULL);
		return;

	case CS_AUTOMAP:
		canvas->usedThisTurn = canvas->game->numMovingGSprites >= 1;
		canvas->f249 = 0;
		canvas->automapStartTime = canvas->app->tickTime;
		Canvas_drawPlayingSoftKeys(canvas);
		return;

	case CS_MENU:
		canvas->menuSystem->time = canvas->app->tickTime;
		if (old == CS_PLAYING)
			MenuSystem_playOpenSound(canvas->menuSystem);
		else if (old == CS_MENU)
			return;
		canvas->numKeys = 0;
		return;

	case CS_CINEMATIC:
		canvas->hud->numMessages = 0;
		canvas->hud->cinematicBottom[0] = '\0';
		canvas->hud->cinematicTop[0] = '\0';
		canvas->hud->numSubtitles = 0;
		canvas->shakeY = 0;
		canvas->shakeX = 0;
		return;

	default:
		return;
	}
}

/* Fade level 0..255 for `elapsed` of `duration` ms; fadeDir 0 fades out. */
static int fadeLevel(Canvas *canvas, int elapsed, int duration)
{
	int level = ((((elapsed << 16) / (duration << 8)) * 0xff00) >> 16) & 0xff;
	if (!canvas->fadeDir)
		level = (0xff - level) & 0xff;
	return level;
}

/* State 0x12: write the save and continue where saveType says. */
static void runSaving(Canvas *canvas)
{
	Game *game = canvas->game;
	int type = canvas->saveType;

	if ((type & SAVE_WORLD) || (type & SAVE_PLAYER)) {
		if (type & SAVE_THEN_TRAVEL) {
			/* level exit: respawn point from the pending map change */
			uint32_t spawn = game->spawnParam;
			if (spawn == 0) {
				Game_saveState(game, canvas->prevMapId, canvas->mapId, 0, 0, 0, 0, 0, type);
			} else {
				int x = (int)(spawn & 0x1f) * 0x40 + 0x20;
				int y = (int)((spawn & 0x3ff) >> 5) * 0x40 + 0x20;
				int angle = Dir_toAngle((int)((spawn & 0x3ffff) >> 10));
				Game_saveState(canvas->game, canvas->prevMapId, canvas->mapId, x, y, angle, x, y,
				               canvas->saveType);
			}
		} else if (type & SAVE_THEN_MENU) {
			uint32_t spawn = game->spawnParam;
			int x = (int)(spawn & 0x1f) * 0x40 + 0x20;
			int y = (int)((spawn & 0x3ff) >> 5) * 0x40 + 0x20;
			int angle = Dir_toAngle((int)((spawn & 0x3ffff) >> 10));
			Game_saveState(canvas->game, canvas->mapId, (uint16_t)canvas->app->menu->f20, x, y, angle,
			               x, y, canvas->saveType);
		} else {
			Game_saveState(game, canvas->mapId, canvas->mapId, canvas->destX, canvas->destY,
			               canvas->destAngle, canvas->prevX, canvas->prevY, type);
		}
		Hud_addMessage(canvas->app->hud, modStr(MOD_STR_GAME_SAVED), 0);
	}

	type = canvas->saveType;
	if (type & SAVE_THEN_MAIN) {
		Canvas_backToMain(canvas, 0);
	} else if (type & SAVE_THEN_TRAVEL) {
		if (canvas->mapId < MAP_FIRST || canvas->mapId > MAP_LAST_TRAVEL)
			Canvas_setState(canvas, CS_LOADING);
		else
			Canvas_setState(canvas, CS_TRAVELMAP);
	} else if (type & SAVE_THEN_MENU) {
		MenuSystem_setMenu(canvas->menuSystem, 10);
	} else {
		Canvas_setState(canvas, CS_PLAYING);
	}
	canvas->saveType = 0;
}

/* Debug overlays: r_speeds, location, debug variable. */
static void drawDebugOverlays(Canvas *canvas)
{
	Applet *app = canvas->app;
	char buf[0x40];

	if (canvas->showSpeeds) {
		Render *render;
		int renderMs = canvas->renderEnd - canvas->renderStart;
		int otherMs = canvas->profF00 - canvas->profEfc;
		int flipMs = canvas->flipEnd - canvas->flipStart;
		int tickMs = canvas->tickEnd - canvas->tickStart;
		int y;

		if (canvas->lastRenderTime == app->tickTime) {
			/* a 3D frame was rendered this tick: latch its timings */
			canvas->renderStart = 0;
			canvas->renderEnd = 0;
			canvas->profEfc = 0;
			canvas->profF00 = 0;
			canvas->speedTick = tickMs;
			canvas->speedFlip = flipMs;
			canvas->speedRender = renderMs;
			canvas->speedOther = otherMs;
		}
		y = app->hud->topBarH;
		App_snprintf(canvas->printMsg, 0x80, modStr(MOD_STR_FMT_MS), canvas->speedRender, canvas->speedOther,
		             canvas->speedTick - (canvas->speedRender + canvas->speedOther), canvas->speedFlip);
		Canvas_drawString(canvas, canvas->printMsg, 0, y, 0, 0);
		render = canvas->render;
		App_snprintf(canvas->printMsg, 0x80, modStr(MOD_STR_FMT_LI), render->wallsDrawn, render->linesDrawn,
		             render->numLines);
		Canvas_drawString(canvas, canvas->printMsg, 0, y + 0xc, 0, 0);
		render = canvas->render;
		App_snprintf(canvas->printMsg, 0x80, modStr(MOD_STR_FMT_ML), render->f1f198, render->dynLinesDrawn,
		             (int)render->numMovingLines);
		Canvas_drawString(canvas, canvas->printMsg, 0, y + 0x18, 0, 0);
		render = canvas->render;
		App_snprintf(canvas->printMsg, 0x80, modStr(MOD_STR_FMT_SP), render->spriteWallsDrawn,
		             render->spritesDrawn, render->numMapSprites);
		Canvas_drawString(canvas, canvas->printMsg, 0, y + 0x24, 0, 0);
		App_snprintf(canvas->printMsg, 0x80, modStr(MOD_STR_FMT_ND), canvas->render->leavesDrawn,
		             canvas->render->numNodes);
		Canvas_drawString(canvas, canvas->printMsg, 0, y + 0x30, 0, 0);
	} else if (canvas->showLocation && canvas->state != CS_AUTOMAP) {
		/* compass label by octant, "**" between the 8 directions */
		static const uint32_t dirNames[8] = {
			MOD_STR_DIR_E, MOD_STR_DIR_NE, MOD_STR_DIR_N, MOD_STR_DIR_NW,
			MOD_STR_DIR_W, MOD_STR_DIR_SW, MOD_STR_DIR_S, MOD_STR_DIR_SE,
		};
		int angle = canvas->viewAngle & 0x3ff;
		uint32_t dir = (angle & 0x7f) == 0 ? dirNames[angle >> 7] : MOD_STR_DIR_NONE;
		int y = app->hud->topBarH * 2 + 0xc;

		App_snprintf(buf, 0x40, modStr(MOD_STR_FMT_LOCATION), canvas->viewX >> 6, canvas->viewY >> 6,
		             modStr(dir));
		Canvas_drawString(canvas, buf, 0, y, 0, 0);
	} else if (canvas->showSaveSize) {
		App_snprintf(buf, 0x20, modStr(MOD_STR_FMT_INT), canvas->debugVar);
		Canvas_drawString(canvas, buf, 0, app->hud->topBarH * 2 + 0xc, 0, 0);
	}
}

/* 0x00022914 One tick of the game. */
void Canvas_run(Canvas *canvas)
{
	Applet *app = canvas->app;
	Sound *snd;
	Game *game;

	Player_checkCanaries(canvas->app->player); /* "Canvas::run top" */
	canvas->app->game->traceCount = 0;
	canvas->app->sound->loopCount = 0;
	if (canvas->app->clipSet)
		Gfx_resetClip(canvas->app);

	if (canvas->benchmark) {
		canvas->benchFrames++;
		canvas->benchRender += canvas->render->profEnd - canvas->render->profStart;
		canvas->benchTotal += canvas->tickEnd - canvas->tickStart;
	}
	if (canvas->frameSkip > 0) {
		canvas->frameSkip--;
		canvas->numKeys = 0;
	}
	canvas->tickStart = (int32_t)GETUPTIMEMS();

	if (canvas->vibrateUntil != 0 && canvas->app->tickTime > canvas->vibrateUntil) {
		ISOUND_StopVibrate();
		canvas->vibrateUntil = 0;
	}

	/* play the sound queued during the last tick */
	snd = canvas->app->sound;
	if (snd->busy) {
		Sound_stopAll(snd);
	} else if ((uint16_t)snd->pendingId != 0) {
		Sound_play(snd, (uint16_t)snd->pendingId, snd->pendingFlags, 3, -1);
		canvas->app->sound->pendingId = 0;
		canvas->app->sound->pendingFlags = 0;
	}

	game = canvas->game;
	if (!game->paused && canvas->state != CS_MENU && canvas->state != CS_DIALOG) {
		app = canvas->app;
		app->gameTime += app->tickTime - app->lastTime;
	}

	Canvas_runInputEvents(canvas);
	Game_gspriteUpdateAll(canvas->game);
	Game_updateMovingWalls(canvas->game);
	Game_runScriptThreads(canvas->game);

	if ((canvas->state == CS_SAVING || canvas->state == CS_LOADING) && canvas->travelFrom != 0) {
		/* show the "Just a moment!" box for one tick before doing the work */
		char *line;
		int w;

		canvas->travelFrom = 0;
		displayFillBlack(0, 0, canvas->screenW, canvas->softKeyBarY);
		w = (int)strlen(modStr(MOD_STR_JUST_A_MOMENT)) * 7 + 9;
		Gfx_setColor(canvas->app, COLOR_WHITE);
		Gfx_drawRect(canvas->app, canvas->viewCenterX - (int)((unsigned)w >> 1), canvas->viewCenterY - 0x18, w,
		             0x30);
		line = canvas->printMsg[0] ? canvas->printMsg : modStr(MOD_STR_PROCESSING);
		Canvas_drawString(canvas, line, canvas->viewCenterX, canvas->viewCenterY - 0xc, 0x11, 0);
		Canvas_drawString(canvas, modStr(MOD_STR_JUST_A_MOMENT), canvas->viewCenterX, canvas->viewCenterY, 0x11,
		                  0);
		IDISPLAY_Update();
		return;
	}
	if (App_getErrorCode(canvas->app) != 0)
		return;

	switch (canvas->state) {
	case CS_LOGOS:
		Canvas_updateLogos(canvas);
		break;

	case CS_MENU:
		Canvas_menuState(canvas);
		break;

	case CS_PLAYING:
	case CS_BENCHMARK:
		Canvas_updatePlaying(canvas);
		break;

	case CS_COMBAT:
		Canvas_combatTurnDone(canvas);
		break;

	case CS_AUTOMAP:
		Canvas_automapState(canvas);
		break;

	case CS_LOADING:
		if (canvas->loadType == 0) {
			if (!Canvas_loadMedia(canvas))
				return;
		} else {
			Game_loadState(canvas->game, canvas->loadType);
			Hud_addMessage(canvas->app->hud, modStr(MOD_STR_GAME_LOADED), 0);
			canvas->loadType = 0;
		}
		break;

	case CS_DIALOG:
		Canvas_updateMovement(canvas);
		Canvas_updateShake(canvas);
		if (!Game_isCinematicActive(canvas->game)) {
			ParticleSystem_update(canvas->particles, 1);
			Hud_draw(canvas->hud);
		} else if (canvas->dialogStyle == 1) {
			Hud_draw(canvas->hud);
		}
		Canvas_drawSoftKeys(canvas, NULL, modStr(MOD_STR_SKIP));
		Canvas_drawDialog(canvas);
		break;

	case CS_LOCK:
		Canvas_combinationLockState(canvas);
		break;

	case CS_INTRO:
		Canvas_drawStoryScreen(canvas);
		if (canvas->storyPage >= canvas->storyPageCount)
			Canvas_restartLevel(canvas);
		break;

	case CS_TRAVELMAP:
		Canvas_drawTravelMap(canvas);
		break;

	case CS_DYING:
		Canvas_updateDying(canvas);
		break;

	case CS_EPILOGUE:
		Canvas_drawStoryScreen(canvas);
		if (canvas->storyPage >= canvas->storyPageCount)
			Canvas_openMainMenu(canvas);
		break;

	case CS_CREDITS:
		Canvas_drawScrollingText(canvas);
		break;

	case CS_SAVING:
		runSaving(canvas);
		break;

	case CS_ERROR:
		Canvas_drawErrorScreen(canvas);
		break;

	case CS_CINEMATIC:
		game = canvas->game;
		ScriptThread_checkWait(game->activeCamera->thread, canvas->app->tickTime);
		game = canvas->game;
		Camera_update(game->activeCamera, game->cameraKey, (int)GETUPTIMEMS() - game->cameraStart);
		Game_updateLerpSprites(canvas->game);
		Canvas_updateMovement(canvas);
		if (canvas->fadeStart != 0) {
			int elapsed = canvas->app->tickTime - canvas->fadeStart;
			if (elapsed > canvas->fadeDuration) {
				if (!canvas->fadeDir)
					Render_fadeView(canvas->render, 0);
			} else {
				Render_fadeView(canvas->render, fadeLevel(canvas, elapsed, canvas->fadeDuration));
			}
		}
		Canvas_drawSoftKeys(canvas, NULL,
		                    canvas->game->skipUntil < canvas->app->tickTime ? modStr(MOD_STR_SKIP) : NULL);
		break;

	case CS_FADE: {
		int elapsed;
		Game_updateLerpSprites(canvas->game);
		elapsed = canvas->app->tickTime - canvas->fadeStart;
		if (elapsed <= 1000) {
			int level = fadeLevel(canvas, elapsed, 1000);
			Canvas_renderView(canvas, canvas->viewX, canvas->viewY, canvas->viewAngle);
			Canvas_updateShake(canvas);
			ParticleSystem_update(canvas->particles, 1);
			Render_fadeView(canvas->render, level);
			Hud_draw(canvas->hud);
		} else {
			canvas->fadeStart = 0;
			if (!canvas->fadeDir)
				Game_processMapChange(canvas->game);
			else
				Canvas_setState(canvas, CS_PLAYING);
		}
		break;
	}

	default:
		break;
	}

	/* a monster noticed the player: play its alert sound */
	if ((canvas->state == CS_PLAYING || canvas->state == CS_AUTOMAP) && canvas->game->alertMonster) {
		int sound = EntityMonster_getRandomSound(canvas->game->alertMonster->monster, 1);
		Sound_play(canvas->app->sound, sound, 0, 3, -1);
		canvas->game->alertMonster = NULL;
	}
	canvas->tickEnd = (int32_t)GETUPTIMEMS();

	if (canvas->state != CS_MENU)
		drawDebugOverlays(canvas);

	canvas->flipStart = (int32_t)GETUPTIMEMS();
	IDISPLAY_Update();
	app = canvas->app;
	if (app->deviceBitmap) {
		/* the original "filled" the device bitmap with raster op 7 after the update */
		AEERect rc = { canvas->screenX, canvas->screenY, canvas->screenW, canvas->screenH };
		IBITMAP_FillRect(app->deviceBitmap, &rc, 0, 7);
	}
	snd = canvas->app->sound;
	if (snd->busy && snd->loopCount == 0)
		Sound_stopAll(snd);
	canvas->flipEnd = (int32_t)GETUPTIMEMS();
	Player_checkCanaries(canvas->app->player); /* "Canvas::run bottom" */
}

/* 0x000225c8 Back from a suspend: redraw the soft keys and restart the music of the state. */
void Canvas_resume(Canvas *canvas)
{
	int resId = 5500;
	int flags;

	Canvas_refreshSoftKeys(canvas);
	switch (canvas->state) {
	case CS_MENU: {
		int menu = canvas->menuSystem->menuId;
		if (menu >= 1 && menu <= 9) {
			flags = 3;
		} else if (menu == 10 || menu == 0xc) {
			resId = 5148; /* level-end / game-completed fanfare */
			flags = 2;
		} else {
			return;
		}
		break;
	}
	case CS_EPILOGUE:
	case CS_INTRO:
		resId = 5501;
		flags = 3;
		break;
	case CS_CREDITS:
		flags = 2;
		break;
	case CS_AUTOMAP:
		canvas->f249 = 0;
		return;
	default:
		return;
	}
	Sound_play(canvas->app->sound, resId, flags, 5, -1);
}

/* 0x00019554 Go to another map: autosave first when leaving a travel-map level. */
void Canvas_loadMap(Canvas *canvas, int mapId, int force)
{
	int state;

	App_dumpCounters(canvas->app);
	canvas->prevMapId = canvas->mapId;
	canvas->mapId = (uint16_t)mapId;
	Sound_clearChannelFlags(canvas->app->sound);
	if (!force && canvas->game->activeLoadType == 0 && (unsigned)(canvas->prevMapId - MAP_FIRST) < 9) {
		strcpy(canvas->printMsg, modStr(MOD_STR_SAVING_GAME));
		canvas->saveType = SAVE_THEN_TRAVEL | SAVE_WORLD | SAVE_PLAYER;
		state = CS_SAVING;
	} else {
		strcpy(canvas->printMsg, modStr(MOD_STR_LOADING_DOTS));
		if (canvas->mapId < MAP_FIRST || canvas->mapId > MAP_LAST_TRAVEL)
			state = CS_LOADING;
		else
			state = CS_TRAVELMAP;
	}
	Canvas_setState(canvas, state);
}

/* 0x000197e8 State 6: load Canvas.mapId. Returns 0 on failure. */
int Canvas_loadMedia(Canvas *canvas)
{
	Game *game = canvas->game;
	Sound *snd;
	uint8_t soundEnabled;
	char *status = NULL;

	canvas->state = CS_LOGOS;
	Canvas_unloadMap(canvas);
	canvas->menuSystem->background = NULL;
	Render_unloadMap(canvas->render);
	Game_unloadMap(canvas->game);
	Sound_freeAllChannels(canvas->app->sound);
	snd = canvas->app->sound;
	soundEnabled = snd->enabled;
	snd->enabled = 0;

	if (!Render_beginLoadMap(canvas->render, (int16_t)canvas->mapId))
		return 0;
	if (canvas->mapId < MAP_LAST_TRAVEL && canvas->player->maxMapReached < canvas->mapId)
		canvas->app->player->maxMapReached = canvas->mapId;

	/* the loading screen, in the map's intro colour */
	Gfx_setColor(canvas->app, (int)canvas->render->introColor);
	Gfx_fillRect(canvas->app, 0, 0, canvas->viewW, canvas->viewH);
	Canvas_drawString(canvas, modStr(MOD_STR_LOADING), canvas->viewCenterX, canvas->viewCenterY - 0x30, 0x11, 0);
	Canvas_drawString(canvas, canvas->render->mapName, canvas->viewCenterX, canvas->viewCenterY - 0x24, 0x11, 0);
	if (game->isSaved)
		status = modStr(MOD_STR_GAME_SAVED);
	else if (game->isLoaded)
		status = modStr(MOD_STR_GAME_LOADED);
	if (status)
		Canvas_drawString(canvas, status, canvas->viewCenterX, canvas->viewH, 0x12, 0);
	IDISPLAY_Update();

	if (!Render_beginLoadMapData(canvas->render))
		return 0;
	Game_loadMapEntities(canvas->game);
	canvas->hud->numMessages = 0;
	canvas->hud->forcedMsg[0] = '\0';
	canvas->hud->msg2[0] = '\0';
	Hud_shuffleBarks(canvas->hud);
	Player_selectWeapon(canvas->player, canvas->player->ce.weapon);
	canvas->frameSkip = 1;
	ParticleSystem_freeAll(canvas->particles);
	canvas->app->sound->enabled = soundEnabled;
	Player_initForLevel(canvas->player);
	Game_loadWorld(canvas->game);

	/* map-start events (tile 0,0), and the post-game events (tile 3,0) */
	Game_executeTile(canvas->game, 0, 0, 0xf000, 1);
	if (canvas->player->gameWon)
		Game_executeTile(canvas->game, 3, 0, 0xf000, 1);

	canvas->nextAmbientTime = canvas->app->gameTime + 8000;
	canvas->nextRandomEvent = canvas->app->gameTime + 12000;
	Game_spawnPlayer(canvas->game);
	Canvas_finishRotation(canvas);
	Game_clearMonsterTurn(canvas->game);
	Canvas_revealAutomap(canvas);

	game = canvas->game;
	if (game->isLoaded == 0) {
		/* autosave on entering a level */
		if (!Game_hasSaveData(canvas->game))
			Game_saveState(canvas->game, canvas->mapId, canvas->mapId, canvas->destX, canvas->destY,
			               canvas->destAngle, canvas->destX, canvas->destY, SAVE_WORLD | SAVE_PLAYER);
		Game_saveLevelSnapshot(canvas->game);
	}
	game = canvas->game;
	game->isLoaded = 0;
	game->isSaved = 0;
	game->activeLoadType = 0;

	/* fade in unless a map-start script changed the state */
	if (canvas->state == CS_LOGOS) {
		canvas->fadeStart = (int32_t)GETUPTIMEMS();
		canvas->fadeDir = 1;
		Canvas_setState(canvas, CS_FADE);
	}

	/* wand glow palette by progress */
	canvas->glowPalette = -1;
	if (canvas->mapId >= MAP_LAST_TRAVEL - 1) {
		canvas->glowPalette = 7;
	} else {
		uint16_t reached = canvas->player->maxMapReached;
		if (reached == MAP_LAST_TRAVEL - 2)
			canvas->glowPalette = 8;
		else if (reached >= MAP_LAST_TRAVEL - 4)
			canvas->glowPalette = 9;
		else if (reached >= MAP_LAST_TRAVEL - 6)
			canvas->glowPalette = 10;
	}

	canvas->game->paused = 0;
	canvas->app->gameTime = (int32_t)GETUPTIMEMS();
	return 1;
}

/* 0x00019b30 Load ui.bsp as the main-menu background (Doom RPG DoomCanvas_LoadMenuMap). */
void Canvas_loadMenuMap(Canvas *canvas)
{
	Applet *app;
	int tile;

	if (!Render_beginLoadMap(canvas->app->render, MAP_MENU))
		return;
	if (!Render_beginLoadMapData(canvas->app->render))
		return;
	canvas->prevMapId = canvas->mapId;
	canvas->mapId = MAP_MENU;
	Game_loadMapEntities(canvas->game);
	Game_executeTile(canvas->game, 0, 0, 0xf000, 1);
	Render_setScreenRect(canvas->app->render, &canvas->originX);
	app = canvas->app;
	tile = app->render->spawnTile;
	canvas->viewX = (tile % 32) * 0x40 + 0x20;
	canvas->viewY = (tile / 32) * 0x40 + 0x20;
	canvas->viewZ = VIEW_EYE_Z;
	canvas->destZ = VIEW_EYE_Z;
	canvas->viewAngle = Dir_toAngle(app->render->spawnDir);
	canvas->app->render->noViewOffset = 1;
	canvas->mapLoaded = 1;
	Sound_play(canvas->app->sound, 5500, 3, 5, -1);
	canvas->fadeStart = canvas->app->tickTime;
	canvas->fadeDir = 1;
}

/* 0x0001a868 Load a saved game (type 1 = saved game, 2 = level snapshot) via state 6. */
void Canvas_loadState(Canvas *canvas, int type, char *text)
{
	canvas->loadType = type;
	Game_saveConfig(canvas->app->game);
	if (text)
		strcpy(canvas->printMsg, text);
	else
		canvas->printMsg[0] = '\0';
	Canvas_setState(canvas, CS_LOADING);
}

/* 0x00023988 Save via state 0x12 (saveType bits, see SAVE_*). */
void Canvas_saveState(Canvas *canvas, int saveType, char *msg)
{
	canvas->saveType = saveType;
	if (msg)
		strcpy(canvas->printMsg, msg);
	else
		canvas->printMsg[0] = '\0';
	Canvas_setState(canvas, CS_SAVING);
}

/* 0x0002b144 */
void Canvas_unloadMap(Canvas *cv)
{
	if (!cv->mapLoaded)
		return;
	cv->mapLoaded = 0;
	Render_loadPalettes(cv->app->render);
	Render_unloadMap(cv->app->render);
	Game_unloadMap(cv->game);
	cv->app->render->noViewOffset = 0;
	Render_setScreenRect(cv->app->render, &cv->sceneX);
}

/* 0x00005888 load == 0: back to the main menu; load != 0: startup without the intro. */
void Canvas_backToMain(Canvas *canvas, int load)
{
	if (!load) {
		Gfx_setColor(canvas->app, COLOR_BLACK);
		Gfx_fillRect(canvas->app, 0, 0, canvas->viewW, canvas->viewH);
		Canvas_drawString(canvas, modStr(MOD_STR_LOADING_DOTS), canvas->viewCenterX, canvas->viewCenterY - 0x18,
		                  0x11, 0);
		Gfx_update(canvas->app);
	}
	Player_reset(canvas->player);
	Render_unloadMap(canvas->render);
	Game_unloadMap(canvas->game);
	if (!load) {
		MenuSystem_setMenu(canvas->app->menuSystem, 1);
		return;
	}
	if (!canvas->skipIntro) {
		MenuSystem_setMenu(canvas->app->menuSystem, 0xe); /* "Enable sounds?" */
		return;
	}
	if (Game_hasSaveData(canvas->game) && App_getConfigInt(canvas->app, "loadSaveGame", 0)) {
		Canvas_loadState(canvas, 1, modStr(MOD_STR_LOADING_GAME));
		return;
	}
	Canvas_loadMap(canvas, (uint16_t)canvas->startMapId, 0);
}

/* 0x00008798 */
void Canvas_openMainMenu(Canvas *canvas)
{
	Sound_clearChannelFlags(canvas->app->sound);
	MenuSystem_setMenu(canvas->app->menuSystem, 10);
}

/* 0x000087c0 After the intro story: free its picture and load the start map. */
void Canvas_restartLevel(Canvas *canvas)
{
	App_freeImage(canvas->app, &canvas->storyImage);
	Canvas_loadMap(canvas, (uint16_t)canvas->startMapId, 0);
}

/* 0x000087ec */
void Canvas_freeLoadImages(Canvas *canvas)
{
	App_freeImage(canvas->app, &canvas->travelMap);
	App_freeImage(canvas->app, &canvas->travelNodes);
}

/* 0x00014c9c */
void Canvas_initEpilogue(Canvas *canvas)
{
	Canvas_initStoryScreen(canvas, 5117, 0, 5); /* epilogue.bmp, 5 pages */
}

/* 0x00014da0 */
void Canvas_initIntro(Canvas *canvas)
{
	Canvas_initStoryScreen(canvas, 5165, 0, 4); /* intro.bmp, 4 pages */
}

/* 0x0001beb0 State 2: paint the menu and its soft keys. */
void Canvas_menuState(Canvas *canvas)
{
	MenuSystem *ms;
	char *left = NULL, *right = NULL;

	MenuSystem_paint(canvas->app->menuSystem);
	ms = canvas->app->menuSystem;
	if (ms->backMenu != -1) {
		left = modStr(MOD_STR_BACK);
		if (ms->menuId == 0x133)
			right = modStr(MOD_STR_MAKE_OFFER);
		else if (ms->selected >= 0 && (ms->items[ms->selected].flags & 0x20))
			right = modStr(MOD_STR_DETAILS);
	}
	Canvas_drawSoftKeys(canvas, left, right);
}

/* 0x00017de0 State 8: the combination lock (code typed with the arrow keys). */
void Canvas_combinationLockState(Canvas *canvas)
{
	if (!canvas->lockDone || canvas->app->tickTime <= canvas->lockTime) {
		size_t typed;
		int cx, cy, w;
		Image *arrows = &canvas->automapCursor;

		Canvas_updatePlaying(canvas);
		typed = strlen(canvas->lockTyped);
		if (typed < 4)
			canvas->lockShown[typed] = ((GETUPTIMEMS() / 300) & 1) ? '_' : ' '; /* blinking cursor */
		Gfx_setColor(canvas->app, 0);
		Gfx_fillRect(canvas->app, canvas->viewCenterX - 0x19, canvas->sceneY + 2, 0x32, 0x10);
		Gfx_setColor(canvas->app, (int)0xffaaaaaa);
		Gfx_drawRect(canvas->app, canvas->viewCenterX - 0x19, canvas->sceneY + 2, 0x32, 0x10);
		Canvas_drawString(canvas, canvas->lockShown, canvas->viewCenterX, canvas->sceneY + 2, 0x11, 1);

		/* the four arrow hints around the view centre (frames stacked vertically) */
		w = arrows->width;
		cx = canvas->sceneW / 2 + canvas->sceneX - canvas->originX;
		cy = canvas->sceneH / 2 + canvas->sceneY - canvas->originY;
		Canvas_drawRegion(canvas, arrows, 0, 0, w, w, 0, cx - 2, cy + 1, 0x12, 0, 0);
		Canvas_drawRegion(canvas, arrows, 0, w, w, w, 0, cx - 2, cy + 0xb, 0x11, 0, 0);
		Canvas_drawRegion(canvas, arrows, 0, w * 2, w, w, 0, cx + 3, cy + 6, 0x24, 0, 0);
		Canvas_drawRegion(canvas, arrows, 0, w * 3, w, w, 0, cx - 7, cy + 6, 0x28, 0, 0);
		return;
	}

	if (strcmp(canvas->lockTyped, canvas->game->password) == 0) {
		Hud_addMessage(canvas->hud, modStr(MOD_STR_CORRECT_COMBO), 1);
		canvas->fedd = 0;
		ScriptThread_run(canvas->dialogOwner);
	} else {
		if (strlen(canvas->lockTyped) == strlen(canvas->game->password)) {
			canvas->app->statTrapsSprung++;
			Hud_addMessage(canvas->hud, modStr(MOD_STR_TRAP_SPRUNG), 3);
			Player_hurtFeedback(canvas->player, 0);
			Player_takeDamage(canvas->player, 0x19, NULL);
			Particles_spawnBlood(canvas->app->combat, 0, (int)0xffff0000, 0x28, 0);
		}
		Counter_dec(canvas->dialogOwner);
	}
	if (canvas->state == CS_LOCK)
		Canvas_setState(canvas, CS_PLAYING);
}

/* 0x000279a4 Shake the view for delayMs (amplitude `intensity`) and vibrate. */
int Canvas_startShake(Canvas *cv, int delayMs, int intensity, int vibrateMs)
{
	if (intensity != 0) {
		cv->shakeUntil = (int32_t)GETUPTIMEMS() + delayMs;
		cv->shakeAmp = intensity;
	}
	if (vibrateMs != 0 && cv->vibrate) {
		cv->vibrateUntil = (int32_t)GETUPTIMEMS() + vibrateMs;
		ISOUND_Vibrate(vibrateMs & 0xffff);
	}
	return 0;
}

/* 0x0000b1cc */
void Canvas_updateShake(Canvas *canvas)
{
	if (canvas->app->tickTime < canvas->shakeUntil) {
		if (!canvas->barOpt5) {
			int r = Random_nextByte(&canvas->app->random);
			canvas->shakeX = r % (canvas->shakeAmp * 2) - canvas->shakeAmp;
		}
		{
			int r = Random_nextByte(&canvas->app->random);
			canvas->shakeY = r % (canvas->shakeAmp * 2) - canvas->shakeAmp;
		}
	} else if (canvas->shakeX != 0 || canvas->shakeY != 0) {
		canvas->shakeY = 0;
		canvas->shakeX = 0;
	}
}

/* 0x00027a14 Start the render benchmark (spin in place, state 10) or the menu spinner variant. */
void Canvas_enterState10(Canvas *cv, int menuMode)
{
	cv->cameraMode = 1;
	cv->benchmark = 1;
	cv->benchFrames = 1;
	cv->benchRender = 0;
	cv->benchHoriz = 0;
	cv->benchTotal = 0;
	if (menuMode) {
		cv->app->menuSystem->spinPeriod = 4;
		return;
	}
	cv->turnStep = 4;
	cv->destAngle = cv->viewAngle;
	Canvas_setState(cv, CS_BENCHMARK);
}
