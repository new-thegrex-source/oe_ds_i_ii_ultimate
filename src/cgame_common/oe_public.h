/*
 * oe_public.h -- the interface between the engine (oe.exe) and a game module
 * (oe/vm/oegame.qvm, oe2/vm/oegame.qvm or the oegame<arch>.dll built from the
 * same sources).
 *
 * The engine is the BREW phone: it owns the window, input, timer, sound
 * output, the user's .bar/.mod files and the save files. A game module is
 * the applet: Orcs & Elves (src/cgame_oe) or Orcs & Elves II (src/cgame_oe2),
 * with all of its rules, AI, scripts, menus and (for now) its own software
 * renderer, which draws the phone screen and hands it to the engine.
 *
 * Shared by both sides: keep it plain C89 without system headers.
 */
#ifndef OE_PUBLIC_H
#define OE_PUBLIC_H

#define OE_GAME_API_VERSION	1

/* Name of the module: vm/oegame.qvm or oegame<ARCH_STRING>.dll in the game folder */
#define OE_GAME_MODULE		"oegame"

/* ---------------------------------------------------------------------------
 * vmMain( command, arg0, arg1, arg2 ): what the engine asks of the game
 * ------------------------------------------------------------------------- */
typedef enum {
	/* (OE_GAME_API_VERSION, screenWidth, screenHeight, screenScale) -> 0, or <0
	 * if the game cannot start (the reason has been printed). Opens the user's
	 * data through the RES/GAMEDATA traps and sends EVT_APP_START.
	 * screenScale & 0xff > 1 is the HD mode: the game lays out a screenWidth x
	 * screenHeight screen, but draws it at screenScale device pixels per game
	 * pixel (the 3D view at full resolution, the 2D art magnified), and
	 * OE_TRAP_SCREEN_UPDATE gets the device-sized image. 0 or 1: no scaling.
	 * screenScale & OE_SCREEN_HW: hardware rendering (see below). */
	OE_GAME_INIT,

	/* () sends EVT_APP_STOP and frees everything */
	OE_GAME_SHUTDOWN,

	/* (eventCode, wParam, dwParam) -> the applet's return value. BREW events
	 * (EVT_KEY with an AVK_* code, EVT_APP_SUSPEND/RESUME, ...), see platform.h */
	OE_GAME_EVENT,

	/* () one tick of the applet's timer -> 1 once the applet has asked to close */
	OE_GAME_TICK,

	/* (OE_INFO_*) -> value; may be called before OE_GAME_INIT */
	OE_GAME_INFO,

	/* () a console command the engine did not know; arguments through
	 * trap_Argc/trap_Argv -> 1 if the game handled it. Used for developer and
	 * QA commands (god, noclip, tp, ...) and the OE_PIPE test protocol. */
	OE_GAME_CONSOLE_COMMAND
} oeGameExport_t;

typedef enum {
	OE_INFO_TICK_MS,		/* the applet's timer period (O&E: 15 ms, O&E II: 40 ms) */
	OE_INFO_FAST_TICK_MS,	/* period for the --fast developer option */
	OE_INFO_SCREEN_WIDTH,	/* default phone screen size */
	OE_INFO_SCREEN_HEIGHT,
	OE_INFO_PAUSE_SOUND_ON_SUSPEND	/* 1 if sounds pause while the window is minimised */
} oeGameInfo_t;

/* ---------------------------------------------------------------------------
 * System calls: what the game asks of the engine (trap_* in oe_syscalls.c).
 * Pointers are into the module's memory. Keep the numbers stable: they are
 * baked into compiled QVMs (oe_syscalls.asm).
 * ------------------------------------------------------------------------- */
