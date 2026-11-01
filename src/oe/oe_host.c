/*
 * The BREW phone around the game module: what main.c of oe-re / oe2-re did
 * as the shell, shared by both games (the two were the same apart from the
 * tick period, the file names and the QA hooks' prefix, which now come from
 * the game descriptor and the module's OE_GAME_INFO).
 *
 *  - opens the user's .bar and .mod from the game folder (size and CRC32 of
 *    the one supported release) and serves them to the module;
 *  - loads the module (vm/oegame.qvm, or oegame<arch> as a DLL with vm_oegame 0);
 *  - the applet's timer: one tick, then the timer is re-armed TICK_MS after
 *    the tick returned, exactly like ISHELL_SetTimer at the end of the tick.
 *    The screen is presented between ticks and never delays them;
 *  - BREW key repeat (one EVT_KEY on press, then every 100 ms after 1.5 s for
 *    the most recent key); the PC keys reach it through "+phone_*" binds;
 *  - suspend/resume when the window is minimised;
 *  - the developer/QA hooks of the ports, from the same environment variables
 *    (OE_* for O&E, OE2_* for O&E II): _TICKS, _KEYS, _CMDS, _SHOT, _SHOT_AT,
 *    _VCLOCK, _PIPE, _SEED, _KEYLOG.
 */
#include "oe_local.h"
#include "../client/keycodes.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#define KEY_REPEAT_START	1500
#define KEY_REPEAT_RATE		100
#define KB_AUTOREPEAT		0x40	/* dwParam flag on repeated EVT_KEYs */

/* BREW codes (cgame_common/platform.h); the engine only needs these */
#define EVT_APP_SUSPEND		2
#define EVT_APP_RESUME		3
#define EVT_KEY				0x100

#define AVK_FIRST			0xE020
#define AVK_LAST			0xE04F

#define OE_MAX_SCREEN		8192	/* device pixels per side of the game's screen image */

const oeGameDesc_t oe_games[] = {
	{ "oe", "Orcs & Elves", "orcsandelves.bar", 2099797, 0x063544a5u, "orcsandelves.mod", 198280, 0xac062b45u, "OE" },
	{ "oe2", "Orcs & Elves II", "orcsandelves2.bar", 2130702, 0xad370c54u, "orcsandelves2.mod", 292116, 0x1631c7f1u, "OE2" },
	{ "oeds", "Orcs & Elves DS", "orcsandelves.bar", 2099797, 0x063544a5u, "orcsandelves.mod", 198280, 0xac062b45u, "OEDS", "rom.nds" },
};
const int oe_numGames = sizeof( oe_games ) / sizeof( oe_games[0] );

/* the phone keys that "+phone_<name>" commands press, and the names the QA hooks use */
static const struct {
	const char	*name;
	int			avk;
} phoneKeys[] = {
	{ "up", 0xE031 }, { "down", 0xE032 }, { "left", 0xE033 }, { "right", 0xE034 }, { "select", 0xE035 },
	{ "soft1", 0xE036 }, { "soft2", 0xE037 }, { "clr", 0xE030 }, { "star", 0xE02B }, { "pound", 0xE02C },
	{ "0", 0xE021 }, { "1", 0xE022 }, { "2", 0xE023 }, { "3", 0xE024 }, { "4", 0xE025 },
	{ "5", 0xE026 }, { "6", 0xE027 }, { "7", 0xE028 }, { "8", 0xE029 }, { "9", 0xE02A },
	{ "volup", 0xE047 }, { "voldown", 0xE048 },
	{ "spell", 0xE038 },		/* AVK_SOFT3: Orcs & Elves DS's spell circle (its touch screen) */
};
#define NUM_PHONE_KEYS ( sizeof( phoneKeys ) / sizeof( phoneKeys[0] ) )

/* the ports' keyboard layout (platform_sdl.c map_key), set when a key has no bind yet */
static const struct {
	const char	*key, *phone;
} defaultBinds[] = {
	{ "UPARROW", "up" }, { "w", "up" }, { "DOWNARROW", "down" }, { "s", "down" },
	{ "LEFTARROW", "left" }, { "RIGHTARROW", "right" },
	{ "a", "1" }, { "d", "3" },					/* strafe */
	{ "ENTER", "select" }, { "KP_ENTER", "select" }, { "SPACE", "select" },
	{ "ESCAPE", "soft1" }, { "q", "spell" }, { "e", "soft2" },
	{ "BACKSPACE", "clr" }, { "TAB", "pound" },	/* automap */
	{ "z", "7" }, { "x", "star" }, { "c", "9" },	/* next / previous weapon, pass turn */
	{ "KP_PLUS", "volup" }, { "PGUP", "volup" }, { "KP_MINUS", "voldown" }, { "PGDN", "voldown" },
	{ "KP_STAR", "star" },
	{ "0", "0" }, { "1", "1" }, { "2", "2" }, { "3", "3" }, { "4", "4" },
	{ "5", "5" }, { "6", "6" }, { "7", "7" }, { "8", "8" }, { "9", "9" },
	{ "KP_INS", "0" }, { "KP_END", "1" }, { "KP_DOWNARROW", "2" }, { "KP_PGDN", "3" }, { "KP_LEFTARROW", "4" },
	{ "KP_5", "5" }, { "KP_RIGHTARROW", "6" }, { "KP_HOME", "7" }, { "KP_UPARROW", "8" }, { "KP_PGUP", "9" },
	/* default gamepad (SDL button order: A/B/X/Y/LB/RB/Back/Start/L3/R3 + axes already as arrows) */
	{ "JOY1", "select" }, { "JOY2", "soft1" }, { "JOY3", "soft2" }, { "JOY4", "pound" },
	{ "JOY5", "star" }, { "JOY6", "7" }, { "JOY7", "clr" }, { "JOY8", "select" },
	{ "JOY9", "1" }, { "JOY10", "3" },
};

typedef struct {
	int		tick, avk;
} scriptKey_t;

typedef struct {
	int		tick;
	char	cmd[128];
} scriptCmd_t;

static struct {
	const oeGameDesc_t	*game;
	vm_t				*vm;
	bar_t				bar;
	byte				*mod;
	int					modSize;

	int					tickMs, fastTickMs;
	int					nextTick;
	int					tick, maxTicks;
	qboolean			vclock, pipe;
	int					vnow;
	qboolean			suspended, pauseSound;

	int					keyCount[AVK_LAST - AVK_FIRST + 1];
	int					heldKey, nextRepeat;

	scriptKey_t			*scriptKeys;
	int					numScriptKeys;
	scriptCmd_t			*scriptCmds;	/* _CMDS "tick:command;...": console lines run at those ticks */
	int					numScriptCmds;
	int					*shotTicks;
	int					numShotTicks;
	char				shotDir[MAX_OSPATH];

	FILE				*capture;		/* "ents PATH": PIPE_WRITE output goes to the file */
	int					pointerUntil;	/* OE_TRAP_POINTER want: the cursor is shown until then */

	qboolean			hw;				/* the game describes its 3D view for the GPU (cl_hw.c) */

	/* the phone screen */
	unsigned short		*rgb565;
	int					screenW, screenH, texW, texH;
	int					screenVersion;

	/* SND_LOAD cache */
	int					sndRes[512], sndHandle[512], numSnd;
} oe;

static cvar_t	*vm_oegame;
static cvar_t	*oe_fast;
static cvar_t	*oe_screen;
static cvar_t	*oe_hd;
static cvar_t	*oe_hudScale;

/* ================================================================ games */

const oeGameDesc_t *OE_GameForDir( const char *dir ) {
	int i;
	for ( i = 0; i < oe_numGames; i++ ) {
		if ( !Q_stricmp( dir, oe_games[i].dir ) )
			return &oe_games[i];
	}
	return NULL;
}

const oeGameDesc_t *OE_CurrentGame( void ) {
	return oe.game;
}

