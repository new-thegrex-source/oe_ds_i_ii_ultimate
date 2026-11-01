/*
 * Sound (Applet+0x1b67bc): 10 media channels.
 *
 * The original created one BREW IMedia (class 0x1005504, CMF/QCELP) per
 * channel, handed it the resource block as an MMD_BUFFER, set the volume and
 * played it. A registered callback (Sound_mediaNotify) restarted looping
 * sounds when the play command reported "done", or cleared the channel's
 * music flag.
 *
 * Here IMedia is a small object of our own: it decodes the CMF once per
 * resource (Pmd_decode, cached for the run) and plays the PCM on the platform
 * mixer channel with the same index. The "done" notification is delivered by
 * SoundMedia_dispatch(), which App_tick calls once per tick, where the BREW
 * shell would have run the callback between events. A channel's media keeps
 * reporting "playing" until that notification has been delivered, as BREW's
 * state only changed through the shell's callbacks.
 */
#include "oe_vm.h"
#include "core_local.h"

/* BREW media constants used by the original (AEEMedia.h) */
enum {
	MM_PARM_MEDIA_DATA = 1,
	MM_PARM_VOLUME = 4,
	MM_CMD_PLAY = 4,
	MM_STATUS_DONE = 2,
	MM_STATE_READY = 2,
	MM_STATE_PLAY = 3,
};

#define NUM_CHANNELS 10

/* AEEMediaCmdNotify, the record Sound_mediaNotify receives */
typedef struct MediaNotify {
	int clsMedia;  /* +0x0 */
	IMedia *media; /* +0x4 */
	int cmd;       /* +0x8 */
	int subCmd;    /* +0xc */
	int status;    /* +0x10 */
} MediaNotify;

struct IMedia {
	int channel;        /* mixer channel = Sound channel index */
	int sound;          /* the engine's handle of the decoded sound (oe-client: the engine decodes the CMF) */
	int volume;         /* 0..100 (AEE_MAX_VOLUME) */
	int state;          /* MM_STATE_READY / MM_STATE_PLAY */
	Sound *notifyUser;
	void (*notify)(Sound *, void *);
};

/* ------------------------------------------------------------ PCM cache */

typedef struct PcmEntry {
	int resId;
	int sound; /* engine sound handle, 0 if the resource could not be decoded */
} PcmEntry;

static PcmEntry pcmCache[256];
static int pcmCount;

static PcmEntry *pcm_lookup(int resId, const void *data, int size)
{
	PcmEntry *e;
	for (int i = 0; i < pcmCount; i++)
		if (pcmCache[i].resId == resId)
			return &pcmCache[i];
	if (pcmCount == (int)(sizeof(pcmCache) / sizeof(pcmCache[0])))
		return NULL;
	e = &pcmCache[pcmCount++];
	e->resId = resId;
	/* oe-client: the engine decodes the resource (it reads the same bytes from the .bar) */
	e->sound = (data != NULL && size > 0) ? Audio_load(resId) : 0;
	if (e->sound == 0)
		Oe_warn("sound %d: cannot decode the CMF data", resId);
	return e;
}

void SoundMedia_freeCache(void)
{
	pcmCount = 0; /* the engine keeps the decoded sounds */
}

/* ------------------------------------------------------------ IMedia */

static IMedia *media_create(int channel)
{
	IMedia *m = calloc(1, sizeof(*m));
	if (m) {
		m->channel = channel;
		m->volume = 100;
		m->state = MM_STATE_READY;
	}
	return m;
}

static void media_release(IMedia *m)
{
	if (!m)
		return;
	if (m->state == MM_STATE_PLAY)
		Audio_stop(m->channel);
	free(m);
}

/* IMEDIA_SetMediaParm(MM_PARM_VOLUME) */
static void media_setVolume(IMedia *m, int volume)
{
	m->volume = volume;
	Audio_setVolume(m->channel, volume);
}

/* IMEDIA_Play */
static void media_play(IMedia *m)
{
	Audio_setVolume(m->channel, m->volume);
	if (m->sound)
		Audio_playSound(m->channel, m->sound, 0);
	m->state = MM_STATE_PLAY;
}

