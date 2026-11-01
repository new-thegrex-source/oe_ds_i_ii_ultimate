/*
===========================================================================
This file is part of the Orcs & Elves client, based on Tremulous.

It is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.
===========================================================================
*/
// cl_hw.c -- the games' 3D view drawn with the Quake 3 renderer (oe_renderer 1),
// and Quake 3 maps (map, devmap, oe_playover, oe_idtech3freecam).
//
// The game module keeps running its own software renderer at the game's
// resolution (the game logic depends on some of what it does) and describes
// what it drew in world space (oe_public.h, OE_TRAP_HW_*): textures converted
// from its palettes, and every frame the camera, textured quads and the
// floor/ceiling tiles. Here that becomes polygons for RE_AddPolyToScene with
// shaders made by RE_RegisterShaderText, drawn by RE_RenderScene without a
// world model, at the window's resolution. The game's 2D (HUD, menus, the
// weapon) goes on top: its screen, transparent where the 3D view is still
// exactly what the game's own renderer drew there.

#include "client.h"

static cvar_t	*oe_renderer;		// 0 the game draws everything, 1 the 3D view on the GPU
static cvar_t	*oe_hwFilter;		// 0 blocky texels (like the phone), 1 smooth
static cvar_t	*oe_playoverScale;	// Quake 3 units per game unit in oe_playover

/*
=======================================================================

TEXTURES

The game's textures are kept in their source form, so that they can be
uploaded again after a vid_restart and with another filter.

=======================================================================
*/

typedef struct {
	oeHwTexture_t	desc;			// desc.id == 0: unused
	byte			*pixels;		// a copy of the source (Z_Malloc)
	byte			*mask;
	unsigned short	palette[256];
	int				uploaded;		// renderer and texture generations it was uploaded for
	qhandle_t		shaders[4];		// per oeHwBlend_t
	int				shaderGen;
} hwTexture_t;

static hwTexture_t	hwTextures[OE_HW_MAX_TEXTURES];
static int			rendererGen = 1;	// bumped by every renderer start (images and shaders are gone)
static int			textureGen = 1;		// bumped when the textures must be uploaded again (filter)
static int			filterUsed = -1;

static int CL_HwPow2( int n ) {
	int p = 1;
	while ( p < n && p < 4096 ) {
		p <<= 1;
	}
	return p;
}

static void CL_HwFreeTexture( hwTexture_t *t ) {
	if ( t->pixels ) {
		Z_Free( t->pixels );
	}
	if ( t->mask ) {
		Z_Free( t->mask );
	}
	Com_Memset( t, 0, sizeof( *t ) );
}

/* the games' colour effects on an RGB565 colour (Render_setPalette) */
static unsigned CL_HwEffect( unsigned c, int flags ) {
	unsigned r = c >> 11, g = ( c >> 5 ) & 63, b = c & 31;
	if ( flags & OE_HWTEX_GREY ) {
		unsigned y = ( r * 2 + g + b * 2 ) / 6;	// on a 5-bit scale
		r = b = y;
		g = y * 2;
	}
	if ( flags & OE_HWTEX_BRIGHT ) {
		r = r + 8 > 31 ? 31 : r + 8;
		g = g + 16 > 63 ? 63 : g + 16;
		b = b + 8 > 31 ? 31 : b + 8;
	}
	return ( r << 11 ) | ( g << 5 ) | b;
}

static int CL_HwImageFlags( const oeHwTexture_t *d ) {
	int flags = IMGFLAG_MIPMAP;
	if ( !oe_hwFilter->integer ) {
		flags |= IMGFLAG_NEAREST;
	}
	if ( d->flags & OE_HWTEX_REPEAT ) {
		flags |= IMGFLAG_REPEAT;
	}
	return flags;
}

/* a mod's picture for the texture (WTEXELS / STEXELS / FTEXELS.txt): uploaded in its place, any
 * size (it covers what the original covers), with the game's colour effects; qfalse without one */
static qboolean CL_HwUploadReplacement( hwTexture_t *t ) {
	const oeHwTexture_t *d = &t->desc;
	const char *path = d->source ? OE_TexelOverride( d->source ) : NULL;
	byte *pic, *rgba;
	int w, h, tw, th, x, y;

	if ( !path || !( pic = OE_LoadImageRGBA( path, &w, &h ) ) ) {
		return qfalse;
	}
	tw = CL_HwPow2( w );
	th = CL_HwPow2( h );
	rgba = Hunk_AllocateTempMemory( tw * th * 4 );
	for ( y = 0; y < th; y++ ) {
		for ( x = 0; x < tw; x++ ) {
			const byte *s = pic + ( ( y * h / th ) * w + x * w / tw ) * 4;
			byte *o = rgba + ( y * tw + x ) * 4;
			int r = s[0], g = s[1], b = s[2];
			if ( d->flags & OE_HWTEX_GREY ) {
				r = g = b = ( r * 299 + g * 587 + b * 114 ) / 1000;
			}
			if ( d->flags & OE_HWTEX_BRIGHT ) {
				r = r + 64 > 255 ? 255 : r + 64;
				g = g + 64 > 255 ? 255 : g + 64;
				b = b + 64 > 255 ? 255 : b + 64;
			}
			o[0] = (byte)r, o[1] = (byte)g, o[2] = (byte)b, o[3] = s[3];
		}
	}
	Z_Free( pic );
	re.UploadImageEx( va( "*oetex/%d", d->id ), rgba, tw, th, CL_HwImageFlags( d ) );
	Hunk_FreeTempMemory( rgba );
	t->uploaded = rendererGen * 1024 + textureGen;
	return qtrue;
}

