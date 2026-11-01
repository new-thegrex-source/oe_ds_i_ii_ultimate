/*
 * Menus and items as in Tremulous' menu files, drawn on a 640 x 480 screen
 * that is scaled into the window (kept 4:3, centred; a fullScreen menu's
 * background covers the whole window).
 *
 *   menuDef { name, rect, fullScreen, visible, style, background, backcolor,
 *             focusColor, border, bordercolor, borderSize, popup,
 *             onOpen { script }, onClose { script }, onESC { script }, itemDef { ... } }
 *   itemDef { name, text, type, style, rect, textalign, textalignx, textaligny,
 *             textscale, textstyle, forecolor, backcolor, bordercolor, border,
 *             borderSize, background, visible, decoration, autowrapped, cvar,
 *             cvarTest, showCvar / hideCvar / enableCvar / disableCvar { "values" },
 *             action { script }, onFocus { script }, leaveFocus { script } }
 *
 * Scripts: open <menu>, close <menu>, exec "<command>", setcvar <cvar> <value>,
 * show <item>, hide <item>, setfocus <item>, uiScript quit; the rest (play,
 * playlooped, transitions, ...) are ignored. Several are separated by ";".
 */
#include "ui_local.h"
#include <math.h>

#define MAX_MENUS		48
#define MAX_ITEMS		768
#define MAX_OPEN		16

/* menudef.h */
#define ITEM_TYPE_TEXT			1
#define ITEM_TYPE_BUTTON		2
#define WINDOW_STYLE_FILLED		1
#define WINDOW_STYLE_SHADER		3
#define WINDOW_BORDER_FULL		1
#define WINDOW_BORDER_HORZ		2
#define WINDOW_BORDER_VERT		3
#define ALIGN_LEFT				0
#define ALIGN_CENTER			1
#define ALIGN_RIGHT				2
#define ITEM_TEXTSTYLE_BLINK		1
#define ITEM_TEXTSTYLE_PULSE		2
#define ITEM_TEXTSTYLE_SHADOWED		3
#define ITEM_TEXTSTYLE_SHADOWEDMORE	6

#define CVAR_SHOW		1
#define CVAR_HIDE		2
#define CVAR_ENABLE		4
#define CVAR_DISABLE	8

typedef struct {
	float	x, y, w, h;
} rect_t;

typedef struct {
	char	name[32];
	char	text[512];
	char	cvar[64];
	int		menu;
	int		type, style, border, textStyle, align;
	int		visible, decoration, autowrap, hidden;
	rect_t	rect;
	float	textAlignX, textAlignY, textScale, borderSize;
	float	fore[4], back[4], borderColor[4];
	int		background;
	char	*action, *onFocus, *leaveFocus;
	char	cvarTest[64];
	char	cvarValues[512];	/* the values of showCvar & co, each followed by a 0, ending with an empty one */
	int		cvarFlags;
} item_t;

typedef struct {
	char	name[32];
	rect_t	rect;
	int		fullScreen, visible, popup, style, border;
	float	back[4], focus[4], borderColor[4], borderSize;
	int		background;
	char	*onOpen, *onClose, *onEsc;
	int		firstItem, numItems;
	int		focusItem;			/* index in items[], -1 none */
} menu_t;

static menu_t	menus[MAX_MENUS];
static int		numMenus;
static item_t	items[MAX_ITEMS];
static int		numItems;
static int		openMenus[MAX_OPEN];	/* indexes in menus[], the last one on top */
static int		numOpen;
static char		loadQueue[8][64];
static int		numLoadQueue;

static float	defaultFocus[4] = { 1, 0.75f, 0, 1 };
static float	shadowColor[4] = { 0, 0, 0, 0.5f };
static int		fontShader, whiteShader, cursorShader;
static fontInfo_t textFont, smallFont, bigFont;
static int		haveTextFont, haveSmallFont, haveBigFont;
static int		uiTime;
static int		vidWidth = 640, vidHeight = 480;
static float	scale = 1, offsetX, offsetY;
static int		mouseX = -1, mouseY = -1;

static char *Menu_Strdup( const char *s ) {
	size_t	n = strlen( s ) + 1;
	char	*d = malloc( n );
	if ( d ) {
		memcpy( d, s, n );
	}
	return d;
}

static void Menu_Copy( char *dst, const char *src, int size ) {
	strncpy( dst, src, size - 1 );
	dst[size - 1] = 0;
}

static void Menu_Free( void ) {
	int i;
	for ( i = 0; i < numItems; i++ ) {
		free( items[i].action );
		free( items[i].onFocus );
		free( items[i].leaveFocus );
	}
	for ( i = 0; i < numMenus; i++ ) {
		free( menus[i].onOpen );
		free( menus[i].onClose );
		free( menus[i].onEsc );
	}
	numItems = numMenus = numOpen = 0;
}

/* ---------------------------------------------------------------- loading */

static qboolean Menu_Color( float *c ) {
	int i;
	for ( i = 0; i < 4; i++ ) {
		if ( !Parse_Number( &c[i] ) ) {
			return qfalse;
		}
	}
	return qtrue;
}

static qboolean Menu_Rect( rect_t *r ) {
	return Parse_Number( &r->x ) && Parse_Number( &r->y ) && Parse_Number( &r->w ) && Parse_Number( &r->h );
}

