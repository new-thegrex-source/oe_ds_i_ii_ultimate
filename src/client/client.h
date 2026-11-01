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
// client.h -- the oe-client: a window that shows the phone screen of the
// game module (scaled with OpenGL), the console, and the keys.
// Network, cgame, ui, sound, cinematics and demos of Tremulous are gone.

#ifndef CLIENT_H
#define CLIENT_H

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../renderer/tr_public.h"
#include "keys.h"
#include "../oe/oe_local.h"

// the console font is scaled with the window: 8x16 cells at 480 lines
#undef SMALLCHAR_WIDTH
#undef SMALLCHAR_HEIGHT
#define SMALLCHAR_WIDTH_BASE	8
#define SMALLCHAR_HEIGHT_BASE	16
#define SMALLCHAR_WIDTH		( cls.charWidth )
#define SMALLCHAR_HEIGHT	( cls.charHeight )

// connstate_t (q_shared.h): CA_DISCONNECTED = no game, only the console; CA_ACTIVE = the game runs

typedef struct {
	connstate_t	state;

	int			realtime;			// ignores pause
	int			realFrametime;		// ignoring pause, so console always works
	int			frametime;

	qboolean	rendererStarted;
	glconfig_t	glconfig;

	qhandle_t	whiteShader;
	qhandle_t	charSetShader;
	qhandle_t	screenShader;		// the phone screen (OE_ScreenImage)
	int			screenVersion;		// last uploaded
	int			lastDrawnVersion;
	int			lastDrawnWidth, lastDrawnHeight;
	int			lastDrawTime;
	qboolean	forceRedraw;

	int			charWidth, charHeight;

	char		stopReason[MAX_STRING_CHARS];	// why the game stopped (shown with the console)
} clientStatic_t;

extern	clientStatic_t		cls;
extern	refexport_t			re;		// interface to refresh

//
// cvars
//
extern	cvar_t	*cl_noprint;
extern	cvar_t	*cl_consoleKeys;
extern	cvar_t	*cl_screenScale;	// 0 fit (keep aspect), 1 integer steps, 2 stretch
extern	cvar_t	*cl_screenFilter;	// 0 nearest, 1 linear
extern	cvar_t	*con_scale;			// 0 auto

extern int g_console_field_width;

//
// cl_main
//
void		CL_Init( void );
void		CL_Shutdown( void );
void		CL_Frame( int msec );
void		CL_StartHunkUsers( qboolean rendererOnly );
void		CL_Vid_Restart_f( void );
void		CL_ShutdownRef( void );
void		CL_InitRef( void );
qboolean	CL_GameVisible( void );
void		CL_GameStopped( const char *reason );

//
// cl_keys
//
void		CL_InitKeyCommands( void );
void		CL_KeyEvent( int key, qboolean down, unsigned time );
void		CL_CharEvent( int key );
void		CL_MouseEvent( int dx, int dy, int time );
void		CL_JoystickEvent( int axis, int value, int time );
int			Key_StringToKeynum( char *str );
char		*Key_KeynumToString( int keynum );
int			Key_GetCatcher( void );
void		Key_SetCatcher( int catcher );
void		Key_KeynameCompletion( void(*callback)(const char *s) );

//
// console
//
void		Con_CheckResize( void );
void		Con_Init( void );
void		Con_Clear_f( void );
void		Con_ToggleConsole_f( void );
void		Con_ClearNotify( void );
void		Con_RunConsole( void );
void		Con_DrawConsole( void );
void		Con_PageUp( void );
void		Con_PageDown( void );
void		Con_Top( void );
void		Con_Bottom( void );
void		Con_Close( void );
void		CL_ConsolePrint( char *txt );
void		CL_LoadConsoleHistory( void );
void		CL_SaveConsoleHistory( void );

//
// cl_scrn
//
void		SCR_Init( void );
void		SCR_UpdateScreen( void );
qboolean	SCR_NeedsRedraw( void );
void		SCR_FillPixelRect( float x, float y, float w, float h, const float *color );
void		SCR_DrawBigString( int x, int y, const char *s, float alpha, qboolean noColorEscape );
void		SCR_DrawSmallStringExt( int x, int y, const char *string, float *setColor, qboolean forceColor, qboolean noColorEscape );
void		SCR_DrawSmallChar( int x, int y, int ch );

//
// cl_hw: the games' 3D view on the GPU, r_testbsp
//
void		CL_HwInit( void );
void		CL_HwRendererStarted( void );
void		CL_HwVidRestart( void );
qboolean	CL_HwDraw( float x, float y, float dw, float dh );
qboolean	CL_HwWorldViewActive( void );	// oe_idtech3freecam: a Quake 3 map over the whole window
void		CL_HwDsView( const vec3_t origin, float yaw, const char *map );	// the free camera over a DS map
qboolean	CL_HwDsViewActive( void );
void		CL_HwDrawWorldView( void );
qboolean	CL_HwWantsMouse( void );		// the free camera looks with the mouse
void		CL_HwFreecamMouse( int dx, int dy );
qboolean	CL_FreePlayActive( void );		// exploring with free movement (oe_moveMode)
qboolean	CL_FreePlayKey( int key, qboolean down );

//
// cl_dsmap: Orcs & Elves DS maps (rom.nds) on the GPU
//
void		CL_DsInit( void );
void		CL_DsRendererStarted( void );
void		CL_DsAddScene( vec3_t viewAxis[3] );
qboolean	CL_DsWalking( void );					// oe_dsWalk: on the floor, against the walls
qboolean	CL_DsModuleMapActive( void );			// the game module's 3D view shows a DS map
void		CL_DsWalk( vec3_t origin, const vec3_t move );

//
// cl_ui: the menus without fs_game (also see oe_local.h)
//
void		CL_UIRefresh( void );
qboolean	CL_UIKeyEvent( int key, qboolean down );	// qtrue if the menus took it
qboolean	CL_UICharEvent( int ch );
void		CL_UIMousePosition( int x, int y );			// drawable pixels

//
// sdl / sys
//
void		IN_Init( void );
void		IN_Frame( void );
void		IN_PointerState( int *x, int *y, int *buttons );	// the mouse in window pixels, buttons 1 left 2 right
void		IN_Shutdown( void );
void		IN_Restart( void );

#endif
