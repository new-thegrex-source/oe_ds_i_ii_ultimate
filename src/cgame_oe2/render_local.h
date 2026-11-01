#ifndef OE2_RENDER_LOCAL_H
#define OE2_RENDER_LOCAL_H

/*
 * Private helpers shared by the render*.c files (the original Render class).
 *
 * The phone's renderer is integer-only. Everything below exists so the port
 * reproduces the ARM arithmetic bit for bit:
 *  - 32-bit multiplies wrap (ARM MUL), so they are done in uint32_t;
 *  - 64-bit products/quotients come from the ARM C runtime helpers
 *    (__rt_ll_mul = int64 * sign-extended int32, __rt_ll_sdiv = truncating);
 *  - shifts of negative values are arithmetic (ASR).
 */

#include "game.h"

/* ---- span drawers -------------------------------------------------------
 * The original created eight 8-byte C++ objects {vtable, Render*} in
 * Render_startup; their one virtual method draws a vertical texture span.
 * The vtables at 0xa74..0xa90 hold self-relative offsets of Span_draw*.
 * The port keeps the object (the Render's span* slots point at it) and
 * replaces the vtable by a function pointer. */
typedef void (*SpanDrawFn)(void *drawer, int x, int y, int u, int du, int n);
typedef struct SpanDrawer {
	SpanDrawFn draw;
	Render *render;
} SpanDrawer;

/* drawer->draw(drawer, x, y, u, du, n): n pixels down from (x, y),
 * texel = curTexels[u >> 12], u += du per pixel. */
#define SPAN_DRAW(d, x, y, u, du, n) (((SpanDrawer *)(d))->draw((d), (x), (y), (u), (du), (n)))

/* ---- constants ---- */
#define TEXEL_PAGE_SIZE   0x2bc00     /* one of 4 texel pages (Render.texelPages) */
#define DEPTH_EMPTY       0x7fffffff  /* column not covered yet */
#define DEPTH_SOLID       0x7ffffffe  /* column closed by a solid wall (occluder pass) */
#define NEAR_PLANE        0x40000     /* 4.0 in 16.16 */
#define SHAPE_END         0x7f        /* bitshape run list terminator (low byte) */

/* Line.flags bits used by the renderer */
#define LINE_TWO_SIDED    0x1
#define LINE_SPRITE       0x2          /* textured with a sprite bitshape (Render_rasterizeWall2) */
#define LINE_FLOORCEIL    0x10000      /* wall spans floor..ceiling heights */
#define LINE_CLIP_CEIL    0x20000
#define LINE_HIDDEN       0x40000
#define LINE_MIRROR       0x100000
#define LINE_NO_DEPTH     0x200000
#define LINE_OCCLUDER     0x20000000   /* occluder pass: only mark columns */

/* ---- ARM-exact integer helpers ---- */
static inline int32_t rmul(int32_t a, int32_t b) /* ARM MUL: low 32 bits */
{
	return (int32_t)((uint32_t)a * (uint32_t)b);
}
static inline int32_t rshl(int32_t a, int s) /* LSL on a signed value */
{
	return (int32_t)((uint32_t)a << s);
}
static inline int64_t rmul64(int64_t a, int32_t b) /* __rt_ll_mul: a * (int64)b, wrapping */
{
	return (int64_t)((uint64_t)a * (uint64_t)(int64_t)b);
}
static inline int32_t rdiv64(int64_t num, int64_t den) /* __rt_ll_sdiv, low word of the quotient */
{
	return (int32_t)(num / den);
}
static inline int64_t rlsl64(int64_t a, int s) /* 64-bit LSL of a signed value */
{
	return (int64_t)((uint64_t)a << s);
}
/* SMULL hi word: (int64)a * b >> 32 */
static inline int32_t rsmulhi(int32_t a, int32_t b)
{
	return (int32_t)(((int64_t)a * b) >> 32);
}

/* Screen pixel pointer inside the IDIB (row stride nPitch bytes). */
static inline uint16_t *Render_fbRow(Render *r, int y)
{
	return (uint16_t *)(r->fbDib->pBmp + (ptrdiff_t)r->fbDib->nPitch * y);
}

/* render_span.c: make Render.fbDib describe Brew_screen() (the original
 * QueryInterface'd the device bitmap for its IDIB once at startup). */
void Render_syncScreenDib(Render *r);

/* render_hw.c: the frame reported for the GPU (cgame_common/oe_hw.h) */
void RenderHw_resetTextures(Render *render);
void RenderHw_beginFrame(Render *render);
void RenderHw_line(Render *render, const Line *line);
void RenderHw_billboard(Render *render, int x, int y, int left, int right, int u1, int u2);

#endif