/* converts a texture to RGBA (power-of-two, texels repeated nearest) and uploads it */
static void CL_HwUpload( hwTexture_t *t ) {
	const oeHwTexture_t *d = &t->desc;
	int tw = CL_HwPow2( d->width ), th = CL_HwPow2( d->height );
	byte *rgba;
	int x, y;

	if ( CL_HwUploadReplacement( t ) ) {
		return;
	}
	rgba = Hunk_AllocateTempMemory( tw * th * 4 );

	for ( y = 0; y < th; y++ ) {
		int sy = y * d->height / th;
		byte *out = rgba + y * tw * 4;
		for ( x = 0; x < tw; x++, out += 4 ) {
			int sx = x * d->width / tw;
			unsigned c;
			int a = 255;
			switch ( d->format ) {
			case OE_HWTEX_COLS8:
				c = t->palette[t->pixels[sx * d->height + sy]];
				break;
			case OE_HWTEX_ROWS8:
				c = t->palette[t->pixels[sy * d->width + sx]];
				a = t->mask[sy * d->width + sx] ? 255 : 0;
				break;
			default:
				c = ( (unsigned short *)t->pixels )[sy * d->width + sx];
				break;
			}
			c = CL_HwEffect( c, d->flags );
			out[0] = (byte)( ( ( c >> 11 ) & 31 ) * 255 / 31 );
			out[1] = (byte)( ( ( c >> 5 ) & 63 ) * 255 / 63 );
			out[2] = (byte)( ( c & 31 ) * 255 / 31 );
			out[3] = (byte)a;
		}
	}

	re.UploadImageEx( va( "*oetex/%d", d->id ), rgba, tw, th, CL_HwImageFlags( d ) );
	Hunk_FreeTempMemory( rgba );
	t->uploaded = rendererGen * 1024 + textureGen;
}

/* the search path changed (fs_usepk3): upload the textures again, a mod's replacements may differ */
void CL_HwTexturesChanged( void ) {
	textureGen++;
}

byte *CL_LoadImageRGBA( const char *name, int *w, int *h ) {
	byte *pic = NULL;
	*w = *h = 0;
	if ( !cls.rendererStarted || !re.LoadImageRGBA ) {
		return NULL;
	}
	re.LoadImageRGBA( name, &pic, w, h );
	return pic;
}

void CL_HwTexture( const oeHwTexture_t *desc, const void *pixels, const byte *mask, const unsigned short *palette ) {
	hwTexture_t *t;
	int n;

	if ( desc->id <= 0 || desc->id >= OE_HW_MAX_TEXTURES || desc->width <= 0 || desc->height <= 0 ||
		desc->width > 1024 || desc->height > 1024 ) {
		return;
	}
	t = &hwTextures[desc->id];
	CL_HwFreeTexture( t );
	t->desc = *desc;
	n = desc->width * desc->height;
	if ( desc->format == OE_HWTEX_RGB565 ) {
		t->pixels = Z_Malloc( n * 2 );
		Com_Memcpy( t->pixels, pixels, n * 2 );
	} else {
		t->pixels = Z_Malloc( n );
		Com_Memcpy( t->pixels, pixels, n );
		if ( palette ) {
			Com_Memcpy( t->palette, palette, sizeof( t->palette ) );
		}
		if ( desc->format == OE_HWTEX_ROWS8 ) {
			t->mask = Z_Malloc( n );
			Com_Memcpy( t->mask, mask, n );
		}
	}
	t->uploaded = 0;	// uploaded when first drawn
	t->shaderGen = 0;
}

static qhandle_t CL_HwShader( int id, int blend ) {
	static const char *stages[4] = {
		"alphaFunc GE128 rgbGen vertex",
		"blendFunc GL_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA rgbGen vertex alphaGen vertex",
		"blendFunc GL_SRC_ALPHA GL_ONE rgbGen vertex alphaGen vertex",
		"alphaFunc GE128 blendFunc GL_DST_COLOR GL_ZERO rgbGen vertex"
	};
	hwTexture_t *t;

	if ( id <= 0 || id >= OE_HW_MAX_TEXTURES || blend < 0 || blend > 3 ) {
		return 0;
	}
	t = &hwTextures[id];
	if ( !t->desc.id ) {
		return 0;
	}
	if ( t->uploaded != rendererGen * 1024 + textureGen ) {
		CL_HwUpload( t );
	}
	if ( t->shaderGen != rendererGen ) {
		Com_Memset( t->shaders, 0, sizeof( t->shaders ) );
		t->shaderGen = rendererGen;
	}
	if ( !t->shaders[blend] ) {
		// the image name is that of the texture; a shader name per blend mode
		t->shaders[blend] = re.RegisterShaderText( va( "*oetex/%d/%d", id, blend ),
			va( "{ cull none nopicmip { %s *oetex/%d %s } }",
				( t->desc.flags & OE_HWTEX_REPEAT ) ? "map" : "clampMap", id, stages[blend] ) );
	}
	return t->shaders[blend];
}

/*
=======================================================================

SCENE

=======================================================================
*/

typedef struct {
	oeHwScene_t		scene;
	oeHwQuad_t		quads[OE_HW_MAX_QUADS];
	short			tiles[2048];
	unsigned short	*snapshot;			// the game's own 3D view (scene.rect), to find what was drawn over it
	int				snapshotSize;
	qboolean		valid;
} hwFrame_t;

static hwFrame_t	hwPending;			// described since the last presented frame
static hwFrame_t	hwShown;			// goes with the presented frame

// the game's screen with the untouched 3D view transparent (RGBA, texture-sized)
static byte			*hwOverlay;
static int			hwOverlaySize;
static qboolean		hwOverlayValid, hwOverlayDirty;
static int			hwOverlayW, hwOverlayH, hwOverlayTexW, hwOverlayTexH;
static qhandle_t	hwOverlayShader;
static int			hwOverlayShaderGen;

static void CL_HwGameStopped( void );
static float CL_HwViewPitch( void );

qboolean CL_HwAvailable( void ) {
	int w, h;
	CL_WindowSize( &w, &h );
	return oe_renderer && oe_renderer->integer && w > 0 && h > 0;
}

