/*
 * System calls of a game module built as a DLL (oegame<arch>.dll). The QVM
 * build uses oe_syscalls.asm instead, which maps the same functions to the
 * negative trap numbers the interpreter hands to the engine.
 */
#ifdef Q3_VM
#error "Do not compile oe_syscalls.c into the QVM: use oe_syscalls.asm"
#endif

#include <stdint.h>
#include <string.h>
#include "oe_vm.h"

#if defined(_WIN32)
#define OE_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define OE_EXPORT __attribute__((visibility("default")))
#else
#define OE_EXPORT
#endif

static intptr_t (*oe_syscall)(intptr_t arg, ...) = (intptr_t (*)(intptr_t, ...))-1;

OE_EXPORT void dllEntry(intptr_t (*syscallptr)(intptr_t arg, ...))
{
	oe_syscall = syscallptr;
}

void trap_Print(const char *text) { oe_syscall(OE_TRAP_PRINT, text); }
void trap_Error(const char *text) { oe_syscall(OE_TRAP_ERROR, text); }
int trap_Milliseconds(void) { return (int)oe_syscall(OE_TRAP_MILLISECONDS); }
int trap_VirtualClock(void) { return (int)oe_syscall(OE_TRAP_VIRTUAL_CLOCK); }
int trap_TimeOfDayMs(void) { return (int)oe_syscall(OE_TRAP_TIME_OF_DAY_MS); }

void trap_Cvar_VariableStringBuffer(const char *name, char *buf, int size) { oe_syscall(OE_TRAP_CVAR_VARIABLE_STRING, name, buf, size); }
void trap_Cvar_Set(const char *name, const char *value) { oe_syscall(OE_TRAP_CVAR_SET, name, value); }
int trap_Argc(void) { return (int)oe_syscall(OE_TRAP_ARGC); }
void trap_Argv(int n, char *buf, int size) { oe_syscall(OE_TRAP_ARGV, n, buf, size); }

int trap_GameDataSize(int which) { return (int)oe_syscall(OE_TRAP_GAMEDATA_SIZE, which); }
int trap_GameDataRead(int which, int offset, void *buf, int len) { return (int)oe_syscall(OE_TRAP_GAMEDATA_READ, which, offset, buf, len); }

int trap_ResSize(int resId) { return (int)oe_syscall(OE_TRAP_RES_SIZE, resId); }
int trap_ResRead(int resId, void *buf, int size) { return (int)oe_syscall(OE_TRAP_RES_READ, resId, buf, size); }
int trap_ResString(int resId, char *buf, int size) { return (int)oe_syscall(OE_TRAP_RES_STRING, resId, buf, size); }

int trap_FileOpen(const char *name, int ofmMode) { return (int)oe_syscall(OE_TRAP_FILE_OPEN, name, ofmMode); }
int trap_FileRead(int h, void *buf, int n) { return (int)oe_syscall(OE_TRAP_FILE_READ, h, buf, n); }
int trap_FileWrite(int h, const void *buf, int n) { return (int)oe_syscall(OE_TRAP_FILE_WRITE, h, buf, n); }
int trap_FileSeek(int h, int seekType, int pos) { return (int)oe_syscall(OE_TRAP_FILE_SEEK, h, seekType, pos); }
int trap_FileSize(int h) { return (int)oe_syscall(OE_TRAP_FILE_SIZE, h); }
void trap_FileClose(int h) { oe_syscall(OE_TRAP_FILE_CLOSE, h); }
int trap_FileRemove(const char *name) { return (int)oe_syscall(OE_TRAP_FILE_REMOVE, name); }
int trap_FileExists(const char *name) { return (int)oe_syscall(OE_TRAP_FILE_EXISTS, name); }
int trap_FileLastError(void) { return (int)oe_syscall(OE_TRAP_FILE_LAST_ERROR); }

void trap_ScreenUpdate(const unsigned short *rgb565, int width, int height, int pitchPixels)
{
	oe_syscall(OE_TRAP_SCREEN_UPDATE, rgb565, width, height, pitchPixels);
}

int trap_SndLoad(int resId) { return (int)oe_syscall(OE_TRAP_SND_LOAD, resId); }
void trap_SndPlay(int channel, int handle, int loops) { oe_syscall(OE_TRAP_SND_PLAY, channel, handle, loops); }
void trap_SndStop(int channel) { oe_syscall(OE_TRAP_SND_STOP, channel); }
int trap_SndIsPlaying(int channel) { return (int)oe_syscall(OE_TRAP_SND_IS_PLAYING, channel); }
void trap_SndSetVolume(int channel, int volume) { oe_syscall(OE_TRAP_SND_SET_VOLUME, channel, volume); }

void trap_Vibrate(int ms) { oe_syscall(OE_TRAP_VIBRATE, ms); }
void trap_SetWindowTitle(const char *title) { oe_syscall(OE_TRAP_SET_WINDOW_TITLE, title); }
void trap_PipeWrite(const char *text) { oe_syscall(OE_TRAP_PIPE_WRITE, text); }

void trap_HwTexture(const oeHwTexture_t *desc, const void *pixels, const unsigned char *mask, const unsigned short *palette)
{
	oe_syscall(OE_TRAP_HW_TEXTURE, desc, pixels, mask, palette);
}
void trap_HwScene(const oeHwScene_t *scene, const oeHwQuad_t *quads, const short *flatTiles, const unsigned short *screen)
{
	oe_syscall(OE_TRAP_HW_SCENE, scene, quads, flatTiles, screen);
}
int trap_HwFreeView(int *view)
{
	return (int)oe_syscall(OE_TRAP_HW_FREEVIEW, view);
}
int trap_FreeMove(oeFreeMove_t *move)
{
	return (int)oe_syscall(OE_TRAP_FREE_MOVE, move);
}
void trap_Exec(const char *text)
{
	oe_syscall(OE_TRAP_EXEC, text);
}
int trap_ResImage(int resId, unsigned short *px, int maxPixels, int *info)
{
	return (int)oe_syscall(OE_TRAP_RES_IMAGE, resId, px, maxPixels, info);
}
void trap_ExportFile(const char *path, const void *data, int len, int w, int h)
{
	oe_syscall(OE_TRAP_EXPORT_FILE, path, data, len, w, h);
}
int trap_DsFile(const char *path, void *buf, int size)
{
	return (int)oe_syscall(OE_TRAP_DS_FILE, path, buf, size);
}
int trap_DsMap(const char *name)
{
	return (int)oe_syscall(OE_TRAP_DS_MAP, name);
}
void trap_DsSprites(const oeDsSprite_t *sprites, int count)
{
	oe_syscall(OE_TRAP_DS_SPRITES, sprites, count);
}
int trap_Pointer(int *out, int want)
{
	return (int)oe_syscall(OE_TRAP_POINTER, out, want);
}
