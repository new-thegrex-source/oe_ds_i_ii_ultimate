/*
 * The applet: creation, subsystem startup, the BREW event handler and the
 * 15 ms tick, error reporting, statistics counters, the GETRAND pool, a few
 * file-system helpers, and the logging used by the port.
 *
 * On the phone AEEClsCreateInstance -> App_create (0xd50) built the Applet with
 * AEEApplet_New and BREW then delivered events and fired a one-shot 15 ms timer
 * that App_tick re-armed. Here main.c plays the shell: App_create(w, h)
 * replaces the class factory and main.c calls App_tick every 15 ms. The
 * timer's armed state is kept as a flag so the tick stops exactly where the
 * original timer stopped (after an error report, after EVT_APP_STOP/SUSPEND).
 */
#include <stdarg.h>
#include <stdio.h>

#include "core_local.h"

Applet *g_app;

/* ISHELL_SetTimer(15, App_tick) armed? */
static int timerArmed;
/* screen size handed to App_create (ISHELL_GetDeviceInfo on the phone) */
static int deviceW, deviceH;

/* Player-visible text kept in the executable (read from the user's .mod). */
#define MOD_STR_SYSTEM_ERROR  0x2fb28u /* default errorStr (App_create 0xd50) */
#define MOD_WSTR_APP_TITLE    0x2fae4u /* UTF-16 title of the error message box (App_tick), 13 AECHARs */
#define MOD_STR_SETTIMER_FAIL 0x2fb0cu /* EVT_APP_START SetTimer failure (fatal error format) */
/* SaveFile_* record sizes: int[9] (maps 6000..6007, then the 1000-byte trailer record) */
#define MOD_SAVE_RECORD_SIZES 0x302f4u

/* AEEDeviceInfo.dwRAM: only shown in the debug memory screen. */
#define DEVICE_RAM (8u << 20)

/* BREW event codes handled beyond platform.h */
enum {
	EVT_DIALOG_END = 0x302,
	EVT_FLIP = 0x405,
	EVT_CLOSE_REQUEST = 0x500,
};

/* ------------------------------------------------------------ logging (port only) */

