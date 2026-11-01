/*
 * The applet: creation/startup, the BREW event handler and 40 ms tick,
 * error reporting, config/save-file probes, the GETRAND pool and logging.
 *
 * On the phone AEEClsCreateInstance -> App_createInstance built the Applet
 * and BREW then delivered events and fired a one-shot 40 ms timer that
 * Applet_Tick re-armed. Here main.c plays the shell: App_create() replaces the
 * class factory and main.c calls Applet_Tick every 40 ms. The re-arming is
 * kept as a flag so the tick stops exactly where the original timer stopped
 * (after an error report, while suspended).
 */
#include <stdarg.h>
#include <stdio.h>

#include "core_local.h"

Applet *g_app;

/* the applet's 40 ms timer (ISHELL_SetTimer/CancelTimer) */
static int timerArmed;
/* screen size handed to App_createInstance (GetDeviceInfo on the phone) */
static int deviceW, deviceH;

/* Player-visible text kept in the executable (read from the user's .mod) */
#define MOD_STR_SYSTEM_ERROR 0x470d8u /* default error text */
#define MOD_WSTR_APP_TITLE   0x47080u /* UTF-16 title of the error message box */

/* Total RAM reported by GetDeviceInfo (AEEDeviceInfo.dwRAM); only shown in
 * the debug memory screen and used for the load-time memory statistics. */
#define DEVICE_RAM (8u << 20)

/* ------------------------------------------------------------ logging */