void CL_HwReset( void ) {
	int i;
	for ( i = 0; i < OE_HW_MAX_TEXTURES; i++ ) {
		if ( hwTextures[i].desc.id ) {
			CL_HwFreeTexture( &hwTextures[i] );
		}
	}
	hwPending.valid = qfalse;
	hwShown.valid = qfalse;
	hwOverlayValid = qfalse;
	CL_DsModuleMap( "" );	// (a DS module loads its map again)
	CL_HwGameStopped();
}

static void CL_HwCopyFrame( hwFrame_t *dst, const hwFrame_t *src ) {
	int n = src->scene.rect[2] * src->scene.rect[3];
	dst->scene = src->scene;
	Com_Memcpy( dst->quads, src->quads, src->scene.numQuads * sizeof( oeHwQuad_t ) );
	Com_Memcpy( dst->tiles, src->tiles, sizeof( dst->tiles ) );
	if ( dst->snapshotSize < n ) {
		if ( dst->snapshot ) {
			free( dst->snapshot );
		}
		dst->snapshot = calloc( n, sizeof( unsigned short ) );	// (malloc: at the window's size it outgrows the zone)
		dst->snapshotSize = n;
	}
	Com_Memcpy( dst->snapshot, src->snapshot, n * sizeof( unsigned short ) );
	dst->valid = src->valid;
}

void CL_HwScene( const oeHwScene_t *scene, const oeHwQuad_t *quads, const short *flatTiles,
		const unsigned short *screen ) {
	int n, y;

	if ( scene->numQuads < 0 || scene->numQuads > OE_HW_MAX_QUADS ||
		scene->rect[2] <= 0 || scene->rect[3] <= 0 || scene->rect[2] > 4096 || scene->rect[3] > 4096 ) {
		hwPending.valid = qfalse;
		return;
	}
	hwPending.scene = *scene;
	Com_Memcpy( hwPending.quads, quads, scene->numQuads * sizeof( oeHwQuad_t ) );
	Com_Memcpy( hwPending.tiles, flatTiles, sizeof( hwPending.tiles ) );

	n = scene->rect[2] * scene->rect[3];
	if ( hwPending.snapshotSize < n ) {
		if ( hwPending.snapshot ) {
			free( hwPending.snapshot );
		}
		hwPending.snapshot = calloc( n, sizeof( unsigned short ) );
		hwPending.snapshotSize = n;
	}
	for ( y = 0; y < scene->rect[3]; y++ ) {
		Com_Memcpy( hwPending.snapshot + y * scene->rect[2], screen + y * scene->screenPitch,
			scene->rect[2] * sizeof( unsigned short ) );
	}
	hwPending.valid = qtrue;
}

void CL_HwScreen( const unsigned short *px, int w, int h, int pitch ) {
	int x, y, tw, th;
	const oeHwScene_t *s;

	if ( !CL_HwAvailable() ) {
		return;
	}
	// the 3D view described since the last frame belongs to this one
	if ( hwPending.valid ) {
		CL_HwCopyFrame( &hwShown, &hwPending );
		hwPending.valid = qfalse;
	} else {
		hwShown.valid = qfalse;
	}

	// the game's screen with the untouched 3D view made transparent
	tw = 1;
	while ( tw < w ) tw <<= 1;
	th = 1;
	while ( th < h ) th <<= 1;
	if ( hwOverlaySize < tw * th * 4 ) {
		if ( hwOverlay ) {
			free( hwOverlay );
		}
		hwOverlaySize = tw * th * 4;
		hwOverlay = calloc( hwOverlaySize, 1 );	// (an HD screen's is tens of MB)
	}
	s = &hwShown.scene;
	for ( y = 0; y < h; y++ ) {
		const unsigned short *src = px + y * pitch;
		byte *out = hwOverlay + y * tw * 4;
		qboolean inRow = hwShown.valid && y >= s->rect[1] && y < s->rect[1] + s->rect[3];
		const unsigned short *snap = inRow ? hwShown.snapshot + ( y - s->rect[1] ) * s->rect[2] - s->rect[0] : NULL;
		for ( x = 0; x < w; x++, out += 4 ) {
			unsigned c = src[x];
			out[0] = (byte)( ( ( c >> 11 ) & 31 ) * 255 / 31 );
			out[1] = (byte)( ( ( c >> 5 ) & 63 ) * 255 / 63 );
			out[2] = (byte)( ( c & 31 ) * 255 / 31 );
			out[3] = ( snap && x >= s->rect[0] && x < s->rect[0] + s->rect[2] && snap[x] == c ) ? 0 : 255;
		}
	}
	hwOverlayW = w;
	hwOverlayH = h;
	hwOverlayTexW = tw;
	hwOverlayTexH = th;
	hwOverlayValid = qtrue;
	hwOverlayDirty = qtrue;	// uploaded when drawn
}

/* game world (16.16, y south) -> Quake 3 world (y north) */
static void CL_HwPoint( const int *p, vec3_t out ) {
	out[0] = p[0] / 65536.0f;
	out[1] = -p[1] / 65536.0f;
	out[2] = p[2] / 65536.0f;
}

static void CL_HwColor( polyVert_t *v, int fade, int alpha ) {
	v->modulate[0] = v->modulate[1] = v->modulate[2] = (byte)fade;
	v->modulate[3] = (byte)alpha;
}

/* view axes for a yaw and pitch in degrees (pitch > 0 looks down, as in Quake 3) */
static void CL_HwViewAxis( float yaw, float pitch, vec3_t axis[3] ) {
	vec3_t angles;
	angles[PITCH] = pitch;
	angles[YAW] = yaw;
	angles[ROLL] = 0;
	AnglesToAxis( angles, axis );
}

/* the refdef of the game's 3D view rectangle in the window (phone screen at sx, sy, scale) */
static void CL_HwViewRect( refdef_t *rd, const oeHwScene_t *s, float sx, float sy, float scale ) {
	rd->x = (int)( sx + ( s->rect[0] + s->shiftX ) * scale );
	rd->y = (int)( sy + ( s->rect[1] + s->shiftY ) * scale );
	rd->width = (int)( s->rect[2] * scale + 0.5f );
	rd->height = (int)( s->rect[3] * scale + 0.5f );
	// the games project with a 90 degree horizontal field and square pixels
	rd->fov_x = 90.0f;
	rd->fov_y = (float)( atan2( s->rect[3], s->rect[2] ) * 360.0 / M_PI );
	rd->time = cls.realtime;
}