qboolean OE_GameRunning( void ) {
	return oe.vm != NULL;
}

/* the game shows a pointer (OE_TRAP_POINTER want): the mouse is not grabbed */
qboolean OE_PointerWanted( void ) {
	return oe.vm && Sys_Milliseconds() - oe.pointerUntil < 0;
}

/* ================================================================ data */

static const char *OE_Env( const char *name ) {
	if ( !oe.game )
		return NULL;
	return getenv( va( "%s_%s", oe.game->envPrefix, name ) );
}

/* Reads one of the user's files from the game folder (or a pk3 in it) into malloc'd memory and checks it is the
 * supported release. */
static byte *OE_LoadUserFile( const char *name, unsigned wantSize, unsigned wantCrc, int *size ) {
	void *buf;
	byte *data;
	int len = FS_ReadFile( name, &buf );
	unsigned crc;

	if ( ( len < 0 || !buf ) && oe.game && oe.game->dsRom ) {
		/* Orcs & Elves DS: the BREW Orcs & Elves files may stay in its folder */
		const char *paths[2];
		int i;
		paths[0] = Cvar_VariableString( "fs_homepath" );
		paths[1] = Cvar_VariableString( "fs_basepath" );
		for ( i = 0; i < 2 && ( len < 0 || !buf ); i++ ) {
			FILE *f = paths[i][0] ? fopen( FS_BuildOSPath( paths[i], "oe", name ), "rb" ) : NULL;
			if ( !f )
				continue;
			fseek( f, 0, SEEK_END );
			len = (int)ftell( f );
			fseek( f, 0, SEEK_SET );
			buf = len > 0 ? Z_Malloc( len ) : NULL;
			if ( !buf || (int)fread( buf, 1, len, f ) != len ) {
				if ( buf )
					Z_Free( buf );
				buf = NULL;
				len = -1;
			}
			fclose( f );
		}
		if ( len >= 0 && buf ) {
			/* (the zone copy is freed below like an FS buffer) */
			crc = OE_Crc32( buf, len );
			if ( (unsigned)len != wantSize || crc != wantCrc ) {
				Com_Printf( S_COLOR_RED "oe/%s: this version is not supported.\n", name );
				Z_Free( buf );
				return NULL;
			}
			data = malloc( len );
			if ( data )
				Com_Memcpy( data, buf, len );
			Z_Free( buf );
			*size = len;
			return data;
		}
	}
	if ( len < 0 || !buf ) {
		Com_Printf( S_COLOR_RED "%s not found.\n", name );
		return NULL;
	}
	crc = OE_Crc32( buf, len );
	if ( (unsigned)len != wantSize || crc != wantCrc ) {
		Com_Printf( S_COLOR_RED "%s: %d bytes, CRC32 %08x; this version is not supported (it needs %u bytes, CRC32 %08x).\n",
			name, len, crc, wantSize, wantCrc );
		FS_FreeFile( buf );
		return NULL;
	}
	data = malloc( len );
	if ( !data ) {
		FS_FreeFile( buf );
		return NULL;
	}
	Com_Memcpy( data, buf, len );
	FS_FreeFile( buf );
	*size = len;
	return data;
}

/*
 * A .bar data resource. A mod may replace one with the file overrides.txt
 * names for it (oe_res.c; not pictures, those keep their size), or a loose
 * file or pk3 entry "bar/<id>" (raw, not gzipped) or "bar/<original name>".
 * The result is malloc'd; *size is set.
 */
static byte *OE_ResLoad( int resId, int *size ) {
	const barEntry_t *e = Bar_Find( &oe.bar, resId );
	const char *override = OE_ResOverride( resId );
	void *buf;
	byte *data;
	int len;

	len = override ? FS_ReadFile( override, &buf ) : -1;
	if ( override && len < 0 )
		Com_Printf( S_COLOR_YELLOW "overrides.txt: %s not found, resource %d comes from the .bar\n", override, resId );
	if ( len < 0 )
		len = FS_ReadFile( va( "bar/%d", resId ), &buf );
	if ( len < 0 && e && e->name[0] )
		len = FS_ReadFile( va( "bar/%s", e->name ), &buf );
	if ( len >= 0 && buf ) {
		data = malloc( len ? len : 1 );
		if ( data )
			Com_Memcpy( data, buf, len );
		FS_FreeFile( buf );
		*size = len;
		return data;
	}
	if ( !e )
		return NULL;
	len = Bar_UncompressedSize( &oe.bar, e );
	if ( len < 0 )
		return NULL;
	data = malloc( len ? len : 1 );
	if ( !data || !Bar_Read( &oe.bar, e, data, len ) ) {
		free( data );
		return NULL;
	}
	*size = len;
	return data;
}

static int OE_ResSize( int resId ) {
	const barEntry_t *e;
	const char *override = OE_ResOverride( resId );
	int len = override ? FS_ReadFile( override, NULL ) : -1;
	if ( len >= 0 )
		return len;
	len = FS_ReadFile( va( "bar/%d", resId ), NULL );
	if ( len >= 0 )
		return len;
	e = Bar_Find( &oe.bar, resId );
	if ( e && e->name[0] && ( len = FS_ReadFile( va( "bar/%s", e->name ), NULL ) ) >= 0 )
		return len;
	return e ? Bar_UncompressedSize( &oe.bar, e ) : -1;
}

static int OE_SndLoad( int resId ) {
	byte *cmf;
	short *pcm;
	int len, samples, i, h;

	for ( i = 0; i < oe.numSnd; i++ ) {
		if ( oe.sndRes[i] == resId )
			return oe.sndHandle[i];
	}
	cmf = OE_ResLoad( resId, &len );
	if ( !cmf )
		return 0;
	if ( !Pmd_Decode( cmf, len, &pcm, &samples ) ) {
		free( cmf );
		return 0;
	}
	free( cmf );
	h = OE_AudioLoad( pcm, samples );
	if ( h && oe.numSnd < (int)ARRAY_LEN( oe.sndRes ) ) {
		oe.sndRes[oe.numSnd] = resId;
		oe.sndHandle[oe.numSnd] = h;
		oe.numSnd++;
	}
	return h;
}

/* ================================================================ the screen */

/* A copy of the presented frame, RGB565 like the game draws it (the client
 * uploads it as is: at the device resolution of the HD mode there is no time
 * for a per-pixel conversion every tick). */
static void OE_ScreenUpdate( const unsigned short *px, int w, int h, int pitch ) {
	int y;

	if ( w <= 0 || h <= 0 || w > OE_MAX_SCREEN || h > OE_MAX_SCREEN || pitch < w )
		return;
	if ( w != oe.screenW || h != oe.screenH ) {
		free( oe.rgb565 );
		for ( oe.texW = 1; oe.texW < w; oe.texW <<= 1 )
			;
		for ( oe.texH = 1; oe.texH < h; oe.texH <<= 1 )
			;
		oe.rgb565 = calloc( (size_t)w * h, sizeof( unsigned short ) );
		if ( !oe.rgb565 ) {
			oe.screenW = oe.screenH = 0;
			return;
		}
		oe.screenW = w;
		oe.screenH = h;
	}
	if ( pitch == w ) {
		Com_Memcpy( oe.rgb565, px, (size_t)w * h * sizeof( unsigned short ) );
	} else {
		for ( y = 0; y < h; y++ )
			Com_Memcpy( oe.rgb565 + (size_t)y * w, px + (size_t)y * pitch, w * sizeof( unsigned short ) );
	}
	oe.screenVersion++;
}

/* The last frame: w x h RGB565 pixels (w per row), for a texture of texW x texH */
const unsigned short *OE_ScreenImage( int *w, int *h, int *texW, int *texH, int *version ) {
	*w = oe.screenW;
	*h = oe.screenH;
	*texW = oe.texW;
	*texH = oe.texH;
	*version = oe.screenVersion;
	return oe.rgb565;
}

