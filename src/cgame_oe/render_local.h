#ifndef OE_RENDER_LOCAL_H
#define OE_RENDER_LOCAL_H

/*
 * Private helpers shared by the render*.c files (the original Render class,
 * applet object 0x114, embedded at Applet+0x16d4).
 *
 * The phone's renderer is integer-only. The helpers below reproduce the ARM
 * arithmetic bit for bit:
 *  - 32-bit multiplies wrap (MUL), so they are done in uint32_t;
 *  - 64-bit products/quotients come from the ARM C runtime helpers
 *    (rt_ll_mul_si = int64 * sign-extended int32, rt_ll_srdiv = truncating);
 *  - shifts of negative values are arithmetic (ASR); the build uses -fwrapv.
 */

#include "game.h"

/* ---- span drawers ----------------------------------------------------------
 * O&E 1 keeps a plain function pointer in Render.spanFunc (+0x0), chosen by
 * Render_setPalette. A span draws n pixels down a screen column starting at
 * (x, y) of the 3D view (Render.framebuffer): texel = texels[u >> 12] (u is a
 * 64-bit 20.12 value), colour = Render.curPalette[texel], u += du per pixel. */
typedef void (*SpanFn)(void *r, int x, int y, long long u, int du, int n);
#define SPAN_CALL(fn, r, x, y, u, du, n) (((SpanFn)(fn))((r), (x), (y), (u), (du), (n)))

/* ---- constants ---- */
#define DEPTH_EMPTY       0x7fffffff  /* column not covered yet (Render_clearColumnZ) */
#define DEPTH_SOLID       0x7ffffffe  /* column closed by an occluder (Render_occludeColumns) */
#define NEAR_PLANE        0x40000     /* 4.0 in 16.16 */
#define SHAPE_END         0x7f        /* bitshape run list terminator (low byte) */
#define MEDIA_NONE        0x7fff      /* wallBase/spriteBase: no media for this tile */

/* Line.flags bits used by the renderer (temp segment Render.seg) */
#define LINE_TWO_SIDED    0x1
#define LINE_SPRITE       0x2          /* textured with a sprite bitshape (Render_rasterizeWall2) */
#define LINE_FLOORCEIL    0x10000      /* wall spans floor..ceiling heights */
#define LINE_CLIP_CEIL    0x20000
#define LINE_HIDDEN       0x40000
#define LINE_MOVING       0x80000      /* dynamic (moving) line */
#define LINE_MIRROR       0x100000
#define LINE_NO_DEPTH     0x200000
#define LINE_DRAWN        0x800000     /* automap: seen this frame */
#define LINE_OCCLUDER     0x20000000   /* occluder pass: only mark columns */

/* ---- ARM-exact integer helpers ---- */
static inline int32_t rmul(int32_t a, int32_t b) /* MUL: low 32 bits */
{
	return (int32_t)((uint32_t)a * (uint32_t)b);
}
static inline int32_t rshl(int32_t a, int s) /* LSL on a signed value */
{
	return (int32_t)((uint32_t)a << s);
}
static inline int64_t rmul64(int64_t a, int32_t b) /* rt_ll_mul_si: a * (int64)b, wrapping */
{
	return (int64_t)((uint64_t)a * (uint64_t)(int64_t)b);
}
static inline int32_t rdiv64(int64_t num, int64_t den) /* rt_ll_srdiv, low word of the quotient */
{
	return (int32_t)(num / den);
}
static inline int64_t rlsl64(int64_t a, int s) /* 64-bit LSL of a signed value */
{
	return (int64_t)((uint64_t)a << s);
}
static inline int32_t rsmulhi(int32_t a, int32_t b) /* SMULL hi word: (int64)a * b >> 32 */
{
	return (int32_t)(((int64_t)a * b) >> 32);
}

/* Row stride of the screen in pixels (IDIB nPitch is in bytes). */
static inline int Render_pitch(const Render *r)
{
	return r->dib->nPitch >> 1;
}
/* Pixel (x, y) of the 3D view: Render.framebuffer is the view's top-left
 * pixel inside the screen IDIB (set by Render_render). */
static inline uint16_t *Render_viewPixel(Render *r, int x, int y)
{
	return r->framebuffer + rmul(Render_pitch(r), y) + x;
}

/* render_span.c: make Render.dib describe Brew_screen() (the original
 * QueryInterface'd the device bitmap for its IDIB once, in Render_startup).
 * Allocates the IDIB on first use. */
void Render_syncScreenDib(Render *r);

/* render_hw.c: the frame reported for the GPU (cgame_common/oe_hw.h) */
void RenderHw_resetTextures(Render *render);
void RenderHw_beginFrame(Render *render);
void RenderHw_line(Render *render, const Line *line);
void RenderHw_billboard(Render *render, int x, int y, int left, int right, int u1, int u2);

#endif
