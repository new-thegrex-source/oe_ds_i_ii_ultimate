#ifndef OE_HW_H
#define OE_HW_H

/*
 * Hardware rendering, game module side (shared by both games; see oe_public.h).
 * The game's renderer keeps drawing as before and, while Hw_enabled(), also
 * reports what it draws: each game's render code calls Hw_beginScene at the
 * start of a frame, adds quads, and Hw_endScene when its 3D view is complete
 * (before the weapon and the 2D are drawn over it); the scene goes to the
 * engine with the next presented frame (Hw_flush). Textures get numbers from
 * Hw_allocTexture and are sent with Hw_texture; each game keeps its own
 * cache of which texture has which number.
 */

#include "oe_vm.h"

void Hw_setEnabled(int on);
int Hw_enabled(void);

/* texture numbers 1..OE_HW_MAX_TEXTURES-1; 0 when all are used.
 * Hw_resetTextures starts over (the old numbers are reused and redefined). */
int Hw_allocTexture(void);
void Hw_resetTextures(void);
void Hw_texture(int id, int format, int width, int height, int flags, int source,
                const void *pixels, const unsigned char *mask, const unsigned short *palette);
/* What the textures are in the game's data (oeHwTexture_t.source, for a mod's replacements):
 * the texel loaders note, for each texture they load, where its texels now are (the "where"
 * the GPU code finds them by) and its number in the texels file (OE_HWSRC_* kinds). */
void Hw_clearSources(void);
void Hw_noteSource(int kind, int where, int number);
int Hw_sourceOf(int kind, int where);	/* OE_HWSRC(kind, number), or 0 */

/* oe_idtech3freecam: at the start of a frame of the 3D view, the free camera's position (world units)
 * and angle when the engine flies one over the map (returns 1). Until Hw_endScene, Hw_freeView() is then
 * 1: the view is rendered from there, the whole map without culling, and seeing it changes nothing in
 * the game (no automap marks, no monsters woken) */
int Hw_queryFreeView(int *x, int *y, int *z, int *angle);
int Hw_freeView(void);

/* one frame of the 3D view */
void Hw_beginScene(void);
oeHwScene_t *Hw_scene(void);         /* the camera etc. to fill in */
void Hw_setRect(int x, int y, int w, int h); /* the view on the game screen (fills the key colour in a fast pass) */
int Hw_fastPass(void);               /* in a scene that skips the 3D view's pixels (QVM) */
#define HW_KEY_COLOR 0x0821          /* RGB565 the fast pass leaves in the view (almost black, rare in the art) */
short *Hw_flatTiles(void);           /* 2048 texture numbers: floor tiles, then ceiling tiles */
oeHwQuad_t *Hw_addQuad(int tex, int blend, int alpha); /* NULL when full or tex is 0 */
/* a vertical quad from (x1, y1) to (x2, y2) (16.16), z from zBottom to zTop (16.16),
 * texture s from s1 (at x1) to s2 (at x2), t from tTop to tBottom (16.16) */
void Hw_wall(int tex, int blend, int alpha, int x1, int y1, int x2, int y2, int zBottom, int zTop,
             int s1, int s2, int tTop, int tBottom);
int Hw_inScene(void);                /* between Hw_beginScene and Hw_endScene */
void Hw_endScene(void);              /* the 3D view is complete: keeps the game's picture of it */
void Hw_fadeScene(int level);        /* the game darkened its finished 3D view (255 = not at all) */
void Hw_flush(void);                 /* the frame is presented: sends the scene (Brew_flushDisplay) */

#endif
