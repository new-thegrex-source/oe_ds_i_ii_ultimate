/*
 * Qualcomm CMF ("cmid") wave tracks -> PCM, shared by both games. Based on
 * pmd.c of oe-re / oe2-re (GPL-3.0); the QCELP frames are decoded by the
 * built-in decoder (oe_qcelp.c, from FFmpeg) instead of libavcodec.
 *
 * Every O&E / O&E II sound and music resource is a CMF file whose single track
 * carries QCELP-13k audio in 0xF1 "wave" events (draft-atarius-cmf-00). Each event is
 *   FF F1 <u16 len> <u16 codec=0x0044> <u32 total> <u8 seq> <len-7 bytes of QCELP packets>
 * Packets are variable rate: a rate byte, then 0/3/7/16/34 payload bytes; each
 * decodes to 160 samples at 8 kHz.
 */
#include "oe_local.h"
#include "oe_qcelp.h"

static unsigned be16( const byte *p ) { return ( p[0] << 8 ) | p[1]; }
static unsigned be32( const byte *p ) { return ( (unsigned)p[0] << 24 ) | ( p[1] << 16 ) | ( p[2] << 8 ) | p[3]; }

static int payload_len( unsigned rate ) {
	switch ( rate ) {
	case 0: case 0x0e: return 0;	/* blank, erasure */
	case 1: return 3;				/* eighth */
	case 2: case 5: return 7;		/* quarter */
	case 3: return 16;				/* half */
	case 4: return 34;				/* full */
	default: return -1;
	}
}

typedef struct {
	short	*s;
	int		n, cap;
} pcmOut_t;

static qboolean pcm_frame( pcmOut_t *o, const float *src, int count ) {
	int i;
	if ( o->n + count > o->cap ) {
		int cap = o->cap ? o->cap * 2 : 8192;
		short *ns;
		while ( cap < o->n + count )
			cap *= 2;
		ns = realloc( o->s, (size_t)cap * sizeof( short ) );
		if ( !ns )
			return qfalse;
		o->s = ns;
		o->cap = cap;
	}
	for ( i = 0; i < count; i++ ) {
		float v = src[i];
		v = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
		o->s[o->n++] = (short)( v * 32767.f );
	}
	return qtrue;
}

qboolean Pmd_Decode( const byte *cmf, int len, short **pcm, int *samples ) {
	static QCELPContext q;
	pcmOut_t out = { 0 };
	byte padded[64];
	float frame[160];
	int p, end, waveVolume = 63;

	*pcm = NULL;
	*samples = 0;
	if ( len < 16 || memcmp( cmf, "cmid", 4 ) != 0 )
		return qfalse;
	for ( p = 8; p + 8 <= len && memcmp( cmf + p, "trac", 4 ) != 0; p++ )
		;
	if ( p + 8 > len )
		return qfalse;
	end = p + 8 + (int)be32( cmf + p + 4 );
	if ( end > len || end < p )
		end = len;
	p += 8;

	Qcelp_Init( &q );

	/* FF E8 vv: wave-channel volume (channel in bits 7-6, 6-bit level), set per resource by the authors.
	 * The phone's CMF player applied it; we scale linearly since its exact curve is unknown. */
	while ( p + 2 <= end ) {
		unsigned b0, cmd;
		p++;	/* delta time */
		b0 = cmf[p++];
		if ( b0 != 0xFF ) {
			p += 2;	/* note event (gate + velocity/octave); unused by wave tracks */
			continue;
		}
		if ( p >= end )
			break;
		cmd = cmf[p++];
		if ( cmd == 0xDF )
			break;
		if ( cmd >= 0xF1 && cmd <= 0xF4 ) {
			unsigned l;
			if ( p + 2 > end )
				break;
			l = be16( cmf + p );
			if ( cmd == 0xF1 && l > 7 && p + 2 + (int)l <= end && be16( cmf + p + 2 ) == 0x0044 ) {
				const byte *qp = cmf + p + 2 + 7, *qe = cmf + p + 2 + l;
				while ( qp < qe ) {
					int pay = payload_len( *qp );
					if ( pay < 0 || qp + 1 + pay > qe )
						break;
					memset( padded, 0, sizeof( padded ) );
					memcpy( padded, qp, (size_t)( 1 + pay ) );
					Qcelp_DecodePacket( &q, padded, 1 + pay, frame );
					if ( !pcm_frame( &out, frame, 160 ) ) {
						free( out.s );
						return qfalse;
					}
					qp += 1 + pay;
				}
			}
			p += 2 + l;
		} else {
			if ( cmd == 0xE8 && p < end )
				waveVolume = cmf[p] & 0x3f;
			p++;	/* one-byte parameter */
		}
	}
	if ( waveVolume < 63 ) {
		int k;
		for ( k = 0; k < out.n; k++ )
			out.s[k] = (short)( out.s[k] * waveVolume / 63 );
	}
	/* (a track with no wave data decodes to silence, as with libavcodec in the ports) */
	*pcm = out.s;
	*samples = out.n;
	return qtrue;
}