static qboolean Menu_Int( int *out ) {
	float f;
	if ( !Parse_Number( &f ) ) {
		return qfalse;
	}
	*out = (int)f;
	return qtrue;
}

static char *Menu_Script( void ) {
	char *buf = malloc( 8192 ), *s;
	if ( !buf ) {
		return NULL;
	}
	if ( !Parse_Expect( "{" ) || !Parse_Block( buf, 8192 ) ) {
		free( buf );
		return NULL;
	}
	s = Menu_Strdup( buf );
	free( buf );
	return s;
}

static int Menu_Shader( void ) {
	char name[256];
	if ( !Parse_String( name, sizeof( name ) ) ) {
		return 0;
	}
	return name[0] ? trap_R_RegisterShaderNoMip( name ) : 0;
}

/* showCvar { "a" "b" }: the values, each ended by a 0, then an empty one */
static void Menu_CvarValues( item_t *it, int flag ) {
	token_t	t;
	int		n = 0;

	it->cvarFlags |= flag;
	if ( !Parse_Expect( "{" ) ) {
		return;
	}
	while ( Parse_Token( &t ) && strcmp( t.string, "}" ) ) {
		int len = (int)strlen( t.string );
		if ( !strcmp( t.string, ";" ) ) {
			continue;
		}
		if ( n + len + 2 < (int)sizeof( it->cvarValues ) ) {
			memcpy( it->cvarValues + n, t.string, len + 1 );
			n += len + 1;
		}
	}
	it->cvarValues[n] = 0;
}

static void Menu_ParseItem( menu_t *m ) {
	token_t	t;
	item_t	*it;

	if ( numItems == MAX_ITEMS ) {
		Parse_Warning( NULL, "too many items" );
		Parse_Expect( "{" );
		{
			char skip[8192];
			Parse_Block( skip, sizeof( skip ) );
		}
		return;
	}
	it = &items[numItems];
	memset( it, 0, sizeof( *it ) );
	it->menu = (int)( m - menus );
	it->textScale = 0.3f;
	it->fore[0] = it->fore[1] = it->fore[2] = it->fore[3] = 1;
	it->borderSize = 1;
	it->visible = 1;
	if ( !Parse_Expect( "{" ) ) {
		return;
	}
	while ( Parse_Token( &t ) ) {
		const char *k = t.string;
		if ( !strcmp( k, "}" ) ) {
			break;
		}
		if ( !Q_stricmpUI( k, "name" ) ) {
			Parse_String( it->name, sizeof( it->name ) );
		} else if ( !Q_stricmpUI( k, "text" ) ) {
			Parse_String( it->text, sizeof( it->text ) );
		} else if ( !Q_stricmpUI( k, "type" ) ) {
			Menu_Int( &it->type );
		} else if ( !Q_stricmpUI( k, "style" ) ) {
			Menu_Int( &it->style );
		} else if ( !Q_stricmpUI( k, "textstyle" ) ) {
			Menu_Int( &it->textStyle );
		} else if ( !Q_stricmpUI( k, "rect" ) ) {
			Menu_Rect( &it->rect );
		} else if ( !Q_stricmpUI( k, "textalign" ) ) {
			Menu_Int( &it->align );
		} else if ( !Q_stricmpUI( k, "textalignx" ) ) {
			Parse_Number( &it->textAlignX );
		} else if ( !Q_stricmpUI( k, "textaligny" ) ) {
			Parse_Number( &it->textAlignY );
		} else if ( !Q_stricmpUI( k, "textscale" ) ) {
			Parse_Number( &it->textScale );
		} else if ( !Q_stricmpUI( k, "forecolor" ) ) {
			Menu_Color( it->fore );
		} else if ( !Q_stricmpUI( k, "backcolor" ) ) {
			Menu_Color( it->back );
		} else if ( !Q_stricmpUI( k, "bordercolor" ) ) {
			Menu_Color( it->borderColor );
		} else if ( !Q_stricmpUI( k, "border" ) ) {
			Menu_Int( &it->border );
		} else if ( !Q_stricmpUI( k, "borderSize" ) ) {
			Parse_Number( &it->borderSize );
		} else if ( !Q_stricmpUI( k, "background" ) ) {
			it->background = Menu_Shader();
		} else if ( !Q_stricmpUI( k, "visible" ) ) {
			Menu_Int( &it->visible );
		} else if ( !Q_stricmpUI( k, "decoration" ) ) {
			it->decoration = 1;
		} else if ( !Q_stricmpUI( k, "autowrapped" ) || !Q_stricmpUI( k, "wrapped" ) ) {
			it->autowrap = 1;
		} else if ( !Q_stricmpUI( k, "cvar" ) ) {
			Parse_String( it->cvar, sizeof( it->cvar ) );
		} else if ( !Q_stricmpUI( k, "cvarTest" ) ) {
			Parse_String( it->cvarTest, sizeof( it->cvarTest ) );
		} else if ( !Q_stricmpUI( k, "showCvar" ) ) {
			Menu_CvarValues( it, CVAR_SHOW );
		} else if ( !Q_stricmpUI( k, "hideCvar" ) ) {
			Menu_CvarValues( it, CVAR_HIDE );
		} else if ( !Q_stricmpUI( k, "enableCvar" ) ) {
			Menu_CvarValues( it, CVAR_ENABLE );
		} else if ( !Q_stricmpUI( k, "disableCvar" ) ) {
			Menu_CvarValues( it, CVAR_DISABLE );
		} else if ( !Q_stricmpUI( k, "action" ) ) {
			free( it->action );
			it->action = Menu_Script();
		} else if ( !Q_stricmpUI( k, "onFocus" ) ) {
			free( it->onFocus );
			it->onFocus = Menu_Script();
		} else if ( !Q_stricmpUI( k, "leaveFocus" ) ) {
			free( it->leaveFocus );
			it->leaveFocus = Menu_Script();
		} else {
			/* what this ui has no use for (models, cinematics, lists, ...) */
			Parse_SkipLine( &t );
		}
	}
	m->numItems++;
	numItems++;
}

