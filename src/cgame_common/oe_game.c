/*
 * Entry point of a game module (vmMain) and the platform services the game
 * code calls (platform.h), all going to the engine. Shared by both games;
 * each game supplies the OeGame_* functions (oe_main.c in cgame_oe and cgame_oe2).
 *
 * In a QVM vmMain must be the first function of the first file given to
 * q3asm: the interpreter starts at code address 0. Keep it first here and
 * keep this file first in the QVM file list (vs2022/build_qvm.cmd).
 */
#include "oe_vm.h"
#include "platform.h"
#include "brew_shim.h"
#include "oe_hw.h"

#include <stdarg.h>
#include <stdio.h>

#if defined(_WIN32) && !defined(Q3_VM)
#define OE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) && !defined(Q3_VM)
#define OE_EXPORT __attribute__((visibility("default")))
#else
#define OE_EXPORT
#endif

OE_EXPORT intptr_t vmMain(int command, int arg0, int arg1, int arg2, int arg3, int arg4, int arg5, int arg6,
                          int arg7, int arg8, int arg9, int arg10, int arg11)
{
	int r;
	(void)arg4; (void)arg5; (void)arg6; (void)arg7; (void)arg8; (void)arg9; (void)arg10; (void)arg11;
	switch (command) {
	case OE_GAME_INIT:
		if (arg0 != OE_GAME_API_VERSION) {
			Oe_print("oegame: engine API version %d, this module needs %d\n", arg0, OE_GAME_API_VERSION);
			return -1;
		}
		Brew_setScreenScale(arg3 & 0xff);
		Hw_setEnabled(arg3 & OE_SCREEN_HW);
		r = OeGame_init(arg1, arg2);
		Brew_flushDisplay();
		return r;
	case OE_GAME_SHUTDOWN:
		OeGame_shutdown();
		return 0;
	case OE_GAME_EVENT:
		r = OeGame_event(arg0, arg1, arg2);
		Brew_flushDisplay();
		return r;
	case OE_GAME_TICK:
		OeGame_tick();
		Brew_flushDisplay();
		return Brew_quitRequested();
	case OE_GAME_INFO:
		return OeGame_info(arg0);
	case OE_GAME_CONSOLE_COMMAND:
		r = OeGame_consoleCommand();
		Brew_flushDisplay();
		return r;
	}
	return -1;
}

/* ---------------------------------------------------------------- platform.h */

uint32_t Plat_uptimeMs(void) { return (uint32_t)trap_Milliseconds(); }
int Plat_virtualClock(void) { return trap_VirtualClock(); }
void Plat_vibrate(int ms) { trap_Vibrate(ms); }
void Plat_setWindowTitle(const char *title) { trap_SetWindowTitle(title); }
void Plat_print(const char *text) { trap_Print(text); }
void Plat_error(const char *text) { trap_Error(text); }

int Audio_load(int resId) { return trap_SndLoad(resId); }
void Audio_playSound(int channel, int handle, int loops) { trap_SndPlay(channel, handle, loops); }
void Audio_stop(int channel) { trap_SndStop(channel); }
int Audio_isPlaying(int channel) { return trap_SndIsPlaying(channel); }
void Audio_setVolume(int channel, int volume0to100) { trap_SndSetVolume(channel, volume0to100); }

void Oe_print(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	trap_Print(buf);
}
