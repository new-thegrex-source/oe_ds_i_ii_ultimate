/*
===========================================================================
This file is part of the Orcs & Elves client, based on Tremulous.

It is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.
===========================================================================
*/
// cl_ui.c -- the user interface module (ui), Tremulous' way: oe.exe started
// without fs_game runs in the base folder and shows the menus of base/ui/*.menu
// drawn by base/vm/ui.qvm (or uix86_64.dll). The menus pick a game; "game <dir>"
// (oe_host.c) then stops the module and starts that game.

#include "client.h"
#include "../ui/ui_public.h"

static vm_t		*uivm;
static int		uiCalls;				// VM_Calls into the module not yet returned (an error leaves them)
static int		uiMouseX = -1, uiMouseY = -1;
static cvar_t	*vm_ui;

/*
====================
CL_UISystemCalls
====================
*/
static qboolean CL_UIRange( intptr_t addr, int len ) {
	if ( len < 0 || !VM_ValidRange( addr, len ) ) {
		Com_Error( ERR_DROP, "ui: bad buffer in a system call" );
		return qfalse;
	}
	return qtrue;
}

static intptr_t CL_UISystemCalls( intptr_t *args ) {
	switch ( args[0] ) {
	case UI_PRINT:
		Com_Printf( "%s", (const char *)VMA( 1 ) );
		return 0;
	case UI_ERROR:
		Com_Error( ERR_DROP, "ui: %s", (const char *)VMA( 1 ) );
		return 0;
	case UI_MILLISECONDS:
		return Sys_Milliseconds();
	case UI_CVAR_SET:
		Cvar_Set( VMA( 1 ), VMA( 2 ) );
		return 0;
	case UI_CVAR_VARIABLESTRINGBUFFER:
		if ( CL_UIRange( args[2], (int)args[3] ) )
			Cvar_VariableStringBuffer( VMA( 1 ), VMA( 2 ), (int)args[3] );
		return 0;
	case UI_ARGC:
		return Cmd_Argc();
	case UI_ARGV:
		if ( CL_UIRange( args[2], (int)args[3] ) )
			Cmd_ArgvBuffer( (int)args[1], VMA( 2 ), (int)args[3] );
		return 0;
	case UI_CMD_EXECUTETEXT:
		Cbuf_AddText( VMA( 1 ) );
		return 0;
	case UI_FS_READFILE: {
		void	*data;
		int		len, size = (int)args[3];
		if ( size <= 0 || !CL_UIRange( args[2], size ) )
			return -1;
		len = FS_ReadFile( VMA( 1 ), &data );
		if ( len < 0 || !data )
			return -1;
		Com_Memcpy( VMA( 2 ), data, len < size ? len : size - 1 );
		( (char *)VMA( 2 ) )[len < size ? len : size - 1] = 0;
		FS_FreeFile( data );
		return len;
	}
	case UI_R_REGISTERSHADERNOMIP:
		return cls.rendererStarted ? re.RegisterShaderNoMip( VMA( 1 ) ) : 0;
	case UI_R_SETCOLOR:
		re.SetColor( args[1] ? VMA( 1 ) : NULL );
		return 0;
	case UI_R_DRAWSTRETCHPIC:
		re.DrawStretchPic( VMF( 1 ), VMF( 2 ), VMF( 3 ), VMF( 4 ), VMF( 5 ), VMF( 6 ), VMF( 7 ), VMF( 8 ), (qhandle_t)args[9] );
		return 0;
	case UI_R_CONSOLEFONT:
		return cls.charSetShader;
	case UI_KEY_GETCATCHER:
		return Key_GetCatcher();
	case UI_KEY_SETCATCHER:
		// the console stays as it is
		Key_SetCatcher( ( (int)args[1] & ~KEYCATCH_CONSOLE ) | ( Key_GetCatcher() & KEYCATCH_CONSOLE ) );
		return 0;
	case UI_R_WHITE:
		return cls.whiteShader;
	case UI_R_REGISTERFONT: {
		fontInfo_t fi;
		if ( !CL_UIRange( args[3], (int)sizeof( fontInfo_t ) ) )
			return 0;
		Com_Memset( &fi, 0, sizeof( fi ) );
		if ( cls.rendererStarted )
			re.RegisterFont( VMA( 1 ), (int)args[2], &fi );
		Com_Memcpy( VMA( 3 ), &fi, sizeof( fi ) );
		return 0;
	}

	case UI_MEMSET:
		if ( CL_UIRange( args[1], (int)args[3] ) )
			Com_Memset( VMA( 1 ), (int)args[2], (int)args[3] );
		return args[1];
	case UI_MEMCPY:
		if ( CL_UIRange( args[1], (int)args[3] ) && CL_UIRange( args[2], (int)args[3] ) )
			Com_Memcpy( VMA( 1 ), VMA( 2 ), (int)args[3] );
		return args[1];
	case UI_MEMMOVE:
		if ( CL_UIRange( args[1], (int)args[3] ) && CL_UIRange( args[2], (int)args[3] ) )
			memmove( VMA( 1 ), VMA( 2 ), (size_t)args[3] );
		return args[1];
	case UI_SQRT: {
		floatint_t fi;
		fi.f = (float)sqrt( VMF( 1 ) );
		return fi.i;
	}
	case UI_SIN: {
		floatint_t fi;
		fi.f = (float)sin( VMF( 1 ) );
		return fi.i;
	}
	}
	Com_Error( ERR_DROP, "ui: bad system call %ld", (long)args[0] );
	return 0;
}