/* PPM of the phone screen, as the ports' OE_SHOT and "shot PATH" wrote */
static void OE_WritePPM( const char *path ) {
	FILE *f;
	int i;
	if ( !oe.rgb565 )
		return;
	f = fopen( path, "wb" );
	if ( !f ) {
		Com_Printf( "Cannot write %s\n", path );
		return;
	}
	fprintf( f, "P6\n%d %d\n255\n", oe.screenW, oe.screenH );
	for ( i = 0; i < oe.screenW * oe.screenH; i++ ) {
		unsigned c = oe.rgb565[i];
		byte rgb[3];
		rgb[0] = (byte)( ( ( c >> 11 ) & 31 ) * 255 / 31 );
		rgb[1] = (byte)( ( ( c >> 5 ) & 63 ) * 255 / 63 );
		rgb[2] = (byte)( ( c & 31 ) * 255 / 31 );
		fwrite( rgb, 1, 3, f );
	}
	fclose( f );
}

static void OE_WriteShot( int tick ) {
	if ( oe.shotDir[0] )
		OE_WritePPM( va( "%s/tick%05d.ppm", oe.shotDir, tick ) );
}

/* ================================================================ system calls */

static qboolean OE_Range( intptr_t addr, int len ) {
	if ( len < 0 || !VM_ValidRange( addr, len ) ) {
		Com_Error( ERR_DROP, "oegame: bad buffer in a system call" );
		return qfalse;
	}
	return qtrue;
}

static int OE_Clock( void ) {
	return oe.vclock ? oe.vnow : Sys_Milliseconds();
}

static int OE_TimeOfDayMs( void ) {
	time_t t;
	const struct tm *lt;
	if ( oe.vclock )
		return (int)( (unsigned)OE_Clock() % 86400000u );
	t = time( NULL );
	lt = localtime( &t );
	if ( !lt )
		return (int)( (unsigned)OE_Clock() % 86400000u );
	return ( lt->tm_hour * 3600 + lt->tm_min * 60 + lt->tm_sec ) * 1000 + (int)( (unsigned)OE_Clock() % 1000u );
}

