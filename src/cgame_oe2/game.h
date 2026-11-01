#ifndef OE2_GAME_H
#define OE2_GAME_H

/* Single include for every engine source file. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "brew_shim.h"
#include "moddata.h"
#include "platform.h"
#include "types.h"
#include "protos.h"

/* port: free movement (freemove.c, oe_moveMode in the options; OE_MOVE_* in oe_public.h) */
#include "oe_public.h"
int FreeMove_mode(void);
int FreeMove_stepCostsTurn(void);
int FreeMove_update(Canvas *cv, int canMove);

/* The original reached every subsystem through GETAPPINSTANCE(); the port keeps
 * one global applet instead. */
extern Applet *g_app;

/* Engine lifecycle used by main.c (implemented in app.c). */
Applet *App_create(int screenW, int screenH);
void App_destroy(Applet *app);

/* Report something the port does not handle yet, once per call site. */
#define OE2_UNIMPLEMENTED(what) \
	do { static int once_; if (!once_) { once_ = 1; Oe2_warn("unimplemented: %s (%s:%d)", what, __FILE__, __LINE__); } } while (0)
void Oe2_warn(const char *fmt, ...);
void Oe2_log(const char *fmt, ...);

/* 1 << n as the ARM computed it (LSL by register): only the low byte of n counts, and shifts of 32..255
 * give 0. In C a shift >= 32 is undefined and x86 would wrap it to n & 31. */
static inline uint32_t arm_bit(int n)
{
	uint32_t s = (uint32_t)n & 0xff;
	return s < 32 ? 1u << s : 0u;
}

#endif