static void Menu_ParseMenu( void ) {
	token_t	t;
	menu_t	*m;

	if ( numMenus == MAX_MENUS ) {
		Parse_Warning( NULL, "too many menus" );
		return;
	}
	m = &menus[numMenus];
	memset( m, 0, sizeof( *m ) );
	m->rect.w = 640;
	m->rect.h = 480;
	memcpy( m->focus, defaultFocus, sizeof( m->focus ) );
	m->borderSize = 1;
	m->firstItem = numItems;
	m->focusItem = -1;
	if ( !Parse_Expect( "{" ) ) {
		return;
	}
	while ( Parse_Token( &t ) ) {
		const char *k = t.string;
		if ( !strcmp( k, "}" ) ) {
			break;
		}
		if ( !Q_stricmpUI( k, "name" ) ) {
			Parse_String( m->name, sizeof( m->name ) );
		} else if ( !Q_stricmpUI( k, "fullScreen" ) || !Q_stricmpUI( k, "fullscreen" ) ) {
			Menu_Int( &m->fullScreen );
		} else if ( !Q_stricmpUI( k, "rect" ) ) {
			Menu_Rect( &m->rect );
		} else if ( !Q_stricmpUI( k, "visible" ) ) {
			Menu_Int( &m->visible );
		} else if ( !Q_stricmpUI( k, "style" ) ) {
			Menu_Int( &m->style );
		} else if ( !Q_stricmpUI( k, "border" ) ) {
			Menu_Int( &m->border );
		} else if ( !Q_stricmpUI( k, "borderSize" ) ) {
			Parse_Number( &m->borderSize );
		} else if ( !Q_stricmpUI( k, "bordercolor" ) ) {
			Menu_Color( m->borderColor );
		} else if ( !Q_stricmpUI( k, "backcolor" ) ) {
			Menu_Color( m->back );
		} else if ( !Q_stricmpUI( k, "focusColor" ) || !Q_stricmpUI( k, "focuscolor" ) ) {
			Menu_Color( m->focus );
		} else if ( !Q_stricmpUI( k, "background" ) ) {
			m->background = Menu_Shader();
		} else if ( !Q_stricmpUI( k, "popup" ) ) {
			m->popup = 1;
		} else if ( !Q_stricmpUI( k, "onOpen" ) ) {
			free( m->onOpen );
			m->onOpen = Menu_Script();
		} else if ( !Q_stricmpUI( k, "onClose" ) ) {
			free( m->onClose );
			m->onClose = Menu_Script();
		} else if ( !Q_stricmpUI( k, "onESC" ) ) {
			free( m->onEsc );
			m->onEsc = Menu_Script();
		} else if ( !Q_stricmpUI( k, "itemDef" ) ) {
			Menu_ParseItem( m );
		} else if ( !Q_stricmpUI( k, "outOfBoundsClick" ) || !Q_stricmpUI( k, "dontCloseAll" ) ) {
			/* flags without a value */
		} else {
			Parse_SkipLine( &t );
		}
	}
	numMenus++;
}

static void Menu_ParseGlobals( void ) {
	token_t t;
	char name[256];
	float pointSize;
	if ( !Parse_Expect( "{" ) ) {
		return;
	}
	while ( Parse_Token( &t ) ) {
		if ( !strcmp( t.string, "}" ) ) {
			break;
		}
		if ( !Q_stricmpUI( t.string, "focusColor" ) ) {
			Menu_Color( defaultFocus );
		} else if ( !Q_stricmpUI( t.string, "shadowColor" ) ) {
			Menu_Color( shadowColor );
		} else if ( !Q_stricmpUI( t.string, "font" ) ) {
			if ( Parse_String( name, sizeof( name ) ) && Parse_Number( &pointSize ) ) {
				trap_R_RegisterFont( name, (int)pointSize, &textFont );
				haveTextFont = 1;
			}
		} else if ( !Q_stricmpUI( t.string, "smallFont" ) ) {
			if ( Parse_String( name, sizeof( name ) ) && Parse_Number( &pointSize ) ) {
				trap_R_RegisterFont( name, (int)pointSize, &smallFont );
				haveSmallFont = 1;
			}
		} else if ( !Q_stricmpUI( t.string, "bigFont" ) ) {
			if ( Parse_String( name, sizeof( name ) ) && Parse_Number( &pointSize ) ) {
				trap_R_RegisterFont( name, (int)pointSize, &bigFont );
				haveBigFont = 1;
			}
		} else if ( !Q_stricmpUI( t.string, "cursor" ) ) {
			if ( Parse_String( name, sizeof( name ) ) ) {
				cursorShader = trap_R_RegisterShaderNoMip( name );
			}
		} else {
			Parse_SkipLine( &t );
		}
	}
}