/* the polygons of the shown frame's 3D view into a new scene, seen along axis (Orcs & Elves DS:
 * the cartridge's map and the module's sprites, cl_dsmap.c) */
static void CL_HwAddScene( vec3_t axis[3] ) {
	const oeHwScene_t *s = &hwShown.scene;
	polyVert_t	v[4];
	int			i, k, fade;

	re.ClearScene();
	if ( CL_DsModuleMapActive() ) {
		CL_DsAddScene( axis );
		return;
	}
	fade = s->fade < 0 ? 0 : s->fade > 255 ? 255 : s->fade;

	for ( i = 0; i < s->numQuads; i++ ) {
		const oeHwQuad_t *q = &hwShown.quads[i];
		qhandle_t sh = CL_HwShader( q->tex, q->blend );
		int alpha = q->blend == OE_HWB_ALPHA || q->blend == OE_HWB_ADD ? q->alpha : 255;
		if ( !sh ) {
			continue;
		}
		for ( k = 0; k < 4; k++ ) {
			CL_HwPoint( q->xyz[k], v[k].xyz );
			v[k].st[0] = q->st[k][0] / 65536.0f;
			v[k].st[1] = q->st[k][1] / 65536.0f;
			CL_HwColor( &v[k], q->blend == OE_HWB_MULTIPLY ? 255 : fade, alpha );
		}
		re.AddPolyToScene( sh, 4, v, 1 );
	}

	// floor and ceiling tiles
	for ( i = 0; i < 2048; i++ ) {
		int tex = hwShown.tiles[i], tx = ( i & 1023 ) & 31, ty = ( i & 1023 ) >> 5;
		int z = i < 1024 ? s->floorZ : s->ceilZ;
		int p[4][3];
		qhandle_t sh;
		if ( tex <= 0 || !( sh = CL_HwShader( tex, OE_HWB_OPAQUE ) ) ) {
			continue;
		}
		p[0][0] = p[3][0] = tx * 64 << 16;
		p[1][0] = p[2][0] = ( tx + 1 ) * 64 << 16;
		p[0][1] = p[1][1] = ty * 64 << 16;
		p[2][1] = p[3][1] = ( ty + 1 ) * 64 << 16;
		for ( k = 0; k < 4; k++ ) {
			p[k][2] = z;
			CL_HwPoint( p[k], v[k].xyz );
			CL_HwColor( &v[k], fade, 255 );
		}
		v[0].st[0] = v[3].st[0] = 0;
		v[1].st[0] = v[2].st[0] = 1;
		v[0].st[1] = v[1].st[1] = 0;
		v[2].st[1] = v[3].st[1] = 1;
		re.AddPolyToScene( sh, 4, v, 1 );
	}
}

/* the 3D view of the shown frame into the window rectangle of the phone screen (x, y, scale) */
static void CL_HwDrawScene( float sx, float sy, float scale ) {
	const oeHwScene_t *s = &hwShown.scene;
	refdef_t	rd;

	Com_Memset( &rd, 0, sizeof( rd ) );
	CL_HwViewRect( &rd, s, sx, sy, scale );
	rd.vieworg[0] = s->viewX / 65536.0f;
	rd.vieworg[1] = -s->viewY / 65536.0f;
	rd.vieworg[2] = s->viewZ / 65536.0f;
	CL_HwViewAxis( s->angle * ( 360.0f / 1024.0f ), CL_HwViewPitch(), rd.viewaxis );
	{
		static int traceFrames;
		if ( Cvar_VariableIntegerValue( "oe_dsTrace" ) && ( traceFrames++ % 60 ) == 0 ) {
			Com_Printf( "cl_hw: drawing %.0f %.0f %.0f angle %d\n", rd.vieworg[0], -rd.vieworg[1], rd.vieworg[2], s->angle );
		}
	}
	CL_HwAddScene( rd.viewaxis );
	rd.rdflags = RDF_NOWORLDMODEL;
	re.RenderScene( &rd );
}

/*
=======================================================================

QUAKE 3 MAPS

map <name> / devmap <name> load maps/<name>.bsp (textures, shaders and all
from the search path, pk3 files included) into the renderer. Then:

oe_playover        the game goes on as before (its steps, HUD, menus,
                   weapon), but its 3D view shows the Quake 3 map: the
                   player's moves and turns since oe_playover started are
                   applied from the map's spawn point (info_player_start,
                   else info_player_intermission, else info_player_*)
oe_idtech3freecam  a spectator's free camera, as in Tremulous before joining a
                   team: no HUD, the mouse looks around, W/A/S/D (or the
                   arrows) fly, space rises, C/Ctrl sinks, Shift is faster,
                   Escape lands. Over the Quake 3 map in oe_playover or after
                   devmap, else over the game's own map: the game then renders
                   its view from the camera, the whole map, without what
                   seeing does in the game (OE_TRAP_HW_FREEVIEW); the game
                   itself waits, it gets no keys.

The same command again goes back to where it started (oemap: to the game's own
view). A map stays loaded until another one is loaded (which restarts the
renderer).

=======================================================================
*/

typedef enum {
	Q3_NONE,
	Q3_PLAYOVER,
	Q3_FREECAM,			// the free camera over the Quake 3 map
	GAME_FREECAM,		// the free camera over the game's own map
	DS_VIEW				// the free camera over an Orcs & Elves DS map (cl_dsmap.c)
} q3Mode_t;

static q3Mode_t	q3Mode;
static char		q3MapName[MAX_QPATH];		// loaded into the renderer
static vec3_t	q3SpawnOrigin;
static float	q3SpawnYaw;

// oe_playover: the game's camera when it started, mapped to the spawn point
static qboolean	poAnchored;
static int		poAnchorX, poAnchorY, poAnchorZ, poAnchorAngle;