/* IMEDIA_GetState (the "state changing" flag is never set) */
static int media_getState(IMedia *m)
{
	return m->state;
}

void SoundMedia_dispatch(Sound *snd)
{
	if (!snd)
		return;
	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		IMedia *m = snd->channels[ch].media;
		MediaNotify n;
		if (!m || m->state != MM_STATE_PLAY || Audio_isPlaying(m->channel))
			continue;
		m->state = MM_STATE_READY;
		if (!m->notify)
			continue;
		memset(&n, 0, sizeof(n));
		n.media = m;
		n.cmd = MM_CMD_PLAY;
		n.status = MM_STATUS_DONE;
		m->notify(m->notifyUser, &n);
	}
}

/* ------------------------------------------------------------ Sound */

/* 0x00028190 */
int Sound_startup(Sound *snd)
{
	snd->enabled = 0;
	snd->curChannel = 0;
	snd->volume = 80;
	for (int i = 0; i < NUM_CHANNELS; i++) {
		snd->channels[i].data = NULL;
		snd->channels[i].media = NULL;
		snd->channels[i].size = 0;
		snd->channels[i].resId = 0;
		snd->channels[i].fe = 0;
		snd->channels[i].playing = 0;
	}
	return 1;
}

/* 0x00002484  IMedia callback: when a play command is done, a looping channel
 * starts again; otherwise its music flag is cleared. */
void Sound_mediaNotify(Sound *snd, void *notify)
{
	MediaNotify *n = notify;
	if (n->cmd != MM_CMD_PLAY || n->status != MM_STATUS_DONE)
		return;
	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		if (snd->channels[ch].media != n->media)
			continue;
		if (snd->channels[ch].loop == 0) {
			snd->channels[ch].playing = 0;
		} else {
			media_play(n->media);
			snd->loopCount++;
		}
	}
}

/* 0x00012954  Release the channel's media and its resource block. */
void Sound_freeChannel(Sound *snd, int ch)
{
	SoundChannel *c = &snd->channels[ch];
	if (c->media)
		media_release(c->media);
	/* ISHELL_FreeResData(data): the block belongs to the resource cache */
	c->data = NULL;
	c->media = NULL;
	c->size = 0;
	c->loop = 0;
	c->resId = 0;
}

/* 0x00027248 */
void Sound_freeAllChannels(Sound *snd)
{
	for (int ch = 0; ch < NUM_CHANNELS; ch++)
		Sound_freeChannel(snd, ch);
}

/* 0x00026cd8 */
void Sound_shutdown(Sound *snd)
{
	Sound_freeAllChannels(snd);
}

/* 0x00027228  Stop looping: every channel with media plays to its end once more. */
void Sound_clearChannelFlags(Sound *snd)
{
	for (int ch = 0; ch < NUM_CHANNELS; ch++)
		if (snd->channels[ch].media != NULL)
			snd->channels[ch].loop = 0;
}

/* 0x000200cc */
void Sound_stopAll(Sound *snd)
{
	Sound_clearChannelFlags(snd);
}

/* 0x0001ca20  Is a music track (a channel with the music flag) still playing? */
int Sound_isMusicPlaying(Sound *snd)
{
	for (int ch = 0; ch < NUM_CHANNELS; ch++)
		if (snd->channels[ch].media != NULL && snd->channels[ch].playing != 0)
			return 1;
	return 0;
}

/* 0x0001ca98  First channel that is not playing (its old sound is freed), or -1.
 * The media objects of the other idle channels are released too. */
int Sound_findFreeChannel(Sound *snd)
{
	int found = -1;
	for (int ch = 0; ch < NUM_CHANNELS; ch++) {
		IMedia *m = snd->channels[ch].media;
		if (m != NULL && media_getState(m) == MM_STATE_PLAY)
			continue;
		if (found == -1) {
			Sound_freeChannel(snd, ch);
			found = ch;
		} else if (m != NULL) {
			media_release(m);
			snd->channels[ch].media = NULL;
		}
	}
	return found;
}

