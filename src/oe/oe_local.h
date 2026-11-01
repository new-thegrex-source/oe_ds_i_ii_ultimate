/*
 * Engine side of Orcs & Elves / Orcs & Elves II: everything both games share
 * below the game logic. The engine plays the role of the BREW phone around
 * the game module (oegame QVM or DLL): it opens the user's .bar and .mod,
 * keeps the save files, decodes and mixes the phone's sounds, turns PC keys
 * into phone keys, runs the applet's timer and shows the phone screen.
 */
#ifndef OE_LOCAL_H
#define OE_LOCAL_H

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../cgame_common/oe_public.h"

#ifndef ARRAY_LEN
#define ARRAY_LEN( x ) ( sizeof( x ) / sizeof( *( x ) ) )
#endif

/* ---------------------------------------------------------------- the games */

typedef struct oeGameDesc_s {
	const char	*dir;			/* game folder and fs_game value: "oe", "oe2" */
	const char	*title;			/* "Orcs & Elves" */
	const char	*barName;		/* the user's resource archive */
	unsigned	barSize, barCrc;	/* the one supported release */
	const char	*modName;		/* the user's BREW executable (the game reads tables from it) */
	unsigned	modSize, modCrc;
	const char	*envPrefix;		/* developer/QA environment variables: OE_*, OE2_* */
	const char	*dsRom;			/* Orcs & Elves DS: the cartridge dump (rom.nds); the .bar and .mod are
								 * Orcs & Elves' (its HUD, menus and sounds), from this folder or oe */
} oeGameDesc_t;

extern const oeGameDesc_t	oe_games[];
extern const int			oe_numGames;

const oeGameDesc_t *OE_GameForDir( const char *dir );

/* ---------------------------------------------------------------- oe_bar.c: BREW resource archive */

typedef struct barEntry_s {
	int			id;
	int			type;			/* 6 data, 1 string */
	unsigned	offset;			/* payload offset (after the MIME header) */
	unsigned	size;			/* payload size (often a gzip stream) */
	char		name[64];		/* original file name from the gzip header */
} barEntry_t;

typedef struct bar_s {
	byte		*data;
	int			size;
	barEntry_t	*entries;
	int			count;
} bar_t;

qboolean	Bar_Open( bar_t *bar, byte *data, int size );		/* takes ownership of data (malloc) */
void		Bar_Close( bar_t *bar );
const barEntry_t *Bar_Find( const bar_t *bar, int id );		/* data resources */
const barEntry_t *Bar_FindByName( const bar_t *bar, const char *name );
int			Bar_UncompressedSize( const bar_t *bar, const barEntry_t *e );
qboolean	Bar_Read( const bar_t *bar, const barEntry_t *e, byte *out, int outSize );
int			Bar_LoadString( const bar_t *bar, int id, char *buf, int size );	/* -1 if missing */
unsigned	OE_Crc32( const byte *data, int size );

/* ---------------------------------------------------------------- oe_res.c: resources from the .pk3 files */

/* overrides.txt (resources by .bar name or id) and WTEXELS / STEXELS / FTEXELS.txt (the 3D view's
 * textures by their number), read from the search path when a game starts and when it changes */
void		OE_ResInit( const bar_t *bar, qboolean qvm );
void		OE_ResShutdown( void );
const char	*OE_ResOverride( int resId );		/* a file for a non-image resource, or NULL */
/* OE_TRAP_RES_IMAGE: the picture shown for image resource resId instead of its .bar bitmap (an
 * override, or the id Tech 3 font), RGB565 with 0xf81f clear; qfalse if none. With out NULL or
 * too small only *w / *h are set. scale: the screen's device pixels per game pixel */
qboolean	OE_ResImage( int resId, int scale, unsigned short *out, int maxPixels, int *w, int *h );
const char	*OE_TexelOverride( int source );	/* oeHwTexture_t.source -> an image file, or NULL */
qboolean	OE_ResWantsHd( void );				/* some picture is shown at the window's resolution */
/* oe_export: writes export/<path> in the game's home folder, a PNG of RGBA data when w, h > 0 */
void		OE_ExportFile( const char *path, const void *data, int len, int w, int h );
void		OE_Export_f( void );				/* the .bar's pictures and data, then the module's textures */
/* an image file as RGBA (Z_Malloc): bmp of any depth here, the rest through the renderer */
byte		*OE_LoadImageRGBA( const char *name, int *w, int *h );

/* ---------------------------------------------------------------- oe_pmd.c / oe_qcelp.c: the phone's sounds */

/* Decodes a Qualcomm CMF resource holding QCELP-13k wave data into 8 kHz mono PCM (malloc). */
qboolean	Pmd_Decode( const byte *cmf, int len, short **pcm, int *samples );

/* ---------------------------------------------------------------- oe_audio.c: 10-channel mixer */

void		OE_AudioInit( void );
void		OE_AudioShutdown( void );
int			OE_AudioLoad( short *pcm, int samples );		/* takes the PCM; returns a handle > 0 */
void		OE_AudioFreeAll( void );
void		OE_AudioPlay( int channel, int handle, int loops );
void		OE_AudioStop( int channel );
qboolean	OE_AudioIsPlaying( int channel );
void		OE_AudioSetVolume( int channel, int volume );
void		OE_AudioPause( qboolean pause );

/* ---------------------------------------------------------------- oe_files.c: BREW files (saves) */