// oe_idtech3freecam (Quake 3 axes, in the game's map too)
static vec3_t	fcOrigin;
static float	fcYaw, fcPitch;
static int		fcLastTime;
static q3Mode_t	fcReturnMode;		// back to this when it ends
static cvar_t	*oe_freecamSpeed;	// units per second (Shift: twice)
static cvar_t	*oe_freecamSensitivity;	// like Quake 3's sensitivity (degrees per count: 0.022 times it)

static void CL_Q3PlayoverView( refdef_t *rd, float *yawOut );

/* finds the spawn point of the loaded map */
static void CL_Q3FindSpawn( void ) {
	static const char *spawns[] = { "info_player_start", "info_player_intermission", "info_player" };
	char	key[MAX_TOKEN_CHARS], value[MAX_TOKEN_CHARS], classname[MAX_TOKEN_CHARS];
	vec3_t	entOrigin;
	float	entYaw = 0;
	int		best = ARRAY_LEN( spawns ), k;
	qboolean hasOrigin = qfalse;

	VectorClear( q3SpawnOrigin );
	q3SpawnYaw = 0;
	classname[0] = 0;
	while ( re.GetEntityToken( key, sizeof( key ) ) ) {
		if ( !strcmp( key, "{" ) ) {
			classname[0] = 0;
			hasOrigin = qfalse;
			entYaw = 0;
			continue;
		}
		if ( !strcmp( key, "}" ) ) {
			for ( k = 0; k < best && hasOrigin; k++ ) {
				if ( k < 2 ? !Q_stricmp( classname, spawns[k] ) : !Q_stricmpn( classname, spawns[k], 11 ) ) {
					best = k;
					VectorCopy( entOrigin, q3SpawnOrigin );
					q3SpawnYaw = entYaw;
					break;
				}
			}
			continue;
		}
		if ( !re.GetEntityToken( value, sizeof( value ) ) ) {
			break;
		}
		if ( !Q_stricmp( key, "classname" ) ) {
			Q_strncpyz( classname, value, sizeof( classname ) );
		} else if ( !Q_stricmp( key, "origin" ) ) {
			hasOrigin = sscanf( value, "%f %f %f", &entOrigin[0], &entOrigin[1], &entOrigin[2] ) == 3;
		} else if ( !Q_stricmp( key, "angle" ) ) {
			entYaw = atof( value );
		} else if ( !Q_stricmp( key, "angles" ) ) {
			vec3_t a;
			if ( sscanf( value, "%f %f %f", &a[0], &a[1], &a[2] ) == 3 ) {
				entYaw = a[1];
			}
		}
	}
	Com_Printf( "%s: from %.0f %.0f %.0f, facing %.0f (%s)\n", q3MapName, q3SpawnOrigin[0], q3SpawnOrigin[1],
		q3SpawnOrigin[2], q3SpawnYaw, best < 2 ? spawns[best] : best < (int)ARRAY_LEN( spawns ) ? "info_player_*" :
		"no spawn point: the map's origin" );
}

/* loads maps/<name>.bsp into the renderer (restarting it when another map is loaded) */
static qboolean CL_Q3LoadMap( const char *arg ) {
	char name[MAX_QPATH];

	Q_strncpyz( name, arg, sizeof( name ) );
	if ( !strchr( name, '/' ) ) {
		Com_sprintf( name, sizeof( name ), "maps/%s", arg );
	}
	COM_DefaultExtension( name, sizeof( name ), ".bsp" );
	if ( FS_ReadFile( name, NULL ) < 0 ) {	// anywhere on the search path, like the map loader
		Com_Printf( "%s not found\n", name );
		return qfalse;
	}
	if ( q3MapName[0] && !Q_stricmp( q3MapName, name ) ) {
		return qtrue;	// already loaded
	}
	if ( q3MapName[0] ) {
		CL_Vid_Restart_f();	// the renderer holds one world until it restarts
	}
	re.LoadWorld( name );
	Q_strncpyz( q3MapName, name, sizeof( q3MapName ) );
	CL_Q3FindSpawn();
	return qtrue;
}

static void CL_Q3StartPlayover( void ) {
	q3Mode = Q3_PLAYOVER;
	poAnchored = qfalse;	// at the game's next frame with a 3D view
	Com_Printf( "oe_playover: the game's steps on %s\n", q3MapName );
}

static void CL_FreecamBegin( q3Mode_t mode, const char *over ) {
	fcPitch = 0;
	fcLastTime = cls.realtime;
	fcReturnMode = q3Mode == Q3_PLAYOVER ? Q3_PLAYOVER : Q3_NONE;
	q3Mode = mode;
	Key_ClearStates();	// the game's keys held until now are let go
	Com_Printf( "oe_idtech3freecam: flying over %s (mouse looks, W/A/S/D fly, space up, C down, Shift faster, "
		"Escape %s)\n", over, mode == DS_VIEW ? "opens the console" : "lands" );
}

static void CL_Q3StartFreecam( void ) {
	if ( q3Mode == Q3_PLAYOVER && poAnchored && hwShown.valid ) {
		// go on from where the game's camera is on the map
		refdef_t rd;
		CL_Q3PlayoverView( &rd, &fcYaw );
		VectorCopy( rd.vieworg, fcOrigin );
	} else {
		VectorCopy( q3SpawnOrigin, fcOrigin );
		fcOrigin[2] += 26;	// eye height
		fcYaw = q3SpawnYaw;
	}
	CL_FreecamBegin( Q3_FREECAM, q3MapName );
}

/* over the game's own map, from the eye of its shown frame */
static void CL_GameStartFreecam( void ) {
	const oeHwScene_t *s = &hwShown.scene;

	fcOrigin[0] = s->viewX / 65536.0f;
	fcOrigin[1] = -s->viewY / 65536.0f;
	fcOrigin[2] = s->viewZ / 65536.0f;
	fcYaw = s->angle * ( 360.0f / 1024.0f );
	CL_FreecamBegin( GAME_FREECAM, "the game's map" );
}

