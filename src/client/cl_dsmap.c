/*
===========================================================================
This file is part of the Orcs & Elves client, based on Tremulous.

It is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.
===========================================================================
*/
// cl_dsmap.c -- Orcs & Elves DS (fs_game oeds): the user's cartridge dump, rom.nds in the game
// folder, read in place (its NitroFS files; data/<path> in the game folder or a pk3 replaces one),
// and its maps drawn with the Quake 3 renderer.
//
// Every BSP leaf of a map has an NDS GX display list in maps/<map>.lz. It is run once, at load,
// into textured triangles (one vertex array per texture and blend), which go to RE_AddPolyToScene
// every frame; the textures come from shaders.bin, textures/texels.bin and textures/palettes.bin.
//
// The map's sprites (.bsp) are drawn the way the game's Render_drawSprite does (0x020491d4): a
// shader with a program (the bytes after it in shaders.bin, run by 0x0204c73c) builds the object
// from transforms, textures, models of textures/models.bin and quads; one without draws a
// billboard (0x020484fc). Both are lit from maps/<map>.light. README-OE-DS-MAPS.md describes the
// formats. The map is explored with the free camera (cl_hw.c), walking with collisions against
// the map's wall lines (oe_dsWalk) or flying; the DS game itself is not here yet.

#include "client.h"

#define DS_ROM			"rom.nds"
#define DS_MAX_FILES	256
#define DS_MAX_MATS		512			// the map's materials
#define DS_MAX_DYN		512			// the sprites' (rebuilt every frame)
#define DS_EYE			36			// above the floor
#define DS_RADIUS		12			// walking: the player's size against the walls

// texture variants (from the TEXIMAGE_PARAM)
#define DS_V_CLEAR0		1			// colour 0 is transparent
#define DS_V_REPEAT		2			// repeats (S or T); else clamped
#define DS_V_FLIPS		4			// mirrored repeat in S
#define DS_V_FLIPT		8			// in T

typedef enum {
	DS_OPAQUE,
	DS_ALPHATEST,					// colour 0 clear
	DS_BLEND						// translucent texels (A3I5, A5I3) or polygon alpha
} dsBlend_t;

// which sides of a polygon are drawn: POLYGON_ATTR bits 6 (back) and 7 (front), from a list's 0x29 or a
// program's 0x16 (its flags byte & 3, 0x0204be88)
typedef enum {
	DS_CULL_NONE,					// both sides (or neither: not left out here)
	DS_CULL_BACK,					// the front only
	DS_CULL_FRONT					// the back only
} dsCull_t;

static int DS_CullOf( int sides ) {
	return sides == 2 ? DS_CULL_BACK : sides == 1 ? DS_CULL_FRONT : DS_CULL_NONE;
}

typedef struct {
	char		path[MAX_QPATH];	// "maps/map_m01.bsp"
	unsigned	ofs, size;
} dsFile_t;

typedef struct {
	int			fmt, sLog, tLog, w, h;
	qboolean	big;				// drawn at 2/3 of its size (sprites of 96 texels for 64 units)
	int			start, end;			// LZ10 stream in texels.bin
} dsTex_t;

typedef struct {
	int			flag;				// 1: lit from the map's light grid
	int			key, pal, tex;		// key: id * 16 + frame
	int			prog, progLen;		// its program in ds.progs
} dsShader_t;

typedef struct {
	int			sh;					// shader index, -1 untextured
	int			variant, blend, cull;
	int			imgW, imgH;			// the uploaded image (texture size, doubled where mirrored)
	polyVert_t	*verts;				// triangles
	int			numVerts, maxVerts;
	qhandle_t	shader;
	int			shaderGen;
} dsMat_t;

typedef struct {
	dsMat_t		*mats;
	int			*num, max;
} dsPool_t;

typedef struct {
	int			x, y, z;			// world units (z: 32 = standing on the floor)
	unsigned	info;				// low byte: tile; 0x4000: wall texture; 0xf00: frame; 0x78000: on a wall
	int			angle;				// 1024 a turn
	int			scale;				// 64 = 1.0 (0: 64), the game's sprite +0x15 (smaller monsters)
} dsSprite_t;

typedef struct {
	float		x1, y1, x2, y2;
} dsWall_t;

typedef struct {
	float		x1, y1, x2, y2;
	int			tex;				// the line's texture byte: shader 0x101 + it
	unsigned	flags;				// 0x10000: two tiles high
} dsDoor_t;

static struct {
	qboolean	romOk;
	char		code[5];
	dsFile_t	files[DS_MAX_FILES];
	int			numFiles;

	// shaders.bin, the texture data and the models
	byte		*texels, *pals, *progs, *models;
	int			texelsSize, palsSize, progsSize, modelsSize;
	dsTex_t		*tex;
	int			numTex;
	int			*palEnd;
	int			numPal;
	int			*modelOfs;
	int			numModels;
	dsShader_t	*shaders;
	int			numShaders;

	// the map
	char		map[MAX_QPATH];
	dsMat_t		mats[DS_MAX_MATS];
	int			numMats, numTris, numLeaves;
	dsMat_t		dyn[DS_MAX_DYN];
	int			numDyn;
	dsSprite_t	*sprites;
	int			numSprites;
	dsWall_t	*walls;
	int			numWalls;
	dsDoor_t	*doors;				// the moving lines (doors, gates) of the .bsp
	int			numDoors;
	dsSprite_t	*doorModels;		// the doors of the .bsp (lines with texture 0x10..0x1f): models
	int			numDoorModels;
	byte		light[0xc00];		// 32 x 32 tiles x RGB
	qboolean	hasLight;
	vec3_t		spawn;
	float		spawnYaw;
	float		eyeZ;
} ds;

// the game module's view (OE_TRAP_DS_MAP, OE_TRAP_DS_SPRITES): its map, its sprites instead of the .bsp's
static qboolean		dsModuleMap;
static dsSprite_t	*dsModuleSprites;
static int			dsNumModuleSprites;
static dsDoor_t		*dsModuleDoors;
static int			dsNumModuleDoors;

static int		dsRendererGen = 1;
static float	dsSin[1024];
static cvar_t	*oe_dsStartMap;
static cvar_t	*oe_dsWalk;
static cvar_t	*oe_dsSprites;

static unsigned DS_U16( const byte *p ) {
	return p[0] | ( p[1] << 8 );
}

static unsigned DS_U32( const byte *p ) {
	return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (unsigned)p[3] << 24 );
}

/*
=======================================================================

ROM

=======================================================================
*/

static qboolean DS_ReadRom( unsigned ofs, void *buf, int len ) {
	fileHandle_t	f;
	int				size = FS_FOpenFileRead( DS_ROM, &f, qtrue );
	qboolean		ok;

	if ( size <= 0 || !f ) {
		if ( f ) {
			FS_FCloseFile( f );
		}
		return qfalse;
	}
	ok = (unsigned)size >= ofs + (unsigned)len && FS_Seek( f, ofs, FS_SEEK_SET ) == 0 && FS_Read( buf, len, f ) == len;
	FS_FCloseFile( f );
	return ok;
}

/* checks rom.nds and lists its NitroFS files (the directory table and the allocation table) */
static qboolean DS_OpenRom( void ) {
	struct {
		int		id;
		char	path[MAX_QPATH];
	} stack[64];
	byte		hdr[0x60], *fnt, *fat;
	unsigned	fntOfs, fntSize, fatOfs, fatSize;
	int			sp = 0;

	if ( ds.romOk ) {
		return qtrue;
	}
	ds.numFiles = 0;
	if ( !DS_ReadRom( 0, hdr, sizeof( hdr ) ) ) {
		Com_Printf( S_COLOR_RED "%s: not found in the game folder\n", DS_ROM );
		return qfalse;
	}
	Com_Memcpy( ds.code, hdr + 12, 4 );
	ds.code[4] = 0;
	if ( Q_strncmp( ds.code, "YOE", 3 ) ) {
		Com_Printf( S_COLOR_RED "%s: not Orcs & Elves DS (game code \"%s\")\n", DS_ROM, ds.code );
		return qfalse;
	}
	fntOfs = DS_U32( hdr + 0x40 );
	fntSize = DS_U32( hdr + 0x44 );
	fatOfs = DS_U32( hdr + 0x48 );
	fatSize = DS_U32( hdr + 0x4c );
	if ( fntSize < 8 || fntSize > 0x100000 || fatSize > 0x100000 ) {
		Com_Printf( S_COLOR_RED "%s: no file system\n", DS_ROM );
		return qfalse;
	}
	fnt = malloc( fntSize );
	fat = malloc( fatSize + 8 );
	if ( !fnt || !fat || !DS_ReadRom( fntOfs, fnt, fntSize ) || !DS_ReadRom( fatOfs, fat, fatSize ) ) {
		free( fnt );
		free( fat );
		Com_Printf( S_COLOR_RED "%s: cannot read its file system\n", DS_ROM );
		return qfalse;
	}

	// directories have ids 0xf000 + n; the entries name files (numbered on from the
	// directory's first file id) and subdirectories
	stack[sp].id = 0;
	stack[sp].path[0] = 0;
	sp++;
	while ( sp > 0 ) {
		char		dir[MAX_QPATH];
		unsigned	p;
		int			id, fid;

		sp--;
		id = stack[sp].id;
		Q_strncpyz( dir, stack[sp].path, sizeof( dir ) );
		if ( (unsigned)id * 8 + 8 > fntSize ) {
			continue;
		}
		p = DS_U32( fnt + id * 8 );
		fid = DS_U16( fnt + id * 8 + 4 );
		while ( p < fntSize ) {
			char	name[128], full[MAX_QPATH];
			int		c = fnt[p++], len = c & 0x7f;

			if ( !c || p + len > fntSize ) {
				break;
			}
			Com_Memcpy( name, fnt + p, len );
			name[len] = 0;
			p += len;
			Com_sprintf( full, sizeof( full ), "%s%s%s", dir, dir[0] ? "/" : "", name );
			if ( c & 0x80 ) {
				if ( p + 2 > fntSize ) {
					break;
				}
				if ( sp < (int)ARRAY_LEN( stack ) ) {
					stack[sp].id = DS_U16( fnt + p ) & 0xfff;
					Q_strncpyz( stack[sp].path, full, sizeof( stack[sp].path ) );
					sp++;
				}
				p += 2;
			} else {
				if ( (unsigned)fid * 8 + 8 <= fatSize && ds.numFiles < DS_MAX_FILES ) {
					dsFile_t *f = &ds.files[ds.numFiles++];
					Q_strncpyz( f->path, full, sizeof( f->path ) );
					f->ofs = DS_U32( fat + fid * 8 );
					f->size = DS_U32( fat + fid * 8 + 4 ) - f->ofs;
				}
				fid++;
			}
		}
	}
	free( fnt );
	free( fat );
	ds.romOk = qtrue;
	Com_Printf( "Orcs & Elves DS: %s (%s), %d files\n", DS_ROM, ds.code, ds.numFiles );
	return qtrue;
}

