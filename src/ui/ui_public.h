/*
 * The interface between the engine and the user interface module (ui), the
 * menu shown when oe.exe starts without fs_game: base/vm/ui.qvm, or
 * base/uix86_64.dll (vm_ui 0 tries the DLL first). It reads its menus from
 * base/ui/main.menu, in the style of Tremulous' menu files.
 *
 * The module draws in window pixels; the engine passes the window size with
 * every refresh. For the menus the engine sets these cvars when the module
 * starts:
 *   ui_installed_<dir>  1 when that game's files are in its folder (oe, oe2, oeds)
 *   ui_gameError        why the last "game <dir>" did not start ("" if it did)
 * and runs "game <dir>" (exec "game oe") to leave the menu for a game.
 */
#ifndef UI_PUBLIC_H
#define UI_PUBLIC_H

#define UI_API_VERSION	1

/* system calls (trap_*): in a QVM call n is address -1 - n (ui_syscalls.asm) */
typedef enum {
	UI_PRINT,						/* (const char *text) */
	UI_ERROR,						/* (const char *text) the module stops */
	UI_MILLISECONDS,				/* () */
	UI_CVAR_SET,					/* (const char *name, const char *value) */
	UI_CVAR_VARIABLESTRINGBUFFER,	/* (const char *name, char *buf, int size) */
	UI_ARGC,						/* () arguments of UI_CONSOLE_COMMAND */
	UI_ARGV,						/* (int n, char *buf, int size) */
	UI_CMD_EXECUTETEXT,				/* (const char *text) appended to the command buffer */
	UI_FS_READFILE,					/* (const char *path, char *buf, int size) -> the file's length, -1 if
									 * missing; at most size - 1 bytes are copied, then a 0 */
	UI_R_REGISTERSHADERNOMIP,		/* (const char *name) -> handle, 0 if none */
	UI_R_SETCOLOR,					/* (const float *rgba) NULL: white */
	UI_R_DRAWSTRETCHPIC,			/* (float x, y, w, h, s1, t1, s2, t2, int handle) window pixels */
	UI_R_CONSOLEFONT,				/* () -> the engine's font: 16 x 16 cells of 8 x 16 pixels, by code */
	UI_KEY_GETCATCHER,				/* () */
	UI_KEY_SETCATCHER,				/* (int catcher) */
	UI_R_WHITE,						/* () -> a white picture, for filled rectangles */
	UI_R_REGISTERFONT,				/* (const char *name, int pointSize, fontInfo_t *font) fill font from fonts/fontImage_<pointSize>.dat */

	/* the C library functions the engine runs natively for the QVM */
	UI_MEMSET = 100,
	UI_MEMCPY,
	UI_MEMMOVE,
	UI_SQRT,
	UI_SIN
} uiImport_t;

/* vmMain commands */
typedef enum {
	UI_INIT,						/* (int apiVersion, int width, int height) -> 0, or -1 to fail */
	UI_SHUTDOWN,					/* () */
	UI_KEY_EVENT,					/* (int key, int down) a key of client/keycodes.h (mouse buttons too) */
	UI_CHAR_EVENT,					/* (int ch) a typed character */
	UI_MOUSE_POSITION,				/* (int x, int y) the pointer in window pixels */
	UI_REFRESH,						/* (int time, int width, int height) draw the menus */
	UI_CONSOLE_COMMAND				/* () -> 1 if the module took the command (UI_ARGC / UI_ARGV) */
} uiExport_t;

#endif