static intptr_t OE_SystemCalls( intptr_t *args ) {
	switch ( args[0] ) {
	case OE_TRAP_PRINT:
		Com_Printf( "%s", (const char *)VMA( 1 ) );
		return 0;
	case OE_TRAP_ERROR:
		Com_Error( ERR_DROP, "%s", (const char *)VMA( 1 ) );
		return 0;
	case OE_TRAP_MILLISECONDS:
		return OE_Clock();
	case OE_TRAP_VIRTUAL_CLOCK:
		return oe.vclock;
	case OE_TRAP_TIME_OF_DAY_MS:
		return OE_TimeOfDayMs();

	case OE_TRAP_CVAR_VARIABLE_STRING:
		if ( OE_Range( args[2], (int)args[3] ) )
			Cvar_VariableStringBuffer( VMA( 1 ), VMA( 2 ), (int)args[3] );
		return 0;
	case OE_TRAP_CVAR_SET:
		Cvar_Set( VMA( 1 ), VMA( 2 ) );
		return 0;
	case OE_TRAP_ARGC:
		return Cmd_Argc();
	case OE_TRAP_ARGV:
		if ( OE_Range( args[2], (int)args[3] ) )
			Cmd_ArgvBuffer( (int)args[1], VMA( 2 ), (int)args[3] );
		return 0;

	case OE_TRAP_GAMEDATA_SIZE:
		return args[1] == OE_DATA_MOD && oe.mod ? oe.modSize : -1;
	case OE_TRAP_GAMEDATA_READ: {
		int ofs = (int)args[2], len = (int)args[4];
		if ( args[1] != OE_DATA_MOD || !oe.mod || ofs < 0 || len <= 0 || ofs >= oe.modSize )
			return 0;
		if ( len > oe.modSize - ofs )
			len = oe.modSize - ofs;
		if ( !OE_Range( args[3], len ) )
			return 0;
		Com_Memcpy( VMA( 3 ), oe.mod + ofs, len );
		return len;
	}

	case OE_TRAP_RES_SIZE:
		return OE_ResSize( (int)args[1] );
	case OE_TRAP_RES_READ: {
		int len, size = (int)args[3];
		byte *data;
		if ( !OE_Range( args[2], size ) || !( data = OE_ResLoad( (int)args[1], &len ) ) )
			return 0;
		if ( len > size )
			len = size;
		Com_Memcpy( VMA( 2 ), data, len );
		free( data );
		return len;
	}
	case OE_TRAP_RES_STRING:
		if ( !OE_Range( args[2], (int)args[3] ) || args[3] <= 0 )
			return -1;
		return Bar_LoadString( &oe.bar, (int)args[1], VMA( 2 ), (int)args[3] );

	case OE_TRAP_FILE_OPEN:
		return OE_FileOpen( VMA( 1 ), (int)args[2] );
	case OE_TRAP_FILE_READ:
		if ( !OE_Range( args[2], (int)args[3] ) )
			return 0;
		return OE_FileRead( (int)args[1], VMA( 2 ), (int)args[3] );
	case OE_TRAP_FILE_WRITE:
		if ( !OE_Range( args[2], (int)args[3] ) )
			return 0;
		return OE_FileWrite( (int)args[1], VMA( 2 ), (int)args[3] );
	case OE_TRAP_FILE_SEEK:
		return OE_FileSeek( (int)args[1], (int)args[2], (int)args[3] );
	case OE_TRAP_FILE_SIZE:
		return OE_FileSize( (int)args[1] );
	case OE_TRAP_FILE_CLOSE:
		OE_FileClose( (int)args[1] );
		return 0;
	case OE_TRAP_FILE_REMOVE:
		return OE_FileRemove( VMA( 1 ) );
	case OE_TRAP_FILE_EXISTS:
		return OE_FileExists( VMA( 1 ) );
	case OE_TRAP_FILE_LAST_ERROR:
		return OE_FileLastError();

	case OE_TRAP_SCREEN_UPDATE: {
		/* up to a 4K-and-more device screen in the HD mode */
		int w = (int)args[2], h = (int)args[3], pitch = (int)args[4];
		if ( w <= 0 || h <= 0 || pitch < w || w > OE_MAX_SCREEN || h > OE_MAX_SCREEN || pitch > OE_MAX_SCREEN )
			return 0;
		if ( OE_Range( args[1], ( ( h - 1 ) * pitch + w ) * 2 ) ) {
			OE_ScreenUpdate( VMA( 1 ), w, h, pitch );
			if ( oe.hw )
				CL_HwScreen( VMA( 1 ), w, h, pitch );
		}
		return 0;
	}

	case OE_TRAP_HW_TEXTURE: {
		const oeHwTexture_t *d;
		int n;
		if ( !oe.hw || !OE_Range( args[1], sizeof( oeHwTexture_t ) ) )
			return 0;
		d = VMA( 1 );
		if ( d->width <= 0 || d->height <= 0 || d->width > 1024 || d->height > 1024 )
			return 0;
		n = d->width * d->height;
		if ( !OE_Range( args[2], d->format == OE_HWTEX_RGB565 ? n * 2 : n ) )
			return 0;
		if ( d->format == OE_HWTEX_ROWS8 && !OE_Range( args[3], n ) )
			return 0;
		if ( d->format != OE_HWTEX_RGB565 && !OE_Range( args[4], 256 * 2 ) )
			return 0;
		CL_HwTexture( d, VMA( 2 ), d->format == OE_HWTEX_ROWS8 ? VMA( 3 ) : NULL,
			d->format != OE_HWTEX_RGB565 ? VMA( 4 ) : NULL );
		return 0;
	}
	case OE_TRAP_HW_SCENE: {
		const oeHwScene_t *s;
		if ( !oe.hw || !OE_Range( args[1], sizeof( oeHwScene_t ) ) )
			return 0;
		s = VMA( 1 );
		if ( s->numQuads < 0 || s->numQuads > OE_HW_MAX_QUADS || s->rect[0] < 0 || s->rect[1] < 0 ||
			s->rect[2] <= 0 || s->rect[3] <= 0 || s->rect[2] > s->screenPitch || s->screenPitch > OE_MAX_SCREEN ||
			s->rect[3] > OE_MAX_SCREEN ) {
			if ( Cvar_VariableIntegerValue( "oe_dsTrace" ) )
				Com_Printf( "OE_TRAP_HW_SCENE: refused (rect %d %d %d %d pitch %d quads %d)\n", s->rect[0], s->rect[1],
					s->rect[2], s->rect[3], s->screenPitch, s->numQuads );
			return 0;
		}
		if ( !OE_Range( args[2], s->numQuads * (int)sizeof( oeHwQuad_t ) ) || !OE_Range( args[3], 2048 * 2 ) ||
			!OE_Range( args[4], ( ( s->rect[3] - 1 ) * s->screenPitch + s->rect[2] ) * 2 ) )
			return 0;
		CL_HwScene( s, VMA( 2 ), VMA( 3 ), VMA( 4 ) );
		return 0;
	}
	case OE_TRAP_HW_FREEVIEW:
		if ( !oe.hw || !OE_Range( args[1], 4 * sizeof( int ) ) )
			return 0;
		return CL_HwFreeView( VMA( 1 ) );
	case OE_TRAP_FREE_MOVE:
		if ( !OE_Range( args[1], sizeof( oeFreeMove_t ) ) )
			return 0;
		return CL_FreeMove( VMA( 1 ) );
	case OE_TRAP_POINTER: {
		int *out;
		if ( !OE_Range( args[1], 3 * sizeof( int ) ) )
			return 0;
		out = VMA( 1 );
		if ( args[2] )
			oe.pointerUntil = Sys_Milliseconds() + 250;	/* (the game asks on every tick while it wants it) */
		return CL_GamePointer( &out[0], &out[1], &out[2] );
	}
	case OE_TRAP_RES_IMAGE: {
		int *info, maxPixels = (int)args[3];
		if ( !OE_Range( args[4], 3 * sizeof( int ) ) || maxPixels < 0 || ( args[2] && !OE_Range( args[2], maxPixels * 2 ) ) )
			return 0;
		info = VMA( 4 );
		return OE_ResImage( (int)args[1], info[2], args[2] ? VMA( 2 ) : NULL, maxPixels, &info[0], &info[1] );
	}
	case OE_TRAP_EXPORT_FILE: {
		int len = (int)args[3], w = (int)args[4], h = (int)args[5];
		if ( w > 0 && h > 0 ) {
			if ( w > 8192 || h > 8192 )
				return 0;
			len = w * h * 4;
		}
		if ( len < 0 || !OE_Range( args[2], len ) )
			return 0;
		OE_ExportFile( VMA( 1 ), VMA( 2 ), len, w, h );
		return 0;
	}
	case OE_TRAP_DS_FILE: {
		int size = (int)args[3];
		if ( !oe.game || !oe.game->dsRom || size < 0 || ( args[2] && !OE_Range( args[2], size ) ) )
			return -1;
		return CL_DsFile( VMA( 1 ), args[2] ? VMA( 2 ) : NULL, args[2] ? size : 0 );
	}
	case OE_TRAP_DS_MAP:
		return oe.game && oe.game->dsRom && oe.hw ? CL_DsModuleMap( VMA( 1 ) ) : 0;
	case OE_TRAP_DS_SPRITES: {
		int n = (int)args[2];
		if ( !oe.game || !oe.game->dsRom || n < 0 || n > OE_DS_MAX_SPRITES || !OE_Range( args[1], n * (int)sizeof( oeDsSprite_t ) ) )
			return 0;
		CL_DsSprites( VMA( 1 ), n );
		return 0;
	}
	case OE_TRAP_EXEC:
		/* ahead of what is queued, run once the module returns (never inside it) */
		Cbuf_ExecuteText( EXEC_INSERT, va( "%s\n", (const char *)VMA( 1 ) ) );
		return 0;

	case OE_TRAP_SND_LOAD:
		return OE_SndLoad( (int)args[1] );
	case OE_TRAP_SND_PLAY:
		OE_AudioPlay( (int)args[1], (int)args[2], (int)args[3] );
		return 0;
	case OE_TRAP_SND_STOP:
		OE_AudioStop( (int)args[1] );
		return 0;
	case OE_TRAP_SND_IS_PLAYING:
		return OE_AudioIsPlaying( (int)args[1] );
	case OE_TRAP_SND_SET_VOLUME:
		OE_AudioSetVolume( (int)args[1], (int)args[2] );
		return 0;

	case OE_TRAP_VIBRATE:
		return 0;	/* the phone buzzed; nothing to do on a PC */
	case OE_TRAP_SET_WINDOW_TITLE:
		Sys_SetWindowTitle( VMA( 1 ) );
		return 0;
	case OE_TRAP_PIPE_WRITE:
		if ( oe.capture ) {
			fputs( VMA( 1 ), oe.capture );
		} else {
			fputs( VMA( 1 ), stdout );
			fflush( stdout );
		}
		return 0;

	case OE_TRAP_MEMSET:
		if ( OE_Range( args[1], (int)args[3] ) )
			Com_Memset( VMA( 1 ), (int)args[2], (int)args[3] );
		return args[1];
	case OE_TRAP_MEMCPY:
		if ( OE_Range( args[1], (int)args[3] ) && OE_Range( args[2], (int)args[3] ) )
			Com_Memcpy( VMA( 1 ), VMA( 2 ), (int)args[3] );
		return args[1];
	case OE_TRAP_MEMMOVE:
		if ( OE_Range( args[1], (int)args[3] ) && OE_Range( args[2], (int)args[3] ) )
			memmove( VMA( 1 ), VMA( 2 ), (size_t)args[3] );
		return args[1];
	case OE_TRAP_SQRT: {
		floatint_t fi;
		fi.f = (float)sqrt( VMF( 1 ) );
		return fi.i;
	}
	case OE_TRAP_SIN: {
		floatint_t fi;
		fi.f = (float)sin( VMF( 1 ) );
		return fi.i;
	}
	}
	Com_Error( ERR_DROP, "oegame: bad system call %ld", (long)args[0] );
	return 0;
}

/* ================================================================ QA hooks */

static int OE_KeyByName( const char *s ) {
	int i;
	for ( i = 0; i < (int)NUM_PHONE_KEYS; i++ ) {
		if ( !Q_stricmp( s, phoneKeys[i].name ) )
			return phoneKeys[i].avk;
	}
	return (int)strtol( s, NULL, 16 );
}