static byte *DS_Lz10( const byte *s, int len, int *outSize );

/* a file of the cartridge as stored (malloc), data/<path> on the search path first; NULL if missing */
static byte *DS_LoadStored( const char *path, int *size ) {
	void	*buf;
	byte	*out;
	int		i, n = FS_ReadFile( va( "data/%s", path ), &buf );

	if ( n >= 0 && buf ) {
		out = malloc( n + 1 );
		if ( out ) {
			Com_Memcpy( out, buf, n );
			*size = n;
		}
		FS_FreeFile( buf );
		return out;
	}
	for ( i = 0; i < ds.numFiles; i++ ) {
		if ( Q_stricmp( ds.files[i].path, path ) ) {
			continue;
		}
		out = malloc( ds.files[i].size + 1 );
		if ( !out || !DS_ReadRom( ds.files[i].ofs, out, ds.files[i].size ) ) {
			free( out );
			return NULL;
		}
		*size = ds.files[i].size;
		return out;
	}
	return NULL;
}

/* a file of the cartridge (malloc); <path>.lz (LZ10) when there is no <path>; NULL if missing */
static byte *DS_LoadFile( const char *path, int *size ) {
	byte	*raw, *out;
	int		rawSize;

	if ( ( out = DS_LoadStored( path, size ) ) != NULL ) {
		return out;
	}
	if ( !( raw = DS_LoadStored( va( "%s.lz", path ), &rawSize ) ) ) {
		return NULL;
	}
	out = DS_Lz10( raw, rawSize, size );
	free( raw );
	return out;
}

/* Nintendo LZ10: 0x10, u24 size, flag bytes (MSB first, 1 = 2-byte back reference of 3..18) */
static byte *DS_Lz10( const byte *s, int len, int *outSize ) {
	int		n, p = 4, d = 0;
	byte	*o;

	if ( len < 4 || s[0] != 0x10 ) {
		return NULL;
	}
	n = s[1] | ( s[2] << 8 ) | ( s[3] << 16 );
	o = malloc( n > 0 ? n : 1 );
	if ( !o ) {
		return NULL;
	}
	while ( d < n && p < len ) {
		int f = s[p++], i;
		for ( i = 0; i < 8 && d < n; i++, f <<= 1 ) {
			int a, b, l, disp, k;
			if ( !( f & 0x80 ) ) {
				if ( p >= len ) {
					break;
				}
				o[d++] = s[p++];
				continue;
			}
			if ( p + 1 >= len ) {
				break;
			}
			a = s[p++];
			b = s[p++];
			l = ( a >> 4 ) + 3;
			disp = ( ( ( a & 15 ) << 8 ) | b ) + 1;
			if ( disp > d ) {
				free( o );
				return NULL;
			}
			for ( k = 0; k < l && d < n; k++, d++ ) {
				o[d] = o[d - disp];
			}
		}
	}
	*outSize = d;
	return o;
}

/*
=======================================================================

TEXTURES

=======================================================================
*/

static void DS_FreeTextures( void ) {
	free( ds.texels );
	free( ds.pals );
	free( ds.progs );
	free( ds.models );
	free( ds.tex );
	free( ds.palEnd );
	free( ds.modelOfs );
	free( ds.shaders );
	ds.texels = ds.pals = ds.progs = ds.models = NULL;
	ds.tex = NULL;
	ds.palEnd = ds.modelOfs = NULL;
	ds.shaders = NULL;
	ds.numTex = ds.numPal = ds.numModels = ds.numShaders = 0;
}

/* shaders.bin: counts (palettes, textures, models, shaders), then the textures, the palettes'
 * end offsets, the models' offsets and the shaders (each with its program) */
static qboolean DS_LoadTextures( void ) {
	byte	*b;
	int		size, p = 16, i, prev = 0;

	if ( ds.tex ) {
		return qtrue;
	}
	b = DS_LoadFile( "shaders.bin", &size );
	ds.texels = DS_LoadFile( "textures/texels.bin", &ds.texelsSize );
	ds.pals = DS_LoadFile( "textures/palettes.bin", &ds.palsSize );
	ds.models = DS_LoadFile( "textures/models.bin", &ds.modelsSize );
	if ( !b || !ds.texels || !ds.pals || size < 16 ) {
		Com_Printf( S_COLOR_RED "Orcs & Elves DS: shaders.bin or the textures are missing\n" );
		free( b );
		DS_FreeTextures();
		return qfalse;
	}
	ds.numPal = DS_U32( b );
	ds.numTex = DS_U32( b + 4 );
	ds.numModels = DS_U32( b + 8 );
	ds.numShaders = DS_U32( b + 12 );
	if ( ds.numPal > 0x10000 || ds.numTex > 0x10000 || ds.numModels > 0x10000 || ds.numShaders > 0x10000 ) {
		goto bad;
	}
	ds.tex = calloc( ds.numTex + 1, sizeof( *ds.tex ) );
	ds.palEnd = calloc( ds.numPal + 1, sizeof( *ds.palEnd ) );
	ds.modelOfs = calloc( ds.numModels + 1, sizeof( *ds.modelOfs ) );
	ds.shaders = calloc( ds.numShaders + 1, sizeof( *ds.shaders ) );
	ds.progs = malloc( size );
	if ( !ds.tex || !ds.palEnd || !ds.modelOfs || !ds.shaders || !ds.progs ) {
		goto bad;
	}
	for ( i = 0; i < ds.numTex; i++, p += 11 ) {
		dsTex_t *t = &ds.tex[i];
		if ( p + 11 > size ) {
			goto bad;
		}
		t->fmt = b[p] & 15;
		t->big = ( b[p] & 0x80 ) != 0;
		t->sLog = b[p + 1] & 7;
		t->tLog = b[p + 2] & 7;
		t->w = b[p + 3] ? b[p + 3] : 256;
		t->h = b[p + 4] ? b[p + 4] : 256;
		t->start = prev;
		t->end = prev = DS_U32( b + p + 5 );
	}
	for ( i = 0; i < ds.numPal; i++, p += 4 ) {
		if ( p + 4 > size ) {
			goto bad;
		}
		ds.palEnd[i] = DS_U32( b + p );
	}
	// the models' end offsets (as the palettes'): model i is modelOfs[i] .. modelOfs[i + 1]
	ds.modelOfs[0] = 0;
	for ( i = 0; i < ds.numModels; i++, p += 4 ) {
		if ( p + 4 > size ) {
			goto bad;
		}
		ds.modelOfs[i + 1] = DS_U32( b + p );
	}
	ds.progsSize = 0;
	for ( i = 0; i < ds.numShaders; i++ ) {
		dsShader_t	*s = &ds.shaders[i];
		int			n;
		if ( p + 8 > size || p + 8 + b[p + 7] > size ) {
			goto bad;
		}
		s->flag = b[p];
		s->key = DS_U16( b + p + 1 );
		s->pal = DS_U16( b + p + 3 );
		s->tex = DS_U16( b + p + 5 );
		n = b[p + 7];
		s->prog = ds.progsSize;
		s->progLen = n;
		Com_Memcpy( ds.progs + ds.progsSize, b + p + 8, n );
		ds.progsSize += n;
		p += 8 + n;
	}
	free( b );
	Com_Printf( "Orcs & Elves DS: %d textures, %d palettes, %d shaders, %d models\n", ds.numTex, ds.numPal,
		ds.numShaders, ds.models ? ds.numModels : 0 );
	return qtrue;

bad:
	Com_Printf( S_COLOR_RED "Orcs & Elves DS: shaders.bin is not as expected\n" );
	free( b );
	DS_FreeTextures();
	return qfalse;
}

/* the shader of id and frame (key id * 16 + frame; frame 0, the first of the id, when there is none) */
static int DS_ShaderIndex( int id, int frame ) {
	int i, first = -1;
	for ( i = 0; i < ds.numShaders; i++ ) {
		if ( ( ds.shaders[i].key & 0xfff0 ) != ( id & 0xfff ) * 16 ) {
			continue;
		}
		if ( ( ds.shaders[i].key & 15 ) == ( frame & 15 ) ) {
			return i;
		}
		if ( first < 0 ) {
			first = i;
		}
	}
	return first;
}

static const dsTex_t *DS_ShaderTex( int sh ) {
	return sh >= 0 && sh < ds.numShaders && ds.shaders[sh].tex < ds.numTex ? &ds.tex[ds.shaders[sh].tex] : NULL;
}

