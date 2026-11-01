/*
 * The phone's sound mixer: OE_SND_CHANNELS channels of 8 kHz mono PCM, mixed
 * into an SDL2 audio device with windowed-sinc resampling. Shared by both
 * games; from the SDL layer of oe-re / oe2-re (GPL-3.0).
 *
 * The game's sounds are 8 kHz PCM; the device runs at 48 kHz or so. Linear
 * interpolation leaves strong images of the source spectrum above 4 kHz (a
 * harsh, metallic edge), so each output sample is a 32-tap Kaiser-windowed
 * sinc over the source, low-passed just under the source Nyquist.
 *
 * Built with OE_NO_AUDIO_DEVICE (headless QA host) there is no device: a
 * sound reports "playing" until it is stopped (see OE_AudioPlay).
 */
#include "oe_local.h"

#ifndef OE_NO_AUDIO_DEVICE
#ifdef USE_LOCAL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif
#endif

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define RS_HALF   16                 /* taps each side of the output position */
#define RS_TAPS   (2 * RS_HALF)
#define RS_PHASES 256
#define RS_CUTOFF 0.45               /* pass band edge, as a fraction of the source rate */
#define RS_BETA   8.0                /* Kaiser window shape */

#define OE_MAX_SOUNDS 512

typedef struct {
	short	*pcm;
	int		samples;
} oeSound_t;

typedef struct {
	const short	*pcm;
	int			samples;
	unsigned	step;		/* 16.16 source samples per output sample */
	unsigned long long pos;	/* 16.16 */
	int			loops;		/* remaining extra plays; -1 forever */
	int			volume;		/* 0..100 */
	int			playing;
} oeChan_t;

static oeSound_t	sounds[OE_MAX_SOUNDS];
static int			numSounds;
static oeChan_t		chans[OE_SND_CHANNELS];
static float		rsKernel[RS_PHASES + 1][RS_TAPS];
static cvar_t		*s_volume;

#ifndef OE_NO_AUDIO_DEVICE
static SDL_AudioDeviceID	dev;
static SDL_AudioSpec		have;
#endif

static double bessel_i0( double x ) {
	double sum = 1.0, term = 1.0;
	int k;
	for ( k = 1; k < 32; k++ ) {
		term *= ( x / ( 2.0 * k ) ) * ( x / ( 2.0 * k ) );
		sum += term;
	}
	return sum;
}

static void rs_build_kernel( void ) {
	int ph, t;
	for ( ph = 0; ph <= RS_PHASES; ph++ ) {
		double frac = (double)ph / RS_PHASES, sum = 0.0, row[RS_TAPS];
		for ( t = 0; t < RS_TAPS; t++ ) {
			double x = (double)( t - ( RS_HALF - 1 ) ) - frac;	/* source offset from the output position */
			double w = x / RS_HALF, sinc;
			double arg = 2.0 * M_PI * RS_CUTOFF * x;
			sinc = fabs( x ) < 1e-9 ? 1.0 : sin( arg ) / arg;
			row[t] = fabs( w ) >= 1.0 ? 0.0 : sinc * bessel_i0( RS_BETA * sqrt( 1.0 - w * w ) ) / bessel_i0( RS_BETA );
			sum += row[t];
		}
		for ( t = 0; t < RS_TAPS; t++ )
			rsKernel[ph][t] = (float)( row[t] / sum );	/* unity gain at DC for every phase */
	}
}

#ifndef OE_NO_AUDIO_DEVICE
/* Source sample at the 16.16 position `pos`, band-limited. Samples outside the clip count as silence. */
static float rs_sample( const oeChan_t *ch, unsigned long long pos ) {
	int ip = (int)( pos >> 16 ), first = ip - ( RS_HALF - 1 );
	unsigned fr = (unsigned)( pos & 0xffff );
	int ph = (int)( fr >> 8 );
	float mix = (float)( fr & 0xff ) / 256.0f, acc = 0.0f;
	const float *k0 = rsKernel[ph], *k1 = rsKernel[ph + 1];
	int t0 = first < 0 ? -first : 0;
	int t1 = first + RS_TAPS > ch->samples ? ch->samples - first : RS_TAPS;
	int t;
	for ( t = t0; t < t1; t++ )
		acc += (float)ch->pcm[first + t] * ( k0[t] + ( k1[t] - k0[t] ) * mix );
	return acc;
}

static void mix_cb( void *ud, Uint8 *stream, int len ) {
	short *out = (short *)stream;
	int frames = len / (int)( sizeof( short ) * have.channels );
	float master = s_volume ? s_volume->value : 1.0f;
	int i, c, k;
	(void)ud;
	for ( i = 0; i < frames; i++ ) {
		float acc = 0.0f;
		int s;
		for ( c = 0; c < OE_SND_CHANNELS; c++ ) {
			oeChan_t *ch = &chans[c];
			if ( !ch->playing )
				continue;
			if ( (unsigned)( ch->pos >> 16 ) >= (unsigned)ch->samples ) {
				if ( ch->loops != 0 ) {
					if ( ch->loops > 0 )
						ch->loops--;
					ch->pos = 0;
				} else if ( (unsigned)( ch->pos >> 16 ) >= (unsigned)ch->samples + RS_HALF ) {
					ch->playing = 0;
					continue;
				}
				/* else: let the filter ring out over silence for RS_HALF source samples (no click) */
			}
			acc += rs_sample( ch, ch->pos ) * (float)ch->volume / 100.0f;
			ch->pos += ch->step;
		}
		acc *= master;
		s = (int)( acc >= 0 ? acc + 0.5f : acc - 0.5f );
		if ( s > 32767 )
			s = 32767;
		if ( s < -32768 )
			s = -32768;
		for ( k = 0; k < have.channels; k++ )
			*out++ = (short)s;
	}
}