static qboolean Menu_LoadFile( const char *path ) {
	token_t t;

	if ( !Parse_Open( path ) ) {
		UI_Printf( "^1%s: no such file\n", path );
		return qfalse;
	}
	while ( Parse_Token( &t ) ) {
		if ( !strcmp( t.string, "{" ) || !strcmp( t.string, "}" ) ) {
			continue;	/* the file's outer braces */
		}
		if ( !Q_stricmpUI( t.string, "assetGlobalDef" ) ) {
			Menu_ParseGlobals();
		} else if ( !Q_stricmpUI( t.string, "menuDef" ) ) {
			Menu_ParseMenu();
		} else if ( !Q_stricmpUI( t.string, "loadMenu" ) ) {
			/* loadMenu { "ui/a.menu" "ui/b.menu" } (Tremulous' menus.txt) */
			token_t f;
			if ( Parse_Expect( "{" ) ) {
				while ( Parse_Token( &f ) && strcmp( f.string, "}" ) ) {
					if ( f.type == TT_STRING && numLoadQueue < 8 ) {
						Menu_Copy( loadQueue[numLoadQueue++], f.string, sizeof( loadQueue[0] ) );
					}
				}
			}
		} else {
			Parse_Warning( &t, "\"%s\" is not known here", t.string );
			Parse_SkipLine( &t );
		}
	}
	Parse_Close();
	return qtrue;
}

qboolean Menu_Load( const char *path ) {
	qboolean ok;
	int i;

	Menu_Free();
	fontShader = trap_R_ConsoleFont();
	whiteShader = trap_R_White();
	cursorShader = 0;
	haveTextFont = haveSmallFont = haveBigFont = 0;
	numLoadQueue = 0;
	ok = Menu_LoadFile( path );
	for ( i = 0; i < numLoadQueue; i++ ) {
		Menu_LoadFile( loadQueue[i] );
	}
	UI_Printf( "%s: %d menus, %d items (fonts: text=%d small=%d big=%d cursor=%d)\n",
		path, numMenus, numItems, haveTextFont, haveSmallFont, haveBigFont, cursorShader != 0 );
	return ok && numMenus > 0;
}

/* ---------------------------------------------------------------- drawing */

static void Menu_Layout( int width, int height ) {
	vidWidth = width > 0 ? width : 640;
	vidHeight = height > 0 ? height : 480;
	scale = (float)vidWidth / 640 < (float)vidHeight / 480 ? (float)vidWidth / 640 : (float)vidHeight / 480;
	offsetX = ( vidWidth - 640 * scale ) * 0.5f;
	offsetY = ( vidHeight - 480 * scale ) * 0.5f;
}

static void Menu_FillWindowRect( float x, float y, float w, float h, const float *color ) {
	if ( color[3] <= 0 ) {
		return;
	}
	trap_R_SetColor( color );
	trap_R_DrawStretchPic( x, y, w, h, 0, 0, 1, 1, whiteShader );
	trap_R_SetColor( NULL );
}

static void Menu_FillRect( float x, float y, float w, float h, const float *color ) {
	Menu_FillWindowRect( offsetX + x * scale, offsetY + y * scale, w * scale, h * scale, color );
}

static void Menu_DrawPic( float x, float y, float w, float h, int shader, const float *color ) {
	trap_R_SetColor( color );
	trap_R_DrawStretchPic( offsetX + x * scale, offsetY + y * scale, w * scale, h * scale, 0, 0, 1, 1, shader );
	trap_R_SetColor( NULL );
}

static void Menu_DrawBorder( const rect_t *r, int border, float size, const float *color ) {
	if ( border == WINDOW_BORDER_FULL || border == WINDOW_BORDER_HORZ ) {
		Menu_FillRect( r->x, r->y, r->w, size, color );
		Menu_FillRect( r->x, r->y + r->h - size, r->w, size, color );
	}
	if ( border == WINDOW_BORDER_FULL || border == WINDOW_BORDER_VERT ) {
		Menu_FillRect( r->x, r->y, size, r->h, color );
		Menu_FillRect( r->x + r->w - size, r->y, size, r->h, color );
	}
}

/* Prefer Tremulous proportional fonts (from assetGlobalDef) when registered;
 * fall back to the engine console font (16x16 cells, 2:1 aspect). */
static fontInfo_t *Menu_PickFont( float textScale ) {
	/* cvars ui_smallFont / ui_bigFont are the scale thresholds in original Tremulous */
	float smallTh = UI_CvarValue( "ui_smallFont" );
	float bigTh = UI_CvarValue( "ui_bigFont" );
	if ( smallTh <= 0 ) smallTh = 0.2f;
	if ( bigTh <= 0 ) bigTh = 0.5f;
	if ( textScale <= smallTh && haveSmallFont )
		return &smallFont;
	if ( textScale >= bigTh && haveBigFont )
		return &bigFont;
	if ( haveTextFont )
		return &textFont;
	return NULL;
}