typedef enum {
	OE_TRAP_PRINT = 0,			/* (const char *text) */
	OE_TRAP_ERROR,				/* (const char *text)  does not return */
	OE_TRAP_MILLISECONDS,		/* () uptime in ms (the deterministic test clock when it runs) */
	OE_TRAP_VIRTUAL_CLOCK,		/* () 1 while the deterministic test clock runs */
	OE_TRAP_TIME_OF_DAY_MS,		/* () ms since local midnight (follows the test clock when it runs) */

	OE_TRAP_CVAR_VARIABLE_STRING,	/* (const char *name, char *buf, int size) */
	OE_TRAP_CVAR_SET,			/* (const char *name, const char *value) */
	OE_TRAP_ARGC,				/* () */
	OE_TRAP_ARGV,				/* (int n, char *buf, int size) */

	OE_TRAP_GAMEDATA_SIZE,		/* (OE_DATA_*) -> size in bytes, or -1 */
	OE_TRAP_GAMEDATA_READ,		/* (OE_DATA_*, int offset, void *buf, int len) -> bytes read */

	OE_TRAP_RES_SIZE,			/* (int resId) -> uncompressed size of a .bar data resource, -1 if missing */
	OE_TRAP_RES_READ,			/* (int resId, void *buf, int size) -> bytes copied */
	OE_TRAP_RES_STRING,			/* (int resId, char *buf, int size) -> length of a .bar string resource, -1 if missing */

	OE_TRAP_FILE_OPEN,			/* (const char *name, int ofmMode) -> handle > 0, or 0 (see FILE_LAST_ERROR) */
	OE_TRAP_FILE_READ,			/* (int h, void *buf, int n) -> bytes read */
	OE_TRAP_FILE_WRITE,			/* (int h, const void *buf, int n) -> n, or 0 on failure */
	OE_TRAP_FILE_SEEK,			/* (int h, int seekType, int pos) -> SUCCESS/EFAILED; the position for (_SEEK_CURRENT, 0) */
	OE_TRAP_FILE_SIZE,			/* (int h) -> size */
	OE_TRAP_FILE_CLOSE,			/* (int h) */
	OE_TRAP_FILE_REMOVE,		/* (const char *name) -> SUCCESS / EFILENOEXISTS */
	OE_TRAP_FILE_EXISTS,		/* (const char *name) -> 1 / 0 */
	OE_TRAP_FILE_LAST_ERROR,	/* () -> BREW error code of the last file operation */

	OE_TRAP_SCREEN_UPDATE,		/* (const unsigned short *rgb565, int width, int height, int pitchPixels) */

	OE_TRAP_SND_LOAD,			/* (int resId) -> handle > 0 of the decoded sound, 0 if it cannot be decoded */
	OE_TRAP_SND_PLAY,			/* (int channel, int handle, int loops) */
	OE_TRAP_SND_STOP,			/* (int channel) */
	OE_TRAP_SND_IS_PLAYING,		/* (int channel) -> 1 / 0 */
	OE_TRAP_SND_SET_VOLUME,		/* (int channel, int volume0to100) */

	OE_TRAP_VIBRATE,			/* (int ms) 0 = stop */
	OE_TRAP_SET_WINDOW_TITLE,	/* (const char *title) */
	OE_TRAP_PIPE_WRITE,			/* (const char *text) OE_PIPE test protocol: write to stdout */

	/* the 3D view on the GPU (OE_GAME_INIT screenFlags & OE_SCREEN_HW), see oeHwScene_t */
	OE_TRAP_HW_TEXTURE,			/* (const oeHwTexture_t *desc, const void *pixels, const unsigned char *mask,
								 *  const unsigned short *palette) define/replace texture desc->id */
	OE_TRAP_HW_SCENE,			/* (const oeHwScene_t *scene, const oeHwQuad_t *quads,
								 *  const short *flatTiles, const unsigned short *view) the 3D view of the
								 *  frame presented next; view = the game's own picture of it (rect[2] x
								 *  rect[3] RGB565, screenPitch per row) */
	OE_TRAP_HW_FREEVIEW,		/* (int view[4]) -> 1 while the engine flies a free camera over the game's
								 *  map (oe_idtech3freecam): view = x, y, z (world units), angle (1024 per
								 *  turn). The game then renders from there, the whole map (no culling) and
								 *  without what seeing does in the game (automap, waking monsters) */
	OE_TRAP_FREE_MOVE,			/* (oeFreeMove_t *move) -> move->mode: the free movement input (oe_moveMode),
								 *  asked every tick while exploring; see oeFreeMove_t */
	OE_TRAP_EXEC,				/* (const char *text) console commands, run after the tick (vid_restart) */
	OE_TRAP_RES_IMAGE,			/* (int resId, unsigned short *px, int maxPixels, int info[3]) -> 1 if image
								 *  resource resId is shown from another picture (overrides.txt in a .pk3,
								 *  or the id Tech 3 font): info[2] in = the screen's scale, info[0..1] out
								 *  = its size; px gets it (RGB565, 0xf81f clear) when maxPixels is enough.
								 *  It covers the same area as the .bar bitmap, whatever its size */
	OE_TRAP_EXPORT_FILE,		/* (const char *path, const void *data, int len, int w, int h) oe_export:
								 *  writes export/<path> in the game's home folder: a PNG of the RGBA
								 *  picture data when w and h are > 0, else len bytes */

	/* Orcs & Elves DS (fs_game oeds): the cartridge and its 3D view, see oeDsSprite_t */
	OE_TRAP_DS_FILE,			/* (const char *path, void *buf, int size) -> size of the cartridge's file
								 *  (NitroFS path, "maps/map_m01.bsp"; <path>.lz is expanded), -1 if
								 *  missing; copies it into buf when size is enough */
	OE_TRAP_DS_MAP,				/* (const char *name) -> 1 when the engine has the map's 3D geometry
								 *  ("map_m01"): the 3D view of OE_TRAP_HW_SCENE then shows it (and the
								 *  sprites of OE_TRAP_DS_SPRITES) instead of the quads; "" for none */
	OE_TRAP_DS_SPRITES,			/* (const oeDsSprite_t *sprites, int count) the sprites of the next
								 *  OE_TRAP_HW_SCENE */
	OE_TRAP_POINTER,			/* (int out[3], int want) -> 1 while the mouse is over the game screen: out =
								 *  x, y in the pixels of the screen given to OE_TRAP_SCREEN_UPDATE, and the
								 *  buttons (1 left, 2 right). want 1: the game shows a pointer now (the DS
								 *  touch screen's spell circle): the engine lets the mouse go and shows the
								 *  cursor until the game stops asking (ask on every tick) */

	OE_TRAP_MEMSET = 100,		/* (void *dst, int c, int n): fast versions for the QVM */
	OE_TRAP_MEMCPY,				/* (void *dst, const void *src, int n) */
	OE_TRAP_MEMMOVE,			/* (void *dst, const void *src, int n) */
	OE_TRAP_SQRT,				/* (float) -> float as int bits */
	OE_TRAP_SIN					/* (float) -> float as int bits */
} oeGameImport_t;