void Oe2_warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("oe2: warning: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

void Oe2_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("oe2: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ------------------------------------------------------------ errors */

/* 0x00002e70  vsnprintf; returns the length written. */
int App_snprintf(char *buf, int n, char *fmt, ...)
{
	va_list ap;
	int r;
	va_start(ap, fmt);
	r = vsnprintf(buf, (size_t)n, fmt, ap);
	va_end(ap);
	return r;
}

/* 0x00002f34 */
void App_setErrorStr(Applet *app, char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(app->errorStr, sizeof(app->errorStr), fmt, ap);
	va_end(ap);
}

/* 0x00002f88 */
void App_setErrorCode(Applet *app, int code)
{
	app->errorCode = code;
}

/* 0x00001cb4  Report an internal limit/consistency failure: the next tick
 * shows "<message> (-666)" and stops the game. */
void App_fatalError(Applet *app, char *fmt, ...)
{
	char msg[0x200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	App_setErrorStr(app, "%s", msg);
	g_app->errorCode = -666;
	app->fatalError = 1;
}

/* ------------------------------------------------------------ memory / time */

/* 0x0001be9c  IHeap_GetMemStats: the port has no BREW heap to measure. */
int App_getUsedMemory(Applet *app)
{
	(void)app;
	return 0;
}

/* 0x0001b9cc */
int App_getFreeMemory(Applet *app)
{
	return (int)app->dwRAM - App_getUsedMemory(app);
}

/* 0x00042120  (reads Applet+0x38, the device RAM size) */
int Obj_getField38(Applet *app)
{
	return (int)app->dwRAM;
}

/* 0x0001c2f8  Debug hook compiled to a no-op in the release build. */
int App_debugCheckpoint(Applet *app, char *name, int v)
{
	(void)app;
	(void)name;
	return v;
}

/* 0x0001c308 */
int App_getUpTimeMS(void)
{
	return (int)GETUPTIMEMS();
}

/* ------------------------------------------------------------ random pool (Applet+0x98) */

/* 0x0002419c */
void Random_init(Random *rnd)
{
	rnd->index = 0x80;
}

/* 0x00045358  Refill the bytes used so far (the first `index` bytes). */
void Random_refill(Random *rnd)
{
	GETRAND(rnd->pool, rnd->index);
	rnd->index = 0;
}

/* 0x0002cb40  (the pool's last byte is never handed out) */
int Random_nextByte(Random *rnd)
{
	if ((uint32_t)rnd->index + 1 > 0x7f)
		Random_refill(rnd);
	return rnd->pool[rnd->index++];
}

/* 0x0002cb78  31-bit non-negative random number. The original loads the word with `ldr r0, [r0, #-4]` at
 * pool + index (0x2cba4); Random_nextByte moves the index one byte at a time, so the load is often unaligned,
 * and the ARM then returns the aligned word rotated right by 8 * (index & 3). */
int Random_nextInt(Random *rnd)
{
	int i, rot;
	uint32_t w;
	if ((uint32_t)rnd->index + 4 > 0x7f)
		Random_refill(rnd);
	i = rnd->index;
	rnd->index = i + 4;
	w = (uint32_t)core_le32(rnd->pool + (i & ~3));
	rot = (i & 3) * 8;
	if (rot)
		w = (w >> rot) | (w << (32 - rot));
	return (int)(w & 0x7fffffff);
}

/* ------------------------------------------------------------ statistics */

/* The original zeroes 0xd3 consecutive ints from Applet+0x28c. Each named
 * statistics field is listed with its original offset so the same index
 * ranges can be kept or cleared. */
typedef struct StatRange {
	uint16_t ofs;   /* original offset */
	uint16_t count; /* ints */
	size_t field;   /* offsetof in our Applet */
} StatRange;

#define STAT(o, f, n) { o, n, offsetof(Applet, f) }
static const StatRange statRanges[] = {
	STAT(0x28c, statXpGained, 1),
	STAT(0x290, statLevelUps, 1),
	STAT(0x294, statHealthGained, 1),
	STAT(0x298, statHealthLost, 1),
	STAT(0x29c, statGoldGained, 1),
	STAT(0x2a0, statGoldSpent, 1),
	STAT(0x2a4, statDamageDealt, 1),
	STAT(0x2a8, statDamageBySubtype, 17),
	STAT(0x2ec, statBiggestHit, 1),
	STAT(0x2f0, statDamageTakenBySubtype, 17),
	STAT(0x334, statMoves, 1),
	STAT(0x338, statStartTime, 1),
	STAT(0x33c, statDamageByWeapon, 11),
	STAT(0x368, statShotsByWeapon, 11),
	STAT(0x394, statHitsByWeapon, 11),
	STAT(0x3c0, statMissesByWeapon, 11),
	STAT(0x3ec, statItemsUsed, 15),
	STAT(0x428, statItemsGained, 15),
	STAT(0x464, statAmmoUsed, 7),
	STAT(0x480, statAmmoGained, 7),
	STAT(0x49c, statTurnsByRing, 9),
	STAT(0x4c0, statArmorLost, 1),
	STAT(0x4c4, statArmorGained, 1),
	STAT(0x4c8, statDeaths, 1),
	STAT(0x4cc, statKillsBySubtype, 17),
	STAT(0x510, statPeakSprites, 1),
	STAT(0x514, statPeakWalls, 1),
	STAT(0x518, statPeak518, 1),
	STAT(0x520, statWorstFrame, 1),
	STAT(0x524, statPurchases, 37),
	STAT(0x5c8, stat5c8, 1),
	STAT(0x5cc, statGibs, 1),
	STAT(0x5d0, stat5d0, 1),
	STAT(0x5d4, statSteals, 1),
};

static int32_t *stat_slot(Applet *app, int index)
{
	int ofs = 0x28c + index * 4;
	for (size_t i = 0; i < sizeof(statRanges) / sizeof(statRanges[0]); i++) {
		const StatRange *r = &statRanges[i];
		if (ofs >= r->ofs && ofs < r->ofs + r->count * 4)
			return (int32_t *)((char *)app + r->field) + (ofs - r->ofs) / 4;
	}
	return NULL; /* a slot the port does not model */
}

/* 0x00034e14  Clear the play statistics; without `all`, the totals that span
 * the whole game (gold, damage dealt, biggest hit, shots, items used, ring
 * turns, purchases ...) are kept. */
void Applet_resetCounters(Applet *app, int all)
{
	for (int i = 0; i < 0xd3; i++) {
		int32_t *slot;
		if (!all && (i == 4 || i == 5 || i == 6 || i == 0x18 || (i >= 0xcf && i <= 0xd2) ||
			     (i >= 0x37 && i <= 0x42) || (i >= 0x58 && i <= 0x67) || (i >= 0x84 && i <= 0x8d)))
			continue;
		slot = stat_slot(app, i);
		if (slot)
			*slot = 0;
	}
	app->statStartTime = (int32_t)GETUPTIMEMS();
}

/* ------------------------------------------------------------ files */

/* 0x00001b9c  Is there room for nFiles new files (and minFree bytes)? */
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
	return 1 - failed;
}

/* 0x0002574c  orcsandelves2.cfg: "Green-Blood: true" turns blood green. */
void Applet_readCfgFile(Applet *app, char *path)
{
	IFile *f = IFILEMGR_OpenFile(app->fileMgr, path, _OFM_READ);
	int size;
	char *text;
	if (f == NULL)
		return;
	size = IFILE_Size(f);
	if (size != 0) {
		text = calloc((size_t)size + 1, 1); /* +1: keep strstr inside the buffer */
		if (text != NULL) {
			String value, tmp, lit;
			IFILE_Read(f, text, (uint32_t)size);
			String_ctor(&value);
			String_getKeyValue(&tmp, 0, text, "Green-Blood:");
			String_assign(&value, &tmp);
			String_dtor(&tmp, 0);
			String_trim(&tmp, &value);
			String_assign(&value, &tmp);
			String_dtor(&tmp, 0);
			String_ctorCStr(&lit, "true");
			app->render->greenBlood = String_equals_40fc(&value, &lit) != 0;
			free(text);
			String_dtor(&lit, 0);
			String_dtor(&value, 0);
		}
	}
	IFILE_Release(f);
}

/* 0x0001ed8c  "Config" holds 'HCFG', version 0x15, ..., 'TCFG'. */
int App_isConfigValid(void)
{
	InputStream in;
	String name;
	int ok = 0;
	InputStream_ctor(&in);
	String_ctorCStr(&name, "Config");
	if (FileStream_open(&in, &name) == 1 && Stream_readInt(&in) == 0x47464348 /* HCFG */) {
		int size = InputStream_getSize(&in);
		uint8_t *buf = InputStream_getBuffer(&in);
		if (size >= 4 && core_le32(buf + size - 4) == 0x47464354 /* TCFG */ && Stream_readInt(&in) == 0x15)
			ok = 1;
	}
	String_dtor(&name, 0);
	InputStream_dtor(&in, 0);
	return ok;
}

/* 0x0001ee50  A save exists: any of SD_2..SD_9 plus SD_0 or SD_1, and a valid Config. */
int App_hasSaveData(Applet *app)
{
	IFileMgr *fm = g_app->fileMgr;
	String name;
	int anyData = 0, has0, has1, ok;
	(void)app;
	String_ctor(&name);
	for (int i = 2; i < 10; i++) {
		String_setLength(&name, 0);
		String_appendCStr_76f0(&name, "SD_");
		String_appendInt(&name, i);
		if (IFILEMGR_Test(fm, String_cstr(&name)) == SUCCESS)
			anyData = 1;
	}
	String_setLength(&name, 0);
	String_appendCStr_76f0(&name, "SD_");
	String_appendInt(&name, 0);
	has0 = IFILEMGR_Test(fm, String_cstr(&name)) == SUCCESS;
	String_setLength(&name, 0);
	String_appendCStr_76f0(&name, "SD_");
	String_appendInt(&name, 1);
	has1 = IFILEMGR_Test(fm, String_cstr(&name)) == SUCCESS;
	ok = anyData && (has1 || has0) ? App_isConfigValid() : 0;
	String_dtor(&name, 0);
	return ok;
}

/* ------------------------------------------------------------ lifecycle */

/* 0x0003e3f8  Allocate every subsystem, read the cfg file and start them in
 * the original order; then enter the intro (or the main menu). */
int App_startup(Applet *app)
{
	app->errorCode = 0;
	app->lastTime2 = 0;
	app->playTime = (int32_t)GETUPTIMEMS();
	app->f150 = 0;
	Applet_resetCounters(app, 1);
	app->fatalError = 0;
	Random_init(&app->random);

	app->canvas = calloc(1, sizeof(Canvas));
	app->render = calloc(1, sizeof(Render));
	app->player = calloc(1, sizeof(Player));
	app->game = calloc(1, sizeof(Game));
	app->entityDefs = calloc(1, sizeof(EntityDefManager));
	app->combat = calloc(1, sizeof(Combat));
	app->hud = calloc(1, sizeof(Hud));
	app->menu = calloc(1, sizeof(Menu));
	app->menuSystem = calloc(1, sizeof(MenuSystem));
	app->particles = calloc(1, sizeof(ParticleSystem));
	app->sndSys = calloc(1, sizeof(Sound));
	app->textUtil = calloc(1, sizeof(TextUtil));
	app->dataBuffer = calloc(1, sizeof(DataBuffer));
	if (!app->canvas || !app->render || !app->player || !app->game || !app->entityDefs || !app->combat ||
	    !app->hud || !app->menu || !app->menuSystem || !app->particles || !app->sndSys || !app->textUtil ||
	    !app->dataBuffer) {
		App_setErrorStr(app, "Out of memory");
		app->errorCode = 1;
		return 0;
	}

	Applet_readCfgFile(app, "orcsandelves2.cfg");
	if (!Render_startup(app->render) || !Player_startup(app->player) || !Canvas_startup(app->canvas) ||
	    !Menu_startup(app->menu) || !MenuSystem_startup(app->menuSystem) || !Sound_startup(app->sndSys) ||
	    !EntityDef_startup(app->entityDefs) || !Game_startup(app->game) ||
	    !ParticleSystem_startup(app->particles) || !Combat_startup(app->combat) || !String_selfTest())
		return 0;

	Game_loadConfig(app->game);
	if (app->canvas->skipIntro == 0)
		Canvas_setState(app->canvas, 0);
	else
		Canvas_backToMain(app->canvas, 1);
	return 1;
}

/* 0x00000c94  Body of AEEClsCreateInstance: build the Applet, get the BREW
 * services and run App_startup. Returns 0 on success, else an error code. */
int App_createInstance(int clsid, IShell *shell, void *module, Applet **ppApplet)
{
	Applet *app;
	*ppApplet = NULL;
	app = calloc(1, sizeof(Applet));
	if (app == NULL)
		return 1;
	*ppApplet = app;
	g_app = app; /* GETAPPINSTANCE() */
	app->clsID = clsid;
	app->m_pIShell = shell;
	app->m_pIModule = module;

	/* ISHELL_GetDeviceInfo */
	app->cxScreen = (uint16_t)deviceW;
	app->cyScreen = (uint16_t)deviceH;
	app->dwRAM = DEVICE_RAM;
	app->display = NULL;
	app->shell = shell;
	app->errorShown = 0;
	app->errorSkip = 0;
	snprintf(app->errorStr, sizeof(app->errorStr), "%s", ModData_str(&g_mod, MOD_STR_SYSTEM_ERROR));
	app->errorCode = 0;

	app->heap = NULL;
	app->initMem = App_getUsedMemory(app);
	app->imageMem = 0;
	app->graphics = IGRAPHICS_Create(Brew_screen());
	if (app->graphics == NULL)
		return 1;
	app->sound = NULL;
	app->fileMgr = IFILEMGR_Get();
	/* SMS / address book / backlight interfaces (+0x7c/+0x80/+0x84) are not created */
	app->deviceBitmap = Brew_screen(); /* IDISPLAY_GetDeviceBitmap */

	if (App_startup(app))
		return 0;
	return app->errorCode;
}

Applet *App_create(int screenW, int screenH)
{
	Applet *app = NULL;
	int r;
	deviceW = screenW;
	deviceH = screenH;
	r = App_createInstance(0, NULL, NULL, &app);
	if (r != 0) {
		Oe2_warn("startup failed: %s (%d)", app ? app->errorStr : "out of memory", r);
		if (app)
			App_destroy(app);
		return NULL;
	}
	if (app->errorCode != 0)
		Oe2_warn("startup: %s (%d)", app->errorStr, app->errorCode);
	return app;
}

/* 0x000014ec  pfnFreeAppData: shut every subsystem down and free it. */
void Applet_freeAppData(Applet *app)
{
	if (app->canvas)
		Canvas_shutdown(app->canvas);
	if (app->render)
		Render_shutdown(app->render);
	if (app->menuSystem)
		MenuSystem_shutdown(app->menuSystem);
	if (app->sndSys)
		Sound_shutdown(app->sndSys);
	if (app->entityDefs)
		EntityDefManager_shutdown(app->entityDefs);
	if (app->particles)
		ParticleSystem_shutdown(app->particles);
	if (app->hud)
		Hud_shutdown(app->hud);
	if (app->combat)
		Combat_shutdown(app->combat);
	if (app->game)
		Game_shutdown(app->game);
	if (app->menu)
		Menus_shutdown(app->menu);

	free(app->canvas);
	app->canvas = NULL;
	free(app->render);
	app->render = NULL;
	free(app->player);
	app->player = NULL;
	free(app->game);
	app->game = NULL;
	free(app->entityDefs);
	app->entityDefs = NULL;
	free(app->combat);
	app->combat = NULL;
	free(app->hud);
	app->hud = NULL;
	free(app->menu);
	app->menu = NULL;
	free(app->menuSystem);
	app->menuSystem = NULL;
	free(app->particles);
	app->particles = NULL;
	free(app->sndSys);
	app->sndSys = NULL;
	free(app->textUtil);
	app->textUtil = NULL;
	free(app->dataBuffer);
	app->dataBuffer = NULL;

	app->deviceBitmap = NULL; /* the screen belongs to the shim */
	if (app->graphics) {
		IGRAPHICS_Release(app->graphics);
		app->graphics = NULL;
	}
}

void App_destroy(Applet *app)
{
	if (!app)
		return;
	Applet_freeAppData(app);
	SoundMedia_freeCache();
	if (g_app == app)
		g_app = NULL;
	free(app);
}

/* 0x0003c3e4 */
void Applet_shutdown(Applet *app)
{
	(void)app;
	ISHELL_CloseApplet();
}

/* BREW event codes the handler distinguishes beyond platform.h */
enum {
	EVT_DIALOG_END = 0x302,
	EVT_FLIP = 0x405,
	EVT_CLOSE_REQUEST = 0x500,
};

/* 0x0000113c */
int Applet_HandleEvent(Applet *app, int evt, int wParam, int dwParam)
{
	(void)dwParam;
	/* The error message box is closed (on the phone: EVT_DIALOG_END). The port
	 * has no dialog, so any key press dismisses it. */
	if (app->errorShown != 0 && (evt == EVT_DIALOG_END || evt == EVT_KEY)) {
		Applet_shutdown(app);
		return 0;
	}
	switch (evt) {
	case EVT_KEY:
		/* the timer stalled for over 3 s: restart it */
		if ((uint32_t)((int32_t)GETUPTIMEMS() - app->time) > 3000)
			timerArmed = 1;
		Canvas_queueKey(app->canvas, (short)wParam);
		return 1;
	case EVT_APP_START:
		IDISPLAY_ClearScreen(MAKE_RGB(0, 0, 0));
		IDISPLAY_Update();
		app->time = app->lastFrameTime = (int32_t)GETUPTIMEMS();
		timerArmed = 1;
		return 1;
	case EVT_APP_STOP:
		return 1;
	case EVT_APP_SUSPEND:
		timerArmed = 0;
		Sound_stopAll(app->sndSys);
		return 1;
	case EVT_APP_RESUME:
		app->time = app->lastFrameTime = (int32_t)GETUPTIMEMS();
		Canvas_resume(app->canvas);
		IDISPLAY_Update();
		timerArmed = 1;
		return 1;
	case EVT_FLIP:
		return 1;
	case EVT_CLOSE_REQUEST:
		ISHELL_CloseApplet();
		return 0;
	default:
		return 0;
	}
}

/* 0x000013a0  The 40 ms timer callback: run one canvas frame, re-arm the timer,
 * or report an error (after which the timer stays off). */
void Applet_Tick(Applet *app)
{
	char msg[0x100], title[0x20];

	if (!timerArmed)
		return;
	timerArmed = 0; /* one-shot, like ISHELL_SetTimer */

	/* IMedia callbacks the shell would have dispatched since the last tick */
	SoundMedia_dispatch(app->sndSys);

	app->time = (int32_t)GETUPTIMEMS();
	Canvas_run(app->canvas);
	app->lastFrameTime = app->time;
	if (app->time - app->lastTime2 > 10000)
		app->lastTime2 = app->time;

	if (app->errorSkip != 0) {
		app->errorSkip = 0;
		return;
	}
	if (app->errorCode == 0) {
		timerArmed = 1;
		if (app->errorCode == 0)
			return;
	}
	/* ISHELL_MessageBoxText(title, "<error> (<code>)"); the port shows it in
	 * the window title and on stderr */
	for (int i = 0; i < (int)sizeof(title) - 1; i++)
		title[i] = (char)ModData_u16(&g_mod, MOD_WSTR_APP_TITLE + 2u * (uint32_t)i);
	title[sizeof(title) - 1] = '\0';
	snprintf(msg, sizeof(msg), "%s (%d)", app->errorStr, app->errorCode);
	memcpy(app->errorStr, msg, sizeof(app->errorStr));
	Oe2_warn("%s: %s", title, app->errorStr);
	snprintf(msg, sizeof(msg), "%s - %s", title, app->errorStr);
	Plat_setWindowTitle(msg);
	app->errorShown = 1;
}
