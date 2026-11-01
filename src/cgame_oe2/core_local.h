#ifndef OE2_CORE_LOCAL_H
#define OE2_CORE_LOCAL_H

/* Helpers shared by the core package's own files (app.c, core_*.c).
 * Nothing here is part of the original engine's interface. */

#include "game.h"

/* Little-endian reads from raw bytes (the original's byte-assembling helpers). */
static inline int32_t core_le32(const uint8_t *p)
{
	return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static inline int16_t core_le16(const uint8_t *p)
{
	return (int16_t)(p[0] | (p[1] << 8));
}

/* core_sound.c: deliver the IMedia "play finished" notifications that the
 * BREW shell would have dispatched between events (called once per tick). */
void SoundMedia_dispatch(Sound *snd);
/* core_sound.c: free the decoded PCM cache (after every channel is stopped). */
void SoundMedia_freeCache(void);

#endif