/* ---------------------------------------------------------------------------
 * Free movement (oe_moveMode, chosen in the games' options). The game keeps
 * its rules on the tile grid (the player stands on a tile and faces one of
 * four directions there), but the view moves freely with the keys and turns
 * with the mouse; crossing into another tile is a step of the game's own.
 * Mode 1: the turn passes on each step (and attack, item, ...), as before;
 * mode 2: the turn passes on its own every turnMs, steps cost none.
 * ------------------------------------------------------------------------- */

#define OE_MOVE_TURNS	0		/* the original: a key, a step or a quarter turn */
#define OE_MOVE_STEPS	1		/* free movement, a turn per step */
#define OE_MOVE_TIMER	2		/* free movement, turns on a timer */

typedef struct {
	int		mode;			/* OE_MOVE_* */
	int		forward;		/* -256..256 */
	int		side;			/* -256..256, positive to the right */
	int		yaw;			/* turned since the last call, 1/64 angle units (1024 per turn), positive to the left */
	int		speed;			/* world units per second */
	int		turnMs;			/* OE_MOVE_TIMER: the time a turn takes */
} oeFreeMove_t;

/* ---------------------------------------------------------------------------
 * Hardware rendering. The game still runs its own renderer (at the game's
 * resolution: it also does work the game logic depends on), and in addition
 * describes what it drew in world space; the engine draws that on the GPU at
 * the window's resolution, under the game's 2D (HUD, menus, weapon), which it
 * takes from the game's screen wherever the game drew over the 3D view.
 *
 * World space is the game's: x east, y south, z up, 64 units per map tile;
 * coordinates are 16.16 fixed point. Angles are 1024 per turn.
 * ------------------------------------------------------------------------- */

/* OE_GAME_INIT screenScale argument: bit flags above the scale (scale = value & 0xff) */
#define OE_SCREEN_HW		0x100	/* describe the 3D view with the HW traps */

typedef enum {
	OE_HWTEX_COLS8,		/* 8-bit palette indices, column after column (walls) */
	OE_HWTEX_ROWS8,		/* 8-bit palette indices row after row, with a mask byte per texel (0 = clear) */
	OE_HWTEX_RGB565		/* RGB565 row after row (floors), no palette */
} oeHwTexFormat_t;