/* Orcs & Elves DS: its map loaded by cl_dsmap.c, the camera from the map's spawn tile */
void CL_HwDsView( const vec3_t origin, float yaw, const char *map ) {
	VectorCopy( origin, fcOrigin );
	fcYaw = yaw;
	CL_FreecamBegin( DS_VIEW, map );
	fcReturnMode = DS_VIEW;	// there is nothing else to go back to
	Con_Close();			// (open at startup until there is something to show)
}

qboolean CL_HwDsViewActive( void ) {
	return q3Mode == DS_VIEW;
}

static void CL_FreecamEnd( void ) {
	if ( q3Mode == DS_VIEW ) {
		Com_Printf( "Orcs & Elves DS: the map's free camera stays (oe_dsmap <map> loads another one)\n" );
		return;
	}
	q3Mode = fcReturnMode;
	if ( q3Mode == Q3_PLAYOVER ) {
		Com_Printf( "oe_idtech3freecam off: back to oe_playover\n" );
	} else {
		Com_Printf( "oe_idtech3freecam off: the game's own view\n" );
	}
}

/* OE_TRAP_HW_FREEVIEW: the camera for the game's renderer, in its world units and angles */
qboolean CL_HwFreeView( int *view ) {
	if ( q3Mode != GAME_FREECAM ) {
		return qfalse;
	}
	view[0] = (int)floor( fcOrigin[0] );
	view[1] = (int)floor( -fcOrigin[1] );
	view[2] = (int)floor( fcOrigin[2] );
	view[3] = (int)floor( fcYaw * ( 1024.0f / 360.0f ) + 0.5f ) & 1023;
	return qtrue;
}

static void CL_Map_f( void ) {
	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: map <name>   (maps/<name>.bsp of Quake 3; oemap for the game's own maps)\n" );
		return;
	}
	if ( !CL_Q3LoadMap( Cmd_Argv( 1 ) ) ) {
		return;
	}
	if ( OE_GameRunning() && CL_HwAvailable() ) {
		CL_Q3StartPlayover();
	} else {
		CL_Q3StartFreecam();
	}
}

static void CL_Devmap_f( void ) {
	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: devmap <name>   (maps/<name>.bsp of Quake 3, free camera; devoemap for the game's own maps)\n" );
		return;
	}
	if ( CL_Q3LoadMap( Cmd_Argv( 1 ) ) ) {
		CL_Q3StartFreecam();
	}
}

static void CL_Playover_f( void ) {
	if ( q3Mode == Q3_PLAYOVER ) {
		q3Mode = Q3_NONE;
		Com_Printf( "oe_playover off: the game's own view\n" );
		return;
	}
	if ( !q3MapName[0] ) {
		Com_Printf( "oe_playover: load a Quake 3 map first (map <name>)\n" );
		return;
	}
	if ( !OE_GameRunning() || !CL_HwAvailable() ) {
		Com_Printf( "oe_playover: needs a running game with oe_renderer 1\n" );
		return;
	}
	CL_Q3StartPlayover();
}

static void CL_Freecam_f( void ) {
	if ( q3Mode == Q3_FREECAM || q3Mode == GAME_FREECAM || q3Mode == DS_VIEW ) {
		CL_FreecamEnd();
	} else if ( q3Mode == Q3_PLAYOVER ) {
		CL_Q3StartFreecam();
	} else if ( OE_GameRunning() && CL_HwAvailable() && hwShown.valid ) {
		CL_GameStartFreecam();
	} else if ( q3MapName[0] ) {
		CL_Q3StartFreecam();
	} else {
		Com_Printf( "oe_idtech3freecam: needs the game's 3D view on screen (with oe_renderer 1) "
			"or a Quake 3 map (map/devmap <name>)\n" );
	}
}

/* back to the game's own view (oemap, devoemap) */
void CL_HwLeaveQ3( void ) {
	q3Mode = Q3_NONE;
}

/* a game started or stopped: no free camera over its map any more */
static void CL_HwGameStopped( void ) {
	if ( q3Mode == GAME_FREECAM ) {
		q3Mode = Q3_NONE;
	}
}

/* the game's camera of the shown frame, moved onto the map */
static void CL_Q3PlayoverView( refdef_t *rd, float *yawOut ) {
	const oeHwScene_t *s = &hwShown.scene;
	float scale = oe_playoverScale->value > 0 ? oe_playoverScale->value : 1.0f;
	float dx = ( s->viewX - poAnchorX ) / 65536.0f, dy = -( s->viewY - poAnchorY ) / 65536.0f;	// Quake 3 axes
	float rot = DEG2RAD( q3SpawnYaw - poAnchorAngle * ( 360.0f / 1024.0f ) );
	float yaw = q3SpawnYaw + ( s->angle - poAnchorAngle ) * ( 360.0f / 1024.0f );

	rd->vieworg[0] = q3SpawnOrigin[0] + scale * ( dx * cos( rot ) - dy * sin( rot ) );
	rd->vieworg[1] = q3SpawnOrigin[1] + scale * ( dx * sin( rot ) + dy * cos( rot ) );
	rd->vieworg[2] = q3SpawnOrigin[2] + 26 + scale * ( s->viewZ - poAnchorZ ) / 65536.0f;
	CL_HwViewAxis( yaw, 0, rd->viewaxis );
	*yawOut = yaw;
}

static void CL_Q3DrawPlayover( float sx, float sy, float scale ) {
	refdef_t rd;
	float yaw;

	if ( !poAnchored ) {
		poAnchorX = hwShown.scene.viewX;
		poAnchorY = hwShown.scene.viewY;
		poAnchorZ = hwShown.scene.viewZ;
		poAnchorAngle = hwShown.scene.angle;
		poAnchored = qtrue;
	}
	re.ClearScene();
	Com_Memset( &rd, 0, sizeof( rd ) );
	CL_HwViewRect( &rd, &hwShown.scene, sx, sy, scale );
	CL_Q3PlayoverView( &rd, &yaw );
	re.RenderScene( &rd );
}