static float Menu_CharHeight( float textScale ) {
	fontInfo_t *font = Menu_PickFont( textScale );
	if ( font )
		return textScale * font->glyphScale * font->glyphs[(int)'A'].height;
	return textScale * 40;	/* console font fallback */
}

static float Menu_TextWidth( const char *s, int n, float textScale ) {
	fontInfo_t *font = Menu_PickFont( textScale );
	float w = 0;
	int i;
	if ( !font ) {
		float h = Menu_CharHeight( textScale );
		return n * (h * 0.5f);
	}
	for ( i = 0; i < n && s[i]; i++ ) {
		unsigned char c = (unsigned char)s[i];
		w += font->glyphs[c].xSkip * textScale * font->glyphScale;
	}
	return w;
}

static void Menu_DrawChars( float x, float y, float textScale, const float *color, const char *s, int n ) {
	fontInfo_t *font = Menu_PickFont( textScale );
	int i;

	trap_R_SetColor( color );
	if ( !font ) {
		/* console font: fixed 2:1 cells */
		float h = textScale * 40, w = h * 0.5f;
		for ( i = 0; i < n && s[i]; i++ ) {
			int c = (unsigned char)s[i];
			if ( c != ' ' ) {
				float col = ( c & 15 ) * 0.0625f, row = ( c >> 4 ) * 0.0625f;
				trap_R_DrawStretchPic( offsetX + ( x + i * w ) * scale, offsetY + y * scale, w * scale, h * scale,
					col, row, col + 0.0625f, row + 0.0625f, fontShader );
			}
		}
	} else {
		float useScale = textScale * font->glyphScale;
		float cx = x;
		for ( i = 0; i < n && s[i]; i++ ) {
			unsigned char c = (unsigned char)s[i];
			glyphInfo_t *g = &font->glyphs[c];
			if ( c != ' ' && g->glyph ) {
				float yadj = useScale * g->top;
				trap_R_DrawStretchPic(
					offsetX + cx * scale,
					offsetY + (y - yadj) * scale,
					g->imageWidth * useScale * scale,
					g->imageHeight * useScale * scale,
					g->s, g->t, g->s2, g->t2, g->glyph );
			}
			cx += g->xSkip * useScale;
		}
	}
	trap_R_SetColor( NULL );
}

/* the lines of a text in a width (0: no wrapping): starts and lengths */
static int Menu_Lines( const char *text, float textScale, float width, int *starts, int *lens, int max ) {
	int n = 0, pos = 0;
	fontInfo_t *font = Menu_PickFont( textScale );
	float useScale = font ? textScale * font->glyphScale : textScale * 20; /* ~char width */

	while ( text[pos] && n < max ) {
		int len = 0, lastSpace = -1;
		float lineW = 0;
		while ( text[pos + len] && text[pos + len] != '\n' ) {
			unsigned char c = (unsigned char)text[pos + len];
			float cw = font ? font->glyphs[c].xSkip * useScale : useScale;
			if ( width > 0 && lineW + cw > width && len > 0 )
				break;
			if ( c == ' ' )
				lastSpace = len;
			lineW += cw;
			len++;
		}
		if ( text[pos + len] && text[pos + len] != '\n' && lastSpace > 0 ) {
			len = lastSpace;	/* break at the last blank */
		}
		starts[n] = pos;
		lens[n] = len;
		n++;
		pos += len;
		if ( text[pos] == '\n' || text[pos] == ' ' ) {
			pos++;
		}
	}
	return n;
}

static void Menu_DrawText( const item_t *it, const char *text, const float *color ) {
	float	h = Menu_CharHeight( it->textScale ), y;
	int		starts[32], lens[32], n, i;
	float	c[4];

	memcpy( c, color, sizeof( c ) );
	if ( it->textStyle == ITEM_TEXTSTYLE_BLINK && ( uiTime / 200 ) & 1 ) {
		return;
	}
	if ( it->textStyle == ITEM_TEXTSTYLE_PULSE ) {
		c[3] *= 0.5f + 0.5f * (float)sin( uiTime / 150.0 );
	}
	n = Menu_Lines( text, it->textScale, it->autowrap ? it->rect.w - it->textAlignX : 0, starts, lens, 32 );
	/* the lines centred in the rectangle's height, moved by textaligny */
	y = it->rect.y + ( it->rect.h - n * h ) * 0.5f + it->textAlignY;
	for ( i = 0; i < n; i++, y += h ) {
		float x, tw = Menu_TextWidth( text + starts[i], lens[i], it->textScale );
		if ( it->align == ALIGN_CENTER ) {
			x = it->rect.x + ( it->rect.w - tw ) * 0.5f + it->textAlignX;
		} else if ( it->align == ALIGN_RIGHT ) {
			x = it->rect.x + it->rect.w - tw - it->textAlignX;
		} else {
			x = it->rect.x + it->textAlignX;
		}
		if ( it->textStyle == ITEM_TEXTSTYLE_SHADOWED || it->textStyle == ITEM_TEXTSTYLE_SHADOWEDMORE ) {
			float d = it->textStyle == ITEM_TEXTSTYLE_SHADOWED ? h / 16 : h / 8;
			float sc[4];
			memcpy( sc, shadowColor, sizeof( sc ) );
			sc[3] *= c[3];
			Menu_DrawChars( x + d, y + d, it->textScale, sc, text + starts[i], lens[i] );
		}
		Menu_DrawChars( x, y, it->textScale, c, text + starts[i], lens[i] );
	}
}