static void OE_ParseHooks( void ) {
	const char *k = OE_Env( "KEYS" ), *a = OE_Env( "SHOT_AT" ), *s;
	char *buf, *tok;

	oe.maxTicks = OE_Env( "TICKS" ) ? atoi( OE_Env( "TICKS" ) ) : -1;
	oe.pipe = OE_Env( "PIPE" ) != NULL;
	oe.vclock = oe.pipe || OE_Env( "VCLOCK" ) != NULL;
	s = OE_Env( "SHOT" );
	Q_strncpyz( oe.shotDir, s ? s : "", sizeof( oe.shotDir ) );

	if ( k && ( buf = malloc( strlen( k ) + 1 ) ) ) {
		strcpy( buf, k );
		oe.scriptKeys = malloc( sizeof( scriptKey_t ) * ( strlen( k ) / 2 + 1 ) );
		for ( tok = strtok( buf, "," ); tok && oe.scriptKeys; tok = strtok( NULL, "," ) ) {
			char *c = strchr( tok, ':' );
			if ( !c )
				continue;
			*c = 0;
			oe.scriptKeys[oe.numScriptKeys].tick = atoi( tok );
			oe.scriptKeys[oe.numScriptKeys].avk = OE_KeyByName( c + 1 );
			oe.numScriptKeys++;
		}
		free( buf );
	}
	s = OE_Env( "CMDS" );
	if ( s && ( buf = malloc( strlen( s ) + 1 ) ) ) {
		strcpy( buf, s );
		oe.scriptCmds = malloc( sizeof( scriptCmd_t ) * ( strlen( s ) / 2 + 1 ) );
		for ( tok = strtok( buf, ";" ); tok && oe.scriptCmds; tok = strtok( NULL, ";" ) ) {
			char *c = strchr( tok, ':' );
			if ( !c )
				continue;
			*c = 0;
			oe.scriptCmds[oe.numScriptCmds].tick = atoi( tok );
			Q_strncpyz( oe.scriptCmds[oe.numScriptCmds].cmd, c + 1, sizeof( oe.scriptCmds[0].cmd ) );
			oe.numScriptCmds++;
		}
		free( buf );
	}
	if ( a && ( buf = malloc( strlen( a ) + 1 ) ) ) {
		strcpy( buf, a );
		oe.shotTicks = malloc( sizeof( int ) * ( strlen( a ) / 2 + 1 ) );
		for ( tok = strtok( buf, "," ); tok && oe.shotTicks; tok = strtok( NULL, "," ) )
			oe.shotTicks[oe.numShotTicks++] = atoi( tok );
		free( buf );
	}

	/* what the game code read from the environment, for the QVM (its getenv reads cvars) */
	if ( OE_Env( "SEED" ) )
		Cvar_Set( "oe_seed", OE_Env( "SEED" ) );
	if ( OE_Env( "KEYLOG" ) )
		Cvar_Set( va( "%s_KEYLOG", oe.game->envPrefix ), OE_Env( "KEYLOG" ) );
}

static void OE_SendEvent( int evt, int wParam, int dwParam ) {
	if ( oe.vm )
		VM_Call( oe.vm, OE_GAME_EVENT, evt, wParam, dwParam );
}

/* runs a console line as a game command (the developer and QA commands live in the module) */
static qboolean OE_RunGameCommand( const char *line ) {
	Cmd_TokenizeString( line );
	return oe.vm && VM_Call( oe.vm, OE_GAME_CONSOLE_COMMAND );
}

/*
 * OE_PIPE: before every tick, one status line on stdout (the game prints it),
 * then one command line from stdin is run, then the tick runs. Commands:
 * empty = just tick; phone key names separated by spaces; "shot PATH";
 * "ents PATH" (the game's entity list into PATH); "god", "noclip", "tp X Y",
 * "raise C V" (game commands); "quit" or EOF ends the run.
 */
static qboolean OE_PipeTick( int tick ) {
	char line[4096], *tok;

	OE_RunGameCommand( va( "pipestatus %d", tick ) );
#ifndef _WIN32
	{
		/* the tty console made stdin non-blocking; the protocol waits for each line */
		int fl = fcntl( fileno( stdin ), F_GETFL, 0 );
		if ( fl != -1 && ( fl & O_NONBLOCK ) )
			fcntl( fileno( stdin ), F_SETFL, fl & ~O_NONBLOCK );
	}
#endif
	if ( !fgets( line, sizeof( line ), stdin ) )
		return qfalse;
	line[strcspn( line, "\r\n" )] = 0;
	if ( !strcmp( line, "quit" ) )
		return qfalse;
	if ( !strncmp( line, "shot ", 5 ) ) {
		OE_WritePPM( line + 5 );
	} else if ( !strncmp( line, "ents ", 5 ) ) {
		oe.capture = fopen( line + 5, "w" );
		if ( oe.capture ) {
			OE_RunGameCommand( "ents" );
			fclose( oe.capture );
			oe.capture = NULL;
		}
	} else if ( !strcmp( line, "god" ) || !strcmp( line, "noclip" ) || !strncmp( line, "tp ", 3 ) ||
		!strncmp( line, "raise ", 6 ) ) {
		OE_RunGameCommand( line );
	} else {
		for ( tok = strtok( line, " " ); tok; tok = strtok( NULL, " " ) )
			OE_SendEvent( EVT_KEY, OE_KeyByName( tok ), 0 );
	}
	return qtrue;
}

/* ================================================================ keys */

void OE_PhoneKey( int avk, qboolean down ) {
	int *count;
	if ( !oe.vm || avk < AVK_FIRST || avk > AVK_LAST )
		return;
	count = &oe.keyCount[avk - AVK_FIRST];
	if ( down ) {
		if ( ( *count )++ > 0 )
			return;		/* already held through another PC key */
		if ( oe.suspended )
			return;		/* the shell delivered no keys while the applet was suspended */
		OE_SendEvent( EVT_KEY, avk, 0 );
		oe.heldKey = avk;
		oe.nextRepeat = Sys_Milliseconds() + KEY_REPEAT_START;
	} else {
		if ( *count > 0 )
			( *count )--;
		if ( *count == 0 && avk == oe.heldKey )
			oe.heldKey = 0;
	}
}

static int OE_PhoneKeyArg( void ) {
	const char *name = Cmd_Argv( 0 ) + 1;	/* "+phone_up" */
	int i;
	if ( Q_stricmpn( name, "phone_", 6 ) )
		return 0;
	name += 6;
	for ( i = 0; i < (int)NUM_PHONE_KEYS; i++ ) {
		if ( !Q_stricmp( name, phoneKeys[i].name ) )
			return phoneKeys[i].avk;
	}
	return 0;
}

/* "oemap N", "devoemap N": a level of the game's own .bar (the module checks and loads it), shown by the
   game again instead of a Quake 3 map (map, devmap) */
static void OE_Oemap_f( void ) {
	if ( !oe.vm ) {
		Com_Printf( "%s: no game is running\n", Cmd_Argv( 0 ) );
		return;
	}
	CL_HwLeaveQ3();
	VM_Call( oe.vm, OE_GAME_CONSOLE_COMMAND );
}

static void OE_PhoneKeyDown_f( void ) { OE_PhoneKey( OE_PhoneKeyArg(), qtrue ); }
static void OE_PhoneKeyUp_f( void ) { OE_PhoneKey( OE_PhoneKeyArg(), qfalse ); }

/* "phonekey NAME": one press, as a script would do it */
static void OE_PhoneKeyPress_f( void ) {
	if ( Cmd_Argc() != 2 ) {
		Com_Printf( "usage: phonekey <up|down|left|right|select|soft1|soft2|clr|star|pound|0..9>\n" );
		return;
	}
	OE_SendEvent( EVT_KEY, OE_KeyByName( Cmd_Argv( 1 ) ), 0 );
}

void OE_SetMinimized( qboolean minimized ) {
	if ( !oe.vm || minimized == oe.suspended )
		return;
	/* A minimised window is the PC's version of the phone interrupting the app (a call, the flip closed): the shell
	 * sent EVT_APP_SUSPEND, delivered no keys while suspended, then EVT_APP_RESUME. */
	oe.suspended = minimized;
	oe.heldKey = 0;
	OE_SendEvent( minimized ? EVT_APP_SUSPEND : EVT_APP_RESUME, 0, 0 );
	if ( oe.pauseSound )
		OE_AudioPause( minimized );
}

