/*
 * The particle system (Applet+0x1b5ebc): blood, gibs and spell effects.
 *
 * 64 particles live on two intrusive lists (active / free) with sentinel
 * nodes. Positions are 24.8 fixed point relative to the view centre and are
 * projected with scaleX/scaleY; velocities are per 1/256 of the update step
 * (16.16 time / 0xa00 ms). A particle with life > 0 is drawn as a filled
 * circle of diameter `life`; life < 0 selects row -(life+1) of a gib image.
 */
#include "core_local.h"

#define NUM_PARTICLES 64

/* gib images (transparent BMPs) */
#define RES_GIBS       5132
#define RES_WOOD_GIBS  5157
#define RES_STONE_GIBS 5185

/* ------------------------------------------------------------ lists */

/* 0x000038c4  Move a particle to the active list. */
void ParticleSystem_linkActive(ParticleSystem *ps, Particle *p)
{
	if (ps->numActive == 0)
		ps->startTime = ps->app->tickTime;
	ps->numActive++;
	p->prev->next = p->next;
	p->next->prev = p->prev;
	p->prev = &ps->activeList;
	p->next = ps->activeList.next;
	ps->activeList.next->prev = p;
	p->prev->next = p;
}

/* 0x000038fc  Move a particle to the free list. */
void ParticleSystem_linkFree(ParticleSystem *ps, Particle *p)
{
	ps->numActive--;
	p->prev->next = p->next;
	p->next->prev = p->prev;
	p->prev = &ps->freeList;
	p->next = ps->freeList.next;
	ps->freeList.next->prev = p;
	p->prev->next = p;
}

/* 0x000070f4 */
void ParticleSystem_freeAll(ParticleSystem *ps)
{
	Particle *p = ps->activeList.next;
	while (p != &ps->activeList) {
		Particle *next = p->next;
		ParticleSystem_linkFree(ps, p);
		p = next;
	}
	ps->numActive = 0;
}

/* ------------------------------------------------------------ startup / shutdown */

/* 0x00029794 */
int ParticleSystem_startup(ParticleSystem *ps)
{
	Applet *app;
	ps->activeList.prev = &ps->activeList;
	ps->activeList.next = &ps->activeList;
	ps->freeList.prev = &ps->freeList;
	ps->freeList.next = &ps->freeList;
	for (int i = 0; i < NUM_PARTICLES; i++) {
		Particle *p = &ps->particles[i];
		p->prev = p;
		p->next = p;
		ParticleSystem_linkFree(ps, p);
	}
	ps->numActive = 0;
	app = ps->app;
	/* particles are drawn in game pixels; the renderer's view is in device
	 * pixels (HD mode), and wide views use the weapon's reference width */
	ps->scaleX = ((Render_spriteRefW(app->render) / Brew_screenScale()) << 16) / 0x8000;
	ps->scaleY = ((app->render->viewH / Brew_screenScale()) << 16) / 0x9400;
	Image_load(app, RES_GIBS, 1, &ps->images[0]);
	Image_load(ps->app, RES_WOOD_GIBS, 1, &ps->images[1]);
	Image_load(ps->app, RES_STONE_GIBS, 1, &ps->images[2]);
	ps->particleSize = 0x18;
	return 1;
}

/* 0x00026e2c */
void ParticleSystem_shutdown(ParticleSystem *ps)
{
	for (int i = 0; i < 3; i++)
		App_freeImage(ps->app, &ps->images[i]);
}

/* ------------------------------------------------------------ spawning */

/* 0x00006954  Distance falloff: 0xae/256 (position) and 0xeb/256 (size) per
 * step beyond the first, times scaleX. Like the original, the size factor is
 * finally derived from the scaled position factor (the 0xeb series is
 * computed and then overwritten). */
void ParticleSystem_setFalloff(ParticleSystem *ps, int n)
{
	if (n < 2) {
		ps->sizeScale = ps->scaleX;
		ps->posScale = ps->scaleX;
		return;
	}
	ps->posScale = 0xae;
	ps->sizeScale = 0xeb;
	for (int i = 1; i < n; i++) {
		ps->posScale = ps->posScale * 0xae >> 8;
		ps->sizeScale = ps->sizeScale * 0xeb >> 8;
	}
	ps->posScale = ps->posScale * ps->scaleX >> 8;
	ps->sizeScale = ps->posScale * ps->scaleX >> 8;
}