/* the cvarTest / showCvar & co state of an item */
static qboolean Menu_CvarMatch( const item_t *it ) {
	char		value[256];
	const char	*v = it->cvarValues;

	UI_CvarString( it->cvarTest, value, sizeof( value ) );
	for ( ;; ) {
		if ( !strcmp( v, value ) ) {
			return qtrue;
		}
		if ( !*v ) {
			return qfalse;
		}
		v += strlen( v ) + 1;
	}
}

static qboolean Menu_ItemVisible( const item_t *it ) {
	if ( !it->visible || it->hidden ) {
		return qfalse;
	}
	if ( it->cvarTest[0] && ( it->cvarFlags & ( CVAR_SHOW | CVAR_HIDE ) ) ) {
		qboolean match = Menu_CvarMatch( it );
		if ( ( it->cvarFlags & CVAR_SHOW ) && !match ) {
			return qfalse;
		}
		if ( ( it->cvarFlags & CVAR_HIDE ) && match ) {
			return qfalse;
		}
	}
	return qtrue;
}

static qboolean Menu_ItemEnabled( const item_t *it ) {
	if ( it->cvarTest[0] && ( it->cvarFlags & ( CVAR_ENABLE | CVAR_DISABLE ) ) ) {
		qboolean match = Menu_CvarMatch( it );
		if ( ( it->cvarFlags & CVAR_ENABLE ) && !match ) {
			return qfalse;
		}
		if ( ( it->cvarFlags & CVAR_DISABLE ) && match ) {
			return qfalse;
		}
	}
	return qtrue;
}

/* can be focused and run */
static qboolean Menu_ItemActive( const item_t *it ) {
	return Menu_ItemVisible( it ) && !it->decoration && Menu_ItemEnabled( it ) &&
		( it->type == ITEM_TYPE_BUTTON || it->action );
}

static void Menu_DrawItem( const menu_t *m, const item_t *it ) {
	float		color[4];
	char		text[1024];
	qboolean	focus = m->focusItem == (int)( it - items );

	if ( !Menu_ItemVisible( it ) ) {
		return;
	}
	if ( it->style == WINDOW_STYLE_FILLED ) {
		Menu_FillRect( it->rect.x, it->rect.y, it->rect.w, it->rect.h, it->back );
	} else if ( it->style == WINDOW_STYLE_SHADER && it->background ) {
		Menu_DrawPic( it->rect.x, it->rect.y, it->rect.w, it->rect.h, it->background, it->fore );
	}
	if ( it->border ) {
		Menu_DrawBorder( &it->rect, it->border, it->borderSize, it->borderColor );
	}
	memcpy( color, focus ? m->focus : it->fore, sizeof( color ) );
	if ( !Menu_ItemEnabled( it ) ) {
		color[3] *= 0.35f;
	}
	Menu_Copy( text, it->text, sizeof( text ) );
	if ( it->cvar[0] ) {
		/* a cvar's value after the text (ui_gameError, ...) */
		char value[512];
		UI_CvarString( it->cvar, value, sizeof( value ) );
		strncat( text, value, sizeof( text ) - strlen( text ) - 1 );
	}
	if ( text[0] ) {
		Menu_DrawText( it, text, color );
	}
}

static void Menu_DrawMenu( menu_t *m ) {
	int i;
	if ( m->fullScreen ) {
		/* the background over the whole window */
		static const float black[4] = { 0, 0, 0, 1 };
		Menu_FillWindowRect( 0, 0, (float)vidWidth, (float)vidHeight, m->back[3] > 0 ? m->back : black );
		if ( m->background ) {
			trap_R_DrawStretchPic( 0, 0, (float)vidWidth, (float)vidHeight, 0, 0, 1, 1, m->background );
		}
	} else {
		if ( m->style == WINDOW_STYLE_FILLED ) {
			Menu_FillRect( m->rect.x, m->rect.y, m->rect.w, m->rect.h, m->back );
		} else if ( m->style == WINDOW_STYLE_SHADER && m->background ) {
			Menu_DrawPic( m->rect.x, m->rect.y, m->rect.w, m->rect.h, m->background, NULL );
		}
		if ( m->border ) {
			Menu_DrawBorder( &m->rect, m->border, m->borderSize, m->borderColor );
		}
	}
	for ( i = 0; i < m->numItems; i++ ) {
		Menu_DrawItem( m, &items[m->firstItem + i] );
	}
}

void Menu_Refresh( int time, int width, int height ) {
	int i, first = 0;

	uiTime = time;
	Menu_Layout( width, height );
	/* from the topmost full screen menu up */
	for ( i = numOpen - 1; i >= 0; i-- ) {
		if ( menus[openMenus[i]].fullScreen ) {
			first = i;
			break;
		}
	}
	for ( i = first; i < numOpen; i++ ) {
		Menu_DrawMenu( &menus[openMenus[i]] );
	}
	/* Tremulous-style cursor (assetGlobalDef cursor) in window pixels */
	if ( cursorShader && mouseX >= 0 && mouseY >= 0 ) {
		float cw = 32.0f * scale, ch = 32.0f * scale;
		/* mouse is already in window pixels (from CL_UIMousePosition) */
		trap_R_SetColor( NULL );
		trap_R_DrawStretchPic( (float)mouseX - cw * 0.5f, (float)mouseY - ch * 0.5f, cw, ch, 0, 0, 1, 1, cursorShader );
	}
}

