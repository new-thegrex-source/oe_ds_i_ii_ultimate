#ifndef OE_BREW_SHIM_H
#define OE_BREW_SHIM_H

/*
 * The small part of the BREW runtime that Orcs & Elves and Orcs & Elves II used,
 * re-implemented on top of platform.h (one shim for both games: O&E II uses a
 * subset of it). Names follow the BREW SDK macros (IGRAPHICS_DrawRect,
 * IBITMAP_BltIn, ...) so ported code reads like the original calls, but these
 * are plain functions over our own structs: there are no vtables.
 *
 * Pixels are RGB565 everywhere, like the phone's 16-bit display.
 */

#include <stddef.h>
#include <stdint.h>

#include "platform.h"

typedef uint8_t boolean;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/* BREW RGBVAL: 0xBBGGRR00 */
typedef uint32_t RGBVAL;
#define MAKE_RGB(r, g, b) (((uint32_t)(b) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(r) << 8))
typedef uint32_t NativeColor; /* RGB565 in the low 16 bits */

typedef struct AEERect { int16_t x, y, dx, dy; } AEERect;
typedef struct AEELine { int16_t sx, sy, ex, ey; } AEELine;
typedef struct AEECircle { int16_t cx, cy, r; } AEECircle;
typedef struct AEEPoint { int16_t x, y; } AEEPoint;

enum { CLIPPING_NONE, CLIPPING_RECT };
typedef struct AEEClip {
	int8_t type;
	AEERect rect;
} AEEClip;

/* Raster ops used with IBITMAP_BltIn / IDISPLAY_BitBlt */
enum { AEE_RO_OR = 0, AEE_RO_XOR = 1, AEE_RO_COPY = 2, AEE_RO_NOT = 3, AEE_RO_MERGENOT = 4,
       AEE_RO_ANDNOT = 5, AEE_RO_TRANSPARENT = 7 };

/* ---- bitmaps (BREW IBitmap / IDIB, native RGB565) ---- */
typedef struct IBitmap {
	uint16_t *px;
	int width, height, pitch;  /* pitch in pixels (device pixels for a scaled bitmap) */
	NativeColor transparent;   /* colour skipped by AEE_RO_TRANSPARENT */
	int ownsPixels;
	/* Device pixels per game pixel (1 = plain bitmap). Only the screen is
	 * scaled (the HD mode): the game still sees width x height, px holds
	 * (width * scale) x (height * scale) pixels, the 2D primitives below draw
	 * scale x scale blocks and the 3D renderer draws the device pixels. */
	int scale;
} IBitmap;

typedef struct AEEBitmapInfo { uint32_t cx, cy, nDepth; } AEEBitmapInfo;

IBitmap *IBITMAP_Create(int w, int h);                 /* zeroed RGB565 bitmap */
void IBITMAP_Release(IBitmap *b);
NativeColor IBITMAP_RGBToNative(IBitmap *b, RGBVAL rgb);
RGBVAL IBITMAP_NativeToRGB(IBitmap *b, NativeColor c);
int IBITMAP_FillRect(IBitmap *b, const AEERect *rc, NativeColor c, int rop);
int IBITMAP_BltIn(IBitmap *dst, int xDst, int yDst, int dx, int dy, IBitmap *src, int xSrc, int ySrc, int rop);
int IBITMAP_SetTransparencyColor(IBitmap *b, NativeColor c);
int IBITMAP_GetInfo(IBitmap *b, AEEBitmapInfo *info);
int IBITMAP_CreateCompatibleBitmap(IBitmap *b, IBitmap **out, int w, int h);
/* port: image resource resId shown from a mod's picture or the id Tech 3 font, at the screen's
 * resolution (b keeps its size; its pixels become scale x larger); nothing if there is none */
void Brew_hdBitmap(int resId, IBitmap *b);
/* an HD bitmap's pixel for game pixel (x, y), sub-pixel (i, j) of a screen block s x s */
uint16_t Brew_hdPixel(const IBitmap *b, int x, int y, int i, int j, int s);
/* blend an HD bitmap (scale > 1) into dst pixel by pixel, at dst's resolution; keyed: 0xf81f is clear */
void Brew_blendHd(IBitmap *dst, int dx, int dy, int w, int h, const IBitmap *src, int sx, int sy, int keyed,
                  int (*op)(int d, int s, void *arg), void *arg);

/* The phone screen. Everything (3D view, HUD, menus) is drawn into this. */
IBitmap *Brew_screen(void);
/* Create/resize the screen; called once at startup. */
void Brew_initScreen(int w, int h);
/* Device pixels per game pixel of the screen (1, or more in the HD mode). Set
 * before Brew_init (the engine passes it to OE_GAME_INIT). */
void Brew_setScreenScale(int scale);
int Brew_screenScale(void);
/* Copy the screen to the window (IDISPLAY_Update). */
void IDISPLAY_Update(void);   /* deferred, like BREW: presented by Brew_flushDisplay */
void Brew_flushDisplay(void); /* host: present a pending update (after each event / tick) */
void IDISPLAY_Backlight(int on);
/* IDISPLAY_SetColor/DrawRect only exist for the fatal-error screen. */
void IDISPLAY_ClearScreen(RGBVAL rgb);