#define OE_HWTEX_REPEAT		0x01	/* tiles (walls, floors); else clamped (sprites) */
#define OE_HWTEX_GREY		0x02	/* the game's grey colour effect */
#define OE_HWTEX_BRIGHT		0x04	/* the game's brighten effect (hit flash) */

/* oeHwTexture_t.source: what the texture is in the game's data, for the replacements of a
 * .pk3 (WTEXELS.txt, STEXELS.txt, FTEXELS.txt): kind << 24 | its number in the texels file
 * (O&E: resource 7512 / 9000 / 7000 + number; O&E II: entry of wtexels / stexels / ftexels.bin) */
#define OE_HWSRC_WALL		1
#define OE_HWSRC_SPRITE		2
#define OE_HWSRC_FLAT		3
#define OE_HWSRC( kind, n )	( ( kind ) << 24 | ( n ) )

typedef struct {
	int		id;				/* the game's texture number, 1..OE_HW_MAX_TEXTURES-1 */
	int		format;			/* oeHwTexFormat_t */
	int		width, height;	/* texels */
	int		flags;			/* OE_HWTEX_* */
	int		source;			/* OE_HWSRC( kind, number ), 0 unknown */
} oeHwTexture_t;

#define OE_HW_MAX_TEXTURES	4096
#define OE_HW_MAX_QUADS		8192

typedef enum {
	OE_HWB_OPAQUE,		/* clear texels are cut out */
	OE_HWB_ALPHA,		/* mixed with what is behind, alpha = texture's share */
	OE_HWB_ADD,			/* added, alpha = strength */
	OE_HWB_MULTIPLY		/* multiplies what is behind */
} oeHwBlend_t;

typedef struct {
	int		tex;			/* texture id */
	int		blend;			/* oeHwBlend_t */
	int		alpha;			/* 0..255 */
	int		xyz[4][3];		/* corners, world 16.16, around the quad */
	int		st[4][2];		/* texture coordinates 16.16 (1.0 = the whole texture) */
} oeHwQuad_t;

typedef struct {
	int		viewX, viewY, viewZ;	/* the eye, world 16.16 */
	int		angle;				/* view direction: 0 = +x, 256 = -y (north), 1024 per turn */
	int		rect[4];			/* the 3D view on the game screen: x, y, w, h (game pixels) */
	int		shiftX, shiftY;		/* screen shake: the view is drawn moved by this (screen pixels, like rect) */
	int		numQuads;
	int		floorZ, ceilZ;		/* the floor and ceiling planes, world 16.16 */
	int		screenPitch;		/* pixels per row of the view picture given to OE_TRAP_HW_SCENE */
	int		fade;				/* 255 = normal, less darkens the view toward black */
} oeHwScene_t;

/* flatTiles: 32 x 32 floor texture ids, then 32 x 32 ceiling texture ids (0 = none),
 * tile (x, y) at y * 32 + x, each 64 x 64 units */

/* A sprite of an Orcs & Elves DS map, drawn by the engine as the DS does (its tile's shader:
 * program or billboard, see README-OE-DS-MAPS.md) */
#define OE_DS_MAX_SPRITES	1024

#define OE_DS_SPRITE		0
#define OE_DS_DOOR			1	/* a moving line (door, gate): x, y to x2, y2; info = its 3D texture
								 * byte (shader 0x101 + it); angle = the line's flags */

typedef struct {
	int			kind;			/* OE_DS_SPRITE / OE_DS_DOOR */
	int			x, y, z;		/* world units (z 32: standing on the floor) */
	int			x2, y2;			/* OE_DS_DOOR: the other end */
	unsigned	info;			/* the .bsp's: tile | frame << 8 | flags (0x4000 wall texture, ...) */
	int			angle;			/* turn about z, 1024 per turn */
	int			alpha;			/* 0..31 (31 opaque) */
	int			scale;			/* 64 = 1.0 (DS sprite +0x15: smaller monsters); 0 is taken as 64 */
} oeDsSprite_t;

/* Files the engine reads for the game (OE_TRAP_GAMEDATA_*) */
typedef enum {
	OE_DATA_MOD					/* the original BREW executable (.mod), the game reads tables from it */
} oeGameData_t;

/* The phone's sound mixer: channels 0..OE_SND_CHANNELS-1, 8 kHz mono PCM */
#define OE_SND_CHANNELS		10
#define OE_SND_RATE			8000

#endif