/* 0x0001a38c  Point channel `slot` at resource resId (raw, not inflated). */
void Sound_loadSound(Sound *snd, int slot, short resId)
{
	InputStream res;
	if (snd->channels[slot].data != NULL)
		Sound_freeChannel(snd, slot);
	InputStream_ctor(&res, snd->app, 0);
	if (trap_ResSize(resId) <= 0) {
		/* DS: its scripts number the cartridge's sounds (sounds/headers.bin, not done yet);
		 * one the BREW .bar does not have is left silent instead of stopping the game */
		InputStream_dtor(&res, 0);
		return;
	}
	if (!Resource_open(&res, resId, 0)) {
		App_setErrorCode(g_app, 0x16);
	} else {
		snd->channels[slot].resId = resId;
		snd->channels[slot].size = InputStream_getSize(&res);
		snd->channels[slot].data = InputStream_getResData(&res);
	}
	InputStream_dtor(&res, 0);
}

/* 0x00020020  New media object for channel ch playing the channel's data. */
void Sound_createMedia(Sound *snd, int ch)
{
	SoundChannel *c = &snd->channels[ch];
	IMedia *m;
	PcmEntry *pcm;
	if (c->media) {
		media_release(c->media);
		c->media = NULL;
	}
	m = media_create(ch); /* ISHELL_CreateInstance(0x1005504) */
	if (m == NULL)
		return;
	/* MM_PARM_MEDIA_DATA = {MMD_BUFFER, data + header length (0 here), size} */
	pcm = pcm_lookup(c->resId, c->data, c->size);
	if (pcm)
		m->sound = pcm->sound;
	media_setVolume(m, snd->volume);
	m->notify = Sound_mediaNotify; /* IMEDIA_RegisterNotify(Sound_mediaNotify, app->sound) */
	m->notifyUser = snd->app->sound;
	c->media = m;
}

/* Music tracks: while one of them plays (music flag set), no other sound starts. */
static int is_music(int resId)
{
	switch (resId) {
	case 0x139c: case 0x1413: case 0x141c: case 0x1424: case 0x1432:
	case 0x1433: case 0x1434: case 0x1435: case 0x157c: case 0x157d:
		return 1;
	default:
		return 0;
	}
}

/* 0x0001dfd4  Play resource resId. flags & 1 loops; flags & 2 only stops the
 * loops and records the request (pendingId/pendingFlags) for later.
 * volume -1 keeps the global volume. (priority is unused in O&E 1.) */
void Sound_play(Sound *snd, int resId, int flags, int priority, int volume)
{
	SoundChannel *c;
	(void)priority;
	if (snd->enabled == 0)
		return;
	snd->curChannel = Sound_findFreeChannel(snd->app->sound);
	if (snd->curChannel < 0)
		return;
	if (flags & 2) {
		Sound_clearChannelFlags(snd);
		snd->pendingId = (int16_t)resId;
		snd->pendingFlags = (uint8_t)(flags & ~2);
		return;
	}
	if (Sound_isMusicPlaying(snd))
		return;
	Sound_loadSound(snd, snd->curChannel, (short)resId);
	Sound_createMedia(snd, snd->curChannel);
	c = &snd->channels[snd->curChannel];
	c->loop = (uint8_t)(flags & 1);
	c->playing = (uint8_t)is_music(resId);
	if (c->media == NULL)
		return; /* (the phone would have crashed: no media / no resource) */
	if (volume != -1)
		media_setVolume(c->media, volume);
	media_play(c->media);
	snd->loopCount++;
}

/* 0x0002be88 */
void Sound_applyVolume(Sound *snd)
{
	for (int ch = 0; ch < NUM_CHANNELS; ch++)
		if (snd->channels[ch].media)
			media_setVolume(snd->channels[ch].media, snd->volume);
}

/* 0x0002c358 */
int Sound_volumeDown(Sound *snd, int d)
{
	snd->volume -= d;
	if (snd->volume < 0)
		snd->volume = 0;
	Sound_applyVolume(snd);
	return snd->volume;
}

/* 0x0002c378 */
int Sound_volumeUp(Sound *snd, int d)
{
	snd->volume += d;
	if (snd->volume > 100)
		snd->volume = 100;
	Sound_applyVolume(snd);
	return snd->volume;
}