void Oe_warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("oe: warning: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

void Oe_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("oe: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ------------------------------------------------------------ errors */

/* 0x000024dc  BREW's VSNPRINTF counted the terminating NUL and the wrapper
 * subtracted one; C's vsnprintf already returns the string length. */
int App_snprintf(char *buf, int n, char *fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = vsnprintf(buf, (size_t)n, fmt, ap);
	va_end(ap);
	return r;
}

/* 0x000025dc */
void App_setErrorStr(Applet *app, char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(app->errorStr, sizeof(app->errorStr), fmt, ap);
	va_end(ap);
}

/* 0x00002618 */
void App_setErrorCode(Applet *app, int code)
{
	app->errorCode = code;
}

/* 0x00001a84 */
int App_getErrorCode(Applet *app)
{
	return app->errorCode;
}

/* 0x0000e628  Internal consistency failure: the next tick shows
 * "<message> (-666)" and stops the game. */
void App_fatalError(Applet *app, char *fmt, ...)
{
	char msg[0x200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	App_setErrorStr(app, "%s", msg);
	g_app->errorCode = -666;
	app->fileDebug = 1;
}

/* ------------------------------------------------------------ memory */

/* 0x00012afc  IHEAP_GetMemStats: the port has no BREW heap to measure. */
int App_getMemUsed(Applet *app)
{
	(void)app;
	return 0;
}

/* 0x00012e6c  Developer config hook ("map", "skipIntro", "help", ...); the
 * release build always returns the default. */
int App_getConfigInt(Applet *app, char *key, int def)
{
	(void)app;
	(void)key;
	return def;
}

/* ------------------------------------------------------------ random pool (Applet+0x8c) */

/* 0x0002bfe8  Refill the bytes used so far (the first `index` bytes). */
void Random_refill(Random *rnd)
{
	GETRAND(rnd->pool, rnd->index);
	rnd->index = 0;
}

/* 0x0001ca44  (the pool's last byte is never handed out) */
int Random_nextByte(Random *rnd)
{
	if ((uint32_t)rnd->index + 1 > 0x7f)
		Random_refill(rnd);
	return rnd->pool[rnd->index++];
}

/* 0x0001ca6c  31-bit non-negative random number. The original did a plain
 * word load at pool+index, which may be unaligned after Random_nextByte; the
 * phone's ARM then loaded the aligned word rotated by (index & 3) bytes. */
int Random_nextInt(Random *rnd)
{
	int i;
	if ((uint32_t)rnd->index + 4 > 0x7f)
		Random_refill(rnd);
	i = rnd->index;
	rnd->index = i + 4;
	return (int)(Random_wordAt(rnd, i) & 0x7fffffff);
}

/* ------------------------------------------------------------ statistics (Applet+0x1b69b8, int[0xbd]) */

/* The counters are consecutive int32 fields statXP..statGibs in the Applet
 * (as in the original); index them as one array. */
#ifndef Q3_VM /* q3lcc has no _Static_assert */
_Static_assert(offsetof(Applet, statGibs) - offsetof(Applet, statXP) == 0xbc * sizeof(int32_t),
	       "statistics counters must stay contiguous");
#endif
#define NUM_COUNTERS 0xbd

static int32_t *counter(Applet *app, int i)
{
	return (int32_t *)((char *)app + offsetof(Applet, statXP)) + i;
}

/* 0x00022280  Clear the play statistics; without `all`, the totals that span
 * the whole game (gold, damage, max hit, per-weapon shots/hits, items used,
 * ring turns, gibs ...) are kept. Either way the level timer restarts. */
void Applet_resetCounters(Applet *app, int all)
{
	if (all == 0) {
		for (int i = 0; i < NUM_COUNTERS; i++) {
			if (i == 4 || i == 5 || i == 6 || i == 0x17 || i == 0xbb || i == 0xbc || (i >= 0x35 && i <= 0x40) ||
			    (i >= 0x56 && i <= 0x63) || (i >= 0x7a && i <= 0x83))
				continue;
			*counter(app, i) = 0;
		}
	} else {
		for (int i = 0; i < NUM_COUNTERS; i++)
			*counter(app, i) = 0;
	}
	app->statLevelTime = (int32_t)GETUPTIMEMS();
}

/* 0x000127e0  Debug: append the level's counters to "counters.txt". */
void App_dumpCounters(Applet *app)
{
	OutputStream out;
	OutputStream_ctor(&out, app);
	File_open(&out, "counters.txt", _OFM_APPEND);
	app->statLevelTime = (int32_t)GETUPTIMEMS() - app->statLevelTime;
	OutputStream_printf(&out, "MAP %d\n", app->canvas->mapId);
	for (int i = 0; i < NUM_COUNTERS; i++) {
		OutputStream_printf(&out, "%d", *counter(app, i));
		if (i != NUM_COUNTERS - 1)
			OutputStream_printf(&out, " ");
	}
	OutputStream_printf(&out, "\n");
	Applet_resetCounters(app, 0);
	OutputStream_dtor(&out, 0);
}

/* ------------------------------------------------------------ save file layout */

static int save_record_size(int i)
{
	return ModData_s32(&g_mod, MOD_SAVE_RECORD_SIZES + 4u * (uint32_t)i);
}

/* 0x00001a90  Largest per-map record (first 9 table entries). */
int SaveFile_maxMapRecordSize(void)
{
	int best = 0;
	for (int i = 0; i < 9; i++)
		if (best < save_record_size(i))
			best = save_record_size(i);
	return best;
}

/* 0x00001ab0  Byte offset of record `slot` in "SaveData": three 400-byte
 * records, two of the largest map size, then the per-map records. */
int SaveFile_recordOffset(int slot)
{
	int ofs;
	switch (slot) {
	case 0:
		return 0;
	case 1:
		return 400;
	case 2:
		return 800;
	case 3:
		return 0x4b0;
	case 4:
		return SaveFile_maxMapRecordSize() + 0x4b0;
	case 5:
		return (SaveFile_maxMapRecordSize() << 1) + 0x4b0;
	default:
		ofs = SaveFile_maxMapRecordSize() * 2 + 0x4b0;
		for (int i = 0; i < slot - 5; i++)
			ofs += save_record_size(i);
		return ofs;
	}
}

/* 0x00001b18 */
int SaveFile_totalSize(void)
{
	return SaveFile_recordOffset(13) + save_record_size(8);
}

/* ------------------------------------------------------------ files */

/* 0x000014f0  Is there room for nFiles new files (and minFree bytes)? */
int App_testFileSystem(Applet *app, int minFree, int nFiles)
{
	char name[32];
	int failed = 0;
	if (IFILEMGR_GetFreeSpace(app->fileMgr, NULL) < (uint32_t)minFree)
		return 0;
	for (int i = 0; i < nFiles; i++) {
		App_snprintf(name, sizeof(name), "testfile%d", i);
		if (IFILEMGR_Test(app->fileMgr, name) != SUCCESS) {
			IFile *f = IFILEMGR_OpenFile(app->fileMgr, name, _OFM_CREATE);
			if (f == NULL)
				failed = 1;
			else
				IFILE_Release(f);
		}
	}
	for (int i = 0; i < nFiles; i++) {
		App_snprintf(name, sizeof(name), "testfile%d", i);
		IFILEMGR_Remove(app->fileMgr, name);
	}
	return !failed;
}

/* 0x0000250c  Remove the file if it exists; 1 unless the removal failed. */
int App_removeFile(Applet *app, char *name)
{
	if (IFILEMGR_Test(app->fileMgr, name) != SUCCESS)
		return 1;
	return IFILEMGR_Remove(app->fileMgr, name) == SUCCESS;
}

/* 0x0002c658  Number in a settings string resource ("ViewSize", "Frames" ...).
 * The phone passed the AECHAR text from ISHELL_LoadResString and converted it
 * into a 32-byte buffer; the port's ISHELL_LoadResString already returns plain
 * text, so `wstr` is a char string here. */
int WStr_toInt(void *wstr)
{
	const char *s = wstr;
	char buf[0x20];
	if (s == NULL || s[0] == '\0')
		return 0;
	memset(buf, 0, sizeof(buf));
	strncpy(buf, s, sizeof(buf) - 1);
	return atoi(buf);
}

/* 0x00006f98  Forget the saved {selection, scroll} of menu slot idx
 * (Menu.indexes, written by Menus_saveIndexes). */
void Struct_clearPair(void *obj, int idx)
{
	Menu *menu = obj;
	menu->indexes[idx * 2 + 1] = 0;
	menu->indexes[idx * 2] = 0;
}

/* ------------------------------------------------------------ lifecycle */

/* 0x000281bc  Wire the embedded subsystems, start them in the original order,
 * load the config and enter the intro (or the main menu). */
int App_initSubsystems(Applet *app)
{
	Canvas *canvas;

	app->errorCode = 0;
	app->time88 = 0;
	app->gameTime = (int32_t)GETUPTIMEMS();
	app->clipSet = 0;
	Applet_resetCounters(app, 1);
	app->fileDebug = 0;
	app->random.index = 0x80; /* the first draw fills the whole pool */

	app->canvasObj.app = app;
	app->canvas = &app->canvasObj;
	app->hudObj.app = app;
	app->hud = &app->hudObj;
	app->renderObj.app = app;
	app->render = &app->renderObj;
	app->menuObj.app = app;
	app->menu = &app->menuObj;
	app->menuSystemObj.app = app;
	app->menuSystem = &app->menuSystemObj;
	app->soundObj.app = app;
	app->sound = &app->soundObj;
	app->entityDefsObj.app = app;
	app->entityDefs = &app->entityDefsObj;
	app->gameObj.app = app;
	app->game = &app->gameObj;
	app->playerObj.app = app;
	app->player = &app->playerObj;
	app->particlesObj.app = app;
	app->particles = &app->particlesObj;
	app->combatObj.app = app;
	app->combat = &app->combatObj;

	if (!Render_startup(&app->renderObj) || !Player_startup(app->player) || !Canvas_startup(app->canvas) ||
	    !Menu_startup(app->menu) || !MenuSystem_startup(app->menuSystem) || !Sound_startup(app->sound) ||
	    !EntityDef_startup(app->entityDefs) || !Game_startup(app->game) ||
	    !ParticleSystem_startup(app->particles) || !Combat_startup(app->combat))
		return 0;

	Game_loadConfig(app->game);
	canvas = app->canvas;
	if (canvas->skipIntro == 0)
		Canvas_setState(canvas, 0);
	else
		Canvas_backToMain(canvas, 1);
	return 1;
}

/* 0x00000d50 (App_create) — the port's class factory. AEEApplet_New allocated
 * the 1.8 MB Applet; then the BREW interfaces were created and
 * App_initSubsystems ran. Returns NULL when startup failed. */
Applet *App_create(int screenW, int screenH)
{
	Applet *app;
	int r;

	deviceW = screenW;
	deviceH = screenH;
	app = calloc(1, sizeof(Applet));
	if (app == NULL) {
		Oe_warn("startup: out of memory");
		return NULL;
	}
	g_app = app; /* GETAPPINSTANCE() */
	app->pfnHandleEvent = NULL;
	app->pfnFreeAppData = NULL;

	/* ISHELL_GetDeviceInfo */
	app->devInfoSize = 0x40;
	app->cxScreen = (uint16_t)deviceW;
	app->cyScreen = (uint16_t)deviceH;
	app->ramTotal = DEVICE_RAM;
	app->display = NULL; /* IDISPLAY_* are global functions in the shim */
	app->shell = NULL;
	app->errorShown = 0;
	app->errorSkip = 0;
	snprintf(app->errorStr, sizeof(app->errorStr), "%s", ModData_str(&g_mod, MOD_STR_SYSTEM_ERROR));
	app->errorCode = 0;

	app->heap = NULL; /* AEECLSID_HEAP: nothing to measure on the host */
	app->heapAtInit = App_getMemUsed(app);
	app->imageMem = 0;
	app->graphics = IGRAPHICS_Create(Brew_screen());
	app->isound = NULL; /* AEECLSID_SOUND is only used for vibration (ISOUND_Vibrate) */
	app->fileMgr = IFILEMGR_Get();
	app->deviceBitmap = Brew_screen(); /* IDISPLAY_GetDeviceBitmap */
	if (app->graphics == NULL || app->deviceBitmap == NULL) {
		Oe_warn("startup: no screen");
		App_destroy(app);
		return NULL;
	}

	r = App_initSubsystems(app) ? 0 : App_getErrorCode(app);
	if (!r && app->errorCode == 0)
		return app;
	/* The phone refused to start the applet when App_initSubsystems failed;
	 * an error set during a successful startup is reported by the first tick. */
	if (r) {
		Oe_warn("startup failed: %s (%d)", app->errorStr, r);
		App_destroy(app);
		return NULL;
	}
	Oe_warn("startup: %s (%d)", app->errorStr, app->errorCode);
	return app;
}

/* 0x000012f0  pfnFreeAppData: shut the subsystems down and release the BREW
 * interfaces. (Game, Combat and Menu have no shutdown here, as in the original.) */
void App_free(Applet *app)
{
	Canvas_shutdown(app->canvas);
	Render_shutdown(app->render);
	MenuSystem_shutdown(app->menuSystem);
	Sound_shutdown(app->sound);
	EntityDefManager_shutdown(app->entityDefs);
	ParticleSystem_shutdown(app->particles);
	Hud_shutdown(app->hud);

	IBITMAP_Release(app->deviceBitmap); /* (the shim keeps the screen itself) */
	app->deviceBitmap = NULL;
	IGRAPHICS_Release(app->graphics);
	app->graphics = NULL;
	app->isound = NULL;
	app->heap = NULL;
	app->fileMgr = NULL;
}

void App_destroy(Applet *app)
{
	if (app == NULL)
		return;
	/* App_free needs the subsystem pointers App_initSubsystems set up */
	if (app->canvas != NULL)
		App_free(app);
	else if (app->graphics != NULL)
		IGRAPHICS_Release(app->graphics);
	SoundMedia_freeCache();
	if (g_app == app)
		g_app = NULL;
	free(app);
}

/* 0x00026ce4 */
void Applet_shutdown(Applet *app)
{
	(void)app;
	ISHELL_CloseApplet();
}

/* 0x0000105c  BREW event handler. */
int App_handleEvent(Applet *app, int evt, int wParam, int dwParam)
{
	(void)dwParam;
	/* The error message box was closed (EVT_DIALOG_END). The port shows the
	 * error in the window title instead of a dialog, so a key press closes it. */
	if (app->errorShown != 0 && (evt == EVT_DIALOG_END || evt == EVT_KEY)) {
		Applet_shutdown(app);
		return 0;
	}
	switch (evt) {
	case EVT_KEY:
		/* the timer stalled for over 3 s: restart it */
		if ((uint32_t)((int32_t)GETUPTIMEMS() - app->tickTime) > 3000)
			timerArmed = 1;
		Canvas_keyPressed(app->canvas, (short)wParam);
		return 1;
	case EVT_APP_START:
		IDISPLAY_ClearScreen(MAKE_RGB(0, 0, 0)); /* SetColor(CLR_USER_BACKGROUND, black) + DrawRect(NULL) */
		IDISPLAY_Update();
		app->tickTime = app->lastTime = (int32_t)GETUPTIMEMS();
		/* (ISHELL_SetTimer cannot fail here; the original reported a failure
		 * with App_fatalError(MOD_STR_SETTIMER_FAIL)) */
		timerArmed = 1;
		return 1;
	case EVT_APP_STOP:
		timerArmed = 0;
		return 1;
	case EVT_APP_SUSPEND:
		timerArmed = 0;
		Sound_clearChannelFlags(app->sound);
		return 1;
	case EVT_APP_RESUME:
		app->menuSystem->dirty = 1;
		app->tickTime = app->lastTime = (int32_t)GETUPTIMEMS();
		Canvas_resume(app->canvas);
		IDISPLAY_Update();
		timerArmed = 1;
		return 1;
	case EVT_KEY_PRESS:
	case EVT_KEY_RELEASE:
		return 0;
	case EVT_FLIP:
		return 1;
	case EVT_CLOSE_REQUEST:
		ISHELL_CloseApplet();
		return 1;
	default:
		return 0;
	}
}

/* 0x00001208  The 15 ms timer callback: run one canvas frame and re-arm the
 * timer, or report the error (after which the timer stays off). */
void App_tick(Applet *app)
{
	char title[14], msg[0x100], line[0x140];

	if (!timerArmed)
		return;
	timerArmed = 0; /* one-shot, like ISHELL_SetTimer */

	/* IMedia callbacks the shell would have dispatched since the last tick */
	SoundMedia_dispatch(app->sound);

	app->tickTime = (int32_t)GETUPTIMEMS();
	Canvas_run(app->canvas);
	app->lastTime = app->tickTime;
	if (app->tickTime - app->time88 > 10000)
		app->time88 = app->tickTime;

	if (app->errorSkip != 0) {
		app->errorSkip = 0;
		return;
	}
	if (app->errorCode == 0)
		timerArmed = 1;
	if (app->errorCode == 0)
		return;

	/* ISHELL_MessageBoxText(L"Orcs & Elves", "<error> (<code>)"); the port
	 * shows it on stderr and in the window title. */
	for (int i = 0; i < (int)sizeof(title) - 1; i++)
		title[i] = (char)ModData_u16(&g_mod, MOD_WSTR_APP_TITLE + 2u * (uint32_t)i);
	title[sizeof(title) - 1] = '\0';
	App_snprintf(msg, sizeof(msg), "%s (%d)", app->errorStr, app->errorCode);
	memcpy(app->errorStr, msg, sizeof(app->errorStr));
	Oe_warn("%s: %s", title, app->errorStr);
	snprintf(line, sizeof(line), "%s - %s", title, app->errorStr);
	Plat_setWindowTitle(line);
	app->errorShown = 1;
}
