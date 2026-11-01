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
// cl_scrn.c -- the window's contents: the phone screen, scaled up with
// OpenGL, and the console over it. All coordinates here are pixels.
//
// A frame is drawn only when something changed (the game presented a new
// screen, the console moves or is open, the window changed), so the GPU
// idles between the game's ticks; the swap never waits for vsync by default
// (r_swapInterval 0) so it cannot delay the game's timer.

#include "client.h"
#include "cl_font.h"

static qboolean	scr_initialized;

/*
================
SCR_Init

Creates the client's images; called after every renderer start.
================
*/
void SCR_Init( void ) {
	static byte		white[8 * 8 * 4];
	byte			*font;
	int				i;

	Com_Memset( white, 255, sizeof( white ) );
	cls.whiteShader = re.UpdateRawImage( "*oe_white", white, 8, 8, qtrue );

	font = Z_Malloc( 256 * 256 * 4 );
	for ( i = 0; i < 256 * 256; i++ ) {
		int a = cl_consoleFont4bit[i >> 1];
		a = ( i & 1 ) ? ( a & 15 ) : ( a >> 4 );
		font[i * 4 + 0] = 255;
		font[i * 4 + 1] = 255;
		font[i * 4 + 2] = 255;
		font[i * 4 + 3] = (byte)( a * 17 );
	}
	cls.charSetShader = re.UpdateRawImage( "*oe_consolefont", font, 256, 256, qfalse );
	Z_Free( font );

	scr_initialized = qtrue;
}

/*
================
SCR_FillPixelRect
================
*/
void SCR_FillPixelRect( float x, float y, float w, float h, const float *color ) {
	re.SetColor( color );
	re.DrawStretchPic( x, y, w, h, 0, 0, 0, 0, cls.whiteShader );
	re.SetColor( NULL );
}

/*
================
SCR_DrawSmallChar

small chars are drawn at native screen resolution
================
*/
void SCR_DrawSmallChar( int x, int y, int ch ) {
	int row, col;
	float frow, fcol;
	float size;

	ch &= 255;

	if ( ch == ' ' ) {
		return;
	}

	if ( y < -SMALLCHAR_HEIGHT ) {
		return;
	}

	row = ch >> 4;
	col = ch & 15;

	frow = row * 0.0625;
	fcol = col * 0.0625;
	size = 0.0625;

	re.DrawStretchPic( x, y, SMALLCHAR_WIDTH, SMALLCHAR_HEIGHT,
					   fcol, frow,
					   fcol + size, frow + size,
					   cls.charSetShader );
}

/*
==================
SCR_DrawSmallStringExt

Draws a multi-colored string with a drop shadow, optionally forcing
to a fixed color.
==================
*/
void SCR_DrawSmallStringExt( int x, int y, const char *string, float *setColor, qboolean forceColor,
		qboolean noColorEscape ) {
	vec4_t		color;
	const char	*s;
	int			xx;

	// draw the colored text
	s = string;
	xx = x;
	re.SetColor( setColor );
	while ( *s ) {
		if ( Q_IsColorString( s ) ) {
			if ( !forceColor ) {
				Com_Memcpy( color, g_color_table[ColorIndex(*(s+1))], sizeof( color ) );
				color[3] = setColor[3];
				re.SetColor( color );
			}
			if ( !noColorEscape ) {
				s += 2;
				continue;
			}
		}
		SCR_DrawSmallChar( xx, y, *s );
		xx += SMALLCHAR_WIDTH;
		s++;
	}
	re.SetColor( NULL );
}

void SCR_DrawBigString( int x, int y, const char *s, float alpha, qboolean noColorEscape ) {
	float	color[4];

	color[0] = color[1] = color[2] = 1.0;
	color[3] = alpha;
	SCR_DrawSmallStringExt( x, y, s, color, qfalse, noColorEscape );
}

/*
================
SCR_UploadScreen

Brings the phone screen texture up to date with the game's last frame.
================
*/
static void SCR_UploadScreen( void ) {
	int w, h, texW, texH, version;
	const unsigned short *rgb565 = OE_ScreenImage( &w, &h, &texW, &texH, &version );

	if ( !rgb565 || ( version == cls.screenVersion && cls.screenShader &&
		 !cl_screenFilter->modified ) ) {
		return;
	}
	cls.screenShader = re.UpdateRawImage565( "*oe_screen", rgb565, w, h, texW, texH, !cl_screenFilter->integer );
	cls.screenVersion = version;
	cl_screenFilter->modified = qfalse;
}

/* where the phone screen (w x h) goes in the window: it keeps its aspect, as large as fits
 * (cl_screenScale 0), whole multiples only (1, sharp pixels), or the whole window (2) */
static void SCR_PhoneRect( int w, int h, float *x, float *y, float *dw, float *dh ) {
	int		vw = cls.glconfig.vidWidth, vh = cls.glconfig.vidHeight;
	float	scale;

	if ( cl_screenScale->integer == 2 ) {
		*dw = vw;
		*dh = vh;
	} else {
		scale = (float)vw / w < (float)vh / h ? (float)vw / w : (float)vh / h;
		if ( cl_screenScale->integer == 1 && scale >= 1.0f ) {
			scale = (float)(int)scale;
		}
		*dw = w * scale;
		*dh = h * scale;
	}
	*x = (float)(int)( ( vw - *dw ) * 0.5f );
	*y = (float)(int)( ( vh - *dh ) * 0.5f );
}