/* ---------------------------------------------------------------- menus and scripts */

static int Menu_Find( const char *name ) {
	int i;
	for ( i = 0; i < numMenus; i++ ) {
		if ( !Q_stricmpUI( menus[i].name, name ) ) {
			return i;
		}
	}
	return -1;
}

static menu_t *Menu_Top( void ) {
	return numOpen ? &menus[openMenus[numOpen - 1]] : NULL;
}

static void Menu_RunScript( const char *script );

static void Menu_SetFocus( menu_t *m, int item ) {
	if ( m->focusItem == item ) {
		return;
	}
	if ( m->focusItem >= 0 && items[m->focusItem].leaveFocus ) {
		Menu_RunScript( items[m->focusItem].leaveFocus );
	}
	m->focusItem = item;
	if ( item >= 0 && items[item].onFocus ) {
		Menu_RunScript( items[item].onFocus );
	}
}

static void Menu_Open( const char *name ) {
	int i, m = Menu_Find( name );
	if ( m < 0 ) {
		UI_Printf( "^3menu \"%s\" not found\n", name );
		return;
	}
	for ( i = 0; i < numOpen; i++ ) {
		if ( openMenus[i] == m ) {
			memmove( &openMenus[i], &openMenus[i + 1], ( numOpen - i - 1 ) * sizeof( openMenus[0] ) );
			numOpen--;
			break;
		}
	}
	if ( numOpen == MAX_OPEN ) {
		memmove( &openMenus[0], &openMenus[1], ( MAX_OPEN - 1 ) * sizeof( openMenus[0] ) );
		numOpen--;
	}
	openMenus[numOpen++] = m;
	menus[m].focusItem = -1;
	if ( menus[m].onOpen ) {
		Menu_RunScript( menus[m].onOpen );
	}
}

static void Menu_Close( const char *name ) {
	int i, m = Menu_Find( name );
	for ( i = 0; i < numOpen; i++ ) {
		if ( openMenus[i] == m ) {
			memmove( &openMenus[i], &openMenus[i + 1], ( numOpen - i - 1 ) * sizeof( openMenus[0] ) );
			numOpen--;
			if ( menus[m].onClose ) {
				Menu_RunScript( menus[m].onClose );
			}
			break;
		}
	}
	if ( !numOpen ) {
		Menu_OpenMain();	/* there is always a menu */
	}
}

static void Menu_ShowItems( const char *name, int hidden ) {
	int i;
	for ( i = 0; i < numItems; i++ ) {
		if ( !Q_stricmpUI( items[i].name, name ) ) {
			items[i].hidden = hidden;
		}
	}
}

/* the next word of a script: a quoted string, ";" or a word; qfalse at the end */
static qboolean Menu_ScriptToken( const char **p, char *out, int size ) {
	const char	*s = *p;
	int			n = 0;

	while ( *s && (unsigned char)*s <= ' ' ) {
		s++;
	}
	if ( !*s ) {
		*p = s;
		return qfalse;
	}
	if ( *s == '"' ) {
		s++;
		while ( *s && *s != '"' ) {
			if ( *s == '\\' && s[1] ) {
				s++;
			}
			if ( n < size - 1 ) {
				out[n++] = *s;
			}
			s++;
		}
		if ( *s ) {
			s++;
		}
	} else if ( *s == ';' ) {
		out[n++] = *s++;
	} else {
		while ( *s && (unsigned char)*s > ' ' && *s != ';' ) {
			if ( n < size - 1 ) {
				out[n++] = *s;
			}
			s++;
		}
	}
	out[n] = 0;
	*p = s;
	return qtrue;
}

static void Menu_RunScript( const char *script ) {
	char		args[4][512];
	int			n;
	const char	*p = script;

	if ( !script ) {
		return;
	}
	for ( ;; ) {
		/* one command: its words up to ";" */
		char word[512];
		qboolean more = qfalse;
		n = 0;
		while ( Menu_ScriptToken( &p, word, sizeof( word ) ) ) {
			more = qtrue;
			if ( !strcmp( word, ";" ) ) {
				break;
			}
			if ( n < 4 ) {
				Menu_Copy( args[n++], word, sizeof( args[0] ) );
			}
		}
		if ( !more ) {
			return;
		}
		if ( !n ) {
			continue;
		}
		if ( !Q_stricmpUI( args[0], "open" ) && n > 1 ) {
			Menu_Open( args[1] );
		} else if ( !Q_stricmpUI( args[0], "close" ) && n > 1 ) {
			Menu_Close( args[1] );
		} else if ( !Q_stricmpUI( args[0], "exec" ) && n > 1 ) {
			trap_Cmd_ExecuteText( args[1] );
			trap_Cmd_ExecuteText( "\n" );
		} else if ( !Q_stricmpUI( args[0], "setcvar" ) && n > 2 ) {
			trap_Cvar_Set( args[1], args[2] );
		} else if ( !Q_stricmpUI( args[0], "show" ) && n > 1 ) {
			Menu_ShowItems( args[1], 0 );
		} else if ( !Q_stricmpUI( args[0], "hide" ) && n > 1 ) {
			Menu_ShowItems( args[1], 1 );
		} else if ( !Q_stricmpUI( args[0], "setfocus" ) && n > 1 ) {
			menu_t *m = Menu_Top();
			int i;
			for ( i = 0; m && i < m->numItems; i++ ) {
				if ( !Q_stricmpUI( items[m->firstItem + i].name, args[1] ) ) {
					Menu_SetFocus( m, m->firstItem + i );
				}
			}
		} else if ( !Q_stricmpUI( args[0], "uiScript" ) && n > 1 && !Q_stricmpUI( args[1], "quit" ) ) {
			trap_Cmd_ExecuteText( "quit\n" );
		} else {
			/* play, playlooped, transition, fadein, ...: nothing to do here */
		}
	}
}

