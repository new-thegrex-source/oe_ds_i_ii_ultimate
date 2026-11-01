/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2000-2009 Darklegion Development

This file is part of the Orcs & Elves client, based on Tremulous.

It is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.
===========================================================================
*/
// cl_main.c -- client main loop: the renderer, the window and the console.
// The game itself runs in the game module (src/oe/oe_host.c); the client
// only shows the phone screen it produces.

#include "client.h"

clientStatic_t	cls;
refexport_t		re;

cvar_t	*cl_noprint;
cvar_t	*cl_consoleKeys;
cvar_t	*cl_screenScale;
cvar_t	*cl_screenFilter;
cvar_t	*con_scale;

/*
=======================================================================

RENDERER

The renderer's hunk allocations come from the C heap here, so that
vid_restart can free them: the game module sits on the engine's hunk
and must not move.

=======================================================================
*/

typedef struct refBlock_s {
	struct refBlock_s	*next;
} refBlock_t;

static refBlock_t	*refBlocks;

static void *CL_RefHunkAlloc( int size, ha_pref pref ) {
	refBlock_t *b;
	(void)pref;
	b = calloc( 1, sizeof( refBlock_t ) + 32 + size );
	if ( !b ) {
		Com_Error( ERR_FATAL, "CL_RefHunkAlloc: out of memory (%d bytes)", size );
	}
	b->next = refBlocks;
	refBlocks = b;
	return (byte *)b + 32;	// keep the alignment the hunk gave
}

#ifdef HUNK_DEBUG
static void *CL_RefHunkAllocDebug( int size, ha_pref pref, char *label, char *file, int line ) {
	(void)label; (void)file; (void)line;
	return CL_RefHunkAlloc( size, pref );
}
#endif

static void CL_RefFreeHunk( void ) {
	while ( refBlocks ) {
		refBlock_t *next = refBlocks->next;
		free( refBlocks );
		refBlocks = next;
	}
}

static void QDECL CL_RefPrintf( int print_level, const char *fmt, ... ) {
	va_list		argptr;
	char		msg[MAXPRINTMSG];

	va_start( argptr, fmt );
	Q_vsnprintf( msg, sizeof( msg ), fmt, argptr );
	va_end( argptr );

	if ( print_level == PRINT_ALL ) {
		Com_Printf( "%s", msg );
	} else if ( print_level == PRINT_WARNING ) {
		Com_Printf( S_COLOR_YELLOW "%s", msg );
	} else if ( print_level == PRINT_DEVELOPER ) {
		Com_DPrintf( S_COLOR_RED "%s", msg );
	}
}

static void *CL_RefMalloc( int size ) {
	return Z_TagMalloc( size, TAG_RENDERER );
}

static void CL_NoDebugSurface( void (*drawPoly)(int color, int numPoints, float *points) ) {
	(void)drawPoly;
}

static void CL_NoCinUpload( int handle ) { (void)handle; }
static int CL_NoCinPlay( const char *arg0, int xpos, int ypos, int width, int height, int bits ) {
	(void)arg0; (void)xpos; (void)ypos; (void)width; (void)height; (void)bits;
	return -1;
}
static e_status CL_NoCinRun( int handle ) { (void)handle; return FMV_EOF; }
static void CL_NoAVIFrame( const byte *buffer, int size ) { (void)buffer; (void)size; }

void CL_InitRef( void ) {
	refimport_t	ri;
	refexport_t	*ret;

	Com_Printf( "----- Initializing Renderer ----\n" );

	Com_Memset( &ri, 0, sizeof( ri ) );
	ri.Cmd_AddCommand = Cmd_AddCommand;
	ri.Cmd_RemoveCommand = Cmd_RemoveCommand;
	ri.Cmd_Argc = Cmd_Argc;
	ri.Cmd_Argv = Cmd_Argv;
	ri.Cmd_ExecuteText = Cbuf_ExecuteText;
	ri.Printf = CL_RefPrintf;
	ri.Error = Com_Error;
	ri.Milliseconds = Sys_Milliseconds;
	ri.Malloc = CL_RefMalloc;
	ri.Free = Z_Free;
#ifdef HUNK_DEBUG
	ri.Hunk_AllocDebug = CL_RefHunkAllocDebug;
#else
	ri.Hunk_Alloc = CL_RefHunkAlloc;
#endif
	ri.Hunk_AllocateTempMemory = Hunk_AllocateTempMemory;
	ri.Hunk_FreeTempMemory = Hunk_FreeTempMemory;
	ri.CM_DrawDebugSurface = CL_NoDebugSurface;
	ri.FS_ReadFile = FS_ReadFile;
	ri.FS_FreeFile = FS_FreeFile;
	ri.FS_WriteFile = FS_WriteFile;
	ri.FS_FreeFileList = FS_FreeFileList;
	ri.FS_ListFiles = FS_ListFiles;
	ri.FS_FileIsInPAK = FS_FileIsInPAK;
	ri.FS_FileExists = FS_FileExists;
	ri.Cvar_Get = Cvar_Get;
	ri.Cvar_Set = Cvar_Set;
	ri.Cvar_CheckRange = Cvar_CheckRange;
	ri.CIN_UploadCinematic = CL_NoCinUpload;
	ri.CIN_PlayCinematic = CL_NoCinPlay;
	ri.CIN_RunCinematic = CL_NoCinRun;
	ri.CL_WriteAVIVideoFrame = CL_NoAVIFrame;

	ret = GetRefAPI( REF_API_VERSION, &ri );

	Com_Printf( "-------------------------------\n" );

	if ( !ret ) {
		Com_Error( ERR_FATAL, "Couldn't initialize refresh" );
	}

	re = *ret;
}