static intptr_t CL_UICall( int command, int a0, int a1, int a2 ) {
	intptr_t r;
	uiCalls++;
	r = VM_Call( uivm, command, a0, a1, a2 );
	uiCalls--;
	return r;
}

/*
====================
CL_UIStart

Loads the module and opens its main menu; qfalse if there is none
====================
*/
qboolean CL_UIStart( void ) {
	int i;

	if ( uivm ) {
		return qtrue;
	}
	if ( !cls.rendererStarted ) {
		return qfalse;
	}
	vm_ui = Cvar_Get( "vm_ui", "0", CVAR_ARCHIVE );	// 0: the DLL if present, else the QVM; 1/2: the QVM
	for ( i = 0; i < oe_numGames; i++ ) {
		const oeGameDesc_t *list[16];
		int k, n = OE_InstalledGames( list, ARRAY_LEN( list ) ), installed = 0;
		for ( k = 0; k < n; k++ ) {
			if ( list[k] == &oe_games[i] )
				installed = 1;
		}
		Cvar_Get( va( "ui_installed_%s", oe_games[i].dir ), "0", CVAR_ROM );
		Cvar_Set2( va( "ui_installed_%s", oe_games[i].dir ), installed ? "1" : "0", qtrue );
	}
	Cvar_Get( "ui_gameError", "", CVAR_TEMP );

	// the module's memory is given back when it stops (the game's module is loaded after it)
	Hunk_SetMark();
	uivm = VM_Create( "ui", CL_UISystemCalls, (vmInterpret_t)vm_ui->integer );
	if ( !uivm ) {
		Hunk_ClearToMark();
		Com_Printf( S_COLOR_YELLOW "No user interface module (vm/ui.qvm or ui" ARCH_STRING DLL_EXT " in " BASEGAME ").\n" );
		return qfalse;
	}
	if ( CL_UICall( UI_INIT, UI_API_VERSION, cls.glconfig.vidWidth, cls.glconfig.vidHeight ) < 0 ) {
		Com_Printf( S_COLOR_RED "The user interface module did not start.\n" );
		CL_UIStop();
		return qfalse;
	}
	Sys_SetWindowTitle( "Orcs & Elves" );
	Key_SetCatcher( KEYCATCH_UI );
	uiMouseX = uiMouseY = -1;
	cls.forceRedraw = qtrue;
	return qtrue;
}

/*
====================
CL_UIStop
====================
*/
void CL_UIStop( void ) {
	if ( !uivm ) {
		return;
	}
	CL_UICall( UI_SHUTDOWN, 0, 0, 0 );
	VM_Free( uivm );
	uivm = NULL;
	Hunk_ClearToMark();
	Key_SetCatcher( Key_GetCatcher() & ~KEYCATCH_UI );
	cls.forceRedraw = qtrue;
}

qboolean CL_UIRunning( void ) {
	return uivm != NULL;
}

/* after an error (ERR_DROP) in the module: drop it without calling it again */
void CL_UIError( void ) {
	if ( uivm && uiCalls ) {
		uiCalls = 0;
		VM_Forced_Unload_Start();
		VM_Free( uivm );
		VM_Forced_Unload_Done();
		uivm = NULL;
		Hunk_ClearToMark();
		Key_SetCatcher( KEYCATCH_CONSOLE );
	}
}

/*
====================
CL_UIRefresh / events
====================
*/
void CL_UIRefresh( void ) {
	if ( uivm ) {
		CL_UICall( UI_REFRESH, cls.realtime, cls.glconfig.vidWidth, cls.glconfig.vidHeight );
	}
}

qboolean CL_UIKeyEvent( int key, qboolean down ) {
	if ( !uivm || !( Key_GetCatcher() & KEYCATCH_UI ) || ( Key_GetCatcher() & KEYCATCH_CONSOLE ) ) {
		return qfalse;
	}
	CL_UICall( UI_KEY_EVENT, key, down, 0 );
	cls.forceRedraw = qtrue;
	return qtrue;
}

qboolean CL_UICharEvent( int ch ) {
	if ( !uivm || !( Key_GetCatcher() & KEYCATCH_UI ) || ( Key_GetCatcher() & KEYCATCH_CONSOLE ) ) {
		return qfalse;
	}
	CL_UICall( UI_CHAR_EVENT, ch, 0, 0 );
	cls.forceRedraw = qtrue;
	return qtrue;
}

/* the pointer in window pixels (sdl_input.c, while the mouse is not grabbed) */
void CL_UIMousePosition( int x, int y ) {
	if ( !uivm || ( x == uiMouseX && y == uiMouseY ) ) {
		return;
	}
	uiMouseX = x;
	uiMouseY = y;
	CL_UICall( UI_MOUSE_POSITION, x, y, 0 );
	cls.forceRedraw = qtrue;
}

/* an unknown console command: the module's ("ui_" commands, menu scripts) */
qboolean CL_UICommand( void ) {
	return uivm && CL_UICall( UI_CONSOLE_COMMAND, 0, 0, 0 );
}
