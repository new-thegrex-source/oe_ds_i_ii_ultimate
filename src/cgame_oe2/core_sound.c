/*
 * Sound (Applet+0x144): 10 media channels.
 *
 * The original created one BREW IMedia (AEECLSID_MEDIAPMD) per channel, gave
 * it the whole .pmd resource as an MMD_BUFFER, set the volume and played it.
 * A registered callback (Sound_mediaNotify) restarted looping sounds when the
 * play command reported "done".
 *
 * Here IMedia is our own small object: it decodes the .pmd once per resource
 * (Pmd_decode, cached for the run) and plays the PCM on the platform mixer
 * channel with the same index. The "done" notification is delivered by
 * SoundMedia_dispatch(), which Applet_Tick calls once per tick, the place
 * where the BREW shell would have run the callback.
 */
#include "core_local.h"

/* BREW media constants used by the original (AEEIMedia.h) */
enum {
	MM_PARM_MEDIA_DATA = 1,
	MM_PARM_VOLUME = 4,
	MM_PARM_CHANNEL_SHARE = 0x10,
	MM_CMD_PLAY = 4,
	MM_STATUS_DONE = 2,
	MM_STATE_READY = 2,
	MM_STATE_PLAY = 3,
};

#define RES_SOUND_BASE 4000

/* AEEMediaCmdNotify, the record Sound_mediaNotify receives */
typedef struct MediaNotify {
	int clsMedia;
	IMedia *media;
	int cmd;
	int subCmd;
	int status;
} MediaNotify;

struct IMedia {
	int channel;               /* mixer channel = Sound channel index */
	int sound;                 /* the engine's handle of the decoded sound (oe-client: the engine decodes the CMF) */
	int volume;                /* 0..100 (AEE_MAX_VOLUME) */
	int state;                 /* MM_STATE_READY / MM_STATE_PLAY */
	Sound *notifyUser;
	void (*notify)(Sound *, void *);
};

/* ------------------------------------------------------------ PCM cache */

