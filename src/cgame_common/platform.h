#ifndef OE_PLATFORM_H
#define OE_PLATFORM_H

/*
 * Host services for the game code, inside the game module. In the original
 * ports (oe-re, oe2-re) this was the SDL layer; here every call goes to the
 * engine through a system call (oe_syscalls.c). The BREW key and event codes
 * stay as they were: the engine translates PC keys to them.
 */

#include <stddef.h>
#include <stdint.h>

#include "oe_public.h"

/* BREW virtual key codes (AEEVCodes.h) */
enum {
	AVK_0 = 0xE021, AVK_1, AVK_2, AVK_3, AVK_4, AVK_5, AVK_6, AVK_7, AVK_8, AVK_9,
	AVK_STAR = 0xE02B, AVK_POUND = 0xE02C, AVK_POWER, AVK_END, AVK_SEND,
	AVK_CLR = 0xE030, AVK_UP, AVK_DOWN, AVK_LEFT, AVK_RIGHT, AVK_SELECT,
	AVK_SOFT1 = 0xE036, AVK_SOFT2, AVK_SOFT3, AVK_SOFT4,
	AVK_VOLUME_UP = 0xE047, AVK_VOLUME_DOWN = 0xE048
};

/* BREW event codes used by the applet */
enum {
	EVT_APP_START = 0, EVT_APP_STOP = 1, EVT_APP_SUSPEND = 2, EVT_APP_RESUME = 3,
	EVT_KEY = 0x100, EVT_KEY_PRESS = 0x101, EVT_KEY_RELEASE = 0x102
};

uint32_t Plat_uptimeMs(void);
/* 1 while the engine runs the deterministic test clock (headless QA runs) */
int Plat_virtualClock(void);
void Plat_vibrate(int ms);
void Plat_setWindowTitle(const char *title);

/* Sound: OE_SND_CHANNELS mixer channels. The engine decodes the CMF/QCELP
 * resource itself (it has the .bar); the game gets a handle for it. */
#define AUDIO_CHANNELS OE_SND_CHANNELS
#define PMD_SAMPLE_RATE OE_SND_RATE
int Audio_load(int resId);                      /* handle > 0, or 0 if it cannot be decoded */
void Audio_playSound(int channel, int handle, int loops);
void Audio_stop(int channel);
int Audio_isPlaying(int channel);
void Audio_setVolume(int channel, int volume0to100);

/* Diagnostics */
void Plat_print(const char *text);
void Plat_error(const char *text);

#endif