/*
============
CL_ShutdownRef
============
*/
void CL_ShutdownRef( void ) {
	if ( !re.Shutdown ) {
		return;
	}
	re.Shutdown( qtrue );
	Com_Memset( &re, 0, sizeof( re ) );
	CL_RefFreeHunk();
	cls.rendererStarted = qfalse;
}

/*
============
CL_InitRenderer

Opens the window (BeginRegistration) and creates the client's images: the
console font, a white texture for fills, and the phone screen (uploaded by
SCR_UpdateScreen whenever the game draws).
============
*/
static void CL_InitRenderer( void ) {
	re.BeginRegistration( &cls.glconfig );
	CL_HwRendererStarted();
	SCR_Init();
	cls.screenShader = 0;
	cls.screenVersion = -1;
	cls.forceRedraw = qtrue;
	Con_CheckResize();
}

void CL_StartHunkUsers( qboolean rendererOnly ) {
	(void)rendererOnly;
	if ( !cls.rendererStarted ) {
		cls.rendererStarted = qtrue;
		CL_InitRenderer();
	}
}

/*
=================
CL_Vid_Restart_f

Restart the video subsystem (r_mode, r_fullscreen, ... changed)
=================
*/
void CL_Vid_Restart_f( void ) {
	// the menus register their pictures with the renderer: they start again with it
	qboolean ui = CL_UIRunning();
	CL_UIStop();
	CL_HwVidRestart();
	CL_ShutdownRef();
	CL_InitRef();
	CL_StartHunkUsers( qtrue );
	if ( ui ) {
		CL_UIStart();
	}
	// the window lost its keys
	Key_ClearStates();
}

/*
=======================================================================

GAME STATE

=======================================================================
*/

void CL_WindowSize( int *width, int *height ) {
	*width = cls.rendererStarted ? cls.glconfig.vidWidth : 0;
	*height = cls.rendererStarted ? cls.glconfig.vidHeight : 0;
}

/* a game module runs, or an Orcs & Elves DS map is shown (fs_game oeds has no module) */
static qboolean CL_GamePresent( void ) {
	return OE_GameRunning() || CL_HwDsViewActive();
}

qboolean CL_GameVisible( void ) {
	return cls.state == CA_ACTIVE && CL_GamePresent();
}

/* the game module stopped with an error: show the console and why */
void CL_GameStopped( const char *reason ) {
	Q_strncpyz( cls.stopReason, reason, sizeof( cls.stopReason ) );
	cls.state = CA_DISCONNECTED;
	Key_SetCatcher( KEYCATCH_CONSOLE );
	cls.forceRedraw = qtrue;
}

/*
==================
CL_Frame
==================
*/
void CL_Frame( int msec ) {
	if ( !com_cl_running || !com_cl_running->integer ) {
		return;
	}

	cls.realFrametime = msec;
	cls.frametime = msec;
	cls.realtime += msec;

	if ( CL_GamePresent() ) {
		cls.state = CA_ACTIVE;
	} else if ( cls.state == CA_ACTIVE ) {
		cls.state = CA_DISCONNECTED;
	}
	// no game: the console, unless the menus are up
	if ( cls.state != CA_ACTIVE && !CL_UIRunning() && !( Key_GetCatcher() & KEYCATCH_CONSOLE ) ) {
		Key_SetCatcher( KEYCATCH_CONSOLE );
	}

	Con_RunConsole();

	if ( SCR_NeedsRedraw() ) {
		SCR_UpdateScreen();
	}
}