void		OE_FilesInit( const char *gameDir );
void		OE_FilesShutdown( void );
int			OE_FileOpen( const char *name, int mode );		/* handle > 0 or 0 */
int			OE_FileRead( int h, void *buf, int n );
int			OE_FileWrite( int h, const void *buf, int n );
int			OE_FileSeek( int h, int seekType, int pos );
int			OE_FileSize( int h );
void		OE_FileClose( int h );
int			OE_FileRemove( const char *name );
qboolean	OE_FileExists( const char *name );
int			OE_FileLastError( void );

/* ---------------------------------------------------------------- oe_host.c: the phone around the applet */

void		OE_Init( void );			/* cvars and commands */
void		OE_Shutdown( void );
qboolean	OE_StartGame( void );		/* for the current fs_game; qfalse (and a message) if it cannot */
void		OE_StopGame( void );
qboolean	OE_GameRunning( void );
const oeGameDesc_t *OE_CurrentGame( void );
void		OE_Frame( void );			/* runs due ticks, key repeat, QA scripts */
void		OE_FilesChanged( void );	/* the search path changed (fs_usepk3): read the overrides again */
void		OE_PhoneKey( int avk, qboolean down );
void		OE_SetMinimized( qboolean minimized );
qboolean	OE_GameCommand( void );		/* forwards an unknown console command to the game */
void		OE_GameError( void );		/* the module failed (ERR_DROP): drop it without calling it again */
int			OE_MsToNextTick( void );	/* how long the main loop may sleep */
qboolean	OE_PipeMode( void );		/* the OE_PIPE test protocol owns stdin/stdout */
qboolean	OE_PointerWanted( void );	/* the game shows a pointer: the mouse stays free, the cursor shown */

/* The phone screen: w x h RGB565 pixels, w per row (for a texW x texH texture, powers of two) */
const unsigned short *OE_ScreenImage( int *w, int *h, int *texW, int *texH, int *version );

/* game choice when no fs_game is given */
int			OE_InstalledGames( const oeGameDesc_t **list, int max );

/* ---------------------------------------------------------------- cl_hw.c: the 3D view on the GPU */

/* The client draws the game's 3D view itself (oe_renderer 1 and a window); the
 * game is then started with OE_SCREEN_HW. cl_null.c: never. */
qboolean	CL_HwAvailable( void );
void		CL_HwReset( void );			/* game started or stopped: forget its textures and scene */
void		CL_HwTexture( const oeHwTexture_t *desc, const void *pixels, const byte *mask, const unsigned short *palette );
void		CL_HwScene( const oeHwScene_t *scene, const oeHwQuad_t *quads, const short *flatTiles,
				const unsigned short *screen );
/* the game presented a frame (OE_TRAP_SCREEN_UPDATE): a 3D view described since the last one goes with it */
void		CL_HwScreen( const unsigned short *px, int w, int h, int pitch );
/* OE_TRAP_HW_FREEVIEW: qtrue and view = x, y, z, angle while the free camera flies over the game's map */
qboolean	CL_HwFreeView( int *view );
void		CL_HwLeaveQ3( void );		/* back to the game's own view (oemap) */
/* an image file through the renderer as RGBA (Z_Malloc); NULL without one (oe-headless) */
byte		*CL_LoadImageRGBA( const char *name, int *w, int *h );
void		CL_HwTexturesChanged( void );	/* upload the 3D view's textures again (the overrides changed) */
/* OE_TRAP_FREE_MOVE: fills the free movement input, returns the mode (oe_moveMode) */
int			CL_FreeMove( oeFreeMove_t *move );
/* fs_game oeds without its module: opens rom.nds and shows its first map (cl_dsmap.c); qfalse (and a
 * message) if it cannot */
qboolean	CL_DsStart( void );
/* OE_TRAP_POINTER: the mouse over the game screen, in the pixels of the screen image; qfalse when it is off it
 * (or there is no window) */
qboolean	CL_GamePointer( int *x, int *y, int *buttons );
/* the module's OE_TRAP_DS_*: a cartridge file, the map shown in the 3D view, its sprites */
int			CL_DsFile( const char *path, void *buf, int size );
qboolean	CL_DsModuleMap( const char *name );
void		CL_DsSprites( const oeDsSprite_t *sprites, int count );

/* ---------------------------------------------------------------- cl_ui.c: the menus without fs_game */

/* oe.exe started without fs_game (vm_use_idtech3_ui 1): the user interface module of the base
 * folder (vm/ui.qvm or the DLL) shows base/ui/main.menu, where a game is picked ("game <dir>",
 * OE_SwitchGame). cl_null.c: no module. */
qboolean	CL_UIStart( void );			/* qfalse (and a message) if there is no module */
void		CL_UIStop( void );
qboolean	CL_UIRunning( void );
void		CL_UIError( void );			/* an ERR_DROP: drop the module if it was running */
qboolean	CL_UICommand( void );		/* forwards an unknown console command to the module */
/* "game <dir>": leaves the menus (or the current game) for that game; qfalse (the reason in
 * ui_gameError) if it cannot start, the menus are then shown again */
qboolean	OE_SwitchGame( const char *dir );
void		OE_StartMenus( void );		/* Com_Init without fs_game: the menus (else a dialog asks) */
void		OE_StartOrDie( void );		/* Com_Init with fs_game: its game, or a fatal error saying why */

/* ---------------------------------------------------------------- elsewhere in the engine */

void		Sys_SetWindowTitle( const char *title );	/* sdl_glimp.c */
int			Key_StringToKeynum( char *str );			/* cl_keys.c */
void		Key_SetBinding( int keynum, const char *binding );
char		*Key_GetBinding( int keynum );
char		*Key_KeynumToString( int keynum );
void		Key_WriteGamepadBindings( fileHandle_t f );

#endif
