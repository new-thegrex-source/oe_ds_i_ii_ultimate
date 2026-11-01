#ifndef OE_UI_LOCAL_H
#define OE_UI_LOCAL_H

/*
 * Private helpers shared by the ui package (hud*.c, ui_*.c): the HUD (Applet
 * object 0x128) and the drawing / screen half of the Canvas (object 0x110).
 */

#include "game.h"

/* ---- tables in the module's data area (read at runtime, never copied) ---- */
#define MOD_BOB_TABLE      0x2d0b4u /* s8[4] {-1,0,1,0}: arrow bob / text shake offsets */
#define MOD_TRAVEL_NODES   0x2d11cu /* int[9]: path-point index of each map's node (maps 6000..6008) */
#define MOD_TRAVEL_PATH    0x2d140u /* {u16 x, u16 y}[61]: travel map path points */

/* ---- strings shared by several files ---- */
#define MOD_STR_CONTINUE   0x2d5d8u

/* ---- colours handed to Gfx_setColor (0xAARRGGBB like the original) ---- */
#define UI_BLACK           ((int)0xff000000u)
#define UI_WHITE           ((int)0xffffffffu)

/* Signed byte n (mod 4) of the bob table: the arrows next to "Continue",
 * "More", "Enter" ... sway by -1/0/+1 px every 100 ms. */
static inline int Ui_bob(int n)
{
	return ModData_s8(&g_mod, MOD_BOB_TABLE + (uint32_t)(n % 4));
}

/* Player-visible string of the module (by file offset). The original passed
 * these literals as char*; the callee never writes through them. */
static inline char *Ui_str(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

/*
 * IDISPLAY_DrawRect(display, rc, RGB_NONE, RGB_NONE, IDF_RECT_FILL): fills a
 * device rectangle (absolute screen coordinates, no view translation) with the
 * display's background colour, which App_handleEvent sets to black at
 * start-up. The original also used fill 0 (black) with a NULL rect for the whole
 * screen; both are this helper.
 */
static inline void Ui_clearScreenRect(int x, int y, int w, int h)
{
	IBitmap *scr = Brew_screen();
	AEERect rc;

	if (scr == NULL)
		return;
	rc.x = (int16_t)x;
	rc.y = (int16_t)y;
	rc.dx = (int16_t)w;
	rc.dy = (int16_t)h;
	IBITMAP_FillRect(scr, &rc, IBITMAP_RGBToNative(scr, MAKE_RGB(0, 0, 0)), AEE_RO_COPY);
}

static inline void Ui_clearScreen(void)
{
	IBitmap *scr = Brew_screen();

	if (scr != NULL)
		Ui_clearScreenRect(0, 0, scr->width, scr->height);
}

#endif