static void OE_DefaultBinds( void ) {
	int i;
	for ( i = 0; i < (int)ARRAY_LEN( defaultBinds ); i++ ) {
		int key = Key_StringToKeynum( (char *)defaultBinds[i].key );
		const char *b;
		if ( key < 0 )
			continue;
		b = Key_GetBinding( key );
		if ( !b || !b[0] )
			Key_SetBinding( key, va( "+phone_%s", defaultBinds[i].phone ) );
	}
	/* configs from before the spell key: Q was soft1 (Escape still is) */
	if ( Cvar_VariableIntegerValue( "oe_bindsVersion" ) < 1 ) {
		int q = Key_StringToKeynum( "q" );
		const char *b = q >= 0 ? Key_GetBinding( q ) : NULL;
		if ( b && !Q_stricmp( b, "+phone_soft1" ) )
			Key_SetBinding( q, "+phone_spell" );
		Cvar_Set( "oe_bindsVersion", "1" );
	}
}


/* write current JOY* binds to HOMEPATH/base/gamepad_custom.cfg */
static void OE_GamepadSave_f( void ) {
	fileHandle_t f;
	const char *home = Cvar_VariableString( "fs_homepath" );
	char path[MAX_OSPATH];
	FILE *fp;

	/* prefer explicit path under base so it is independent of the current fs_game */
	Com_sprintf( path, sizeof( path ), "%s/base/gamepad_custom.cfg", home );
	fp = fopen( path, "w" );
	if ( fp ) {
		fprintf( fp, "// custom gamepad layout (JOY binds)\n" );
		{
			int i;
			for ( i = K_JOY1 ; i <= K_JOY32 ; i++ ) {
				const char *b = Key_GetBinding( i );
				if ( b && b[0] )
					fprintf( fp, "bind %s \"%s\"\n", Key_KeynumToString( i ), b );
			}
		}
		fclose( fp );
		Com_Printf( "Gamepad layout saved to %s\n", path );
		return;
	}
	/* fallback: engine write path */
	f = FS_FOpenFileWrite( "gamepad_custom.cfg" );
	if ( !f ) {
		Com_Printf( "gamepad_save: cannot write gamepad_custom.cfg\n" );
		return;
	}
	FS_Printf( f, "// custom gamepad layout (JOY binds)\n" );
	Key_WriteGamepadBindings( f );
	FS_FCloseFile( f );
	Com_Printf( "Gamepad layout saved to gamepad_custom.cfg\n" );
}

/*
 * Simple remap entry point (called from Options menu).
 * Shows current JOY bindings and saves the layout. Full interactive
 * assign UI (press a button to bind a chosen action) can be extended;
 * for now rebind via console then gamepad_save, or use the defaults.
 */
static void OE_GamepadRemap_f( void ) {
	int i;
	Com_Printf( "\n=== Gamepad remap ===\n" );
	Com_Printf( "Current JOY bindings:\n" );
	for ( i = K_JOY1 ; i <= K_JOY16 ; i++ ) {
		const char *b = Key_GetBinding( i );
		if ( b && b[0] )
			Com_Printf( "  %s -> %s\n", Key_KeynumToString( i ), b );
	}
	Com_Printf( "To change: bind JOY1 \"+phone_select\"   (then gamepad_save)\n" );
	Com_Printf( "Actions: up down left right select soft1 soft2 clr star pound 0-9\n" );
	Com_Printf( "Layout is saved to HOMEPATH/base/gamepad_custom.cfg\n" );
	Com_Printf( "====================\n" );
	OE_GamepadSave_f();
}

/* ================================================================ lifecycle */

static void OE_Game_f( void );

void OE_Init( void ) {
	int i;
	vm_oegame = Cvar_Get( "vm_oegame", "0", CVAR_ARCHIVE );	// 0: the DLL if present, else the QVM; 1/2: the QVM
	oe_fast = Cvar_Get( "oe_fast", "0", CVAR_ARCHIVE );
	oe_screen = Cvar_Get( "oe_screen", "", CVAR_ARCHIVE | CVAR_LATCH );	// "": fit the window, "phone": the game's own, or WxH
	oe_hd = Cvar_Get( "oe_hd", "1", CVAR_ARCHIVE | CVAR_LATCH );	// draw at the window's resolution (DLL module only)
	oe_hudScale = Cvar_Get( "oe_hudScale", "0", CVAR_ARCHIVE );	// window pixels per game pixel, 0: about the phone's height (read at start)
	Cvar_Get( "oe_softkeyBar", "1", CVAR_ARCHIVE );	// the games' bottom soft-key bar (read when a game lays out its screen)
	Cvar_Get( "oe_seed", "", CVAR_TEMP );
	Cvar_Get( "oe_bindsVersion", "0", CVAR_ARCHIVE );
	for ( i = 0; i < (int)NUM_PHONE_KEYS; i++ ) {
		Cmd_AddCommand( va( "+phone_%s", phoneKeys[i].name ), OE_PhoneKeyDown_f );
		Cmd_AddCommand( va( "-phone_%s", phoneKeys[i].name ), OE_PhoneKeyUp_f );
	}
	Cmd_AddCommand( "phonekey", OE_PhoneKeyPress_f );
	Cmd_AddCommand( "oe_export", OE_Export_f );
	Cmd_AddCommand( "oemap", OE_Oemap_f );
	Cmd_AddCommand( "devoemap", OE_Oemap_f );
	Cmd_AddCommand( "defaultbinds", OE_DefaultBinds );
	Cmd_AddCommand( "gamepad_save", OE_GamepadSave_f );
	Cmd_AddCommand( "gamepad_remap", OE_GamepadRemap_f );
	Cmd_AddCommand( "game", OE_Game_f );
	// 1 only when oe.exe started without fs_game: the base folder's menus (vm/ui.qvm) are up
	Cvar_Get( "vm_use_idtech3_ui", "0", CVAR_ROM );
	OE_AudioInit();
}

void OE_Shutdown( void ) {
	OE_StopGame();
	OE_AudioShutdown();
}

/*
 * The game's screen for the window. In: the phone screen the game asks for
 * (176x220). Out: the screen size the game lays out, and the device pixels
 * per game pixel (the HD mode) it draws at.
 *
 * oe_screen "" fits the window: a landscape screen about as tall as the
 * phone's, scaled by a whole number to fill the window exactly (1920x1080:
 * 384x216 times 5). "phone" keeps the game's own screen, "WxH" sets one (the
 * ports' --screen). The game lays out screens from 128x128 to 640x640.
 *
 * Only a DLL module draws at the device resolution (oe_hd 1): the bytecode
 * interpreter is far too slow for it, so a QVM draws the game screen and the
 * window magnifies it. Without a window (oe-headless) the screen is the
 * phone's, as the QA traces expect.
 */
static int OE_ScreenLayout( int *w, int *h ) {
	int winW, winH, scale = 1, sw, sh;

	CL_WindowSize( &winW, &winH );
	if ( !Q_stricmp( oe_screen->string, "phone" ) ) {
		/* the game's own screen */
	} else if ( oe_screen->string[0] ) {
		if ( sscanf( oe_screen->string, "%dx%d", &sw, &sh ) == 2 && sw >= 128 && sh >= 128 && sw <= 640 && sh <= 640 ) {
			*w = sw;
			*h = sh;
		} else {
			Com_Printf( "oe_screen \"%s\" ignored: use \"\", \"phone\" or WxH between 128x128 and 640x640\n",
				oe_screen->string );
		}
	} else if ( winW > 0 && winH > 0 ) {
		if ( oe_hudScale->integer > 0 )
			scale = oe_hudScale->integer;	/* the HUD scale chosen in the options */
		else
			scale = ( winH + *h / 2 ) / *h;	/* nearest whole multiple of the phone's height */
		if ( scale < 1 )
			scale = 1;
		while ( winW / scale > 640 || winH / scale > 640 )
			scale++;
		while ( scale > 1 && ( winW / scale < 128 || winH / scale < 128 ) )
			scale--;
		*w = ( winW / scale ) & ~1;	/* the games keep the width even */
		*h = winH / scale;
		if ( *w < 128 || *h < 128 ) {
			/* a tiny window: the phone screen, magnified */
			*w = VM_Call( oe.vm, OE_GAME_INFO, OE_INFO_SCREEN_WIDTH );
			*h = VM_Call( oe.vm, OE_GAME_INFO, OE_INFO_SCREEN_HEIGHT );
			scale = 1;
		}
	}
	if ( winW <= 0 || winH <= 0 || !oe_hd->integer || !VM_IsNative( oe.vm ) )
		return 1;
	/* manual sizes: as large as fits */
	if ( scale == 1 ) {
		scale = winW / *w < winH / *h ? winW / *w : winH / *h;
		if ( scale < 1 )
			scale = 1;
	}
	return scale;
}