/* one RGB555 palette colour as RGB */
static void DS_PalColor( int pal, int index, byte *out ) {
	int start = pal > 0 && pal <= ds.numPal ? ds.palEnd[pal - 1] : 0, end = pal < ds.numPal ? ds.palEnd[pal] : start;
	int ofs = start + index * 2;
	unsigned c = ofs + 2 <= end && ofs + 2 <= ds.palsSize ? DS_U16( ds.pals + ofs ) : 0x7c1f;
	out[0] = (byte)( ( c & 31 ) * 255 / 31 );
	out[1] = (byte)( ( ( c >> 5 ) & 31 ) * 255 / 31 );
	out[2] = (byte)( ( ( c >> 10 ) & 31 ) * 255 / 31 );
}

/* decodes the material's texture (native NDS texels) into an image of the hardware size
 * (8 << sLog by 8 << tLog, doubled where mirrored) and uploads it */
static void DS_UploadTexture( const dsMat_t *m ) {
	const dsShader_t	*sh = &ds.shaders[m->sh];
	const dsTex_t		*t = DS_ShaderTex( m->sh );
	byte				*px = NULL, *rgba;
	int					pxSize = 0, bw, bh, x, y, flags;

	if ( t && t->start < t->end && t->end <= ds.texelsSize ) {
		px = DS_Lz10( ds.texels + t->start, t->end - t->start, &pxSize );
	}
	bw = t ? 8 << t->sLog : 8;
	bh = t ? 8 << t->tLog : 8;
	rgba = Hunk_AllocateTempMemory( m->imgW * m->imgH * 4 );
	for ( y = 0; y < m->imgH; y++ ) {
		for ( x = 0; x < m->imgW; x++ ) {
			byte	*o = rgba + ( y * m->imgW + x ) * 4;
			int		sx = x < bw ? x : 2 * bw - 1 - x, sy = y < bh ? y : 2 * bh - 1 - y;
			int		i, v, a = 255;

			if ( !px || !t ) {
				o[0] = o[2] = 255, o[1] = 0, o[3] = 255;
				continue;
			}
			// rows are stored w texels long (the game lays them out at the VRAM width itself);
			// past the stored texels the edge repeats
			if ( sy >= t->h ) {
				sy = t->h - 1;
			}
			if ( sx >= t->w ) {
				sx = t->w - 1;
			}
			i = sy * t->w + sx;
			switch ( t->fmt ) {
			case 1:		// A3I5
				v = i < pxSize ? px[i] : 0;
				DS_PalColor( sh->pal, v & 31, o );
				a = ( v >> 5 ) * 255 / 7;
				break;
			case 2:		// 4 colours
				v = i / 4 < pxSize ? ( px[i / 4] >> ( ( i & 3 ) * 2 ) ) & 3 : 0;
				DS_PalColor( sh->pal, v, o );
				a = v == 0 && ( m->variant & DS_V_CLEAR0 ) ? 0 : 255;
				break;
			case 3:		// 16 colours
				v = i / 2 < pxSize ? ( px[i / 2] >> ( ( i & 1 ) * 4 ) ) & 15 : 0;
				DS_PalColor( sh->pal, v, o );
				a = v == 0 && ( m->variant & DS_V_CLEAR0 ) ? 0 : 255;
				break;
			case 4:		// 256 colours
				v = i < pxSize ? px[i] : 0;
				DS_PalColor( sh->pal, v, o );
				a = v == 0 && ( m->variant & DS_V_CLEAR0 ) ? 0 : 255;
				break;
			case 6:		// A5I3
				v = i < pxSize ? px[i] : 0;
				DS_PalColor( sh->pal, v & 7, o );
				a = ( v >> 3 ) * 255 / 31;
				break;
			case 7:		// direct colour
				v = i * 2 + 1 < pxSize ? DS_U16( px + i * 2 ) : 0;
				o[0] = (byte)( ( v & 31 ) * 255 / 31 );
				o[1] = (byte)( ( ( v >> 5 ) & 31 ) * 255 / 31 );
				o[2] = (byte)( ( ( v >> 10 ) & 31 ) * 255 / 31 );
				a = ( v & 0x8000 ) ? 255 : 0;
				break;
			default:	// 4x4 compressed: not used by the game
				o[0] = o[2] = 255, o[1] = 0;
				break;
			}
			o[3] = (byte)a;
		}
	}
	free( px );
	flags = IMGFLAG_MIPMAP;
	if ( !Cvar_VariableIntegerValue( "oe_hwFilter" ) ) {
		flags |= IMGFLAG_NEAREST;
	}
	if ( m->variant & DS_V_REPEAT ) {
		flags |= IMGFLAG_REPEAT;
	}
	re.UploadImageEx( va( "*ds/%d/%d", m->sh, m->variant ), rgba, m->imgW, m->imgH, flags );
	Hunk_FreeTempMemory( rgba );
}

static qhandle_t DS_MatShader( dsMat_t *m ) {
	static const char *culls[3] = { "none", "back", "front" };	// (the DS axes mirror the winding)
	static const char *stages[3] = {
		"rgbGen vertex",
		"alphaFunc GE128 rgbGen vertex",
		"blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA rgbGen vertex alphaGen vertex"
	};

	if ( m->shaderGen == dsRendererGen ) {
		return m->shader;
	}
	m->shaderGen = dsRendererGen;
	if ( m->sh < 0 ) {
		m->shader = re.RegisterShaderText( va( "*ds/none/%d/%d", m->blend, m->cull ),
			va( "{ cull %s nopicmip { map $whiteimage %s } }", culls[m->cull], stages[m->blend] ) );
		return m->shader;
	}
	DS_UploadTexture( m );
	m->shader = re.RegisterShaderText( va( "*ds/%d/%d/%d/%d", m->sh, m->variant, m->blend, m->cull ),
		va( "{ cull %s nopicmip { %s *ds/%d/%d %s } }", culls[m->cull], ( m->variant & DS_V_REPEAT ) ? "map" : "clampMap",
			m->sh, m->variant, stages[m->blend] ) );
	return m->shader;
}

/*
=======================================================================

GEOMETRY

=======================================================================
*/

/* the material in a pool for a shader index, TEXIMAGE_PARAM and blend; NULL when it is full */
static dsMat_t *DS_Material( const dsPool_t *pool, int sh, unsigned param, int blend, int cull ) {
	const dsTex_t	*t = DS_ShaderTex( sh );
	int				variant = 0, i;
	dsMat_t			*m;

	if ( !t ) {
		sh = -1;
	} else {
		if ( param & ( 1 << 29 ) ) {
			variant |= DS_V_CLEAR0;
		}
		if ( param & ( 3 << 16 ) ) {
			variant |= DS_V_REPEAT;
		}
		if ( ( param & ( 1 << 16 ) ) && ( param & ( 1 << 18 ) ) ) {
			variant |= DS_V_FLIPS;
		}
		if ( ( param & ( 1 << 17 ) ) && ( param & ( 1 << 19 ) ) ) {
			variant |= DS_V_FLIPT;
		}
		if ( t->fmt == 1 || t->fmt == 6 ) {
			blend = DS_BLEND;
		} else if ( blend == DS_OPAQUE && ( variant & DS_V_CLEAR0 ) ) {
			blend = DS_ALPHATEST;
		}
	}
	for ( i = 0; i < *pool->num; i++ ) {
		m = &pool->mats[i];
		if ( m->sh == sh && m->variant == variant && m->blend == blend && m->cull == cull ) {
			return m;
		}
	}
	if ( *pool->num == pool->max ) {
		return NULL;
	}
	m = &pool->mats[( *pool->num )++];
	Com_Memset( m, 0, sizeof( *m ) );
	m->sh = sh;
	m->variant = variant;
	m->blend = blend;
	m->cull = cull;
	m->imgW = ( t ? 8 << t->sLog : 8 ) * ( ( variant & DS_V_FLIPS ) ? 2 : 1 );
	m->imgH = ( t ? 8 << t->tLog : 8 ) * ( ( variant & DS_V_FLIPT ) ? 2 : 1 );
	return m;
}

typedef struct {
	float	xyz[3], s, t;		// DS axes; s, t in texels
	byte	color[4];
} dsVtx_t;

static void DS_AddTri( dsMat_t *m, const dsVtx_t *a, const dsVtx_t *b, const dsVtx_t *c ) {
	const dsVtx_t	*v[3];
	int				k;

	if ( !m ) {
		return;
	}
	v[0] = a, v[1] = b, v[2] = c;
	if ( m->numVerts + 3 > m->maxVerts ) {
		int			n = m->maxVerts ? m->maxVerts * 2 : 768;
		polyVert_t	*p = realloc( m->verts, n * sizeof( polyVert_t ) );
		if ( !p ) {
			return;
		}
		m->verts = p;
		m->maxVerts = n;
	}
	for ( k = 0; k < 3; k++ ) {
		polyVert_t *o = &m->verts[m->numVerts++];
		// DS (x east, y south, z up) -> Quake 3 (y north)
		o->xyz[0] = v[k]->xyz[0];
		o->xyz[1] = -v[k]->xyz[1];
		o->xyz[2] = v[k]->xyz[2];
		o->st[0] = v[k]->s / m->imgW;
		o->st[1] = v[k]->t / m->imgH;
		Com_Memcpy( o->modulate, v[k]->color, 4 );
	}
}

/* GX matrices: row vectors, v' = v * M; a new transform applies first (M = A * M) */
static void DS_MatMul( float *cur, const float *a ) {
	float	r[16];
	int		i, j, k;
	for ( i = 0; i < 4; i++ ) {
		for ( j = 0; j < 4; j++ ) {
			float s = 0;
			for ( k = 0; k < 4; k++ ) {
				s += a[i * 4 + k] * cur[k * 4 + j];
			}
			r[i * 4 + j] = s;
		}
	}
	Com_Memcpy( cur, r, sizeof( r ) );
}

