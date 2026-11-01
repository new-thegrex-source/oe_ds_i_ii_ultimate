#ifndef OE_VM_H
#define OE_VM_H

/*
 * Game module side of oe_public.h: the system calls into the engine.
 * In a DLL they go through the function pointer the engine passes to
 * dllEntry (oe_syscalls.c); in a QVM each trap_* is a negative address
 * assigned in oe_syscalls.asm.
 */

#include "oe_public.h"

void trap_Print(const char *text);
void trap_Error(const char *text);
int trap_Milliseconds(void);
int trap_VirtualClock(void);
int trap_TimeOfDayMs(void);

void trap_Cvar_VariableStringBuffer(const char *name, char *buf, int size);
void trap_Cvar_Set(const char *name, const char *value);
int trap_Argc(void);
void trap_Argv(int n, char *buf, int size);

int trap_GameDataSize(int which);
int trap_GameDataRead(int which, int offset, void *buf, int len);

int trap_ResSize(int resId);
int trap_ResRead(int resId, void *buf, int size);
int trap_ResString(int resId, char *buf, int size);

int trap_FileOpen(const char *name, int ofmMode);
int trap_FileRead(int h, void *buf, int n);
int trap_FileWrite(int h, const void *buf, int n);
int trap_FileSeek(int h, int seekType, int pos);
int trap_FileSize(int h);
void trap_FileClose(int h);
int trap_FileRemove(const char *name);
int trap_FileExists(const char *name);
int trap_FileLastError(void);

void trap_ScreenUpdate(const unsigned short *rgb565, int width, int height, int pitchPixels);

int trap_SndLoad(int resId);
void trap_SndPlay(int channel, int handle, int loops);
void trap_SndStop(int channel);
int trap_SndIsPlaying(int channel);
void trap_SndSetVolume(int channel, int volume);

void trap_Vibrate(int ms);
void trap_SetWindowTitle(const char *title);
void trap_PipeWrite(const char *text);

void trap_HwTexture(const oeHwTexture_t *desc, const void *pixels, const unsigned char *mask, const unsigned short *palette);
void trap_HwScene(const oeHwScene_t *scene, const oeHwQuad_t *quads, const short *flatTiles, const unsigned short *screen);
int trap_HwFreeView(int *view);
int trap_FreeMove(oeFreeMove_t *move);
void trap_Exec(const char *text);
int trap_ResImage(int resId, unsigned short *px, int maxPixels, int *info);
void trap_ExportFile(const char *path, const void *data, int len, int w, int h);
int trap_DsFile(const char *path, void *buf, int size);		/* Orcs & Elves DS: a cartridge file */
int trap_DsMap(const char *name);
void trap_DsSprites(const oeDsSprite_t *sprites, int count);
int trap_Pointer(int *out, int want);	/* the mouse over the screen: out = x, y, buttons; want: show a cursor */

/* the port's rows in the games' Options menus (oe_options.c) */
int Options_count(void);
void Options_row(int i, char *label, char *value);	/* label 24 bytes, value 14 */
int Options_select(int i);							/* 1: show the rows again */
int Options_softkeyBar(void);						/* oe_softkeyBar: the bottom soft-key bar is drawn */

/* printf to the engine console */
void Oe_print(const char *fmt, ...);

/* Implemented by each game (cgame_oe/oe_main.c, cgame_oe2/oe_main.c), called by vmMain (oe_game.c) */
int OeGame_info(int what);
int OeGame_init(int screenW, int screenH);	/* 0 ok */
void OeGame_shutdown(void);
int OeGame_event(int evt, int wParam, int dwParam);
void OeGame_tick(void);
int OeGame_consoleCommand(void);		/* 1 if handled */

#endif