/* ---- IGraphics: 2D primitives on the screen ---- */
typedef struct IGraphics {
	IBitmap *dst;
	NativeColor color, fillColor, background;
	uint8_t alpha, fillAlpha;
	boolean fill;
	AEERect clip;     /* always valid; defaults to the whole screen */
	int16_t tx, ty;   /* IGRAPHICS_Translate origin */
} IGraphics;

IGraphics *IGRAPHICS_Create(IBitmap *dst);
void IGRAPHICS_Release(IGraphics *g);
RGBVAL IGRAPHICS_SetBackground(IGraphics *g, uint8_t r, uint8_t gr, uint8_t b);
RGBVAL IGRAPHICS_SetColor(IGraphics *g, uint8_t r, uint8_t gr, uint8_t b, uint8_t a);
RGBVAL IGRAPHICS_SetFillColor(IGraphics *g, uint8_t r, uint8_t gr, uint8_t b, uint8_t a);
boolean IGRAPHICS_SetFillMode(IGraphics *g, boolean fill);
boolean IGRAPHICS_SetClip(IGraphics *g, const AEEClip *clip, uint8_t flags);
void IGRAPHICS_Translate(IGraphics *g, int16_t x, int16_t y);
int IGRAPHICS_DrawLine(IGraphics *g, const AEELine *ln);
int IGRAPHICS_DrawRect(IGraphics *g, const AEERect *rc);  /* frame in color, filled with fillColor if fill mode */
int IGRAPHICS_DrawCircle(IGraphics *g, const AEECircle *c);
int IGRAPHICS_ClearRect(IGraphics *g, const AEERect *rc); /* fill with background colour */
void IGRAPHICS_Update(IGraphics *g);

/* ---- files in gamedata/ (BREW IFileMgr / IFile) ---- */
typedef struct IFile IFile;
typedef struct IFileMgr { int lastError; } IFileMgr;
enum { _OFM_READ = 1, _OFM_READWRITE = 2, _OFM_CREATE = 4, _OFM_APPEND = 8 };
enum { SUCCESS = 0, EFAILED = 1, EFILEEXISTS = 0x100, EFILENOEXISTS = 0x101, EFSFULL = 0x106 };
IFileMgr *IFILEMGR_Get(void);
IFile *IFILEMGR_OpenFile(IFileMgr *m, const char *name, int mode);
int IFILEMGR_Remove(IFileMgr *m, const char *name);
int IFILEMGR_Test(IFileMgr *m, const char *name);          /* SUCCESS if it exists */
int IFILEMGR_GetLastError(IFileMgr *m);
uint32_t IFILEMGR_GetFreeSpace(IFileMgr *m, uint32_t *total);
int32_t IFILE_Read(IFile *f, void *buf, uint32_t n);
uint32_t IFILE_Write(IFile *f, const void *buf, uint32_t n);
int32_t IFILE_Size(IFile *f);
void IFILE_Release(IFile *f);
enum { _SEEK_START = 0, _SEEK_END = 1, _SEEK_CURRENT = 2 };
int IFILE_Seek(IFile *f, int seekType, int32_t pos);        /* SUCCESS / EFAILED, like BREW; BREW contract also used by
                                                             * the game (File_tell 0x2958, OutputStream_seek 0x244c8):
                                                             * _SEEK_CURRENT with pos 0 returns the current position */

/* ---- resources from the game's .bar (read by the engine) ---- */
/* Decompressed resource bytes (owned by the cache; valid until Brew_shutdown). */
const uint8_t *Brew_loadResource(int resId, size_t *size);
/* String resource from the .bar (ISHELL_LoadResString). O&E 1 keeps its settings in strings 1..9.
 * The phone returned AECHARs; the port returns the plain text. Returns its length, or 0 if missing. */
int ISHELL_LoadResString(int resId, char *buf, int bufSize);

/* ---- misc AEEStdLib ---- */
uint32_t GETUPTIMEMS(void);
/* Milliseconds since midnight, local time (AEEStdLib GETTIMEMS). Render_render uses it as the texture animation clock. */
uint32_t GETTIMEMS(void);
void GETRAND(uint8_t *buf, int n);
/* The applet asks the shell to close it (quit from the menu). */
void ISHELL_CloseApplet(void);
int Brew_quitRequested(void);
void ISOUND_Vibrate(int ms);
void ISOUND_StopVibrate(void);

/* BREW objects the original kept pointers to but the port does not need
 * (shell, display, heap, streams, SMS, animated images). Kept opaque so the
 * struct layouts still compile; the port leaves them NULL. */
typedef struct IShell IShell;
typedef struct IDisplay IDisplay;
typedef struct IHeap IHeap;
typedef struct ISound ISound;
typedef struct IAStream IAStream;
typedef struct IBase IBase;
typedef struct IImage IImage;
/* A playing sound (BREW IMedia); implemented by the Sound module. */
typedef struct IMedia IMedia;

/* ---- lifecycle ---- */
/* Loads the .mod tables from the engine and creates the screen; 0 ok. gameName is used in messages. */
int Brew_init(const char *gameName, int screenW, int screenH);
void Brew_shutdown(void);

#endif