/* random value in [lo, hi] from the low 16 bits of a Random_nextInt draw */
static int rand_range(Random *rnd, int lo, int hi)
{
	int r = Random_nextInt(rnd) & 0xffff;
	return r % (hi - lo + 1) + lo;
}

/* 0x000275ec  Emit n dot particles after nImages gib-image particles whose
 * frame row cycles through `range` rows from a random start. */
void ParticleSystem_spawn(ParticleSystem *ps, int n, int nBurst, int range)
{
	Random *rnd = &ps->app->random;
	int originX = 0, originY = 0, firstFrame, imagesLeft = nBurst;

	if (ps->singleOrigin != 0) {
		/* (when min == max the original keeps the raw value, not << 8) */
		originX = ps->xMax;
		if (ps->xMax != ps->xMin)
			originX = rand_range(rnd, ps->xMin, ps->xMax) * 0x100;
		originY = ps->yMax;
		if (ps->yMax != ps->yMin)
			originY = rand_range(rnd, ps->yMin, ps->yMax) * 0x100;
	}
	firstFrame = Random_nextInt(rnd) % range;

	for (int i = 0; i < n + nBurst; i++, imagesLeft--) {
		Particle *p = ps->freeList.next;
		int life;
		if (p == &ps->freeList)
			return;
		if (ps->singleOrigin == 0) {
			p->x = rand_range(rnd, ps->xMin, ps->xMax) * 0x100;
			p->y = rand_range(rnd, ps->yMin, ps->yMax) * 0x100;
		} else {
			p->x = originX;
			p->y = originY;
		}
		if (ps->aimRadius < 1) {
			/* (when min == max the original stores the raw value, unscaled) */
			p->vx = ps->vxMax;
			if (ps->vxMax != ps->vxMin)
				p->vx = (rand_range(rnd, ps->vxMin, ps->vxMax) << 16) / 0x6400;
			p->vy = ps->vyMax;
			if (ps->vyMax != ps->vyMin)
				p->vy = (rand_range(rnd, ps->vyMin, ps->vyMax) << 16) / 0x6400;
		} else {
			/* fly towards the aim point (the draw is made and discarded) */
			Random_nextInt(rnd);
			p->vx = ((ps->aimX * 0x100 - p->x) * 0x100) / 0x2800;
			p->vy = ((ps->aimY * 0x100 - p->y) * 0x100) / 0x2800;
		}
		if (imagesLeft >= 1) {
			life = -((firstFrame + imagesLeft - 1) % range + 1); /* image row */
		} else {
			life = ps->lifeMax;
			if (ps->lifeMax != ps->lifeMin)
				life = rand_range(rnd, ps->lifeMin, ps->lifeMax);
		}
		p->life = life;
		if (life != 0) {
			p->vyAccum = 0;
			ParticleSystem_linkActive(ps, p);
		}
	}
}

/* 8.8 scale with rounding, as the original computed it */
static int scale88(int factor, int v)
{
	return (factor * v * 0x100 + 0x8000) >> 16;
}

/* 0x00024204  Scale the emitter for distance `dist` (1 = adjacent), clip it to
 * the view and emit. `nImages` gib images cycle through `range` rows. */