static void DS_MatIdentity( float *m ) {
	Com_Memset( m, 0, 16 * sizeof( float ) );
	m[0] = m[5] = m[10] = m[15] = 1;
}

static void DS_Scale( float *m, float x, float y, float z ) {
	float a[16];
	DS_MatIdentity( a );
	a[0] = x, a[5] = y, a[10] = z;
	DS_MatMul( m, a );
}

static void DS_Translate( float *m, float x, float y, float z ) {
	float a[16];
	DS_MatIdentity( a );
	a[12] = x, a[13] = y, a[14] = z;
	DS_MatMul( m, a );
}

/* about the X (axis 0), Y or Z axis by angle (1024 a turn), as the game's MULT_3x3s */
static void DS_Rotate( float *m, int axis, int angle ) {
	float	a[16], s = dsSin[angle & 1023], c = dsSin[( angle + 256 ) & 1023];
	int		i = ( axis + 1 ) % 3, j = ( axis + 2 ) % 3;
	DS_MatIdentity( a );
	a[i * 4 + i] = c, a[i * 4 + j] = s;
	a[j * 4 + i] = -s, a[j * 4 + j] = c;
	DS_MatMul( m, a );
}

static void DS_Transform( const float *m, float x, float y, float z, float *out ) {
	out[0] = x * m[0] + y * m[4] + z * m[8] + m[12];
	out[1] = x * m[1] + y * m[5] + z * m[9] + m[13];
	out[2] = x * m[2] + y * m[6] + z * m[10] + m[14];
}

static int DS_S10( int v ) {
	v &= 0x3ff;
	return v >= 0x200 ? v - 0x400 : v;
}

/* parameter words of the GX commands; -1 unknown */
static int DS_GxParams( int cmd ) {
	switch ( cmd ) {
	case 0x00: case 0x11: case 0x15: case 0x41: return 0;
	case 0x10: case 0x12: case 0x13: case 0x14: case 0x20: case 0x21: case 0x22: case 0x24: case 0x25:
	case 0x26: case 0x27: case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x30: case 0x31: case 0x32:
	case 0x33: case 0x40: case 0x50: case 0x60: case 0x72: return 1;
	case 0x23: case 0x71: return 2;
	case 0x1b: case 0x1c: case 0x70: return 3;
	case 0x1a: return 9;
	case 0x17: case 0x19: return 12;
	case 0x16: case 0x18: return 16;
	case 0x34: return 32;
	default: return -1;
	}
}

/* how a GX list is run: the map's leaves (textures from the patches), or a model of a sprite
 * (one texture, the sprite's matrix, light and alpha) */
typedef struct {
	const dsPool_t	*pool;
	const byte		*patches;		// map: { list offset (TEXIMAGE_PARAM), PLTT_BASE's, shader id }
	int				numPatches;
	int				fixedSh;		// model: >= 0, the texture of every polygon
	unsigned		fixedParam;
	byte			color[3];		// starting vertex colour
	int				alpha;			// 1..31, multiplies the list's polygon alpha
	int				cull;			// dsCull_t until the list sets its own (0x29)
	float			m[16];			// starting matrix
} dsRun_t;

static vec3_t *dsModelBox;		// oe_dsmodels: the list's vertices only grow these bounds

/* runs packed GX commands into triangles */
static void DS_RunList( const byte *b, int size, const dsRun_t *run ) {
	float		m[16], stack[31][16];
	dsVtx_t		strip[4], v;
	int			sp = 0, p = 0, prim = 0, n = 0, total = 0, texSh = run->fixedSh, polyAlpha = 31, cull = run->cull, k;
	unsigned	texParam = run->fixedParam;
	float		vx = 0, vy = 0, vz = 0;

	Com_Memcpy( m, run->m, sizeof( m ) );
	Com_Memset( &v, 0, sizeof( v ) );
	v.color[0] = run->color[0], v.color[1] = run->color[1], v.color[2] = run->color[2], v.color[3] = 255;
	while ( p + 4 <= size ) {
		unsigned w = DS_U32( b + p );
		p += 4;
		for ( k = 0; k < 4; k++ ) {
			int			cmd = ( w >> ( k * 8 ) ) & 0xff, np = DS_GxParams( cmd ), par = p, i;
			qboolean	vtx = qfalse;
			float		a[16];

			if ( cmd == 0 && k > 0 ) {
				continue;
			}
			if ( np < 0 || p + np * 4 > size ) {
				return;
			}
			p += np * 4;
#define P( i ) ( (int)DS_U32( b + par + ( i ) * 4 ) )
			switch ( cmd ) {
			case 0x11:
				if ( sp < 31 ) {
					Com_Memcpy( stack[sp++], m, sizeof( m ) );
				}
				break;
			case 0x12:
				if ( sp > 0 ) {
					Com_Memcpy( m, stack[--sp], sizeof( m ) );
				}
				break;
			case 0x15:
				Com_Memcpy( m, run->m, sizeof( m ) );
				break;
			case 0x1b:
				DS_Scale( m, P( 0 ) / 4096.0f, P( 1 ) / 4096.0f, P( 2 ) / 4096.0f );
				break;
			case 0x1c:
				DS_Translate( m, P( 0 ) / 4096.0f, P( 1 ) / 4096.0f, P( 2 ) / 4096.0f );
				break;
			case 0x17:
			case 0x19:
				DS_MatIdentity( a );
				for ( i = 0; i < 12; i++ ) {
					a[( i / 3 ) * 4 + i % 3] = P( i ) / 4096.0f;
				}
				if ( cmd == 0x17 ) {
					Com_Memcpy( m, run->m, sizeof( m ) );	// (loaded relative to where the list starts)
					DS_MatMul( m, a );
				} else {
					DS_MatMul( m, a );
				}
				break;
			case 0x20:
				v.color[0] = (byte)( ( P( 0 ) & 31 ) * 255 / 31 );
				v.color[1] = (byte)( ( ( P( 0 ) >> 5 ) & 31 ) * 255 / 31 );
				v.color[2] = (byte)( ( ( P( 0 ) >> 10 ) & 31 ) * 255 / 31 );
				break;
			case 0x22:
				v.s = (short)( P( 0 ) & 0xffff ) / 16.0f;
				v.t = (short)( P( 0 ) >> 16 ) / 16.0f;
				break;
			case 0x23:
				vx = (short)( P( 0 ) & 0xffff ) / 4096.0f, vy = (short)( P( 0 ) >> 16 ) / 4096.0f;
				vz = (short)( P( 1 ) & 0xffff ) / 4096.0f;
				vtx = qtrue;
				break;
			case 0x24:
				vx = DS_S10( P( 0 ) ) / 64.0f, vy = DS_S10( P( 0 ) >> 10 ) / 64.0f, vz = DS_S10( P( 0 ) >> 20 ) / 64.0f;
				vtx = qtrue;
				break;
			case 0x25:
				vx = (short)( P( 0 ) & 0xffff ) / 4096.0f, vy = (short)( P( 0 ) >> 16 ) / 4096.0f;
				vtx = qtrue;
				break;
			case 0x26:
				vx = (short)( P( 0 ) & 0xffff ) / 4096.0f, vz = (short)( P( 0 ) >> 16 ) / 4096.0f;
				vtx = qtrue;
				break;
			case 0x27:
				vy = (short)( P( 0 ) & 0xffff ) / 4096.0f, vz = (short)( P( 0 ) >> 16 ) / 4096.0f;
				vtx = qtrue;
				break;
			case 0x28:
				vx += DS_S10( P( 0 ) ) / 4096.0f, vy += DS_S10( P( 0 ) >> 10 ) / 4096.0f, vz += DS_S10( P( 0 ) >> 20 ) / 4096.0f;
				vtx = qtrue;
				break;
			case 0x29:
				polyAlpha = ( P( 0 ) >> 16 ) & 31;
				cull = DS_CullOf( ( P( 0 ) >> 6 ) & 3 );
				break;
			case 0x2a:
				if ( run->fixedSh >= 0 ) {
					break;
				}
				texParam = (unsigned)P( 0 );
				texSh = -1;
				for ( i = 0; i < run->numPatches; i++ ) {
					if ( (int)DS_U32( run->patches + i * 12 ) == par ) {
						texSh = DS_ShaderIndex( (int)DS_U32( run->patches + i * 12 + 8 ), 0 );
					}
				}
				if ( !( ( texParam >> 26 ) & 7 ) ) {
					texSh = -1;		// untextured polygons
				}
				break;
			case 0x40:
				prim = P( 0 ) & 3;
				n = total = 0;
				break;
			case 0x41:
				n = total = 0;
				break;
			}
#undef P
			if ( !vtx ) {
				continue;
			}
			DS_Transform( m, vx, vy, vz, v.xyz );
			if ( dsModelBox ) {
				AddPointToBounds( v.xyz, dsModelBox[0], dsModelBox[1] );
				continue;
			}
			{
				int		alpha = ( polyAlpha ? polyAlpha : 31 ) * run->alpha / 31;
				dsMat_t	*mat;

				v.color[3] = (byte)( alpha * 255 / 31 );
				mat = DS_Material( run->pool, texSh, texParam, alpha < 31 ? DS_BLEND : DS_OPAQUE, cull );
				total++;
				switch ( prim ) {
				case 0:		// triangles
					strip[n++] = v;
					if ( n == 3 ) {
						DS_AddTri( mat, &strip[0], &strip[1], &strip[2] );
						n = 0;
					}
					break;
				case 1:		// quads
					strip[n++] = v;
					if ( n == 4 ) {
						DS_AddTri( mat, &strip[0], &strip[1], &strip[2] );
						DS_AddTri( mat, &strip[0], &strip[2], &strip[3] );
						n = 0;
					}
					break;
				case 2:		// triangle strip: the last three, every other one turned around
					if ( n == 3 ) {
						strip[0] = strip[1], strip[1] = strip[2], n = 2;
					}
					strip[n++] = v;
					if ( n == 3 ) {
						if ( total & 1 ) {
							DS_AddTri( mat, &strip[0], &strip[1], &strip[2] );
						} else {
							DS_AddTri( mat, &strip[1], &strip[0], &strip[2] );
						}
					}
					break;
				default:	// quad strip: a quad per two vertices
					strip[n++] = v;
					if ( n == 4 ) {
						DS_AddTri( mat, &strip[0], &strip[1], &strip[3] );
						DS_AddTri( mat, &strip[0], &strip[3], &strip[2] );
						strip[0] = strip[2], strip[1] = strip[3], n = 2;
					}
					break;
				}
			}
		}
	}
}

