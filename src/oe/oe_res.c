/*
 * Resources from the search path (the .pk3 files and the game folder) in place
 * of the .bar's, for mods (an HD mod in particular):
 *
 * overrides.txt     "name=path" per line: the .bar resource called name (the
 *                   file name kept in its gzip header, e.g. Logos.bmp, m_01.bsp,
 *                   any case) or with that number (O&E has resources without
 *                   names) comes from path. A picture (.bmp in the .bar) keeps
 *                   its size for the game and is shown from path in its place,
 *                   at the screen's resolution: path may be larger (HD), any
 *                   format the engine reads (png, tga, jpg, bmp); clear where
 *                   its alpha is below half or it is magenta. Anything else
 *                   (maps, tables, sounds) is given to the game as path's bytes.
 * WTEXELS.txt,      "WALL172=path": the 3D view's wall / sprite / floor texture
 * STEXELS.txt,      with that number (the digits of the key; see OE_HWSRC) is
 * FTEXELS.txt       drawn on the GPU from path, again any size. The game's own
 *                   renderer and rules keep the original texels.
 *
 * Lines starting with //, # or ; are comments. The files are read again when a
 * game starts and when fs_usepk3 changes. Also here: oe_useidtech3fonts 1
 * draws the games' fonts with the id Tech 3 one (gfx/2d/bigchars, e.g. in
 * Tremulous' data pk3), laid into the games' font sheets.
 */
#include "oe_local.h"
#include "../zlib/zlib.h"

#define MAX_OVERRIDES	1024

typedef struct {
	int		key;			/* resource id, or texel number */
	char	path[MAX_QPATH];
} override_t;

typedef struct {
	override_t	*list;
	int			count;
} overrideList_t;

static const bar_t		*resBar;
static qboolean			resQvm;
static overrideList_t	resOverrides;
static overrideList_t	texelOverrides[4];	/* by OE_HWSRC_* */
static cvar_t			*oe_useidtech3fonts;

/* the last picture made for OE_ResImage (asked twice: size, then pixels) */
static struct {
	int				resId, scale, w, h;
	unsigned short	*px;
} resCache = { -1 };

/* ---------------------------------------------------------------- lists */

static void OE_ResFreeList( overrideList_t *l ) {
	if ( l->list ) {
		Z_Free( l->list );
	}
	l->list = NULL;
	l->count = 0;
}

static const barEntry_t *OE_ResFindName( const char *name ) {
	int i;
	for ( i = 0; resBar && i < resBar->count; i++ ) {
		if ( resBar->entries[i].type == 6 && !Q_stricmp( resBar->entries[i].name, name ) ) {
			return &resBar->entries[i];
		}
	}
	return NULL;
}

static void OE_ResTrim( char *s ) {
	char *p = s;
	int n;
	while ( *p == ' ' || *p == '\t' ) {
		p++;
	}
	memmove( s, p, strlen( p ) + 1 );
	n = (int)strlen( s );
	while ( n > 0 && ( s[n - 1] == ' ' || s[n - 1] == '\t' ) ) {
		s[--n] = 0;
	}
}