qboolean CL_HwWorldViewActive( void ) {
	return ( q3Mode == Q3_FREECAM && q3MapName[0] ) || q3Mode == GAME_FREECAM || q3Mode == DS_VIEW;
}

#define FC_KEY( a, b ) ( ( keys[a].down || keys[b].down ) ? 1.0f : 0.0f )

/*
=======================================================================

FREE MOVEMENT (oe_moveMode, see oeFreeMove_t)

While the game explores in a free mode it asks for the input every tick
(OE_TRAP_FREE_MOVE): W/S or up/down move, A/D step sideways, left/right
turn, the mouse looks around (up and down only in the GPU view: the game's
view is level) and the left button acts (the phone's select). The game's
own movement keys do nothing there; in its menus they work as before.

=======================================================================
*/

static cvar_t	*oe_moveMode;		// OE_MOVE_*
static cvar_t	*oe_moveSpeed;		// world units per second (64 a tile)
static cvar_t	*oe_turnTime;		// ms per turn in OE_MOVE_TIMER
static int		fpLastAsk;			// cls.realtime of the game's last OE_TRAP_FREE_MOVE
static float	fpYaw;				// turned since then, degrees
static float	fpPitch;			// looking up/down, degrees (the GPU view only)

/* the game is exploring in a free mode (it asked for the input lately) */
qboolean CL_FreePlayActive( void ) {
	return oe_moveMode->integer > 0 && OE_GameRunning() && q3Mode != GAME_FREECAM && q3Mode != Q3_FREECAM &&
		cls.realtime - fpLastAsk < 250;
}

int CL_FreeMove( oeFreeMove_t *move ) {
	static int lastTime;
	float dt = ( cls.realtime - lastTime ) * 0.001f;
	int mode = oe_moveMode->integer;

	lastTime = cls.realtime;
	Com_Memset( move, 0, sizeof( *move ) );
	if ( mode < OE_MOVE_TURNS || mode > OE_MOVE_TIMER ) {
		mode = OE_MOVE_TURNS;
	}
	move->mode = mode;
	if ( mode == OE_MOVE_TURNS ) {
		return mode;
	}
	if ( !CL_FreePlayActive() ) {
		fpYaw = 0;	// the mouse moved for something else
	}
	fpLastAsk = cls.realtime;
	move->speed = oe_moveSpeed->integer > 0 ? oe_moveSpeed->integer : 192;
	move->turnMs = oe_turnTime->integer >= 100 ? oe_turnTime->integer : 100;
	if ( q3Mode == GAME_FREECAM || q3Mode == Q3_FREECAM ) {
		return mode;	// the keys and the mouse fly the free camera
	}
	if ( dt < 0 || dt > 0.1f ) {
		dt = dt < 0 ? 0 : 0.1f;
	}
	if ( !( Key_GetCatcher() & KEYCATCH_CONSOLE ) ) {
		move->forward = (int)( 256 * ( FC_KEY( 'w', K_UPARROW ) - FC_KEY( 's', K_DOWNARROW ) ) );
		move->side = (int)( 256 * ( FC_KEY( 'd', 'd' ) - FC_KEY( 'a', 'a' ) ) );
		fpYaw += 150.0f * dt * ( FC_KEY( K_LEFTARROW, K_LEFTARROW ) - FC_KEY( K_RIGHTARROW, K_RIGHTARROW ) );
	}
	move->yaw = (int)( fpYaw * ( 1024.0f * 64.0f / 360.0f ) );
	fpYaw -= move->yaw * ( 360.0f / ( 1024.0f * 64.0f ) );	// keep the remainder
	return mode;
}

/* the left button acts while exploring freely: qtrue if taken */
qboolean CL_FreePlayKey( int key, qboolean down ) {
	if ( key != K_MOUSE1 || ( down && !CL_FreePlayActive() ) ) {
		return qfalse;
	}
	OE_PhoneKey( 0xE035, down );	// AVK_SELECT: use / attack
	return qtrue;
}

/* the free camera, or free movement, takes the mouse (relative, the cursor hidden) unless the console is down */
qboolean CL_HwWantsMouse( void ) {
	return ( CL_HwWorldViewActive() || CL_FreePlayActive() ) && !( Key_GetCatcher() & KEYCATCH_CONSOLE );
}

/* mouse movement (counts): looks around, like Quake 3's m_yaw / m_pitch 0.022 */
void CL_HwFreecamMouse( int dx, int dy ) {
	float k = 0.022f * oe_freecamSensitivity->value;

	if ( !CL_HwWantsMouse() ) {
		return;
	}
	if ( !CL_HwWorldViewActive() ) {
		// free movement: the game turns, the GPU view also looks up and down
		fpYaw -= dx * k;
		fpPitch += dy * k;
		fpPitch = fpPitch > 60 ? 60 : fpPitch < -60 ? -60 : fpPitch;
		return;
	}
	fcYaw -= dx * k;
	fcPitch += dy * k;
	fcPitch = fcPitch > 89 ? 89 : fcPitch < -89 ? -89 : fcPitch;
	fcYaw = AngleNormalize360( fcYaw );
}

/* the pitch of the game's view on the GPU */
static float CL_HwViewPitch( void ) {
	return CL_FreePlayActive() ? fpPitch : 0;
}