/*
=======================================================================

INIT / SHUTDOWN

=======================================================================
*/

void CL_MouseEvent( int dx, int dy, int time ) {
	(void)time;
	CL_HwFreecamMouse( dx, dy );	// the phone has no pointer: only the free camera uses the mouse
}

void CL_JoystickEvent( int axis, int value, int time ) {
	(void)axis; (void)value; (void)time;
}

static void CL_Clientinfo_f( void ) {
	const oeGameDesc_t *g = OE_CurrentGame();
	Com_Printf( "%s\n", OE_VERSION );
	Com_Printf( "game: %s\n", g ? g->title : "none" );
	Com_Printf( "window: %dx%d\n", cls.glconfig.vidWidth, cls.glconfig.vidHeight );
	if ( cls.stopReason[0] ) {
		Com_Printf( "stopped: %s\n", cls.stopReason );
	}
}

void CL_Init( void ) {
	Com_Printf( "----- Client Initialization -----\n" );

	Con_Init();

	Com_Memset( &cls, 0, sizeof( cls ) );
	cls.state = CA_DISCONNECTED;
	cls.charWidth = SMALLCHAR_WIDTH_BASE;
	cls.charHeight = SMALLCHAR_HEIGHT_BASE;

	cl_noprint = Cvar_Get( "cl_noprint", "0", 0 );
	cl_consoleKeys = Cvar_Get( "cl_consoleKeys", "~ ` 0x7e 0x60", CVAR_ARCHIVE );
	cl_screenScale = Cvar_Get( "cl_screenScale", "0", CVAR_ARCHIVE );
	cl_screenFilter = Cvar_Get( "cl_screenFilter", "0", CVAR_ARCHIVE );
	con_scale = Cvar_Get( "con_scale", "0", CVAR_ARCHIVE );

	// full screen at the desktop resolution (r_width/r_height 0), the game laid
	// out for it (oe_screen, oe_hd); registered here first so that these are
	// the renderer's defaults
	Cvar_Get( "r_mode", "-1", CVAR_ARCHIVE | CVAR_LATCH );
	Cvar_Get( "r_width", "0", CVAR_ARCHIVE | CVAR_LATCH );
	Cvar_Get( "r_height", "0", CVAR_ARCHIVE | CVAR_LATCH );
	Cvar_Get( "r_allowResize", "1", CVAR_ARCHIVE );
	Cvar_Get( "r_swapInterval", "0", CVAR_ARCHIVE );	// a vsync wait must not delay the game's timer
	Cvar_Get( "r_fullscreen", "1", CVAR_ARCHIVE | CVAR_LATCH );
	{
		// configs saved before: the 704x880 phone window was the default then
		cvar_t *version = Cvar_Get( "cl_videoDefaults", "0", CVAR_ARCHIVE );
		if ( version->integer < 1 ) {
			if ( Cvar_VariableIntegerValue( "r_width" ) == 704 && Cvar_VariableIntegerValue( "r_height" ) == 880 ) {
				Cvar_Set( "r_width", "0" );
				Cvar_Set( "r_height", "0" );
				Cvar_Set( "r_fullscreen", "1" );
			}
			Cvar_Set( "cl_videoDefaults", "1" );
		}
	}

	Cmd_AddCommand( "vid_restart", CL_Vid_Restart_f );
	Cmd_AddCommand( "clientinfo", CL_Clientinfo_f );
	CL_HwInit();	// before the renderer: it sets renderer limits

	CL_InitRef();
	CL_StartHunkUsers( qfalse );

	CL_LoadConsoleHistory();

	Cvar_Set( "cl_running", "1" );

	Com_Printf( "----- Client Initialization Complete -----\n" );
}

void CL_Shutdown( void ) {
	static qboolean recursive = qfalse;

	if ( !( com_cl_running && com_cl_running->integer ) ) {
		return;
	}

	Com_Printf( "----- CL_Shutdown -----\n" );

	if ( recursive ) {
		Com_Printf( "WARNING: Recursive shutdown\n" );
		return;
	}
	recursive = qtrue;

	CL_UIStop();
	Com_WriteConfiguration();
	CL_ShutdownRef();

	Cmd_RemoveCommand( "vid_restart" );
	Cmd_RemoveCommand( "clientinfo" );

	Cvar_Set( "cl_running", "0" );

	recursive = qfalse;

	Com_Memset( &cls, 0, sizeof( cls ) );
	Key_SetCatcher( 0 );

	Com_Printf( "-----------------------\n" );
}