/* "key=value" lines of file into l; keyOf turns a key into a number (-1: skip it) */
static void OE_ResReadList( const char *file, overrideList_t *l, int ( *keyOf )( const char *key ) ) {
	char *text, *p, *line, *eq;
	int len;

	OE_ResFreeList( l );
	len = FS_ReadFile( file, (void **)&text );
	if ( len <= 0 || !text ) {
		return;
	}
	l->list = Z_Malloc( MAX_OVERRIDES * sizeof( override_t ) );
	for ( p = text; *p && l->count < MAX_OVERRIDES; ) {
		char key[MAX_QPATH], value[MAX_QPATH];
		int k;
		line = p;
		while ( *p && *p != '\n' && *p != '\r' ) {
			p++;
		}
		if ( *p ) {
			*p++ = 0;
		}
		while ( *line == ' ' || *line == '\t' ) {
			line++;
		}
		if ( !*line || *line == '#' || *line == ';' || ( line[0] == '/' && line[1] == '/' ) ) {
			continue;
		}
		eq = strchr( line, '=' );
		if ( !eq ) {
			continue;
		}
		*eq = 0;
		Q_strncpyz( key, line, sizeof( key ) );
		Q_strncpyz( value, eq + 1, sizeof( value ) );
		OE_ResTrim( key );
		OE_ResTrim( value );
		if ( !key[0] || !value[0] ) {
			continue;
		}
		k = keyOf( key );
		if ( k < 0 ) {
			Com_Printf( S_COLOR_YELLOW "%s: no resource \"%s\" in the game\n", file, key );
			continue;
		}
		l->list[l->count].key = k;
		Q_strncpyz( l->list[l->count].path, value, sizeof( l->list[0].path ) );
		l->count++;
	}
	FS_FreeFile( text );
	if ( l->count ) {
		Com_Printf( "%s: %d replacement%s\n", file, l->count, l->count == 1 ? "" : "s" );
	}
}

static const char *OE_ResLookup( const overrideList_t *l, int key ) {
	int i;
	for ( i = l->count - 1; i >= 0; i-- ) {		/* the last line wins */
		if ( l->list[i].key == key ) {
			return l->list[i].path;
		}
	}
	return NULL;
}

/* overrides.txt: a .bar name or a resource number */
static int OE_ResKeyOf( const char *key ) {
	const barEntry_t *e;
	const char *p;
	for ( p = key; *p >= '0' && *p <= '9'; p++ ) {
	}
	if ( !*p && p != key ) {
		return atoi( key );
	}
	e = OE_ResFindName( key );
	return e ? e->id : -1;
}

/* *TEXELS.txt: the number at the end of the key (WALL172 -> 172) */
static int OE_TexelKeyOf( const char *key ) {
	const char *p = key + strlen( key );
	while ( p > key && p[-1] >= '0' && p[-1] <= '9' ) {
		p--;
	}
	return *p ? atoi( p ) : -1;
}

static void OE_ResRead( void ) {
	static const char *texelFiles[4] = { NULL, "WTEXELS.txt", "STEXELS.txt", "FTEXELS.txt" };
	int k;

	resCache.resId = -1;
	OE_ResReadList( "overrides.txt", &resOverrides, OE_ResKeyOf );
	for ( k = OE_HWSRC_WALL; k <= OE_HWSRC_FLAT; k++ ) {
		OE_ResReadList( texelFiles[k], &texelOverrides[k], OE_TexelKeyOf );
	}
}

void OE_ResInit( const bar_t *bar, qboolean qvm ) {
	oe_useidtech3fonts = Cvar_Get( "oe_useidtech3fonts", "0", CVAR_ARCHIVE | CVAR_LATCH );
	resBar = bar;
	resQvm = qvm;
	OE_ResRead();
}

void OE_ResShutdown( void ) {
	int k;
	OE_ResFreeList( &resOverrides );
	for ( k = 0; k < 4; k++ ) {
		OE_ResFreeList( &texelOverrides[k] );
	}
	if ( resCache.px ) {
		Z_Free( resCache.px );
	}
	resCache.px = NULL;
	resCache.resId = -1;
	resBar = NULL;
}

void OE_FilesChanged( void ) {
	if ( resBar ) {
		OE_ResRead();
		CL_HwTexturesChanged();
	}
}

static qboolean OE_ResIsImage( int resId ) {
	const barEntry_t *e = resBar ? Bar_Find( resBar, resId ) : NULL;
	int n = e ? (int)strlen( e->name ) : 0;
	return n > 4 && !Q_stricmp( e->name + n - 4, ".bmp" );
}

const char *OE_ResOverride( int resId ) {
	if ( !resBar || OE_ResIsImage( resId ) ) {
		return NULL;	/* pictures are shown through OE_ResImage, the game keeps the original's size */
	}
	return OE_ResLookup( &resOverrides, resId );
}