void ParticleSystem_scaledSpawn(ParticleSystem *ps, int count, int nImages, int dist, int range)
{
	Applet *app;
	Canvas *cv;
	int fs, fp, n;

	ParticleSystem_setFalloff(ps, dist);
	fs = ps->sizeScale;
	n = ((fs * count * 0x100 >> 8) + 0x80) >> 8;
	if (n == 0)
		return;
	fp = ps->posScale;
	ps->vxMin = scale88(fp, ps->vxMin);
	ps->vxMax = scale88(fp, ps->vxMax);
	ps->vyMin = scale88(fp, ps->vyMin);
	ps->vyMax = scale88(fp, ps->vyMax);
	ps->gravity = scale88(fp, ps->gravity);
	app = ps->app;
	cv = app->canvas;
	ps->clipX2 = cv->viewW;
	/* the floor line of the 3D view at that distance */
	switch (dist) {
	case 2:
		ps->clipY2 = cv->sceneY + cv->sceneH - ((ps->scaleY * 0x2e00 + 0x8000) >> 16);
		break;
	case 3:
		ps->clipY2 = cv->sceneY + cv->sceneH - ((ps->scaleY * 0x3800 + 0x8000) >> 16);
		break;
	case 4:
		ps->clipY2 = cv->sceneY + cv->sceneH - ((ps->scaleY * 0x3d00 + 0x8000) >> 16);
		break;
	default:
		ps->clipY2 = cv->viewH - app->hud->bottomBarH;
		break;
	}
	ps->xMin = scale88(fp, ps->xMin);
	ps->xMax = scale88(fp, ps->xMax);
	ps->yMin = scale88(fp, ps->yMin);
	ps->yMax = scale88(fp, ps->yMax);
	ps->lifeMin = scale88(fs, ps->lifeMin);
	ps->lifeMax = scale88(fs, ps->lifeMax);
	if (ps->lifeMax == 0)
		ps->lifeMax = 1;
	ParticleSystem_spawn(ps, n, nImages, range);
}

/* ------------------------------------------------------------ update / draw */

/* 0x0001d848  Move every particle by the time since the last update; with
 * `draw`, draw them (dots as filled circles, gibs as image frames) and retire
 * those that left the clip rectangle or reached the aim point. */
void ParticleSystem_update(ParticleSystem *ps, int draw)
{
	Applet *app;
	Particle *p, *next;
	int step;

	if (ps->numActive == 0)
		return;
	app = ps->app;
	step = ps->startTime < app->tickTime ? ((app->tickTime - ps->startTime) << 16) / 0xa00 : 0;
	Gfx_setColor(app, ps->color);
	Gfx_setClip(ps->app, ps->clipX1, ps->clipY1, ps->clipX2 - ps->clipX1, ps->clipY2 - ps->clipY1);

	for (p = ps->activeList.next; p != &ps->activeList; p = next) {
		int oldX = p->x, oldY = p->y, vx = p->vx;
		int sx, sy, half, life;
		Canvas *cv;
		next = p->next;

		p->x = oldX + (step * vx >> 8);
		p->vyAccum += (step * step >> 8) * ps->gravity >> 8;
		p->y = p->vyAccum + (step * p->vy >> 8) + oldY;
		if (draw == 0)
			continue;

		app = ps->app;
		cv = app->canvas;
		sx = (ps->scaleX * p->x >> 16) + cv->viewCenterX;
		sy = (ps->scaleY * p->y >> 16) + cv->viewCenterY;
		life = p->life;
		half = life < 0 ? ps->particleSize >> 1 : life >> 1;

		if (sx - half > ps->clipX2 || sx + half < ps->clipX1 || sy - half > ps->clipY2 || sy + half < ps->clipY1) {
			ParticleSystem_linkFree(ps, p);
			continue;
		}
		if (ps->aimRadius >= 1) {
			int aimSX = cv->viewCenterX + ps->aimX, aimSY = cv->viewCenterY + ps->aimY;
			int dx = sx - aimSX, dy = sy - aimSY;
			int oldDx = (ps->scaleX * oldX >> 16) + cv->viewCenterX - aimSX;
			int oldDy = (ps->scaleY * oldY >> 16) + cv->viewCenterY - aimSY;
			/* passed the target on either axis, or arrived within the radius */
			if ((dx < 0) != (oldDx < 0) || (dy < 0) != (oldDy < 0) ||
			    ((dx < 0 ? -dx : dx) <= ps->aimRadius && (dy < 0 ? -dy : dy) <= ps->aimRadius)) {
				ParticleSystem_linkFree(ps, p);
				continue;
			}
		}
		if (life < 1) {
			int size = ps->particleSize;
			int frame = (app->tickTime / 0xaf) & 3;
			int w, h;
			if (vx > 0)
				frame = 3 - frame;
			/* cut the frame at the clip edges */
			if (sx + half > ps->clipX2) {
				w = ps->clipX2 - sx + half;
			} else {
				w = size;
				if (sx - half < ps->clipX1) {
					w = sx - ps->clipX1 + half;
					sx = ps->clipX1 + half;
				}
			}
			if (sy + half > ps->clipY2) {
				h = ps->clipY2 - sy + half;
			} else {
				h = size;
				if (sy - half < ps->clipY1) {
					h = sy - ps->clipY1 + half;
					sy = ps->clipY1 + half;
				}
			}
			if (w > 0 && h > 0)
				Canvas_drawRegion(cv, &ps->images[ps->imageType], frame * size, -(life + 1) * size, w, h, 0,
						  sx - half, sy - half, 0, 0, 0);
		} else {
			Gfx_fillCircle(app, sx, sy, life >> 1);
		}
	}
	IGRAPHICS_SetClip(ps->app->graphics, NULL, 0);
	ps->startTime = ps->app->tickTime;
	if (draw != 0)
		ps->f8e8 = ps->app->tickTime;
}