/*
=======================================================================

SPRITES

=======================================================================
*/

/* the map's light at a point: the four nearest tiles' RGB, bilinear (0x02046c38) */
static void DS_LightAt( float x, float y, byte *out ) {
	int		ix = (int)x, iy = (int)y, tx = ix >> 6, ty = iy >> 6, fx = ix & 63, fy = iy & 63, k;
	const byte *l;

	if ( !ds.hasLight || tx < 0 || ty < 0 || tx > 30 || ty > 30 ) {
		out[0] = out[1] = out[2] = 255;
		return;
	}
	l = ds.light + ( ty * 32 + tx ) * 3;
	for ( k = 0; k < 3; k++ ) {
		int c = l[k] * ( 64 - fx ) * ( 64 - fy ) + l[k + 3] * fx * ( 64 - fy ) + l[k + 96] * ( 64 - fx ) * fy + l[k + 99] * fx * fy;
		out[k] = (byte)( c >> 12 );
	}
}

/* a value of a sprite program: 4 bytes { type | phase << 4, low, high, period / 32 } over time
 * (0x0204bce0): constant, sawtooth, sine, square or random between low and high (+ bias), << 12 */
static int DS_Expr( const byte *e, int bias, int time ) {
	int type = e[0] & 15, phase = ( e[0] & 0xf0 ) << 4, period = e[3] << 5, u;
	int lo = ( e[1] + bias ) << 12, hi = ( e[2] + bias ) << 12;

	u = ( phase + ( period ? (int)( ( (long long)time << 12 ) / period ) : 0 ) ) & 0xfff;
	switch ( type ) {
	case 1:
		return lo + (int)( ( (long long)u * ( hi - lo ) ) >> 12 );
	case 2:
	case 5:		// (a table of the game's; a sine is close)
		return (int)( ( hi - lo ) / 2 * dsSin[( u >> 2 ) & 1023] ) + ( lo + hi ) / 2;
	case 3:
		return u < 0x801 ? lo : hi;
	case 4:
		return hi > lo ? lo + rand() % ( hi - lo ) : lo;
	default:
		return lo;
	}
}

/* a quad of a sprite: corners in DS world space, texture corners in texels */
static void DS_Quad( dsMat_t *mat, vec3_t p[4], const float st[4][2], const byte *color, int alpha ) {
	dsVtx_t v[4];
	int		k;
	for ( k = 0; k < 4; k++ ) {
		VectorCopy( p[k], v[k].xyz );
		v[k].s = st[k][0];
		v[k].t = st[k][1];
		v[k].color[0] = color[0], v[k].color[1] = color[1], v[k].color[2] = color[2];
		v[k].color[3] = (byte)( alpha * 255 / 31 );
	}
	DS_AddTri( mat, &v[0], &v[1], &v[2] );
	DS_AddTri( mat, &v[0], &v[2], &v[3] );
}

static dsMat_t *DS_DynMaterial( int sh, unsigned param, int alpha, int cull ) {
	dsPool_t pool;
	pool.mats = ds.dyn;
	pool.num = &ds.numDyn;
	pool.max = DS_MAX_DYN;
	return DS_Material( &pool, sh, param, alpha < 31 ? DS_BLEND : DS_OPAQUE, cull );
}

/* a sprite program's 0x1c: what the bound texture or model is drawn as. The textures of
 * sprites are stored bottom row first: the top of a quad is t = height - 1 */
static void DS_ProgDraw( int kind, const float *m, int sh, unsigned param, const byte *color, int alpha, int cull,
		const vec3_t right ) {
	const dsTex_t	*t = DS_ShaderTex( sh );
	dsMat_t			*mat = DS_DynMaterial( sh, param, alpha, cull );
	vec3_t			p[4];
	float			st[4][2], w, h, hw;

	if ( !mat ) {
		return;
	}
	if ( kind < 0xfd ) {
		// a model: its list has no texture of its own
		dsRun_t run;
		dsPool_t pool;
		if ( !ds.models || kind >= ds.numModels || ds.modelOfs[kind] >= ds.modelOfs[kind + 1] ||
			ds.modelOfs[kind + 1] > ds.modelsSize ) {
			return;
		}
		pool.mats = ds.dyn;
		pool.num = &ds.numDyn;
		pool.max = DS_MAX_DYN;
		Com_Memset( &run, 0, sizeof( run ) );
		run.pool = &pool;
		run.fixedSh = sh;
		run.fixedParam = param;
		run.color[0] = color[0], run.color[1] = color[1], run.color[2] = color[2];
		run.alpha = alpha;
		run.cull = cull;
		Com_Memcpy( run.m, m, sizeof( run.m ) );
		DS_RunList( ds.models + ds.modelOfs[kind], ds.modelOfs[kind + 1] - ds.modelOfs[kind], &run );
		return;
	}
	if ( !t ) {
		return;
	}
	w = (float)t->w;
	h = (float)( t->h - 1 );
	st[0][0] = 0, st[0][1] = h;
	st[1][0] = 0, st[1][1] = 0;
	st[2][0] = w, st[2][1] = 0;
	st[3][0] = w, st[3][1] = h;
	hw = t->big ? w / 3 : w / 2;
	if ( t->big && kind != 0xfd ) {
		h = h * 2 / 3;
	}
	if ( kind == 0xff ) {
		// facing the camera, standing on the object's origin
		vec3_t	o, up = { 0, 0, 1 };
		float	sx = (float)sqrt( m[0] * m[0] + m[1] * m[1] + m[2] * m[2] );
		float	sz = (float)sqrt( m[8] * m[8] + m[9] * m[9] + m[10] * m[10] );
		VectorSet( o, m[12], m[13], m[14] );
		VectorMA( o, -hw * sx, right, p[0] );
		VectorMA( p[0], h * sz, up, p[0] );
		VectorMA( o, -hw * sx, right, p[1] );
		VectorMA( o, hw * sx, right, p[2] );
		VectorMA( o, hw * sx, right, p[3] );
		VectorMA( p[3], h * sz, up, p[3] );
	} else if ( kind == 0xfe ) {
		// upright in the object's Y-Z plane
		DS_Transform( m, 0, hw, h, p[0] );
		DS_Transform( m, 0, hw, 0, p[1] );
		DS_Transform( m, 0, -hw, 0, p[2] );
		DS_Transform( m, 0, -hw, h, p[3] );
	} else {
		// flat on the object's X-Y plane, at its texture size
		DS_Transform( m, 0, 0, 0, p[0] );
		DS_Transform( m, 0, st[0][1], 0, p[1] );
		DS_Transform( m, w, st[0][1], 0, p[2] );
		DS_Transform( m, w, 0, 0, p[3] );
	}
	DS_Quad( mat, p, (const float ( * )[2])st, color, alpha );
}