/* pictures shown from other ones (overrides of .bmp resources, the id Tech 3 font): the game
 * screen should then be drawn at the window's resolution, or they would gain nothing */
qboolean OE_ResWantsHd( void ) {
	int i;
	if ( oe_useidtech3fonts && oe_useidtech3fonts->integer ) {
		return qtrue;
	}
	for ( i = 0; i < resOverrides.count; i++ ) {
		if ( OE_ResIsImage( resOverrides.list[i].key ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

const char *OE_TexelOverride( int source ) {
	int kind = ( source >> 24 ) & 0xff;
	if ( kind < OE_HWSRC_WALL || kind > OE_HWSRC_FLAT ) {
		return NULL;
	}
	return OE_ResLookup( &texelOverrides[kind], source & 0xffffff );
}

/* ---------------------------------------------------------------- pictures */

static unsigned rd16( const byte *p ) { return p[0] | ( p[1] << 8 ); }
static unsigned rd32( const byte *p ) { return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (unsigned)p[3] << 24 ); }

/* an uncompressed BMP of 1, 4, 8, 24 or 32 bits (the games' own are 4 and 8) as RGBA, magenta clear */
static byte *OE_DecodeBmp( const byte *b, int len, int *w, int *h ) {
	unsigned ofs, hdr, bpp, comp, ncol, x, y, stride;
	int width, height, top;
	byte *out;

	if ( len < 54 || b[0] != 'B' || b[1] != 'M' ) {
		return NULL;
	}
	ofs = rd32( b + 10 );
	hdr = rd32( b + 14 );
	width = (int)rd32( b + 18 );
	height = (int)rd32( b + 22 );
	bpp = rd16( b + 28 );
	comp = rd32( b + 30 );
	ncol = rd32( b + 46 );
	top = height < 0;
	if ( top ) {
		height = -height;
	}
	if ( width <= 0 || height <= 0 || width > 8192 || height > 8192 || ( comp != 0 && comp != 3 ) ||
		( bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32 ) ) {
		return NULL;
	}
	if ( !ncol ) {
		ncol = bpp <= 8 ? 1u << bpp : 0;
	}
	stride = ( ( width * bpp + 31 ) / 32 ) * 4;
	if ( ofs + (unsigned long long)stride * height > (unsigned)len || 14 + hdr + ncol * 4 > (unsigned)len ) {
		return NULL;
	}
	out = Z_Malloc( width * height * 4 );
	for ( y = 0; y < (unsigned)height; y++ ) {
		const byte *row = b + ofs + stride * ( top ? y : height - 1 - y );
		byte *o = out + y * width * 4;
		for ( x = 0; x < (unsigned)width; x++, o += 4 ) {
			const byte *c;
			unsigned i;
			if ( bpp == 24 || bpp == 32 ) {
				c = row + x * ( bpp / 8 );
				o[0] = c[2], o[1] = c[1], o[2] = c[0];
				o[3] = 255;	/* (32-bit BMPs rarely carry a real alpha: magenta is the key) */
			} else {
				i = bpp == 8 ? row[x] : bpp == 4 ? ( x & 1 ? row[x >> 1] & 15 : row[x >> 1] >> 4 ) :
					( row[x >> 3] >> ( 7 - ( x & 7 ) ) ) & 1;
				c = i < ncol ? b + 14 + hdr + i * 4 : b + 14 + hdr;
				o[0] = c[2], o[1] = c[1], o[2] = c[0], o[3] = 255;
			}
			if ( ( o[0] & 0xf8 ) == 0xf8 && ( o[1] & 0xfc ) == 0 && ( o[2] & 0xf8 ) == 0xf8 ) {
				o[3] = 0;	/* the games' colour key: whatever becomes RGB565 0xf81f, as in Image_load */
			}
		}
	}
	*w = width;
	*h = height;
	return out;
}

byte *OE_LoadImageRGBA( const char *name, int *w, int *h ) {
	const char *ext = COM_GetExtension( name );
	byte *pic = NULL;
	void *data;
	int len;

	*w = *h = 0;
	if ( !Q_stricmp( ext, "bmp" ) ) {
		len = FS_ReadFile( name, &data );
		if ( len > 0 && data ) {
			pic = OE_DecodeBmp( data, len, w, h );
			FS_FreeFile( data );
		}
	} else {
		pic = CL_LoadImageRGBA( name, w, h );
	}
	if ( !pic ) {
		Com_Printf( S_COLOR_YELLOW "cannot read the picture %s\n", name );
	}
	return pic;
}

static unsigned short OE_To565( const byte *c ) {
	unsigned short v;
	if ( c[3] < 128 ) {
		return 0xf81f;	/* clear */
	}
	v = (unsigned short)( ( ( c[0] & 0xf8 ) << 8 ) | ( ( c[1] & 0xfc ) << 3 ) | ( c[2] >> 3 ) );
	return v;	/* (magenta-ish is clear, as the game's own pictures) */
}

/* RGBA (sw x sh) -> RGB565 (dw x dh): the average of the source pixels each one covers */
static void OE_Resample( const byte *src, int sw, int sh, unsigned short *dst, int dw, int dh ) {
	int x, y;
	for ( y = 0; y < dh; y++ ) {
		int y0 = y * sh / dh, y1 = ( y + 1 ) * sh / dh;
		if ( y1 <= y0 ) {
			y1 = y0 + 1;
		}
		for ( x = 0; x < dw; x++ ) {
			int x0 = x * sw / dw, x1 = ( x + 1 ) * sw / dw, i, j, n = 0, opaque = 0;
			unsigned r = 0, g = 0, b = 0;
			byte c[4];
			if ( x1 <= x0 ) {
				x1 = x0 + 1;
			}
			for ( j = y0; j < y1; j++ ) {
				for ( i = x0; i < x1; i++ ) {
					const byte *p = src + ( j * sw + i ) * 4;
					n++;
					if ( p[3] >= 128 ) {
						opaque++;
						r += p[0], g += p[1], b += p[2];
					}
				}
			}
			if ( opaque * 2 < n ) {
				dst[y * dw + x] = 0xf81f;
				continue;
			}
			c[0] = (byte)( r / opaque ), c[1] = (byte)( g / opaque ), c[2] = (byte)( b / opaque ), c[3] = 255;
			dst[y * dw + x] = OE_To565( c );
		}
	}
}

/* the .bar bitmap of resId, decoded (Z_Malloc RGBA) */
static byte *OE_ResBarImage( int resId, int *w, int *h ) {
	const barEntry_t *e = Bar_Find( resBar, resId );
	int len = e ? Bar_UncompressedSize( resBar, e ) : -1;
	byte *raw, *pic;
	if ( len <= 0 ) {
		return NULL;
	}
	raw = Z_Malloc( len );
	pic = Bar_Read( resBar, e, raw, len ) ? OE_DecodeBmp( raw, len, w, h ) : NULL;
	Z_Free( raw );
	return pic;
}

/* 0 not a font, 1 the small one (9 x 12 cells, 7 apart), 2 the large one (13 x 17, 10 apart) */
static int OE_ResFontOf( int resId ) {
	const barEntry_t *e = Bar_Find( resBar, resId );
	if ( !e ) {
		return 0;
	}
	if ( !Q_stricmp( e->name, "font.bmp" ) ) {
		return 1;
	}
	if ( !Q_stricmp( e->name, "larger_font.bmp" ) || !Q_stricmp( e->name, "font_lg.bmp" ) ) {
		return 2;
	}
	return 0;
}

/* The id Tech 3 font laid into a game's font sheet (glyph c - 0x21 at column & 15, row >> 4), at
 * t device pixels per game pixel: its characters in the sheet's main colour (the one the game
 * tints) with a shadow in its darkest; the game's own glyphs past '~' (runes, icons) stay. */
static unsigned short *OE_ResIdTech3Font( int resId, int t, int *outW, int *outH ) {
	int font = OE_ResFontOf( resId ), cellW = font == 2 ? 13 : 9, cellH = font == 2 ? 17 : 12;
	int adv = font == 2 ? 10 : 7, w, h, cw, ch, x, y, g, i;
	int counts[64], best = 0;
	byte *sheet, *chars;
	unsigned short *out, main = 0xffff, shadow = 0, c;
	unsigned darkest = 0xffffffff;

	sheet = OE_ResBarImage( resId, &w, &h );
	chars = sheet ? CL_LoadImageRGBA( "gfx/2d/bigchars", &cw, &ch ) : NULL;
	if ( !chars || cw < 16 || ch < 16 ) {
		if ( sheet ) {
			Z_Free( sheet );
			Com_Printf( S_COLOR_YELLOW "oe_useidtech3fonts: no gfx/2d/bigchars in the search path\n" );
		}
		if ( chars ) {
			Z_Free( chars );
		}
		return NULL;
	}

	/* the sheet's colours: the most common one is the text, the darkest its shadow */
	Com_Memset( counts, 0, sizeof( counts ) );
	for ( i = 0; i < w * h; i++ ) {
		const byte *p = sheet + i * 4;
		unsigned lum = p[0] * 2 + p[1] * 5 + p[2];
		if ( p[3] < 128 ) {
			continue;
		}
		counts[( p[0] >> 6 ) << 4 | ( p[1] >> 6 ) << 2 | p[2] >> 6]++;
		if ( lum < darkest ) {
			darkest = lum;
			shadow = OE_To565( p );
		}
	}
	/* (of the light colours: the outline is often the most common one) */
	best = -1;
	for ( i = 0; i < 64; i++ ) {
		int lum = ( ( i >> 4 ) * 2 + ( ( i >> 2 ) & 3 ) * 5 + ( i & 3 ) ) * 85 / 8;
		if ( lum >= 96 && counts[i] && ( best < 0 || counts[i] > counts[best] ) ) {
			best = i;
		}
	}
	if ( best < 0 ) {
		for ( best = 0, i = 1; i < 64; i++ ) {
			if ( counts[i] > counts[best] ) {
				best = i;
			}
		}
	}
	for ( i = 0; i < w * h; i++ ) {
		const byte *p = sheet + i * 4;
		if ( p[3] >= 128 && ( ( p[0] >> 6 ) << 4 | ( p[1] >> 6 ) << 2 | p[2] >> 6 ) == best ) {
			main = OE_To565( p );	/* an exact colour of that group (the game tints by key colour) */
			break;
		}
	}

	/* the sheet magnified, then the characters drawn over their cells */
	out = Z_Malloc( w * t * h * t * sizeof( *out ) );
	OE_Resample( sheet, w, h, out, w * t, h * t );
	for ( g = 0; g < 0x7f - 0x21; g++ ) {
		int c0 = g + 0x21, gx = ( g & 15 ) * cellW * t, gy = ( g >> 4 ) * cellH * t;
		int bw = ( adv + 1 ) * t, bh = ( cellH - 1 ) * t, sx = ( c0 & 15 ) * ( cw / 16 ), sy = ( c0 >> 4 ) * ( ch / 16 );
		int sh = t > 1 ? t / 2 : 1;
		if ( gy + cellH * t > h * t || gx + cellW * t > w * t ) {
			break;
		}
		for ( y = 0; y < cellH * t; y++ ) {
			for ( x = 0; x < cellW * t; x++ ) {
				out[( gy + y ) * w * t + gx + x] = 0xf81f;
			}
		}
		for ( y = 0; y < bh; y++ ) {
			for ( x = 0; x < bw; x++ ) {
				/* bilinear coverage of the 16 x 16 character cell */
				float u = ( x + 0.5f ) * ( cw / 16 ) / bw - 0.5f, v = ( y + 0.5f ) * ( ch / 16 ) / bh - 0.5f;
				int u0 = (int)floor( u ), v0 = (int)floor( v ), k;
				float fu = u - u0, fv = v - v0, a = 0;
				for ( k = 0; k < 4; k++ ) {
					int uu = u0 + ( k & 1 ), vv = v0 + ( k >> 1 );
					float wgt = ( k & 1 ? fu : 1 - fu ) * ( k >> 1 ? fv : 1 - fv );
					if ( uu >= 0 && vv >= 0 && uu < cw / 16 && vv < ch / 16 ) {
						a += wgt * chars[( ( sy + vv ) * cw + sx + uu ) * 4 + 3];
					}
				}
				if ( a >= 128 ) {
					unsigned short *o = &out[( gy + y ) * w * t + gx + x];
					o[0] = main;
					/* shadow down and right, under nothing of the glyph */
					if ( x + sh < cellW * t && y + sh < cellH * t ) {
						c = o[sh * w * t + sh];
						if ( c == 0xf81f ) {
							o[sh * w * t + sh] = shadow;
						}
					}
				}
			}
		}
	}
	Z_Free( sheet );
	Z_Free( chars );
	*outW = w * t;
	*outH = h * t;
	return out;
}

/* the picture shown for resId at screen scale t, sized a whole multiple of the original (W*s x H*s, s <= t) */
static unsigned short *OE_ResMakeImage( int resId, int t, int *outW, int *outH ) {
	const char *path = OE_ResLookup( &resOverrides, resId );
	byte *pic, *orig;
	int w, h, ow, oh, s;
	unsigned short *out;

	if ( !path ) {
		if ( oe_useidtech3fonts && oe_useidtech3fonts->integer && OE_ResFontOf( resId ) ) {
			return OE_ResIdTech3Font( resId, t, outW, outH );
		}
		return NULL;
	}
	orig = OE_ResBarImage( resId, &ow, &oh );
	pic = orig ? OE_LoadImageRGBA( path, &w, &h ) : NULL;
	if ( !pic ) {
		if ( orig ) {
			Z_Free( orig );
		}
		return NULL;
	}
	Z_Free( orig );
	/* as many device pixels per game pixel as the picture has (rounded), at most the screen's */
	s = ( w + ow / 2 ) / ow;
	if ( s > t ) {
		s = t;
	}
	if ( s < 1 ) {
		s = 1;
	}
	out = Z_Malloc( ow * s * oh * s * sizeof( *out ) );
	OE_Resample( pic, w, h, out, ow * s, oh * s );
	Z_Free( pic );
	*outW = ow * s;
	*outH = oh * s;
	return out;
}

qboolean OE_ResImage( int resId, int scale, unsigned short *out, int maxPixels, int *w, int *h ) {
	if ( !resBar || !OE_ResIsImage( resId ) ) {
		return qfalse;
	}
	if ( resQvm ) {
		scale = 1;	/* the bytecode draws the game screen, the window magnifies it */
	}
	if ( scale < 1 || scale > 16 ) {
		scale = 1;
	}
	if ( resCache.resId != resId || resCache.scale != scale ) {
		if ( resCache.px ) {
			Z_Free( resCache.px );
		}
		resCache.px = OE_ResMakeImage( resId, scale, &resCache.w, &resCache.h );
		resCache.resId = resId;
		resCache.scale = scale;
	}
	if ( !resCache.px ) {
		return qfalse;
	}
	*w = resCache.w;
	*h = resCache.h;
	if ( out && maxPixels >= resCache.w * resCache.h ) {
		Com_Memcpy( out, resCache.px, resCache.w * resCache.h * sizeof( *out ) );
	}
	return qtrue;
}

/* ---------------------------------------------------------------- oe_export

oe_export writes, into export\ in the game's home folder, what a mod starts
from: the .bar's pictures as PNG (ui/<name>.png), its other named resources as
they are (data/<name>), an overrides.txt naming them all (every line commented
out: a mod keeps the lines of what it changes), then the game module adds the
3D view's textures (Textures/WTEXELS/WALLn.png ...) with their WTEXELS.txt,
STEXELS.txt and FTEXELS.txt. The folder is a mod as it is: zip it into a .pk3. */

static void OE_PngChunk( byte *out, int *pos, const char *type, const byte *data, int len ) {
	byte *p = out + *pos;
	unsigned crc;
	p[0] = (byte)( len >> 24 ), p[1] = (byte)( len >> 16 ), p[2] = (byte)( len >> 8 ), p[3] = (byte)len;
	Com_Memcpy( p + 4, type, 4 );
	if ( len ) {
		Com_Memcpy( p + 8, data, len );
	}
	crc = (unsigned)crc32( 0L, p + 4, (uInt)( len + 4 ) );
	p[8 + len] = (byte)( crc >> 24 ), p[9 + len] = (byte)( crc >> 16 ), p[10 + len] = (byte)( crc >> 8 ), p[11 + len] = (byte)crc;
	*pos += len + 12;
}

/* RGBA -> PNG (malloc), the image data as stored deflate blocks (this engine has no compressor) */
static byte *OE_EncodePng( const byte *rgba, int w, int h, int *outLen ) {
	int rawLen = ( w * 4 + 1 ) * h, blocks = rawLen / 65535 + 1, zLen = 2 + rawLen + blocks * 5 + 4;
	byte *raw = malloc( rawLen ), *z = malloc( zLen ), *png = malloc( zLen + 64 );
	byte ihdr[13];
	int y, pos, zp = 0, left;
	unsigned adler;

	for ( y = 0; y < h; y++ ) {
		raw[y * ( w * 4 + 1 )] = 0;	/* filter: none */
		Com_Memcpy( raw + y * ( w * 4 + 1 ) + 1, rgba + y * w * 4, w * 4 );
	}
	z[zp++] = 0x78, z[zp++] = 0x01;
	for ( pos = 0, left = rawLen; left > 0 || pos == 0; ) {
		int n = left > 65535 ? 65535 : left;
		z[zp++] = left <= 65535 ? 1 : 0;	/* BFINAL, stored */
		z[zp++] = (byte)n, z[zp++] = (byte)( n >> 8 ), z[zp++] = (byte)~n, z[zp++] = (byte)( ~n >> 8 );
		Com_Memcpy( z + zp, raw + pos, n );
		zp += n, pos += n, left -= n;
		if ( !n ) {
			break;
		}
	}
	adler = (unsigned)adler32( 1L, raw, (uInt)rawLen );
	z[zp++] = (byte)( adler >> 24 ), z[zp++] = (byte)( adler >> 16 ), z[zp++] = (byte)( adler >> 8 ), z[zp++] = (byte)adler;

	Com_Memcpy( png, "\x89PNG\r\n\x1a\n", 8 );
	pos = 8;
	ihdr[0] = (byte)( w >> 24 ), ihdr[1] = (byte)( w >> 16 ), ihdr[2] = (byte)( w >> 8 ), ihdr[3] = (byte)w;
	ihdr[4] = (byte)( h >> 24 ), ihdr[5] = (byte)( h >> 16 ), ihdr[6] = (byte)( h >> 8 ), ihdr[7] = (byte)h;
	ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = ihdr[11] = ihdr[12] = 0;	/* 8-bit RGBA */
	OE_PngChunk( png, &pos, "IHDR", ihdr, 13 );
	OE_PngChunk( png, &pos, "IDAT", z, zp );
	OE_PngChunk( png, &pos, "IEND", NULL, 0 );
	free( raw );
	free( z );
	*outLen = pos;
	return png;
}

void OE_ExportFile( const char *path, const void *data, int len, int w, int h ) {
	char name[MAX_QPATH];
	if ( !path[0] || strstr( path, ".." ) || strchr( path, ':' ) || path[0] == '/' || path[0] == '\\' ) {
		Com_Printf( S_COLOR_YELLOW "oe_export: bad path %s\n", path );
		return;
	}
	Com_sprintf( name, sizeof( name ), "export/%s", path );
	if ( w > 0 && h > 0 ) {
		int pngLen;
		byte *png = OE_EncodePng( data, w, h, &pngLen );
		FS_WriteFile( name, png, pngLen );
		free( png );
	} else {
		FS_WriteFile( name, data, len );
	}
}

/* several .bar resources may share a name: those are exported and listed by number */
static qboolean OE_ResNameShared( const barEntry_t *e ) {
	int i;
	for ( i = 0; i < resBar->count; i++ ) {
		if ( &resBar->entries[i] != e && resBar->entries[i].type == 6 && !Q_stricmp( resBar->entries[i].name, e->name ) ) {
			return qtrue;
		}
	}
	return qfalse;
}

void OE_Export_f( void ) {
	char *list;
	int i, size = 65536, used = 0, images = 0, files = 0;

	if ( !resBar ) {
		Com_Printf( "oe_export: start a game first\n" );
		return;
	}
	list = Z_Malloc( size );
	Com_sprintf( list + used, size - used,
		"// oe-client: the .bar's resources, exported by oe_export. A mod keeps (uncommented) the\r\n"
		"// lines of what it changes; pictures may be larger (HD), they cover the original's area.\r\n" );
	used += (int)strlen( list + used );
	for ( i = 0; i < resBar->count; i++ ) {
		const barEntry_t *e = &resBar->entries[i];
		int n = (int)strlen( e->name ), len, w, h;
		char key[MAX_QPATH], file[MAX_QPATH];
		byte *data, *pic;
		if ( e->type != 6 || !n || strchr( e->name, '/' ) || strchr( e->name, '\\' ) || used > size - 256 ) {
			continue;
		}
		len = Bar_UncompressedSize( resBar, e );
		if ( len <= 0 ) {
			continue;
		}
		data = Z_Malloc( len );
		if ( !Bar_Read( resBar, e, data, len ) ) {
			Z_Free( data );
			continue;
		}
		/* a name several resources share: listed by number, the file named after both */
		if ( OE_ResNameShared( e ) ) {
			Com_sprintf( key, sizeof( key ), "%d", e->id );
			Com_sprintf( file, sizeof( file ), "%d_%s", e->id, e->name );
		} else {
			Q_strncpyz( key, e->name, sizeof( key ) );
			Q_strncpyz( file, e->name, sizeof( file ) );
		}
		if ( n > 4 && !Q_stricmp( e->name + n - 4, ".bmp" ) && ( pic = OE_DecodeBmp( data, len, &w, &h ) ) ) {
			char png[MAX_QPATH];
			COM_StripExtension( file, png, sizeof( png ) );
			OE_ExportFile( va( "ui/%s.png", png ), pic, 0, w, h );
			Com_sprintf( list + used, size - used, "//%s=ui/%s.png\r\n", key, png );
			used += (int)strlen( list + used );
			Z_Free( pic );
			images++;
		} else {
			OE_ExportFile( va( "data/%s", file ), data, len, 0, 0 );
			Com_sprintf( list + used, size - used, "//%s=data/%s\r\n", key, file );
			used += (int)strlen( list + used );
			files++;
		}
		Z_Free( data );
	}
	OE_ExportFile( "overrides.txt", list, used, 0, 0 );
	Z_Free( list );
	Com_Printf( "oe_export: %d pictures, %d other resources\n", images, files );

	/* the 3D view's textures: the game module knows their formats */
	Cmd_TokenizeString( "oe_export" );
	if ( !OE_GameCommand() ) {
		Com_Printf( S_COLOR_YELLOW "oe_export: this game module exports no textures\n" );
	}
	Com_Printf( "oe_export: written to %s\n", FS_BuildOSPath( Cvar_VariableString( "fs_homepath" ),
		Cvar_VariableString( "fs_game" ), "export" ) );
}