/* ------------------------------------------------------------ emitter presets (called with Combat*) */

/* clip rectangle below the HUD's top bar */
static int top_clip(Combat *combat)
{
	return combat->app->hud->topBarH;
}

/* 0x0001f880  Blood/gibs for the combat target at distance `dist`. count -1 =
 * Combat_calcGibScale. y shifts the emitter vertically. */
void Particles_spawnBlood(void *combatPtr, int dist, int color, int count, int y)
{
	Combat *combat = combatPtr;
	ParticleSystem *ps = combat->app->particles;
	Entity *target = combat->target;
	int range = 8, nImages;
	uint8_t imageType = 0;

	if (target != NULL && target->monster != NULL)
		target->monster->flashUntil = combat->app->tickTime + 100;
	if (count == -1)
		count = Combat_calcGibScale(combat, dist);
	ps->vxMin = -0x96;
	ps->vxMax = 100;
	if (combat->f7c == 0) {
		ps->vyMin = -0xa0;
		ps->vyMax = -0x3c;
	} else {
		ps->vyMin = -0xaf;
		ps->vyMax = -0x4b;
	}
	if (combat->chargeLevel > 0) {
		ps->vxMax = 0x19;
		ps->vxMin = -300;
	}
	ps->xMin = -6;
	ps->xMax = 6;
	ps->yMin = y - 9;
	ps->yMax = y + 0xb;
	ps->color = color;
	ps->gravity = 10;
	ps->lifeMin = 1;
	ps->lifeMax = 4;
	ps->clipX1 = 0;
	ps->clipY1 = top_clip(combat);
	ps->clipX2 = combat->canvas->sceneW;
	ps->aimRadius = 0;
	ps->clipY2 = combat->canvas->sceneH;
	ps->singleOrigin = 0;
	target = combat->target;
	if (target != NULL && target->monster != NULL) {
		if (target->monster->petrifyTurns == 0) {
			if (target->def->eSubType == 2)
				imageType = 1; /* wood */
			else if (target->def->eSubType == 3)
				range = 4;
		} else {
			imageType = 2; /* stone */
		}
		ps->imageType = imageType;
	}
	nImages = combat->f7c == 0 ? 0 : 5;
	ParticleSystem_scaledSpawn(ps, count, nImages, dist, range);
}

/* 0x0001f9b0  Wood debris (dist 1 adds 10 splinter images). */
void Particles_spawnPreset1(void *combatPtr, int dist)
{
	Combat *combat = combatPtr;
	ParticleSystem *ps = combat->app->particles;
	ps->vxMin = -0x5f;
	ps->vxMax = 0x5f;
	ps->vyMin = -0xb4;
	ps->vyMax = -0x4b;
	ps->xMin = -6;
	ps->xMax = 6;
	ps->yMin = 9;
	ps->yMax = 10;
	ps->color = (int)0xff845d39;
	ps->gravity = 0xc;
	ps->lifeMin = 1;
	ps->lifeMax = 4;
	ps->clipX1 = 0;
	ps->clipY1 = top_clip(combat);
	ps->clipX2 = combat->canvas->viewW;
	ps->clipY2 = combat->canvas->viewH - combat->app->hud->bottomBarH;
	ps->aimRadius = 0;
	ps->singleOrigin = 1;
	ps->imageType = 1;
	ParticleSystem_scaledSpawn(ps, 0x19, dist == 1 ? 10 : 0, dist, 8);
}