/* runs a shader's program (0x0204c73c) from the sprite's matrix */
static void DS_RunProgram( const dsShader_t *sh, const float *base, const byte *light, int time, const vec3_t right ) {
	const byte	*pc = ds.progs + sh->prog, *end = pc + sh->progLen;
	float		m[16];
	int			bindSh = -1, alpha = 31, cull = DS_CULL_NONE, i;
	unsigned	param = 0;
	qboolean	texMatrix = qfalse;		// 0x18: the transforms move the texture (not done here)
	byte		color[3];

	Com_Memcpy( m, base, sizeof( m ) );
	color[0] = light[0], color[1] = light[1], color[2] = light[2];
	while ( pc < end ) {
		int op = *pc++, v = 0;

		if ( op <= 0x15 ) {
			if ( pc + 4 > end ) {
				return;
			}
			v = DS_Expr( pc, ( op >= 0xc && op != 0x14 ) ? -0x80 : 0, time );
			pc += 4;
		}
		switch ( op ) {
		case 8: case 9: case 10:		// scale one axis (1.0 = 64)
			if ( !texMatrix ) {
				float s[3] = { 1, 1, 1 };
				s[op & 3] = ( v >> 6 ) / 4096.0f;
				DS_Scale( m, s[0], s[1], s[2] );
			}
			break;
		case 0xb:
			if ( !texMatrix ) {
				DS_Scale( m, ( v >> 6 ) / 4096.0f, ( v >> 6 ) / 4096.0f, ( v >> 6 ) / 4096.0f );
			}
			break;
		case 0x14:
			if ( !texMatrix ) {
				DS_Scale( m, ( v >> 6 ) / 4096.0f, ( v >> 6 ) / 4096.0f, 1 );
			}
			break;
		case 0xc: case 0xd: case 0xe:	// move along one axis
			if ( !texMatrix ) {
				float t[3] = { 0, 0, 0 };
				t[op & 3] = v / 4096.0f;
				DS_Translate( m, t[0], t[1], t[2] );
			}
			break;
		case 0xf:
			if ( !texMatrix ) {
				DS_Translate( m, v / 4096.0f, v / 4096.0f, v / 4096.0f );
			}
			break;
		case 0x15:
			if ( !texMatrix ) {
				DS_Translate( m, v / 4096.0f, v / 4096.0f, 0 );
			}
			break;
		case 0x10: case 0x11: case 0x12:	// turn about one axis
			if ( !texMatrix ) {
				DS_Rotate( m, op - 0x10, v >> 9 );
			}
			break;
		case 0x13:
			if ( !texMatrix ) {
				for ( i = 0; i < 3; i++ ) {
					DS_Rotate( m, i, v >> 9 );
				}
			}
			break;
		case 0x16:					// polygon attributes: alpha
			if ( pc + 5 > end ) {
				return;
			}
			cull = DS_CullOf( pc[0] & 3 );
			alpha = DS_Expr( pc + 1, 0, time ) >> 12;
			alpha = alpha < 0 ? 0 : alpha > 31 ? 31 : alpha;
			pc += 5;
			break;
		case 0x17: {				// bind texture: u16 id, frame, flags
			int id, frame, f;
			const dsTex_t *t;
			if ( pc + 7 > end ) {
				return;
			}
			id = DS_U16( pc );
			frame = ( DS_Expr( pc + 2, 0, time ) >> 12 ) & 0xff;
			f = pc[6];
			pc += 7;
			bindSh = DS_ShaderIndex( id, frame );
			t = DS_ShaderTex( bindSh );
			param = ( t ? (unsigned)t->fmt << 26 : 0 ) | ( ( f & 1 ) << 29 ) | ( ( ( f >> 3 ) & 3 ) << 16 ) |
				( ( ( f >> 5 ) & 3 ) << 18 );
			break;
		}
		case 0x18:
			texMatrix = qtrue;
			break;
		case 0x19:
			texMatrix = qfalse;
			break;
		case 0x1a:
			if ( !texMatrix ) {
				DS_MatIdentity( m );
			}
			break;
		case 0x1b:
			Com_Memcpy( m, base, sizeof( m ) );
			break;
		case 0x1c:
			if ( pc + 1 > end ) {
				return;
			}
			if ( alpha > 0 && bindSh >= 0 ) {
				DS_ProgDraw( *pc, m, bindSh, param, color, alpha, cull, right );
			}
			pc++;
			break;
		default:					// 0..7: material colours (lighting), not used here
			if ( op > 0x1c ) {
				return;
			}
			break;
		}
	}
}

/* a shader without a program: a billboard (0x020484fc), on a wall when the sprite says so */
static void DS_Billboard( const dsSprite_t *s, int sh, const byte *light, const vec3_t right ) {
	const dsTex_t	*t = DS_ShaderTex( sh );
	dsMat_t			*mat;
	vec3_t			p[4], r, o;
	float			st[4][2], hw, h, z, scale = ( s->scale ? s->scale : 64 ) / 64.0f;

	if ( !t ) {
		return;
	}
	mat = DS_DynMaterial( sh, ( (unsigned)t->fmt << 26 ) | ( 1 << 29 ), 31, DS_CULL_NONE );
	if ( !mat ) {
		return;
	}
	hw = ( t->w - 1 ) / ( t->big ? 3.0f : 2.0f ) * scale;
	h = (float)( t->h - 1 );
	z = s->z + ( ( s->info & 0x4000000 ) ? 32.0f : -32.0f );
	VectorSet( o, (float)s->x, (float)s->y, z );
	if ( s->info & 0x8000 ) {
		VectorSet( r, -1, 0, 0 );
	} else if ( s->info & 0x10000 ) {
		VectorSet( r, 1, 0, 0 );
	} else if ( s->info & 0x40000 ) {
		VectorSet( r, 0, 1, 0 );
	} else if ( s->info & 0x20000 ) {
		VectorSet( r, 0, -1, 0 );
	} else {
		VectorCopy( right, r );
	}
	st[0][0] = 0, st[0][1] = h;
	st[1][0] = 0, st[1][1] = 0;
	st[2][0] = (float)t->w, st[2][1] = 0;
	st[3][0] = (float)t->w, st[3][1] = h;
	if ( !( s->info & 0x100000 ) != !( s->info & 0x18000 ) ) {	// (on a wall facing x it is turned around)
		int k;
		for ( k = 0; k < 4; k++ ) {
			st[k][0] = t->w - st[k][0];		// mirrored
		}
	}
	VectorMA( o, -hw, r, p[0] );
	p[0][2] += h * ( t->big ? 2.0f / 3 : 1.0f ) * scale;
	VectorMA( o, -hw, r, p[1] );
	VectorMA( o, hw, r, p[2] );
	VectorMA( o, hw, r, p[3] );
	p[3][2] = p[0][2];
	DS_Quad( mat, p, (const float ( * )[2])st, light, 31 );
}

/* a map sprite, as Render_drawSprite: the shader of its tile and frame runs its program from the
 * sprite's place and turn; without one it is a billboard */
static void DS_DrawSprite( const dsSprite_t *s, int time, const vec3_t right ) {
	int			tile = s->info & 0xff, sh;
	byte		light[3];
	float		base[16];

	if ( s->info & 0x4000 ) {
		tile += 0x101;		// a wall texture
	}
	sh = DS_ShaderIndex( tile, ( s->info >> 8 ) & 15 );
	if ( sh < 0 ) {
		return;
	}
	if ( !ds.shaders[sh].progLen ) {
		DS_LightAt( (float)s->x, (float)s->y, light );
		DS_Billboard( s, sh, light, right );
		return;
	}
	if ( ds.shaders[sh].flag & 1 ) {
		DS_LightAt( (float)s->x, (float)s->y, light );
	} else {
		light[0] = light[1] = light[2] = 255;
	}
	// in the game's order (0x02048110): the place, then the turns (GX: the last one applies first)
	DS_MatIdentity( base );
	DS_Translate( base, (float)s->x, (float)s->y, s->z + ( ( s->info & 0x2000000 ) ? 32.0f : -32.0f ) );
	if ( s->info & 0x2000000 ) {
		DS_Rotate( base, 0, 512 );		// hanging: upside down, from above
	}
	DS_Rotate( base, 2, s->angle );
	if ( s->scale && s->scale != 64 ) {
		DS_Scale( base, s->scale / 64.0f, s->scale / 64.0f, s->scale / 64.0f );
	}
	DS_RunProgram( &ds.shaders[sh], base, light, time, right );
}

/* a moving line (door, gate) as DS 0x0204b3c4: an upright quad along the line, 64 units high
 * (128 with flag 0x10000), the texture of shader 0x101 + its byte (64 texels across, its rows
 * bottom first), lit from the map's light at its middle */
static void DS_DrawDoor( const dsDoor_t *d ) {
	int			sh = DS_ShaderIndex( d->tex + 0x101, 0 );
	float		h = ( d->flags & 0x10000 ) ? 128.0f : 64.0f, st[4][2];
	byte		light[3];
	vec3_t		p[4];
	dsMat_t		*mat;

	if ( sh < 0 || !DS_ShaderTex( sh ) ) {
		return;
	}
	mat = DS_DynMaterial( sh, ( (unsigned)DS_ShaderTex( sh )->fmt << 26 ) | ( 1u << 29 ) | ( 1u << 16 ), 31, DS_CULL_NONE );
	if ( !mat ) {
		return;
	}
	DS_LightAt( ( d->x1 + d->x2 ) / 2, ( d->y1 + d->y2 ) / 2, light );
	VectorSet( p[0], d->x1, d->y1, h );
	VectorSet( p[1], d->x1, d->y1, 0 );
	VectorSet( p[2], d->x2, d->y2, 0 );
	VectorSet( p[3], d->x2, d->y2, h );
	st[0][0] = 0, st[0][1] = h;
	st[1][0] = 0, st[1][1] = 0;
	st[2][0] = 64, st[2][1] = 0;
	st[3][0] = 64, st[3][1] = h;
	DS_Quad( mat, p, (const float ( * )[2])st, light, 31 );
}

/*
=======================================================================

THE MAP

=======================================================================
*/

static void DS_FreeMap( void ) {
	int i;
	for ( i = 0; i < ds.numMats; i++ ) {
		free( ds.mats[i].verts );
	}
	for ( i = 0; i < ds.numDyn; i++ ) {
		free( ds.dyn[i].verts );
	}
	free( ds.sprites );
	free( ds.walls );
	free( ds.doors );
	ds.doors = NULL;
	ds.numDoors = 0;
	free( ds.doorModels );
	ds.doorModels = NULL;
	ds.numDoorModels = 0;
	Com_Memset( ds.mats, 0, sizeof( ds.mats ) );
	Com_Memset( ds.dyn, 0, sizeof( ds.dyn ) );
	ds.sprites = NULL;
	ds.walls = NULL;
	ds.numMats = ds.numDyn = ds.numTris = ds.numLeaves = ds.numSprites = ds.numWalls = 0;
	ds.hasLight = qfalse;
	ds.map[0] = 0;
}

/* "m01" -> "map_m01" */
static void DS_MapName( const char *arg, char *name, int size ) {
	if ( !Q_stricmpn( arg, "map_", 4 ) ) {
		Q_strncpyz( name, arg, size );
	} else {
		Com_sprintf( name, size, "map_%s", arg );
	}
}