void Menu_OpenMain( void ) {
	char error[16];
	numOpen = 0;
	Menu_Open( "main" );
	if ( !numOpen && numMenus ) {
		openMenus[numOpen++] = 0;	/* no "main": the first one */
	}
	/* a game that did not start: its message, if the menus have a place for it */
	UI_CvarString( "ui_gameError", error, sizeof( error ) );
	if ( error[0] && Menu_Find( "gameerror" ) >= 0 ) {
		Menu_Open( "gameerror" );
	}
}

/* ---------------------------------------------------------------- input */

static int Menu_ItemAt( const menu_t *m, int x, int y ) {
	float	vx = ( x - offsetX ) / scale, vy = ( y - offsetY ) / scale;
	int		i;
	for ( i = m->numItems - 1; i >= 0; i-- ) {
		const item_t *it = &items[m->firstItem + i];
		if ( Menu_ItemActive( it ) && vx >= it->rect.x && vx < it->rect.x + it->rect.w && vy >= it->rect.y &&
			 vy < it->rect.y + it->rect.h ) {
			return m->firstItem + i;
		}
	}
	return -1;
}

void Menu_MouseMove( int x, int y ) {
	menu_t	*m = Menu_Top();
	int		item;
	mouseX = x;
	mouseY = y;
	if ( !m ) {
		return;
	}
	item = Menu_ItemAt( m, x, y );
	if ( item >= 0 ) {
		Menu_SetFocus( m, item );
	}
}

/* the next item that can take the focus, dir 1 / -1 */
static void Menu_MoveFocus( menu_t *m, int dir ) {
	int i, cur = m->focusItem >= 0 ? m->focusItem - m->firstItem : ( dir > 0 ? -1 : m->numItems );
	for ( i = 0; i < m->numItems; i++ ) {
		cur += dir;
		if ( cur < 0 ) {
			cur = m->numItems - 1;
		} else if ( cur >= m->numItems ) {
			cur = 0;
		}
		if ( Menu_ItemActive( &items[m->firstItem + cur] ) ) {
			Menu_SetFocus( m, m->firstItem + cur );
			return;
		}
	}
}

void Menu_Key( int key, qboolean down ) {
	menu_t *m = Menu_Top();
	if ( !down || !m ) {
		return;
	}
	switch ( key ) {
	case K_ESCAPE:
		if ( m->onEsc ) {
			Menu_RunScript( m->onEsc );
		} else if ( numOpen > 1 ) {
			Menu_Close( m->name );
		}
		return;
	case K_UPARROW:
	case K_KP_UPARROW:
	case K_LEFTARROW:
		Menu_MoveFocus( m, -1 );
		return;
	case K_DOWNARROW:
	case K_KP_DOWNARROW:
	case K_RIGHTARROW:
	case K_TAB:
		Menu_MoveFocus( m, 1 );
		return;
	case K_MOUSE1: {
		int item = Menu_ItemAt( m, mouseX, mouseY );
		if ( item < 0 ) {
			return;
		}
		Menu_SetFocus( m, item );
		Menu_RunScript( items[item].action );
		return;
	}
	case K_ENTER:
	case K_KP_ENTER:
		if ( m->focusItem >= 0 && Menu_ItemActive( &items[m->focusItem] ) ) {
			Menu_RunScript( items[m->focusItem].action );
		}
		return;
	default:
		return;
	}
}

void Menu_Char( int ch ) {
	(void)ch;	/* no edit fields yet */
}

/* ui_menu <name> opens a menu; ui_reload reads the menu files again */
qboolean Menu_Command( const char *cmd ) {
	char arg[64];
	if ( !Q_stricmpUI( cmd, "ui_menu" ) ) {
		trap_Argv( 1, arg, sizeof( arg ) );
		if ( arg[0] ) {
			Menu_Open( arg );
		} else {
			int i;
			for ( i = 0; i < numMenus; i++ ) {
				UI_Printf( "  %s\n", menus[i].name );
			}
		}
		return qtrue;
	}
	if ( !Q_stricmpUI( cmd, "ui_reload" ) ) {
		Menu_Load( "ui/main.menu" );
		Menu_OpenMain();
		return qtrue;
	}
	return qfalse;
}
