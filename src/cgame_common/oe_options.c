/*
 * The port's rows in the games' Options menus (shared by the games; each
 * game's menu code shows them after its own rows and hands a selected row
 * back here). They are the engine's settings, kept in its cvars:
 *
 *   HUD scale   oe_hudScale: window pixels per game pixel, Auto or x1..x8;
 *               the game lays its screen out when it starts, so a new
 *               value shows "(next)" until then
 *   Softkey bar oe_softkeyBar: the grey bar of soft-key labels at the
 *               bottom; off, the HUD and the weapon sit at the screen's
 *               edge (also laid out at the start: "(next)")
 *   Resolution  r_width x r_height from r_availableModes, or the desktop's
 *   Monitor     r_display: Auto (where the window is) or 1..r_numDisplays;
 *               the row is there only with more than one monitor
 *   Fullscreen  r_fullscreen
 *   Apply video vid_restart with the three above
 *   Movement    oe_moveMode: Turns (the original), Steps (free movement, a
 *               turn per step), Real time (free movement, timed turns)
 */
#include "oe_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OPT_HUD, OPT_SOFTKEYS, OPT_RESOLUTION, OPT_DISPLAY, OPT_FULLSCREEN, OPT_APPLY, OPT_MOVEMENT, OPT_JOYSTICK, OPT_GAMEPAD_REMAP, OPT_COUNT };

static int hudAtStart = -1;		/* oe_hudScale when the game laid out its screen */
static int softkeysAtStart = -1;	/* oe_softkeyBar then */
static int rows[OPT_COUNT];		/* the rows shown: OPT_* */
static int numRows;

static int cvarInt(const char *name)
{
	char buf[32];
	trap_Cvar_VariableStringBuffer(name, buf, sizeof(buf));
	return atoi(buf);
}

static void cvarSetInt(const char *name, int v)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "%d", v);
	trap_Cvar_Set(name, buf);
}

int Options_softkeyBar(void)
{
	if (softkeysAtStart < 0)
		softkeysAtStart = cvarInt("oe_softkeyBar") != 0;
	return softkeysAtStart;
}

int Options_count(void)
{
	int i;

	if (hudAtStart < 0)
		hudAtStart = cvarInt("oe_hudScale");
	Options_softkeyBar();
	numRows = 0;
	for (i = 0; i < OPT_COUNT; i++) {
		if (i == OPT_DISPLAY && cvarInt("r_numDisplays") < 2)
			continue;
		rows[numRows++] = i;
	}
	return numRows;
}

static int rowId(int i)
{
	if (numRows == 0)
		Options_count();
	return i >= 0 && i < numRows ? rows[i] : -1;
}

/* row i: its label (24 bytes) and value (14 bytes) */
void Options_row(int i, char *label, char *value)
{
	int v;

	value[0] = 0;
	switch (rowId(i)) {
	case OPT_HUD:
		strcpy(label, "HUD scale");
		v = cvarInt("oe_hudScale");
		if (v <= 0)
			strcpy(value, "Auto");
		else
			snprintf(value, 14, "x%d", v);
		if (v != hudAtStart)
			strcat(value, " (next)");
		break;
	case OPT_SOFTKEYS:
		strcpy(label, "Softkey bar");
		v = cvarInt("oe_softkeyBar") != 0;
		strcpy(value, v ? "On" : "Off");
		if (v != softkeysAtStart)
			strcat(value, " (next)");
		break;
	case OPT_RESOLUTION:
		strcpy(label, "Resolution");
		if (cvarInt("r_width") <= 0 || cvarInt("r_height") <= 0)
			strcpy(value, "Desktop");
		else
			snprintf(value, 14, "%dx%d", cvarInt("r_width"), cvarInt("r_height"));
		break;
	case OPT_DISPLAY:
		strcpy(label, "Monitor");
		v = cvarInt("r_display");
		if (v <= 0 || v > cvarInt("r_numDisplays"))
			strcpy(value, "Auto");
		else
			snprintf(value, 14, "%d", v);
		break;
	case OPT_FULLSCREEN:
		strcpy(label, "Fullscreen");
		strcpy(value, cvarInt("r_fullscreen") ? "On" : "Off");
		break;
	case OPT_APPLY:
		strcpy(label, "Apply video");
		break;
	case OPT_MOVEMENT:
		strcpy(label, "Movement");
		v = cvarInt("oe_moveMode");
		strcpy(value, v == OE_MOVE_STEPS ? "Steps" : v == OE_MOVE_TIMER ? "Real time" : "Turns");
		break;
	case OPT_JOYSTICK:
		strcpy(label, "Gamepad");
		strcpy(value, cvarInt("in_joystick") ? "On" : "Off");
		break;
	case OPT_GAMEPAD_REMAP:
		strcpy(label, "Remap gamepad");
		strcpy(value, "");
		break;
	default:
		label[0] = 0;
		break;
	}
}

/* the next of the window sizes the display offers after the current one ("Desktop" first) */
static void nextResolution(void)
{
	char modes[1024], *p, *x;
	int w = cvarInt("r_width"), h = cvarInt("r_height");
	int takeNext = w <= 0 || h <= 0, mw, mh;

	trap_Cvar_VariableStringBuffer("r_availableModes", modes, sizeof(modes));
	for (p = modes; *p; ) {
		while (*p == ' ')
			p++;
		x = strchr(p, 'x');
		if (!*p || !x)
			break;
		mw = atoi(p);
		mh = atoi(x + 1);	/* "WxH WxH ..." */
		if (takeNext && mw > 0 && mh > 0) {
			cvarSetInt("r_width", mw);
			cvarSetInt("r_height", mh);
			return;
		}
		if (mw == w && mh == h)
			takeNext = 1;
		while (*p && *p != ' ')
			p++;
	}
	/* past the last one: the desktop's own */
	trap_Cvar_Set("r_width", "0");
	trap_Cvar_Set("r_height", "0");
}

/* row i selected: changes its setting; returns 1 when the rows should be shown again */
int Options_select(int i)
{
	int v;

	switch (rowId(i)) {
	case OPT_HUD:
		v = cvarInt("oe_hudScale") + 1;
		cvarSetInt("oe_hudScale", v > 8 ? 0 : v);
		return 1;
	case OPT_SOFTKEYS:
		cvarSetInt("oe_softkeyBar", !cvarInt("oe_softkeyBar"));
		return 1;
	case OPT_RESOLUTION:
		nextResolution();
		return 1;
	case OPT_DISPLAY:
		v = cvarInt("r_display") + 1;
		cvarSetInt("r_display", v > cvarInt("r_numDisplays") ? 0 : v);
		/* the sizes listed are the old monitor's: its desktop size until Apply lists the new one's */
		trap_Cvar_Set("r_width", "0");
		trap_Cvar_Set("r_height", "0");
		return 1;
	case OPT_FULLSCREEN:
		cvarSetInt("r_fullscreen", !cvarInt("r_fullscreen"));
		return 1;
	case OPT_APPLY:
		trap_Exec("vid_restart");
		return 0;
	case OPT_MOVEMENT:
		v = cvarInt("oe_moveMode") + 1;
		cvarSetInt("oe_moveMode", v > OE_MOVE_TIMER ? OE_MOVE_TURNS : v);
		return 1;
	case OPT_JOYSTICK:
		cvarSetInt("in_joystick", !cvarInt("in_joystick"));
		trap_Exec("in_restart");
		return 1;
	case OPT_GAMEPAD_REMAP:
		/* open the remap UI (engine side); layout is saved to base/gamepad_custom.cfg */
		trap_Exec("gamepad_remap");
		return 0;
	}
	return 0;
}