static void lock( void ) { if ( dev ) SDL_LockAudioDevice( dev ); }
static void unlock( void ) { if ( dev ) SDL_UnlockAudioDevice( dev ); }
#else
static void lock( void ) {}
static void unlock( void ) {}
#endif

void OE_AudioInit( void ) {
	int c;
	s_volume = Cvar_Get( "s_volume", "0.8", CVAR_ARCHIVE );
	for ( c = 0; c < OE_SND_CHANNELS; c++ ) {
		chans[c].volume = 100;
		chans[c].playing = 0;
	}
#ifndef OE_NO_AUDIO_DEVICE
	if ( dev )
		return;
	if ( !Cvar_Get( "s_initsound", "1", 0 )->integer ) {
		Com_Printf( "Sound disabled (s_initsound 0)\n" );
		return;
	}
	if ( !SDL_WasInit( SDL_INIT_AUDIO ) && SDL_InitSubSystem( SDL_INIT_AUDIO ) != 0 ) {
		Com_Printf( "Sound disabled: %s\n", SDL_GetError() );
		return;
	}
	{
		SDL_AudioSpec want;
		SDL_zero( want );
		want.freq = 48000;
		want.format = AUDIO_S16SYS;
		want.channels = 2;
		want.samples = 1024;
		want.callback = mix_cb;
		dev = SDL_OpenAudioDevice( NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE );
		if ( !dev || have.format != AUDIO_S16SYS ) {
			Com_Printf( "Sound disabled: %s\n", SDL_GetError() );
			if ( dev )
				SDL_CloseAudioDevice( dev );
			dev = 0;
			return;
		}
	}
	rs_build_kernel();
	Com_Printf( "Sound: %d Hz, %d channels (%s)\n", have.freq, have.channels, SDL_GetCurrentAudioDriver() );
	SDL_PauseAudioDevice( dev, 0 );
#else
	(void)rs_build_kernel;
#endif
}

void OE_AudioShutdown( void ) {
#ifndef OE_NO_AUDIO_DEVICE
	if ( dev )
		SDL_CloseAudioDevice( dev );
	dev = 0;
#endif
	OE_AudioFreeAll();
}

int OE_AudioLoad( short *pcm, int samples ) {
	if ( numSounds == OE_MAX_SOUNDS ) {
		free( pcm );
		return 0;
	}
	sounds[numSounds].pcm = pcm;
	sounds[numSounds].samples = pcm ? samples : 0;
	numSounds++;
	return numSounds;
}

void OE_AudioFreeAll( void ) {
	int i;
	lock();
	for ( i = 0; i < OE_SND_CHANNELS; i++ )
		chans[i].playing = 0;
	unlock();
	for ( i = 0; i < numSounds; i++ )
		free( sounds[i].pcm );
	numSounds = 0;
}

void OE_AudioPlay( int channel, int handle, int loops ) {
	oeChan_t *ch;
	if ( channel < 0 || channel >= OE_SND_CHANNELS || handle <= 0 || handle > numSounds )
		return;
	if ( !sounds[handle - 1].pcm || sounds[handle - 1].samples <= 0 )
		return;
#ifndef OE_NO_AUDIO_DEVICE
	if ( !dev )
		return;
	lock();
	ch = &chans[channel];
	ch->pcm = sounds[handle - 1].pcm;
	ch->samples = sounds[handle - 1].samples;
	ch->step = (unsigned)( ( (unsigned long long)OE_SND_RATE << 16 ) / (unsigned long long)have.freq );
	ch->pos = 0;
	ch->loops = loops;
	ch->playing = 1;
	unlock();
#else
	/* no device: the sound "plays" until it is stopped, as it did in the ports' QA runs (a dummy SDL device
	 * consumed audio in real time while the virtual clock raced ahead, so no sound ever ended within a run) */
	ch = &chans[channel];
	ch->pcm = sounds[handle - 1].pcm;
	ch->samples = sounds[handle - 1].samples;
	ch->pos = 0;
	ch->loops = loops;
	ch->playing = 1;
#endif
}

void OE_AudioStop( int channel ) {
	if ( channel < 0 || channel >= OE_SND_CHANNELS )
		return;
	lock();
	chans[channel].playing = 0;
	unlock();
}

qboolean OE_AudioIsPlaying( int channel ) {
	int p;
	if ( channel < 0 || channel >= OE_SND_CHANNELS )
		return qfalse;
#ifndef OE_NO_AUDIO_DEVICE
	if ( !dev )
		return qfalse;
#endif
	lock();
	/* a sound counts as finished at its last sample, even while the resampler's tail still rings out */
	p = chans[channel].playing && (unsigned)( chans[channel].pos >> 16 ) < (unsigned)chans[channel].samples;
	unlock();
	return p ? qtrue : qfalse;
}

void OE_AudioSetVolume( int channel, int volume ) {
	if ( channel < 0 || channel >= OE_SND_CHANNELS )
		return;
	if ( volume < 0 )
		volume = 0;
	if ( volume > 100 )
		volume = 100;
	lock();
	chans[channel].volume = volume;
	unlock();
}

void OE_AudioPause( qboolean pause ) {
#ifndef OE_NO_AUDIO_DEVICE
	if ( dev )
		SDL_PauseAudioDevice( dev, pause ? 1 : 0 );
#else
	(void)pause;
#endif
}