/* 0x0001fa6c  Radial burst. */
void Particles_spawnBurst(void *combatPtr, int dist, int color, int count)
{
	Combat *combat = combatPtr;
	ParticleSystem *ps = combat->app->particles;
	ps->vxMin = -300;
	ps->vxMax = 300;
	ps->vyMin = -300;
	ps->vyMax = 300;
	ps->xMin = -8;
	ps->xMax = 8;
	ps->yMin = -8;
	ps->color = color;
	ps->yMax = 8;
	ps->gravity = 1;
	ps->lifeMin = 1;
	ps->lifeMax = 4;
	ps->clipX1 = 0;
	ps->clipY1 = top_clip(combat);
	ps->clipX2 = combat->canvas->sceneW;
	ps->clipY2 = combat->canvas->sceneH;
	ps->aimRadius = 0;
	ps->singleOrigin = 1;
	ParticleSystem_scaledSpawn(ps, count, 0, dist, 8);
}

/* 0x0001fb08 */
void Particles_spawnPreset3(void *combatPtr, int dist, int color)
{
	Combat *combat = combatPtr;
	ParticleSystem *ps = combat->app->particles;
	ps->vxMin = -0x55;
	ps->vxMax = 0x55;
	ps->vyMin = -0xa0;
	ps->vyMax = -100;
	ps->xMin = -0x10;
	ps->xMax = 0x10;
	ps->yMin = -9;
	ps->color = color;
	ps->yMax = 0xb;
	ps->gravity = 1;
	ps->lifeMin = 1;
	ps->lifeMax = 4;
	ps->clipX1 = 0;
	ps->clipY1 = top_clip(combat);
	ps->clipX2 = combat->canvas->viewW;
	ps->aimRadius = 0;
	ps->clipY2 = combat->canvas->viewH - combat->app->hud->bottomBarH;
	ps->singleOrigin = 0;
	ParticleSystem_scaledSpawn(ps, 0xf, 0, dist, 8);
}

/* 0x0001fbb0  45 particles converging on a point just below the view centre
 * (the velocity ranges are not set: aimed particles compute their own). */
void Particles_spawnToTarget(void *combatPtr, int color)
{
	Combat *combat = combatPtr;
	ParticleSystem *ps = combat->app->particles;
	ps->xMin = -0x2c;
	ps->xMax = 0x2c;
	ps->yMin = -0x2c;
	ps->color = color;
	ps->yMax = 0x2c;
	ps->gravity = 0;
	ps->lifeMin = 1;
	ps->lifeMax = 4;
	ps->clipX1 = 0;
	ps->clipY1 = top_clip(combat);
	ps->clipX2 = combat->canvas->viewW;
	ps->clipY2 = combat->canvas->viewH - combat->app->hud->bottomBarH;
	ps->aimRadius = 6;
	ps->aimX = 0;
	ps->aimY = 0xf;
	ps->singleOrigin = 0;
	ParticleSystem_scaledSpawn(ps, 0x2d, 0, 1, 8);
}

/* 0x0001fc50  Wood image particles at distance 1. */
void Particles_spawnImage1(void *combatPtr, int color)
{
	Combat *combat = combatPtr;
	ParticleSystem *ps = combat->app->particles;
	ps->vxMin = -0x14;
	ps->vxMax = 0x14;
	ps->vyMin = -0x14;
	ps->vyMax = -10;
	ps->xMin = -0x10;
	ps->xMax = 0x10;
	ps->yMin = -0x10;
	ps->color = color;
	ps->yMax = 0x10;
	ps->gravity = 6;
	ps->lifeMin = 1;
	ps->lifeMax = 4;
	ps->clipX1 = 0;
	ps->clipY1 = top_clip(combat);
	ps->clipX2 = combat->canvas->viewW;
	ps->clipY2 = combat->canvas->viewH - combat->app->hud->bottomBarH;
	ps->aimRadius = 0;
	ps->singleOrigin = 0;
	ps->imageType = 1;
	ParticleSystem_scaledSpawn(ps, 0x1e, 10, 1, 8);
}