static qboolean OE_StartModule( void );

qboolean OE_StartGame( void ) {
	const char *dir = Cvar_VariableString( "fs_game" );
	const oeGameDesc_t *game;

	if ( oe.vm )
		return qtrue;
	Com_Memset( &oe, 0, sizeof( oe ) );
	game = oe.game = OE_GameForDir( dir );
	if ( !oe.game ) {
		Com_Printf( S_COLOR_RED "fs_game is \"%s\": set it to oe (Orcs & Elves), oe2 (Orcs & Elves II) or oeds (Orcs & Elves DS).\n", dir );
		return qfalse;
	}
	if ( game->dsRom && CL_DsFile( "shaders.bin", NULL, 0 ) < 0 ) {
		return qfalse;	/* (no cartridge: CL_DsFile said why) */
	}
	/* (Orcs & Elves DS: +set oe_dsViewer 1 explores the cartridge's maps instead of the game) */
	if ( !( game->dsRom && Cvar_VariableIntegerValue( "oe_dsViewer" ) ) && OE_StartModule() )
		return qtrue;
	if ( game->dsRom ) {
		/* without its module (or the Orcs & Elves .bar / .mod it needs): the cartridge's maps */
		OE_StopGame();
		oe.game = game;
		Com_Printf( S_COLOR_YELLOW "%s: the game module did not start; the maps of the cartridge can be explored.\n", game->title );
		Sys_SetWindowTitle( game->title );
		return CL_DsStart();
	}
	return qfalse;
}

/* the module of the current game (oe.game), with the user's .bar and .mod */
static qboolean OE_StartModule( void ) {
	int w, h, r, scale;

	{
		int size;
		byte *bar = OE_LoadUserFile( oe.game->barName, oe.game->barSize, oe.game->barCrc, &size );
		if ( !bar || !Bar_Open( &oe.bar, bar, size ) ) {
			OE_StopGame();
			return qfalse;
		}
		oe.mod = OE_LoadUserFile( oe.game->modName, oe.game->modSize, oe.game->modCrc, &oe.modSize );
		if ( !oe.mod ) {
			OE_StopGame();
			return qfalse;
		}
	}

	OE_FilesInit( oe.game->dir );
	OE_ParseHooks();
	OE_DefaultBinds();
	/* custom gamepad layout (written by the remap UI) overrides defaults */
	Cbuf_AddText( "exec gamepad_custom.cfg\n" );

	oe.vm = VM_Create( OE_GAME_MODULE, OE_SystemCalls, (vmInterpret_t)vm_oegame->integer );
	if ( !oe.vm ) {
		Com_Printf( S_COLOR_RED "Cannot load the game module (vm/%s.qvm in %s).\n", OE_GAME_MODULE, oe.game->dir );
		OE_StopGame();
		return qfalse;
	}
	oe.tickMs = VM_Call( oe.vm, OE_GAME_INFO, OE_INFO_TICK_MS );
	oe.fastTickMs = VM_Call( oe.vm, OE_GAME_INFO, OE_INFO_FAST_TICK_MS );
	oe.pauseSound = VM_Call( oe.vm, OE_GAME_INFO, OE_INFO_PAUSE_SOUND_ON_SUSPEND );
	if ( oe.tickMs <= 0 )
		oe.tickMs = 15;
	if ( oe.fastTickMs <= 0 )
		oe.fastTickMs = oe.tickMs;
	OE_ResInit( &oe.bar, !VM_IsNative( oe.vm ) );	/* a mod's overrides (oe_res.c) */
	w = VM_Call( oe.vm, OE_GAME_INFO, OE_INFO_SCREEN_WIDTH );
	h = VM_Call( oe.vm, OE_GAME_INFO, OE_INFO_SCREEN_HEIGHT );
	scale = OE_ScreenLayout( &w, &h );
	/* hardware rendering: the GPU draws the 3D view at the window's resolution, the
	 * game's own drawing (its 2D) stays at the game's (QVM or DLL alike) */
	oe.hw = CL_HwAvailable();
	CL_HwReset();
	if ( oe.hw && !OE_ResWantsHd() )
		scale = 1;	/* (with a mod's HD pictures the screen stays at the window's size; the
					 * software view then only does its game work, the GPU draws it) */

	Sys_SetWindowTitle( oe.game->title );
	oe.vnow = 1000;
	r = VM_Call( oe.vm, OE_GAME_INIT, OE_GAME_API_VERSION, w, h, scale | ( oe.hw ? OE_SCREEN_HW : 0 ) );
	if ( r < 0 ) {
		Com_Printf( S_COLOR_RED "%s could not start.\n", oe.game->title );
		OE_StopGame();
		return qfalse;
	}
	oe.nextTick = Sys_Milliseconds();
	if ( oe.hw ) {
		Com_Printf( "%s: %dx%d, 3D view on the GPU, %d ms timer\n", oe.game->title, w, h, oe.tickMs );
	} else if ( scale > 1 ) {
		Com_Printf( "%s: %dx%d drawn at %dx%d, %d ms timer\n", oe.game->title, w, h, w * scale, h * scale, oe.tickMs );
	} else {
		Com_Printf( "%s: %dx%d, %d ms timer\n", oe.game->title, w, h, oe.tickMs );
	}
	return qtrue;
}

/* also used after a game error (ERR_DROP); the module is then not asked to shut down */
static void OE_FreeGame( qboolean callShutdown ) {
	if ( oe.vm ) {
		if ( callShutdown )
			VM_Call( oe.vm, OE_GAME_SHUTDOWN );
		VM_Free( oe.vm );
		oe.vm = NULL;
	}
	OE_AudioFreeAll();
	OE_FilesShutdown();
	OE_ResShutdown();
	if ( oe.hw )
		CL_HwReset();
	oe.hw = qfalse;
	Bar_Close( &oe.bar );
	free( oe.mod );
	oe.mod = NULL;
	free( oe.scriptKeys );
	free( oe.scriptCmds );
	free( oe.shotTicks );
	oe.scriptKeys = NULL;
	oe.scriptCmds = NULL;
	oe.shotTicks = NULL;
	oe.numScriptKeys = oe.numScriptCmds = oe.numShotTicks = 0;
	if ( oe.capture )
		fclose( oe.capture );
	oe.capture = NULL;
	oe.numSnd = 0;
}

void OE_StopGame( void ) {
	OE_FreeGame( qtrue );
}

void OE_GameError( void ) {
	OE_FreeGame( qfalse );
}

/* ms until the next tick is due (0: now); the main loop sleeps at most this long */
int OE_MsToNextTick( void ) {
	int d;
	if ( !oe.vm || oe.vclock )
		return 0;
	d = oe.nextTick - Sys_Milliseconds();
	if ( oe.heldKey ) {
		int r = oe.nextRepeat - Sys_Milliseconds();
		if ( r < d )
			d = r;
	}
	return d > 0 ? d : 0;
}

