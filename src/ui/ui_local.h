/*
 * The user interface module (ui): the menus of oe.exe started without
 * fs_game, read from base/ui/main.menu. A small relative of Tremulous' ui:
 * the same menu files (menuDef / itemDef, #include, #define, enum), the
 * same scripts (open, close, exec, setcvar, show, hide), drawn with the
 * engine's font.
 */
#ifndef UI_LOCAL_H
#define UI_LOCAL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#include "ui_public.h"
#include "../client/keycodes.h"

typedef int qboolean;
#define qfalse	0
#define qtrue	1

#define KEYCATCH_UI		0x0002	/* (q_shared.h) */

/* font support (from q_shared.h) for RegisterFont / glyph drawing */
#define MAX_QPATH 64
#define GLYPH_START 0
#define GLYPH_END 255
#define GLYPHS_PER_FONT (GLYPH_END - GLYPH_START + 1)
typedef int qhandle_t;
typedef struct {
  int height;
  int top;
  int bottom;
  int pitch;
  int xSkip;
  int imageWidth;
  int imageHeight;
  float s, t, s2, t2;
  qhandle_t glyph;
  char shaderName[32];
} glyphInfo_t;

typedef struct {
  glyphInfo_t glyphs[GLYPHS_PER_FONT];
  float glyphScale;
  char name[MAX_QPATH];
} fontInfo_t;

/* ---------------------------------------------------------------- system calls (ui_syscalls.c / .asm) */

void	trap_Print( const char *text );
void	trap_Error( const char *text );
int		trap_Milliseconds( void );
void	trap_Cvar_Set( const char *name, const char *value );
void	trap_Cvar_VariableStringBuffer( const char *name, char *buf, int size );
int		trap_Argc( void );
void	trap_Argv( int n, char *buf, int size );
void	trap_Cmd_ExecuteText( const char *text );
int		trap_FS_ReadFile( const char *path, char *buf, int size );
int		trap_R_RegisterShaderNoMip( const char *name );
void	trap_R_SetColor( const float *rgba );
void	trap_R_DrawStretchPic( float x, float y, float w, float h, float s1, float t1, float s2, float t2, int shader );
int		trap_R_ConsoleFont( void );
int		trap_Key_GetCatcher( void );
void	trap_Key_SetCatcher( int catcher );
int		trap_R_White( void );
void	trap_R_RegisterFont( const char *name, int pointSize, fontInfo_t *font );

/* ---------------------------------------------------------------- ui_main.c */

void	UI_Printf( const char *fmt, ... );
int		Q_stricmpUI( const char *a, const char *b );
void	UI_CvarString( const char *name, char *buf, int size );
float	UI_CvarValue( const char *name );

/* ---------------------------------------------------------------- ui_parse.c: the menu files */

/* A token stream over a file with Tremulous' precompiler directives: #include, #define (without
 * arguments), #undef, #ifdef / #ifndef / #else / #endif, and C enums, whose names become defines. */
typedef enum { TT_NONE, TT_STRING, TT_NUMBER, TT_NAME, TT_PUNCT } tokenType_t;

typedef struct {
	tokenType_t	type;
	char		string[1024];
	float		number;
	int			line;				/* in its file */
	char		file[64];
} token_t;

qboolean	Parse_Open( const char *path );		/* qfalse if the file is missing */
void		Parse_Close( void );
qboolean	Parse_Token( token_t *t );			/* qfalse at the end */
void		Parse_Unread( const token_t *t );	/* the next Parse_Token returns it again */
qboolean	Parse_Number( float *out );			/* a number, or an expression: + - * / ( ) */
qboolean	Parse_String( char *out, int size );	/* a string or a name */
qboolean	Parse_Expect( const char *punct );
void		Parse_Warning( const token_t *t, const char *fmt, ... );
void		Parse_SkipLine( const token_t *t );		/* the rest of t's line, and a { } block after it */
/* the tokens of a { } block (the opening { already read) as one script string */
qboolean	Parse_Block( char *out, int size );

/* ---------------------------------------------------------------- ui_menu.c: menus, items, scripts */

qboolean	Menu_Load( const char *path );
void		Menu_OpenMain( void );
void		Menu_Refresh( int time, int width, int height );
void		Menu_Key( int key, qboolean down );
void		Menu_Char( int ch );
void		Menu_MouseMove( int x, int y );
qboolean	Menu_Command( const char *cmd );

#endif
