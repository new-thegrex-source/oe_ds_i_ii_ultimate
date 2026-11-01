#ifndef OE_GAME_H
#define OE_GAME_H

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

/* The original reached every subsystem through GETAPPINSTANCE(); the port keeps
 * one global applet instead. */
extern Applet *g_app;

/* Engine lifecycle used by main.c (implemented in app.c). */
Applet *App_create(int screenW, int screenH);
void App_destroy(Applet *app);

/* Render sprites are numbered from 1 (Entity.info low 16 bits). The original also indexes "sprite 0" for the
 * WORLD entity (info 0), e.g. Canvas_updateFacing measures its distance: that read 0x2c bytes before the array,
 * the unused tail of lines[1498..1499] (zero until written; no map has more than 1269 lines) followed by
 * Render.numLines, whose low byte is sprite 0's scale. Index 0 and out-of-range indexes therefore yield one
 * persistent scratch sprite (stray writes stay, as they did in that unused memory) whose scale reads as the
 * low byte of numLines. A write to its scale does not reach numLines (it would have corrupted it). */
Sprite *Render_spriteAt(Render *render, int idx1);

/* 1 << n as the ARM computed it with a register shift (LSL Rs): only the low byte of n counts and 32 or more
 * gives 0, where C would be undefined. In range it is the plain shift. */
static inline uint32_t arm_bit(uint32_t n)
{
	n &= 0xff;
	return n < 32 ? 1u << n : 0u;
}

/* The 32-bit word at byte i of the random pool as the ARM's `ldr` read it: an unaligned load returns the
 * aligned word rotated right by 8 * (i & 3) (Random_nextInt 0x1ca6c, Player_updateWandChat 0x2bfba). */
static inline uint32_t Random_wordAt(const Random *rnd, int i)
{
	const uint8_t *w = &rnd->pool[i & ~3];
	uint32_t v = (uint32_t)w[0] | (uint32_t)w[1] << 8 | (uint32_t)w[2] << 16 | (uint32_t)w[3] << 24;
	int rot = (i & 3) * 8;
	return rot ? (v >> rot) | (v << (32 - rot)) : v;
}

/* Report something the port does not handle yet, once per call site. */
#define OE_UNIMPLEMENTED(what) \
	do { static int once_; if (!once_) { once_ = 1; Oe_warn("unimplemented: %s (%s:%d)", what, __FILE__, __LINE__); } } while (0)
void Oe_warn(const char *fmt, ...);
void Oe_log(const char *fmt, ...);

/* port: free movement (freemove.c, oe_moveMode in the options; OE_MOVE_* in oe_public.h) */
#include "oe_public.h"
int FreeMove_mode(void);
int FreeMove_stepCostsTurn(void);
int FreeMove_update(Canvas *cv, int canMove);

#endif
