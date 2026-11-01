/*
 * BREW resource archive (.bar), shared by both games. Based on bar.c of
 * oe-re / oe2-re (GPL-3.0): O&E 1 also stores string resources (type 1),
 * O&E II only data resources, so the O&E 1 reader serves both.
 *
 * Header (little endian):
 *   0x00 u16 version (0x11)   0x06 u16 number of type-directory entries
 *   0x08 u32 type directory offset (8 bytes each: u16 type, u16 first id, u16 last-first, u16 first index)
 *   0x10 u32 entry offset table   0x18 u32 offset of the first entry (= end of the table)
 * Each data entry: u16 header length (including itself), MIME string, payload (usually gzip).
 * String entries: one encoding byte, then the text without terminator.
 */
#include "oe_local.h"
#include "../zlib/zlib.h"

static unsigned rd16( const byte *p ) { return p[0] | ( p[1] << 8 ); }
static unsigned rd32( const byte *p ) { return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( (unsigned)p[3] << 24 ); }

unsigned OE_Crc32( const byte *data, int size ) {
	return (unsigned)crc32( 0L, data, (uInt)size );
}

qboolean Bar_Open( bar_t *bar, byte *data, int size ) {
	unsigned ntypes, k, j;
	unsigned typedir, tbl, tblend, nent;

	Com_Memset( bar, 0, sizeof( *bar ) );
	bar->data = data;
	bar->size = size;
	if ( size < 0x20 ) {
		Bar_Close( bar );
		return qfalse;
	}

	ntypes = rd16( data + 6 );
	typedir = rd32( data + 0x08 );
	tbl = rd32( data + 0x10 );
	tblend = rd32( data + 0x18 );
	/* 64-bit sums so a corrupt header cannot wrap past the checks */
	if ( (unsigned long long)typedir + (unsigned long long)ntypes * 8 > (unsigned)size || tbl >= tblend ||
		tblend > (unsigned)size ) {
		Bar_Close( bar );
		return qfalse;
	}
	nent = ( tblend - tbl ) / 4;
	bar->entries = calloc( nent ? nent : 1, sizeof( barEntry_t ) );
	if ( !bar->entries ) {
		Bar_Close( bar );
		return qfalse;
	}

	for ( k = 0; k < ntypes; k++ ) {
		const byte *t = data + typedir + k * 8;
		unsigned type = rd16( t ), first = rd16( t + 2 ), span = rd16( t + 4 ), idx = rd16( t + 6 );
		if ( type != 6 && type != 1 )
			continue;
		for ( j = 0; j <= span; j++ ) {
			unsigned o, e, hl;
			barEntry_t *be;
			if ( idx + j >= nent || (unsigned)bar->count >= nent )
				break;
			o = rd32( data + tbl + 4 * ( idx + j ) );
			e = ( idx + j + 1 < nent ) ? rd32( data + tbl + 4 * ( idx + j + 1 ) ) : (unsigned)size;
			if ( size < 2 || o > (unsigned)size - 2 || e > (unsigned)size || e < o )
				continue;
			hl = type == 1 ? 1 : rd16( data + o );
			if ( ( type != 1 && hl < 2 ) || hl > e - o )
				continue;
			be = &bar->entries[bar->count++];
			be->id = (int)( first + j );
			be->type = (int)type;
			be->offset = o + hl;
			be->size = e - o - hl;
			/* gzip FNAME */
			if ( be->size > 10 && data[be->offset] == 0x1f && data[be->offset + 1] == 0x8b ) {
				const byte *g = data + be->offset;
				unsigned p = 10;
				if ( g[3] & 4 )
					p = p + 2 <= be->size ? p + 2 + rd16( g + p ) : be->size;
				if ( ( g[3] & 8 ) && p < be->size ) {
					int n = 0;
					while ( p + n < be->size && g[p + n] && n < (int)sizeof( be->name ) - 1 )
						n++;
					Com_Memcpy( be->name, g + p, n );
				}
			}
		}
	}
	return qtrue;
}

void Bar_Close( bar_t *bar ) {
	if ( bar->data )
		free( bar->data );
	if ( bar->entries )
		free( bar->entries );
	Com_Memset( bar, 0, sizeof( *bar ) );
}

const barEntry_t *Bar_Find( const bar_t *bar, int id ) {
	int i;
	for ( i = 0; i < bar->count; i++ )
		if ( bar->entries[i].id == id && bar->entries[i].type == 6 )
			return &bar->entries[i];
	return NULL;
}

const barEntry_t *Bar_FindByName( const bar_t *bar, const char *name ) {
	int i;
	if ( *name == '/' )
		name++;
	for ( i = 0; i < bar->count; i++ )
		if ( !strcmp( bar->entries[i].name, name ) )
			return &bar->entries[i];
	return NULL;
}

int Bar_LoadString( const bar_t *bar, int id, char *buf, int n ) {
	int i;
	for ( i = 0; i < bar->count; i++ ) {
		const barEntry_t *e = &bar->entries[i];
		int len;
		if ( e->id != id || e->type != 1 )
			continue;
		len = (int)e->size < n - 1 ? (int)e->size : n - 1;
		Com_Memcpy( buf, bar->data + e->offset, len );
		buf[len] = 0;
		return len;
	}
	if ( n )
		buf[0] = 0;
	return -1;
}

static qboolean Bar_IsGzip( const bar_t *bar, const barEntry_t *e ) {
	const byte *src = bar->data + e->offset;
	return e->size >= 18 && src[0] == 0x1f && src[1] == 0x8b;
}

int Bar_UncompressedSize( const bar_t *bar, const barEntry_t *e ) {
	if ( !e )
		return -1;
	if ( !Bar_IsGzip( bar, e ) )
		return (int)e->size;
	return (int)rd32( bar->data + e->offset + e->size - 4 );	/* gzip ISIZE */
}

qboolean Bar_Read( const bar_t *bar, const barEntry_t *e, byte *out, int outSize ) {
	const byte *src;
	unsigned isize;
	z_stream zs;

	if ( !e )
		return qfalse;
	src = bar->data + e->offset;
	if ( !Bar_IsGzip( bar, e ) ) {
		if ( outSize < (int)e->size )
			return qfalse;
		Com_Memcpy( out, src, e->size );
		return qtrue;
	}
	isize = rd32( src + e->size - 4 );
	if ( outSize < (int)isize )
		return qfalse;
	Com_Memset( &zs, 0, sizeof( zs ) );
	if ( inflateInit2( &zs, 16 + MAX_WBITS ) != Z_OK )
		return qfalse;
	zs.next_in = (Bytef *)src;
	zs.avail_in = e->size;
	zs.next_out = out;
	zs.avail_out = isize;
	if ( inflate( &zs, Z_FINISH ) != Z_STREAM_END || zs.total_out != isize ) {
		inflateEnd( &zs );
		return qfalse;
	}
	inflateEnd( &zs );
	return qtrue;
}