/* OE_TRAP_POINTER: the mouse over the phone screen, in its pixels */
qboolean CL_GamePointer( int *px, int *py, int *buttons ) {
	int		w, h, texW, texH, version, mx, my;
	float	x, y, dw, dh;

	*px = *py = *buttons = 0;
	if ( !cls.rendererStarted || !OE_ScreenImage( &w, &h, &texW, &texH, &version ) || w <= 0 || h <= 0 ) {
		return qfalse;
	}
	SCR_PhoneRect( w, h, &x, &y, &dw, &dh );
	IN_PointerState( &mx, &my, buttons );
	if ( mx < x || my < y || mx >= x + dw || my >= y + dh ) {
		return qfalse;
	}
	*px = (int)( ( mx - x ) * w / dw );
	*py = (int)( ( my - y ) * h / dh );
	return qtrue;
}
/*
================
SCR_DrawPhoneScreen

The phone screen keeps its aspect: as large as fits (cl_screenScale 0),
whole multiples only (1, sharp pixels), or the whole window (2).
================
*/
static void SCR_DrawPhoneScreen( void ) {
	static const float black[4] = { 0, 0, 0, 1 };
	int w, h, texW, texH, version;
	float dw, dh, x, y;
	int vw = cls.glconfig.vidWidth, vh = cls.glconfig.vidHeight;

	SCR_FillPixelRect( 0, 0, vw, vh, black );

	if ( CL_HwWorldViewActive() ) {
		CL_HwDrawWorldView();
		return;
	}
	if ( !CL_GameVisible() || !OE_ScreenImage( &w, &h, &texW, &texH, &version ) || w <= 0 || h <= 0 ) {
		return;
	}

	SCR_PhoneRect( w, h, &x, &y, &dw, &dh );
	if ( CL_HwDraw( x, y, dw, dh ) ) {
		return;	// the 3D view on the GPU with the game's 2D on top
	}
	SCR_UploadScreen();
	if ( !cls.screenShader ) {
		return;
	}
	re.DrawStretchPic( x, y, dw, dh, 0, 0, (float)w / texW, (float)h / texH, cls.screenShader );
}

/*
================
SCR_NeedsRedraw
================
*/
qboolean SCR_NeedsRedraw( void ) {
	int w, h, texW, texH, version;
	int fps = com_maxfps->integer > 0 ? com_maxfps->integer : 60;

	if ( !scr_initialized || !cls.rendererStarted || com_minimized->integer ) {
		return qfalse;
	}
	if ( cls.forceRedraw || cls.glconfig.vidWidth != cls.lastDrawnWidth || cls.glconfig.vidHeight != cls.lastDrawnHeight ) {
		return qtrue;
	}
	if ( CL_HwWorldViewActive() || CL_UIRunning() ) {
		return cls.realtime - cls.lastDrawTime >= 1000 / fps;	// the camera flies, the menus animate
	}
	if ( OE_ScreenImage( &w, &h, &texW, &texH, &version ) && version != cls.lastDrawnVersion ) {
		return qtrue;	// a new frame from the game
	}
	if ( cl_screenFilter->modified || cl_screenScale->modified ) {
		return qtrue;
	}
	// the console (and its cursor) animates; otherwise refresh now and then for
	// window systems that lose the contents
	if ( ( Key_GetCatcher() & KEYCATCH_CONSOLE ) || !CL_GameVisible() ) {
		return cls.realtime - cls.lastDrawTime >= 1000 / fps;
	}
	return cls.realtime - cls.lastDrawTime >= 250;
}

/*
==================
SCR_UpdateScreen
==================
*/
void SCR_UpdateScreen( void ) {
	static int	recursive;
	int w, h, texW, texH, version;
	int scale;

	if ( !scr_initialized ) {
		return;
	}
	if ( ++recursive > 2 ) {
		Com_Error( ERR_FATAL, "SCR_UpdateScreen: recursively called" );
	}
	recursive = 1;

	// console font size follows the window height
	scale = con_scale->integer > 0 ? con_scale->integer : cls.glconfig.vidHeight / 480;
	if ( scale < 1 ) {
		scale = 1;
	}
	if ( scale > 8 ) {
		scale = 8;
	}
	if ( cls.charWidth != SMALLCHAR_WIDTH_BASE * scale ) {
		cls.charWidth = SMALLCHAR_WIDTH_BASE * scale;
		cls.charHeight = SMALLCHAR_HEIGHT_BASE * scale;
		Con_CheckResize();
	}

	re.BeginFrame( STEREO_CENTER );
	SCR_DrawPhoneScreen();
	CL_UIRefresh();
	Con_DrawConsole();
	re.EndFrame( NULL, NULL );

	if ( OE_ScreenImage( &w, &h, &texW, &texH, &version ) ) {
		cls.lastDrawnVersion = version;
	}
	cls.lastDrawnWidth = cls.glconfig.vidWidth;
	cls.lastDrawnHeight = cls.glconfig.vidHeight;
	cls.lastDrawTime = cls.realtime;
	cls.forceRedraw = qfalse;
	cl_screenScale->modified = qfalse;

	recursive = 0;
}