/* the .bsp: header (spawn), nodes (skipped), lines (the walls), sprites */
static qboolean DS_ParseBsp( const byte *b, int size ) {
	static const float dirYaw[4] = { 180, 0, 90, 270 };	// the header's spawn direction (Dir codes)
	int p = 25, n, i, numA, numB;

	if ( size < 25 || b[0] != 7 ) {
		return qfalse;
	}
	{
		int tile = DS_U16( b + 17 );
		ds.eyeZ = b[22] + DS_EYE;
		ds.spawn[0] = ( tile & 31 ) * 64 + 32;
		ds.spawn[1] = -( ( tile >> 5 ) * 64 + 32 );
		ds.spawn[2] = ds.eyeZ;
		ds.spawnYaw = dirYaw[b[19] & 3];
	}
	if ( p + 2 > size ) {
		return qfalse;
	}
	n = DS_U16( b + p );
	p += 2 + n * 10;

	// lines: x1, y1, x2, y2 (<< 3), texture, 3D texture, u32 flags
	if ( p + 2 > size ) {
		return qfalse;
	}
	n = DS_U16( b + p );
	p += 2;
	if ( p + n * 10 > size ) {
		return qfalse;
	}
	ds.walls = calloc( n + 1, sizeof( *ds.walls ) );
	ds.doors = calloc( n + 1, sizeof( *ds.doors ) );
	ds.doorModels = calloc( n + 1, sizeof( *ds.doorModels ) );
	if ( !ds.walls || !ds.doors || !ds.doorModels ) {
		return qfalse;
	}
	for ( i = 0; i < n; i++, p += 10 ) {
		dsWall_t	*w = &ds.walls[ds.numWalls++];
		unsigned	flags = DS_U32( b + p + 6 );
		float		d = 0;
		w->x1 = b[p] * 8.0f, w->y1 = b[p + 1] * 8.0f;
		w->x2 = b[p + 2] * 8.0f, w->y2 = b[p + 3] * 8.0f;
		// doors sit 3 units off their tile edge (as the BREW .bsp)
		if ( flags & 0x300 ) {
			d = ( flags & 0x8 ) ? 3.0f : ( flags & 0x10 ) ? -3.0f : 0;
		}
		if ( flags & 0x200 ) {
			w->x1 += d, w->x2 += d;
		} else if ( flags & 0x100 ) {
			w->y1 += d, w->y2 += d;
		}
		if ( flags & 0x80000 ) {
			// a moving line: not in the leaves' geometry, drawn on its own
			dsDoor_t *door = &ds.doors[ds.numDoors++];
			door->x1 = w->x1, door->y1 = w->y1, door->x2 = w->x2, door->y2 = w->y2;
			door->tex = b[p + 4];
			door->flags = flags;
		} else if ( b[p + 4] >= 0x10 && b[p + 4] <= 0x1f && !( flags & ( 0x1000000 | 0x40000 | 0x40 | 2 ) ) ) {
			// a door (0x0204b6f4): the program of tile texture + 0x101 at the line's middle
			dsSprite_t *s = &ds.doorModels[ds.numDoorModels++];
			s->x = (int)( w->x1 + w->x2 ) / 2;
			s->y = (int)( w->y1 + w->y2 ) / 2;
			s->z = 32;
			s->info = b[p + 4] | 0x4000;
			s->angle = ( flags & 0x100 ) ? 0x300 : ( flags & 0x200 ) ? 0x200 : 0;
		}
	}

	// sprites: numA { x, y, u32 info } at z 32, numB { x, y, z + 32, u32 info }
	if ( p + 4 > size ) {
		return qfalse;
	}
	numA = DS_U16( b + p );
	numB = DS_U16( b + p + 2 );
	p += 4;
	if ( p + numA * 6 + numB * 7 > size ) {
		return qfalse;
	}
	ds.sprites = calloc( numA + numB + 1, sizeof( *ds.sprites ) );
	for ( i = 0; i < numA + numB; i++ ) {
		dsSprite_t *s = &ds.sprites[ds.numSprites++];
		s->x = b[p] * 8;
		s->y = b[p + 1] * 8;
		if ( i < numA ) {
			s->z = 32;
			s->info = DS_U32( b + p + 2 );
			p += 6;
		} else {
			s->z = b[p + 2] - 32;
			s->info = DS_U32( b + p + 3 );
			p += 7;
		}
		// on a wall: turned to face away from it (Render_drawSprite)
		s->angle = ( s->info & 0x8000 ) ? 0x300 : ( s->info & 0x10000 ) ? 0x100 : ( s->info & 0x40000 ) ? 0x200 : 0;
	}
	return qtrue;
}

/* loads maps/<name>.bsp, maps/<name>.lz (the leaves' geometry) and maps/<name>.light */
static qboolean DS_LoadMap( const char *arg ) {
	char		name[MAX_QPATH];
	byte		*bsp, *raw, *lz, *light;
	int			bspSize = 0, rawSize = 0, lzSize = 0, lightSize = 0, o = 0;
	dsPool_t	pool;
	dsRun_t		run;

	if ( !DS_OpenRom() || !DS_LoadTextures() ) {
		return qfalse;
	}
	DS_MapName( arg, name, sizeof( name ) );
	bsp = DS_LoadFile( va( "maps/%s.bsp", name ), &bspSize );
	raw = DS_LoadStored( va( "maps/%s.lz", name ), &rawSize );
	lz = raw ? DS_Lz10( raw, rawSize, &lzSize ) : NULL;
	free( raw );
	if ( !bsp || !lz ) {
		Com_Printf( S_COLOR_RED "%s: no such map (oe_dsmap lists them)\n", name );
		free( bsp );
		free( lz );
		return qfalse;
	}

	DS_FreeMap();
	if ( !DS_ParseBsp( bsp, bspSize ) ) {
		Com_Printf( S_COLOR_RED "%s.bsp: not as expected\n", name );
		free( bsp );
		free( lz );
		return qfalse;
	}
	Q_strncpyz( ds.map, name, sizeof( ds.map ) );
	light = DS_LoadFile( va( "maps/%s.light", name ), &lightSize );
	if ( light && lightSize >= (int)sizeof( ds.light ) ) {
		Com_Memcpy( ds.light, light, sizeof( ds.light ) );
		ds.hasLight = qtrue;
	}
	free( light );

	// one block per leaf: header size, list size, patches { list offset (TEXIMAGE_PARAM), its
	// PLTT_BASE, shader id }, then the list
	pool.mats = ds.mats;
	pool.num = &ds.numMats;
	pool.max = DS_MAX_MATS;
	Com_Memset( &run, 0, sizeof( run ) );
	run.pool = &pool;
	run.fixedSh = -1;
	run.color[0] = run.color[1] = run.color[2] = 255;
	run.alpha = 31;
	run.cull = DS_CULL_NONE;
	DS_MatIdentity( run.m );
	while ( o + 12 <= lzSize ) {
		int hdr = (int)DS_U32( lz + o ), size = (int)DS_U32( lz + o + 4 ), np = (int)DS_U32( lz + o + 8 );
		if ( hdr < 12 || size < 0 || np < 0 || 12 + np * 12 > hdr || o + hdr + size > lzSize ) {
			Com_Printf( S_COLOR_YELLOW "%s.lz: unexpected block at %d\n", name, o );
			break;
		}
		run.patches = lz + o + 12;
		run.numPatches = np;
		DS_RunList( lz + o + hdr, size, &run );
		ds.numLeaves++;
		o += hdr + size;
	}
	for ( o = 0; o < ds.numMats; o++ ) {
		ds.numTris += ds.mats[o].numVerts / 3;
	}
	free( bsp );
	free( lz );
	Com_Printf( "%s: %d leaves, %d triangles, %d materials, %d sprites, %d walls\n", name, ds.numLeaves,
		ds.numTris, ds.numMats, ds.numSprites, ds.numWalls );
	return qtrue;
}

/*
=======================================================================

THE VIEW

=======================================================================
*/

/* the map's triangles and its sprites into the scene (cl_hw.c's free camera draws it) */
void CL_DsAddScene( vec3_t viewAxis[3] ) {
	vec3_t	right;
	int		i;

	// the sprites again for this frame, facing the camera (Quake 3's left -> the DS's right)
	for ( i = 0; i < ds.numDyn; i++ ) {
		ds.dyn[i].numVerts = 0;
	}
	VectorSet( right, -viewAxis[1][0], viewAxis[1][1], 0 );
	VectorNormalize( right );
	if ( oe_dsSprites->integer && dsModuleMap ) {
		for ( i = 0; i < dsNumModuleSprites; i++ ) {
			DS_DrawSprite( &dsModuleSprites[i], cls.realtime, right );
		}
	} else if ( oe_dsSprites->integer ) {
		for ( i = 0; i < ds.numSprites; i++ ) {
			DS_DrawSprite( &ds.sprites[i], cls.realtime, right );
		}
		for ( i = 0; i < ds.numDoorModels; i++ ) {
			DS_DrawSprite( &ds.doorModels[i], cls.realtime, right );
		}
	}
	// the doors: where the game module has them, else as the .bsp places them
	if ( dsModuleMap ) {
		for ( i = 0; i < dsNumModuleDoors; i++ ) {
			DS_DrawDoor( &dsModuleDoors[i] );
		}
	} else {
		for ( i = 0; i < ds.numDoors; i++ ) {
			DS_DrawDoor( &ds.doors[i] );
		}
	}

	for ( i = 0; i < ds.numMats + ds.numDyn; i++ ) {
		dsMat_t		*m = i < ds.numMats ? &ds.mats[i] : &ds.dyn[i - ds.numMats];
		qhandle_t	sh;
		if ( m->numVerts < 3 || !( sh = DS_MatShader( m ) ) ) {
			continue;
		}
		re.AddPolyToScene( sh, 3, m->verts, m->numVerts / 3 );
	}
}

void CL_DsRendererStarted( void ) {
	dsRendererGen++;	// images and shaders are gone
}

/* walking (oe_dsWalk): the eye stays at the map's height */
qboolean CL_DsWalking( void ) {
	return oe_dsWalk->integer && ds.map[0];
}