void OE_Frame( void ) {
	int i, quit;

	if ( !oe.vm )
		return;

	if ( oe.heldKey && Sys_Milliseconds() - oe.nextRepeat >= 0 ) {
		/* one repeat per due time; after a stall (map load, save) the shell did not send a burst */
		OE_SendEvent( EVT_KEY, oe.heldKey, KB_AUTOREPEAT );
		oe.nextRepeat += KEY_REPEAT_RATE;
		if ( Sys_Milliseconds() - oe.nextRepeat >= 0 )
			oe.nextRepeat = Sys_Milliseconds() + KEY_REPEAT_RATE;
	}
	if ( !oe.vm || ( !oe.vclock && Sys_Milliseconds() - oe.nextTick < 0 ) )
		return;

	if ( oe.vclock )
		oe.vnow = 1000 + oe.tick * oe.tickMs;
	if ( oe.pipe && !OE_PipeTick( oe.tick ) ) {
		Cbuf_AddText( "quit\n" );
		oe.pipe = qfalse;
		oe.maxTicks = oe.tick;
		return;
	}
	for ( i = 0; i < oe.numScriptKeys; i++ ) {
		if ( oe.scriptKeys[i].tick == oe.tick )
			OE_SendEvent( EVT_KEY, oe.scriptKeys[i].avk, 0 );
	}
	for ( i = 0; i < oe.numScriptCmds; i++ ) {
		if ( oe.scriptCmds[i].tick == oe.tick )
			Cbuf_AddText( va( "%s\n", oe.scriptCmds[i].cmd ) );
	}
	quit = VM_Call( oe.vm, OE_GAME_TICK );
	/* the tick re-armed the applet's timer at its end (ISHELL_SetTimer); the deferred screen update happened inside
	 * that period, so presenting must not delay the next tick */
	oe.nextTick = Sys_Milliseconds() + ( oe_fast->integer ? oe.fastTickMs : oe.tickMs );
	for ( i = 0; i < oe.numShotTicks; i++ ) {
		if ( oe.shotTicks[i] == oe.tick )
			OE_WriteShot( oe.tick );
	}
	if ( ++oe.tick == oe.maxTicks ) {
		OE_WriteShot( oe.tick );
		quit = 1;
	}
	if ( quit ) {
		Com_Printf( "oegame: the applet closed itself (tick %d)\n", oe.tick );
		/* the applet closed itself (Exit in the main menu): the program ends with it */
		oe.maxTicks = oe.tick;
		Cbuf_AddText( "quit\n" );
	}
}

qboolean OE_GameCommand( void ) {
	return oe.vm && VM_Call( oe.vm, OE_GAME_CONSOLE_COMMAND );
}

qboolean OE_PipeMode( void ) {
	return oe.pipe;
}

/* ================================================================ choosing the game */

/* why game g did not start, for the user */
static void OE_WhyNotStarted( const oeGameDesc_t *g, const char *dir, char *buf, int size ) {
	if ( g && g->dsRom ) {
		Com_sprintf( buf, size, "%s could not start.\n\nPut the dump of your Orcs & Elves DS cartridge, named %s, into\n%s\n(see the console log for details).",
			g->title, g->dsRom, FS_BuildOSPath( Cvar_VariableString( "fs_basepath" ), g->dir, "" ) );
	} else if ( g ) {
		Com_sprintf( buf, size, "%s could not start.\n\nPut your copy of the BREW game, %s and %s, into\n%s\n(see the console log for details).",
			g->title, g->barName, g->modName, FS_BuildOSPath( Cvar_VariableString( "fs_basepath" ), g->dir, "" ) );
	} else {
		Com_sprintf( buf, size, "fs_game \"%s\" is not a supported game: use +set fs_game oe, oe2 or oeds", dir );
	}
}

/* oe.exe started with fs_game: its game, or a fatal error saying what is missing */
void OE_StartOrDie( void ) {
	char why[MAX_STRING_CHARS];
	const char *dir = Cvar_VariableString( "fs_game" );

	if ( OE_StartGame() ) {
		return;
	}
	OE_WhyNotStarted( OE_GameForDir( dir ), dir, why, sizeof( why ) );
	Com_Error( ERR_FATAL, "%s", why );
}

/* the base folder's menus (vm_use_idtech3_ui 1), or the game question without them */
static void OE_ShowMenus( void ) {
	const char *names[8];
	int i, n, choice;

	Cvar_Set2( "vm_use_idtech3_ui", "1", qtrue );
	if ( CL_UIStart() ) {
		return;
	}
	if ( Cvar_VariableString( "ui_gameError" )[0] ) {
		Com_Error( ERR_FATAL, "%s", Cvar_VariableString( "ui_gameError" ) );
	}
	// no module: ask with a dialog (Sys_ChooseGame)
	n = oe_numGames < 8 ? oe_numGames : 8;
	for ( i = 0; i < n; i++ ) {
		names[i] = oe_games[i].title;
	}
	choice = Sys_ChooseGame( names, n );
	if ( choice < 0 ) {
		Sys_Quit();
	}
	OE_SwitchGame( oe_games[choice < n ? choice : 0].dir );
}

/* oe.exe started without fs_game: the menus of the base folder pick the game */
void OE_StartMenus( void ) {
	Cvar_Get( "ui_gameError", "", CVAR_TEMP );
	OE_ShowMenus();
}

/* the search path and the config for fs_game dir ("": the base folder of the menus) */
static void OE_UseFolder( const char *dir ) {
	Com_WriteConfiguration();
	Cvar_Set2( "fs_game", dir, qtrue );
	FS_SwitchGame();
	Com_ExecuteCfg();
	FS_CheckUsePk3();
}

qboolean OE_SwitchGame( const char *dir ) {
	const oeGameDesc_t *g = OE_GameForDir( dir );
	char why[MAX_STRING_CHARS];

	if ( !g ) {
		OE_WhyNotStarted( NULL, dir, why, sizeof( why ) );
		Com_Printf( S_COLOR_YELLOW "%s\n", why );
		Cvar_Set( "ui_gameError", why );
		return qfalse;
	}
	if ( oe.vm && oe.game == g ) {
		return qtrue;
	}
	CL_UIStop();
	OE_StopGame();
	Cvar_Set2( "vm_use_idtech3_ui", "0", qtrue );	// a game: the menus' module is not used
	OE_UseFolder( g->dir );
	Cvar_Set( "ui_gameError", "" );
	Hunk_SetMark();
	if ( OE_StartGame() ) {
		return qtrue;
	}
	// back to the menus, which say why
	OE_WhyNotStarted( g, dir, why, sizeof( why ) );
	OE_StopGame();
	Hunk_ClearToMark();
	OE_UseFolder( "" );
	Cvar_Set( "ui_gameError", why );
	OE_ShowMenus();
	return qfalse;
}

/* game <dir>: leaves the menus (or the game) for that game */
static void OE_Game_f( void ) {
	int i;
	if ( Cmd_Argc() < 2 ) {
		const char *cur = Cvar_VariableString( "fs_game" );
		Com_Printf( "usage: game <dir>   (now: %s)\n", cur[0] ? cur : BASEGAME " (the menus)" );
		for ( i = 0; i < oe_numGames; i++ ) {
			Com_Printf( "  %-5s %s\n", oe_games[i].dir, oe_games[i].title );
		}
		return;
	}
	OE_SwitchGame( Cmd_Argv( 1 ) );
}

/* games whose .bar (Orcs & Elves DS: cartridge dump) is present in the game folder of the current search path */
int OE_InstalledGames( const oeGameDesc_t **list, int max ) {
	int i, n = 0;
	for ( i = 0; i < oe_numGames && n < max; i++ ) {
		const char *base = Cvar_VariableString( "fs_basepath" ), *home = Cvar_VariableString( "fs_homepath" );
		const char *file = oe_games[i].dsRom ? oe_games[i].dsRom : oe_games[i].barName;
		FILE *f = NULL;
		if ( home[0] )
			f = fopen( FS_BuildOSPath( home, oe_games[i].dir, file ), "rb" );
		if ( !f && base[0] )
			f = fopen( FS_BuildOSPath( base, oe_games[i].dir, file ), "rb" );
		if ( f ) {
			fclose( f );
			list[n++] = &oe_games[i];
		}
	}
	return n;
}