/* oe_idtech3freecam: the map over the whole window */
void CL_HwDrawWorldView( void ) {
	refdef_t	rd;
	vec3_t		dir;
	float		dt = ( cls.realtime - fcLastTime ) * 0.001f;
	float		move = 0, side = 0, up = 0, speed;

	if ( dt < 0 ) {
		dt = 0;
	} else if ( dt > 0.1f ) {
		dt = 0.1f;
	}
	fcLastTime = cls.realtime;
	if ( !( Key_GetCatcher() & KEYCATCH_CONSOLE ) ) {
		move = FC_KEY( 'w', K_UPARROW ) - FC_KEY( 's', K_DOWNARROW );
		side = FC_KEY( 'd', K_RIGHTARROW ) - FC_KEY( 'a', K_LEFTARROW );
		up = FC_KEY( K_SPACE, K_SPACE ) - FC_KEY( 'c', K_CTRL );
	}
	speed = oe_freecamSpeed->value * ( keys[K_SHIFT].down ? 2.0f : 1.0f ) * dt;

	// where it looks, sideways level, up and down straight
	Com_Memset( &rd, 0, sizeof( rd ) );
	CL_HwViewAxis( fcYaw, fcPitch, rd.viewaxis );
	dir[0] = (float)sin( DEG2RAD( fcYaw ) );
	dir[1] = (float)-cos( DEG2RAD( fcYaw ) );
	dir[2] = 0;
	if ( q3Mode == DS_VIEW && CL_DsWalking() ) {
		// walking on a DS map: level, at eye height, sliding along the walls
		vec3_t step;
		step[0] = speed * ( move * (float)cos( DEG2RAD( fcYaw ) ) + side * dir[0] );
		step[1] = speed * ( move * (float)sin( DEG2RAD( fcYaw ) ) + side * dir[1] );
		step[2] = 0;
		CL_DsWalk( fcOrigin, step );
	} else {
		VectorMA( fcOrigin, speed * move, rd.viewaxis[0], fcOrigin );
		VectorMA( fcOrigin, speed * side, dir, fcOrigin );
		fcOrigin[2] += speed * up;
	}

	if ( q3Mode == GAME_FREECAM ) {
		// the game's last frame, rendered from (about) here
		re.ClearScene();
		if ( hwShown.valid ) {
			CL_HwAddScene( rd.viewaxis );
		}
		rd.rdflags = RDF_NOWORLDMODEL;
	} else if ( q3Mode == DS_VIEW ) {
		re.ClearScene();
		CL_DsAddScene( rd.viewaxis );
		rd.rdflags = RDF_NOWORLDMODEL;
	} else {
		re.ClearScene();
	}
	rd.width = cls.glconfig.vidWidth;
	rd.height = cls.glconfig.vidHeight;
	rd.fov_x = 90.0f;
	rd.fov_y = (float)( atan2( rd.height, rd.width ) * 360.0 / M_PI );	// with fov_x 90
	VectorCopy( fcOrigin, rd.vieworg );
	rd.time = cls.realtime;
	re.RenderScene( &rd );
}

/*
=======================================================================

DRAWING

=======================================================================
*/

/* the phone screen at (x, y) size (dw, dh): qtrue if drawn here (the 3D view and the 2D on top) */
qboolean CL_HwDraw( float x, float y, float dw, float dh ) {
	int w, h, texW, texH, version;

	if ( !CL_HwAvailable() || !hwOverlayValid || !OE_ScreenImage( &w, &h, &texW, &texH, &version ) ||
		w != hwOverlayW || h != hwOverlayH ) {
		return qfalse;
	}
	if ( filterUsed != oe_hwFilter->integer ) {
		filterUsed = oe_hwFilter->integer;
		textureGen++;	// upload the textures again with the other filter
	}
	if ( hwOverlayDirty || hwOverlayShaderGen != rendererGen || cl_screenFilter->modified ) {
		hwOverlayShader = re.UpdateRawImage( "*oe_overlay", hwOverlay, hwOverlayTexW, hwOverlayTexH,
			!cl_screenFilter->integer );
		hwOverlayShaderGen = rendererGen;
		hwOverlayDirty = qfalse;
	}
	if ( hwShown.valid ) {
		if ( q3Mode == Q3_PLAYOVER && q3MapName[0] ) {
			CL_Q3DrawPlayover( x, y, dw / w );
		} else {
			CL_HwDrawScene( x, y, dw / w );
		}
	}
	re.DrawStretchPic( x, y, dw, dh, 0, 0, (float)w / hwOverlayTexW, (float)h / hwOverlayTexH, hwOverlayShader );
	return qtrue;
}

void CL_HwRendererStarted( void ) {
	rendererGen++;
	hwOverlayShader = 0;
	CL_DsRendererStarted();
}

void CL_HwInit( void ) {
	oe_renderer = Cvar_Get( "oe_renderer", "1", CVAR_ARCHIVE );	// read when a game starts
	oe_hwFilter = Cvar_Get( "oe_hwFilter", "0", CVAR_ARCHIVE );
	oe_playoverScale = Cvar_Get( "oe_playoverScale", "1", CVAR_ARCHIVE );
	oe_freecamSpeed = Cvar_Get( "oe_freecamSpeed", "320", CVAR_ARCHIVE );
	oe_freecamSensitivity = Cvar_Get( "oe_freecamSensitivity", "5", CVAR_ARCHIVE );	// also free movement's mouse
	oe_moveMode = Cvar_Get( "oe_moveMode", "0", CVAR_ARCHIVE );
	oe_moveSpeed = Cvar_Get( "oe_moveSpeed", "192", CVAR_ARCHIVE );
	oe_turnTime = Cvar_Get( "oe_turnTime", "500", CVAR_ARCHIVE );
	// the games' scenes are polygons, a few thousand per frame (a DS map: up to about 30000 triangles)
	Cvar_Get( "r_maxpolys", "65536", 0 );
	Cvar_Get( "r_maxpolyverts", "196608", 0 );
	Cmd_AddCommand( "map", CL_Map_f );
	Cmd_AddCommand( "devmap", CL_Devmap_f );
	Cmd_AddCommand( "r_testbsp", CL_Devmap_f );	// the older name
	Cmd_AddCommand( "oe_playover", CL_Playover_f );
	Cmd_AddCommand( "oe_idtech3freecam", CL_Freecam_f );
	CL_DsInit();
}

void CL_HwVidRestart( void ) {
	q3MapName[0] = 0;	// the renderer drops its world
	if ( q3Mode != GAME_FREECAM && q3Mode != DS_VIEW ) {	// (a DS map is not in the renderer)
		q3Mode = Q3_NONE;
	}
	fcReturnMode = Q3_NONE;
}