typedef struct PcmEntry {
	int resId;
	int tried;
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
	e->tried = 1;
	/* oe-client: the engine decodes the resource (it reads the same bytes from the .bar) */
	e->sound = (data != NULL && size > 0) ? Audio_load(resId) : 0;
	if (e->sound == 0)
		Oe2_warn("sound %d: cannot decode .pmd", resId);
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

static void media_setVolume(IMedia *m, int volume)
{
	m->volume = volume;
	Audio_setVolume(m->channel, volume);
}

static void media_play(IMedia *m)
{
	Audio_setVolume(m->channel, m->volume);
	if (m->sound)
		Audio_playSound(m->channel, m->sound, 0);
	m->state = MM_STATE_PLAY;
}

/* On BREW the app is single-threaded: a medium goes from PLAY to READY only in the shell callback that also
 * delivers MM_STATUS_DONE (SoundMedia_dispatch here), and Sound_mediaNotify restarts looping music in that same
 * callback. Reading the mixer directly would let Sound_findFreeChannel take a music channel that finished
 * mid-tick before its loop restart, silencing the music. */
static int media_getState(IMedia *m)
{
	return m->state;
}

void SoundMedia_dispatch(Sound *snd)
{
	if (!snd)
		return;
	for (int ch = 0; ch < 10; ch++) {
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

/* 0x0003e380 */
int Sound_startup(Sound *snd)
{
	snd->app = g_app;
	snd->enabled = 0;
	snd->priority = 0;
	snd->volume = 80;
	snd->curChannel = 0;
	for (int i = 0; i < 10; i++) {
		snd->channels[i].data = NULL;
		snd->channels[i].media = NULL;
		snd->channels[i].size = 0;
		snd->channels[i].resId = 0;
		snd->channels[i].priority = 0;
	}
	return 1;
}

/* 0x00002df4  IMedia callback: a looping channel whose play finished starts again. */
void Sound_mediaNotify(Sound *snd, void *notify)
{
	MediaNotify *n = notify;
	if (n->cmd != MM_CMD_PLAY || n->status != MM_STATUS_DONE)
		return;
	for (int ch = 0; ch < 10; ch++) {
		if (snd->channels[ch].media == n->media && snd->channels[ch].loop != 0) {
			media_play(n->media);
			snd->numPlayed++;
		}
	}
}

/* 0x0001ba10  Release the channel's media and its .pmd data. */
void Sound_freeChannel(Sound *snd, int ch)
{
	SoundChannel *c = &snd->channels[ch];
	if (c->media)
		media_release(c->media);
	free(c->data);
	c->data = NULL;
	c->media = NULL;
	c->size = 0;
	c->loop = 0;
	c->resId = 0;
}

/* 0x0003cb78 */
void Sound_freeAllChannels(Sound *snd)
{
	for (int ch = 0; ch < 10; ch++)
		Sound_freeChannel(snd, ch);
}

/* 0x0003c3e0 */
void Sound_shutdown(Sound *snd)
{
	Sound_freeAllChannels(snd);
}

/* 0x00031910  Stop every channel (data stays loaded). */
void Sound_stopAll(Sound *snd)
{
	for (int ch = 0; ch < 10; ch++) {
		IMedia *m = snd->channels[ch].media;
		if (m) {
			snd->channels[ch].loop = 0;
			media_release(m);
			snd->channels[ch].media = NULL;
		}
	}
	snd->priority = 0;
	snd->pending = 0;
}

/* 0x0003cb6c  Stop everything at the next Sound_flushPending. */
void Sound_requestStopAll(Sound *snd)
{
	snd->pending = 1;
}

/* 0x00014028 */
void Sound_releasePending(Sound *sound)
{
	if (sound->pending != 0 && sound->numPlayed == 0)
		Sound_stopAll(sound);
}

/* 0x0002cbb4  First channel that is not playing (its old sound is freed), or -1.
 * Media objects of other idle channels are released too. */
int Sound_findFreeChannel(Sound *snd)
{
	int found = -1;
	for (int ch = 0; ch < 10; ch++) {
		IMedia *m = snd->channels[ch].media;
		if (m != NULL && media_getState(m) == MM_STATE_PLAY)
			continue; /* (BREW's "state changing" flag is never set here) */
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

/* 0x00028860  Load sound resource 4000+idx into channel `slot`. */
void Sound_loadSound(Sound *snd, int slot, int idx)
{
	InputStream res;
	if (snd->channels[slot].data != NULL)
		Sound_freeChannel(snd, slot);
	InputStream_ctor(&res);
	if (!Resource_open(&res, (int16_t)(idx + RES_SOUND_BASE), 1)) {
		App_setErrorCode(g_app, 0x16);
	} else {
		res.isOpen = 0; /* the channel takes over the buffer */
		/* store full resource ID (4000+idx) so Audio_load / OE_SndLoad find the CMF */
		snd->channels[slot].resId = (int16_t)(idx + RES_SOUND_BASE);
		snd->channels[slot].size = InputStream_getSize(&res);
		snd->channels[slot].data = InputStream_getBuffer(&res);
	}
	InputStream_dtor(&res, 0);
}

/* 0x000317d8  New media object for channel ch playing the channel's data. */
int Sound_createMedia(Sound *snd, int ch)
{
	SoundChannel *c = &snd->channels[ch];
	IMedia *m;
	PcmEntry *pcm;
	if (c->media) {
		media_release(c->media);
		c->media = NULL;
	}
	m = media_create(ch);
	if (!m)
		return 0;
	/* MM_PARM_CHANNEL_SHARE = 1: the mixer always shares */
	/* MM_PARM_MEDIA_DATA = {MMD_BUFFER, data, size} */
	pcm = pcm_lookup(c->resId, c->data, c->size);
	if (pcm)
		m->sound = pcm->sound;
	media_setVolume(m, snd->volume);
	m->notify = Sound_mediaNotify;
	m->notifyUser = snd->app->sndSys;
	c->media = m;
	return 1;
}

/* 0x0002f0c0  Play sound idx. flags & 1 loops; flags & 2 defers the request:
 * the next Sound_flushPending stops everything and the one after plays it.
 * volume -1 keeps the global volume. */
void Sound_play(Sound *snd, int resId, int flags, int priority, int volume)
{
	int ch;
	if (snd->pending != 0 || snd->enabled == 0)
		return;
	ch = Sound_findFreeChannel(snd->app->sndSys);
	snd->curChannel = ch;
	if (ch < 0)
		return;
	if (flags & 2) {
		snd->pending = 1;
		snd->pendingId = (int16_t)resId;
		snd->pendingFlags = (uint8_t)(flags & ~2);
		return;
	}
	Sound_loadSound(snd, ch, resId);
	if (!Sound_createMedia(snd, snd->curChannel))
		return;
	snd->channels[snd->curChannel].loop = flags & 1;
	snd->priority = priority;
	if (volume != -1)
		media_setVolume(snd->channels[snd->curChannel].media, volume);
	media_play(snd->channels[snd->curChannel].media);
	snd->numPlayed++;
}

/* 0x0003d7b0  Called once per frame: a pending stop-all happens now, else a
 * queued sound starts. */
void Sound_flushPending(Sound *snd)
{
	snd->numPlayed = 0;
	if (snd->pending != 0) {
		Sound_stopAll(snd);
		return;
	}
	if (snd->pendingId != 0) {
		Sound_play(snd, snd->pendingId, snd->pendingFlags, 3, -1);
		snd->pendingId = 0;
		snd->pendingFlags = 0;
	}
}

/* 0x0004530c */
void Sound_applyVolume(Sound *snd)
{
	for (int ch = 0; ch < 10; ch++)
		if (snd->channels[ch].media)
			media_setVolume(snd->channels[ch].media, snd->volume);
}

/* 0x000457f8 */
int Sound_volumeDown(Sound *snd, int d)
{
	snd->volume -= d;
	if (snd->volume < 0)
		snd->volume = 0;
	Sound_applyVolume(snd);
	return snd->volume;
}

/* 0x0004582c */
int Sound_volumeUp(Sound *snd, int d)
{
	snd->volume += d;
	if (snd->volume > 100)
		snd->volume = 100;
	Sound_applyVolume(snd);
	return snd->volume;
}