/* walking: moves origin (Quake 3 axes) by move, sliding along the map's wall lines */
void CL_DsWalk( vec3_t origin, const vec3_t move ) {
	float	len = (float)sqrt( move[0] * move[0] + move[1] * move[1] );
	int		steps = (int)( len / 4 ) + 1, s, i, k;
	float	x = origin[0], y = -origin[1];		// DS axes

	for ( s = 0; s < steps; s++ ) {
		x += move[0] / steps;
		y -= move[1] / steps;
		for ( k = 0; k < 2; k++ ) {			// (twice: corners where two walls push)
			for ( i = 0; i < ds.numWalls; i++ ) {
				const dsWall_t *w = &ds.walls[i];
				float dx = w->x2 - w->x1, dy = w->y2 - w->y1, l2 = dx * dx + dy * dy, t, px, py, d;
				t = l2 > 0 ? ( ( x - w->x1 ) * dx + ( y - w->y1 ) * dy ) / l2 : 0;
				t = t < 0 ? 0 : t > 1 ? 1 : t;
				px = x - ( w->x1 + t * dx );
				py = y - ( w->y1 + t * dy );
				d = (float)sqrt( px * px + py * py );
				if ( d < DS_RADIUS && d > 0.001f ) {
					x += px / d * ( DS_RADIUS - d );
					y += py / d * ( DS_RADIUS - d );
				}
			}
		}
	}
	origin[0] = x;
	origin[1] = -y;
	origin[2] = ds.eyeZ;
}

static void DS_ListMaps( void ) {
	int i;
	Com_Printf( "usage: oe_dsmap <map>   (Orcs & Elves DS maps:" );
	for ( i = 0; i < ds.numFiles; i++ ) {
		const char *p = ds.files[i].path;
		int len = strlen( p );
		if ( !Q_stricmpn( p, "maps/", 5 ) && len > 9 && !Q_stricmp( p + len - 4, ".bsp" ) ) {
			Com_Printf( " %.*s", len - 9, p + 5 );
		}
	}
	Com_Printf( ")\n" );
}

static void CL_DsMap_f( void ) {
	if ( !DS_OpenRom() ) {
		Com_Printf( "oe_dsmap: needs Orcs & Elves DS (%s in the game folder, fs_game oeds)\n", DS_ROM );
		return;
	}
	if ( Cmd_Argc() < 2 ) {
		DS_ListMaps();
		return;
	}
	if ( DS_LoadMap( Cmd_Argv( 1 ) ) ) {
		CL_HwDsView( ds.spawn, ds.spawnYaw, ds.map );
	}
}

/* oe_dsmodels: the bounds of each model of textures/models.bin, as drawn from the origin */
static void CL_DsModels_f( void ) {
	vec3_t	box[2];
	int		i;
	if ( !ds.models ) {
		Com_Printf( "oe_dsmodels: no cartridge\n" );
		return;
	}
	for ( i = 0; i < ds.numModels; i++ ) {
		dsRun_t	run;
		if ( ds.modelOfs[i] >= ds.modelOfs[i + 1] || ds.modelOfs[i + 1] > ds.modelsSize ) {
			continue;
		}
		Com_Memset( &run, 0, sizeof( run ) );
		run.fixedSh = 0;
		run.alpha = 31;
		run.cull = DS_CULL_NONE;
		DS_MatIdentity( run.m );
		ClearBounds( box[0], box[1] );
		dsModelBox = box;
		DS_RunList( ds.models + ds.modelOfs[i], ds.modelOfs[i + 1] - ds.modelOfs[i], &run );
		dsModelBox = NULL;
		Com_Printf( "model %2d: %6.1f %6.1f %6.1f .. %6.1f %6.1f %6.1f\n", i, box[0][0], box[0][1], box[0][2],
			box[1][0], box[1][1], box[1][2] );
	}
}

/* oe_dsshader <id>: the shaders of a tile id (hex), their textures and programs */
static void CL_DsShader_f( void ) {
	int id, i, k;
	if ( Cmd_Argc() < 2 || !DS_LoadTextures() ) {
		Com_Printf( "usage: oe_dsshader <tile id, hex>\n" );
		return;
	}
	id = (int)strtol( Cmd_Argv( 1 ), NULL, 16 );
	for ( i = 0; i < ds.numShaders; i++ ) {
		const dsShader_t	*s = &ds.shaders[i];
		const dsTex_t		*t = DS_ShaderTex( i );
		char				hex[3 * 256 + 1];
		if ( ( s->key >> 4 ) != ( id & 0xfff ) ) {
			continue;
		}
		hex[0] = 0;
		for ( k = 0; k < s->progLen && k < 256; k++ ) {
			Q_strcat( hex, sizeof( hex ), va( "%02x ", ds.progs[s->prog + k] ) );
		}
		Com_Printf( "shader %d key %04x flag %02x pal %d tex %d", i, s->key, s->flag, s->pal, s->tex );
		if ( t ) {
			Com_Printf( " (fmt %d %dx%d log %d/%d)", t->fmt, t->w, t->h, t->sLog, t->tLog );
		}
		Com_Printf( "\n  prog %s\n", hex );
	}
}

/* oe_dsgoto <x> <y> [yaw]: the camera to the middle of tile x, y (0..31; y south) */
static void CL_DsGoto_f( void ) {
	vec3_t o;
	if ( !ds.map[0] || Cmd_Argc() < 3 ) {
		Com_Printf( "usage: oe_dsgoto <tile x> <tile y> [yaw]   (on a map loaded with oe_dsmap)\n" );
		return;
	}
	o[0] = atoi( Cmd_Argv( 1 ) ) * 64 + 32.0f;
	o[1] = -( atoi( Cmd_Argv( 2 ) ) * 64 + 32.0f );
	o[2] = ds.eyeZ;
	CL_HwDsView( o, Cmd_Argc() > 3 ? (float)atof( Cmd_Argv( 3 ) ) : ds.spawnYaw, ds.map );
}

/*
=======================================================================

THE GAME MODULE (OE_TRAP_DS_*)

=======================================================================
*/

/* a file of the cartridge for the module: its size (-1 if missing), copied when size is enough */
int CL_DsFile( const char *path, void *buf, int size ) {
	byte	*data;
	int		n = 0;

	if ( !DS_OpenRom() || !( data = DS_LoadFile( path, &n ) ) ) {
		return -1;
	}
	if ( buf && size >= n ) {
		Com_Memcpy( buf, data, n );
	}
	free( data );
	return n;
}

/* the module's 3D view shows this map ("" none): qtrue if it is loaded */
qboolean CL_DsModuleMap( const char *name ) {
	dsNumModuleSprites = 0;
	if ( !name || !name[0] ) {
		dsModuleMap = qfalse;
		return qfalse;
	}
	if ( !Q_stricmp( ds.map, name ) || DS_LoadMap( name ) ) {
		dsModuleMap = qtrue;
		return qtrue;
	}
	dsModuleMap = qfalse;
	return qfalse;
}

qboolean CL_DsModuleMapActive( void ) {
	return dsModuleMap && ds.map[0];
}

/* the sprites of the module's next scene */
void CL_DsSprites( const oeDsSprite_t *sprites, int count ) {
	int i;
	if ( !dsModuleSprites ) {
		dsModuleSprites = calloc( OE_DS_MAX_SPRITES, sizeof( *dsModuleSprites ) );
		dsModuleDoors = calloc( OE_DS_MAX_SPRITES, sizeof( *dsModuleDoors ) );
		if ( !dsModuleSprites || !dsModuleDoors ) {
			return;
		}
	}
	dsNumModuleSprites = dsNumModuleDoors = 0;
	for ( i = 0; i < count && i < OE_DS_MAX_SPRITES; i++ ) {
		dsSprite_t *s = &dsModuleSprites[dsNumModuleSprites];
		if ( sprites[i].alpha <= 0 ) {
			continue;
		}
		if ( sprites[i].kind == OE_DS_DOOR ) {
			dsDoor_t *d = &dsModuleDoors[dsNumModuleDoors++];
			d->x1 = (float)sprites[i].x, d->y1 = (float)sprites[i].y;
			d->x2 = (float)sprites[i].x2, d->y2 = (float)sprites[i].y2;
			d->tex = (int)( sprites[i].info & 0xff );
			d->flags = (unsigned)sprites[i].angle;
			continue;
		}
		s->x = sprites[i].x;
		s->y = sprites[i].y;
		s->z = sprites[i].z;
		s->info = sprites[i].info;
		s->angle = sprites[i].angle;
		s->scale = sprites[i].scale;
		dsNumModuleSprites++;
	}
}

/* fs_game oeds: the cartridge instead of a BREW game; qfalse (and a message) if it cannot start */
qboolean CL_DsStart( void ) {
	if ( !DS_OpenRom() ) {
		return qfalse;
	}
	if ( !DS_LoadMap( oe_dsStartMap->string ) && !DS_LoadMap( "map_m01" ) ) {
		return qfalse;
	}
	CL_HwDsView( ds.spawn, ds.spawnYaw, ds.map );
	return qtrue;
}

void CL_DsInit( void ) {
	int i;
	for ( i = 0; i < 1024; i++ ) {
		dsSin[i] = (float)sin( i * ( 2 * M_PI / 1024 ) );
	}
	oe_dsStartMap = Cvar_Get( "oe_dsStartMap", "map_m01", CVAR_ARCHIVE );
	oe_dsWalk = Cvar_Get( "oe_dsWalk", "1", CVAR_ARCHIVE );			// 0: fly through the walls
	oe_dsSprites = Cvar_Get( "oe_dsSprites", "1", 0 );
	Cmd_AddCommand( "oe_dsmap", CL_DsMap_f );
	Cmd_AddCommand( "oe_dsgoto", CL_DsGoto_f );
	Cmd_AddCommand( "oe_dsmodels", CL_DsModels_f );
	Cmd_AddCommand( "oe_dsshader", CL_DsShader_f );
}
