/*
 * Hardware rendering, game module side: the frame's quads and floor tiles,
 * sent to the engine with OE_TRAP_HW_SCENE together with the next presented
 * frame (see oe_hw.h, oe_public.h).
 */
#include "oe_hw.h"
#include "brew_shim.h"

#include <stdlib.h>
#include <string.h>

static int hwOn;
static int nextTexture = 1;
static int inScene, sceneReady;
static int freeView;
static oeHwScene_t scene;
static oeHwQuad_t quads[OE_HW_MAX_QUADS];
static short flatTiles[2048];
static unsigned short *snapshot;	/* the view's screen pixels (grown as needed) */
static int snapshotSize;

void Hw_setEnabled(int on) { hwOn = on != 0; }
int Hw_enabled(void) { return hwOn; }
int Hw_inScene(void) { return inScene; }

/* In a QVM the bytecode interpreter cannot afford the 3D view's pixels at the
 * screen sizes of a PC window: the game's renderer then does everything but
 * them (the BSP walk, the column occlusion, all the game state it touches)
 * and the view is left in a key colour, which tells the engine where the
 * game drew 2D over it. The DLL draws its view fully (exact under blended 2D). */
int Hw_fastPass(void)
{
#ifdef Q3_VM
	return inScene;
#else
	/* nobody sees the game's own picture of a free camera's view; an HD screen (a mod's HD
	 * pictures on the GPU path) would cost the full software view at the window's size */
	return inScene && (freeView || Brew_screenScale() > 1);
#endif
}

int Hw_queryFreeView(int *x, int *y, int *z, int *angle)
{
	int view[4];

	freeView = hwOn && trap_HwFreeView(view);
	if (freeView) {
		*x = view[0];
		*y = view[1];
		*z = view[2];
		*angle = view[3] & 0x3ff;
	}
	return freeView;
}

int Hw_freeView(void) { return freeView; }

int Hw_allocTexture(void)
{
	if (nextTexture >= OE_HW_MAX_TEXTURES)
		return 0;
	return nextTexture++;
}

void Hw_resetTextures(void) { nextTexture = 1; }

void Hw_texture(int id, int format, int width, int height, int flags, int source,
                const void *pixels, const unsigned char *mask, const unsigned short *palette)
{
	oeHwTexture_t d;
	if (!hwOn || id <= 0)
		return;
	d.id = id;
	d.format = format;
	d.width = width;
	d.height = height;
	d.flags = flags;
	d.source = source;
	trap_HwTexture(&d, pixels, mask, palette);
}

/* where the loaded texels came from: (kind, where they are now) -> number in the texels file */
#define MAX_SOURCES 2048
static struct { int kind, where, number; } sources[MAX_SOURCES];
static int numSources;

void Hw_clearSources(void) { numSources = 0; }

void Hw_noteSource(int kind, int where, int number)
{
	if (numSources < MAX_SOURCES) {
		sources[numSources].kind = kind;
		sources[numSources].where = where;
		sources[numSources].number = number;
		numSources++;
	}
}

int Hw_sourceOf(int kind, int where)
{
	int i;
	for (i = 0; i < numSources; i++)
		if (sources[i].kind == kind && sources[i].where == where)
			return OE_HWSRC(kind, sources[i].number);
	return 0;
}

void Hw_beginScene(void)
{
	memset(&scene, 0, sizeof(scene));
	memset(flatTiles, 0, sizeof(flatTiles));
	scene.fade = 255;
	inScene = hwOn;
	sceneReady = 0;
}

void Hw_setRect(int x, int y, int w, int h)
{
	scene.rect[0] = x;
	scene.rect[1] = y;
	scene.rect[2] = w;
	scene.rect[3] = h;
	if (Hw_fastPass() && Brew_screen() != NULL) {
		/* the rect is in the screen's (device) pixels, like the 3D renderer's */
		IBitmap *scr = Brew_screen();
		int i, j;
		for (j = y; j < y + h && j < scr->height * scr->scale; j++) {
			unsigned short *p = scr->px + j * scr->pitch;
			for (i = x; i < x + w && i < scr->width * scr->scale; i++)
				p[i] = HW_KEY_COLOR;
		}
	}
}

oeHwScene_t *Hw_scene(void) { return &scene; }
short *Hw_flatTiles(void) { return flatTiles; }

oeHwQuad_t *Hw_addQuad(int tex, int blend, int alpha)
{
	oeHwQuad_t *q;
	if (!inScene || tex <= 0 || scene.numQuads >= OE_HW_MAX_QUADS)
		return NULL;
	q = &quads[scene.numQuads++];
	q->tex = tex;
	q->blend = blend;
	q->alpha = alpha;
	return q;
}

void Hw_wall(int tex, int blend, int alpha, int x1, int y1, int x2, int y2, int zBottom, int zTop,
             int s1, int s2, int tTop, int tBottom)
{
	oeHwQuad_t *q = Hw_addQuad(tex, blend, alpha);
	if (q == NULL)
		return;
	q->xyz[0][0] = x1; q->xyz[0][1] = y1; q->xyz[0][2] = zTop;
	q->xyz[1][0] = x2; q->xyz[1][1] = y2; q->xyz[1][2] = zTop;
	q->xyz[2][0] = x2; q->xyz[2][1] = y2; q->xyz[2][2] = zBottom;
	q->xyz[3][0] = x1; q->xyz[3][1] = y1; q->xyz[3][2] = zBottom;
	q->st[0][0] = s1; q->st[0][1] = tTop;
	q->st[1][0] = s2; q->st[1][1] = tTop;
	q->st[2][0] = s2; q->st[2][1] = tBottom;
	q->st[3][0] = s1; q->st[3][1] = tBottom;
}

/* the game's own picture of the 3D view, to tell the engine where 2D was drawn over it */
static int takeSnapshot(void)
{
	IBitmap *screen = Brew_screen();
	int y, x0 = scene.rect[0], y0 = scene.rect[1], w = scene.rect[2], h = scene.rect[3];
	if (screen == NULL || w <= 0 || h <= 0 || x0 < 0 || y0 < 0 || x0 + w > screen->width * screen->scale ||
	    y0 + h > screen->height * screen->scale)
		return 0;
	if (snapshotSize < w * h) {
		free(snapshot);
		snapshot = malloc((size_t)w * h * sizeof(unsigned short));
		snapshotSize = snapshot ? w * h : 0;
		if (!snapshot)
			return 0;
	}
	for (y = 0; y < h; y++)
		memcpy(snapshot + y * w, screen->px + (y0 + y) * screen->pitch + x0, (size_t)w * sizeof(unsigned short));
	scene.screenPitch = w;
	return 1;
}

void Hw_endScene(void)
{
	if (!inScene)
		return;
	inScene = 0;
	freeView = 0;
	sceneReady = takeSnapshot();
}

void Hw_fadeScene(int level)
{
	if (!sceneReady)
		return;
	scene.fade = level;
	sceneReady = takeSnapshot();
}

void Hw_flush(void)
{
	if (!sceneReady)
		return;
	sceneReady = 0;
	trap_HwScene(&scene, quads, flatTiles, snapshot);
}
