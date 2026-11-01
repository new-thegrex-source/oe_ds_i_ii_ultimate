/*
 * Graphics wrapper (Canvas+0xe10) over IGraphics, colour helpers, and the
 * particle system (Applet+0x140) used for blood, gibs and spell effects.
 */
#include "core_local.h"

/* ------------------------------------------------------------ Graphics */

/* 0x000241c0 */
void Graphics_init(Graphics *g)
{
	Applet *app = g_app;
	g->g = app->graphics;
	g->display = app->display;
	g->screenRect = app->canvas->screenRect;
	g->origin = app->canvas->viewRect;
}

/* 0x00039d88  ARGB -> pen, background and fill colour. */
void Graphics_setColor(Graphics *g, int argb)
{
	uint8_t r = (uint8_t)(argb >> 16), gr = (uint8_t)(argb >> 8), b = (uint8_t)argb;
	IGRAPHICS_SetColor(g->g, r, gr, b, 0xff);
	IGRAPHICS_SetBackground(g->g, r, gr, b);
	IGRAPHICS_SetFillColor(g->g, r, gr, b, 0xff);
}

/* 0x00039d1c  Clip rectangle relative to the view origin. */
void Graphics_setClip(Graphics *g, short x, short y, short w, short h)
{
	AEEClip clip;
	clip.type = CLIPPING_RECT;
	clip.rect.x = (int16_t)(g->origin[0] + x);
	clip.rect.y = (int16_t)(g->origin[1] + y);
	clip.rect.dx = w;
	clip.rect.dy = h;
	IGRAPHICS_SetClip(g->g, &clip, 0);
	g->clipSet = 1;
}

/* 0x0000fcac */
void Graphics_drawLine(Graphics *g, int x1, int y1, int x2, int y2)
{
	AEELine ln;
	ln.sx = (int16_t)(g->origin[0] + x1);
	ln.sy = (int16_t)(g->origin[1] + y1);
	ln.ex = (int16_t)(g->origin[0] + x2);
	ln.ey = (int16_t)(g->origin[1] + y2);
	IGRAPHICS_DrawLine(g->g, &ln);
}

/* 0x000103b0  Outline covering w+1 x h+1 pixels. */
void Graphics_drawRect(Graphics *g, int x, int y, int w, int h)
{
	AEERect rc;
	rc.x = (int16_t)(g->origin[0] + x);
	rc.y = (int16_t)(g->origin[1] + y);
	rc.dx = (int16_t)(w + 1);
	rc.dy = (int16_t)(h + 1);
	IGRAPHICS_SetFillMode(g->g, FALSE);
	IGRAPHICS_DrawRect(g->g, &rc);
}

/* 0x00018f4c  Solid rectangle in the current (background) colour. */
void Graphics_fillRect(Graphics *g, int x, int y, int w, int h)
{
	AEERect rc;
	rc.x = (int16_t)(g->origin[0] + x);
	rc.y = (int16_t)(g->origin[1] + y);
	rc.dx = (int16_t)w;
	rc.dy = (int16_t)h;
	IGRAPHICS_ClearRect(g->g, &rc);
}

/* 0x00018ec4 */
void Graphics_fillCircle(Graphics *g, int x, int y, int r)
{
	AEECircle c;
	c.cx = (int16_t)(g->origin[0] + x);
	c.cy = (int16_t)(g->origin[1] + y);
	c.r = (int16_t)r;
	IGRAPHICS_SetFillMode(g->g, TRUE);
	IGRAPHICS_DrawCircle(g->g, &c);
}

/* 0x0000c7fc  0xRRGGBB -> RGB565 (sign-extended 16-bit, as the original returned). */
int Color_rgb888to565(void *x, int rgb)
{
	uint32_t v = (uint32_t)rgb;
	(void)x;
	return (int16_t)((((v & 0xffffff) >> 19) << 11) | (((v & 0xffff) >> 10) << 5) | ((v & 0xff) >> 3));
}

/* 0x000080f4  Per-channel multiply of two RGB565 colours. */
int Color565_mul(int a, int b)
{
	uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
	return (int)((((ub >> 11) * (ua >> 11) + 0x1f) * 0x40 & 0xf800) |
		     ((((ub & 0x7ff) >> 5) * ((ua & 0x7ff) >> 5) + 0x3f) >> 1 & 0x7e0) |
		     ((ub & 0x1f) * (ua & 0x1f) + 0x1f) >> 5);
}

/* ------------------------------------------------------------ small helpers */

/* 0x0000415c */
int Math_abs(int x)
{
	return x < 0 ? -x : x;
}

/* 0x00009b64 */
void Vec3_zero(int *v)
{
	v[2] = 0;
	v[1] = 0;
	v[0] = 0;
}

/* 0x00009e18 */
void Vec3_copy(int *dst, int *src)
{
	int y = src[1], z = src[2];
	dst[0] = src[0];
	dst[1] = y;
	dst[2] = z;
}

/* ------------------------------------------------------------ particles */

/* Gib images: 0 blood/gore, 1 wood, 2 stone, 3 bones. */
static Image *particle_image(ParticleSystem *ps, int type)
{
	switch (type) {
	case 0: return &ps->imgGibs;
	case 1: return &ps->imgWoodGibs;
	case 2: return &ps->imgStoneGibs;
	default: return &ps->imgSkeletonGibs;
	}
}

/* 0x000056b4  Move a particle to the active list. */
void ParticleSystem_linkActive(ParticleSystem *ps, Particle *node)
{
	if (ps->numActive == 0)
		ps->lastTime = g_app->time;
	ps->numActive++;
	node->prev->next = node->next;
	node->next->prev = node->prev;
	node->next = ps->activeList.next;
	node->prev = &ps->activeList;
	ps->activeList.next->prev = node;
	node->prev->next = node;
}

/* 0x00005730  Move a particle to the free list. */
void ParticleSystem_linkFree(ParticleSystem *ps, Particle *node)
{
	ps->numActive--;
	node->prev->next = node->next;
	node->next->prev = node->prev;
	node->prev = &ps->freeList;
	node->next = ps->freeList.next;
	ps->freeList.next->prev = node;
	node->prev->next = node;
}

/* 0x00009b78 */
void ParticleSystem_freeAll(ParticleSystem *ps)
{
	Particle *node = ps->activeList.next;
	while (node != &ps->activeList) {
		Particle *next = node->next;
		ParticleSystem_linkFree(ps, node);
		node = next;
	}
	ps->numActive = 0;
}

/* 0x000400ec */
int ParticleSystem_startup(ParticleSystem *ps)
{
	Applet *app = g_app;
	ps->activeList.prev = &ps->activeList;
	ps->activeList.next = &ps->activeList;
	ps->freeList.prev = &ps->freeList;
	ps->freeList.next = &ps->freeList;
	for (int i = 0; i < 64; i++) {
		Particle *node = &ps->particles[i];
		node->prev = node;
		node->next = node;
		ParticleSystem_linkFree(ps, node);
	}
	ps->numActive = 0;
	/* particles are drawn in game pixels; the renderer's view is in device
	 * pixels (HD mode), and wide views use the weapon's reference width */
	ps->scale = ((Render_spriteRefW(app->render) / Brew_screenScale()) << 16) / 0x8000;
	ps->heightFactor = ((app->render->height / Brew_screenScale()) << 16) / 0x9400;
	Image_load(app, app->render->greenBlood == 0 ? 0xd : 0xe, &ps->imgGibs, 1);
	Image_load(app, 0x32, &ps->imgWoodGibs, 1);
	Image_load(app, 0x29, &ps->imgStoneGibs, 1);
	Image_load(app, 0x28, &ps->imgSkeletonGibs, 1);
	ps->particleSize = 0x18;
	return 1;
}

/* 0x0003c65c */
void ParticleSystem_shutdown(ParticleSystem *ps)
{
	Applet *app = g_app;
	App_freeImage(app, &ps->imgGibs);
	App_freeImage(app, &ps->imgWoodGibs);
	App_freeImage(app, &ps->imgStoneGibs);
	App_freeImage(app, &ps->imgSkeletonGibs);
}

/* 0x000093f4  Size falloff with distance n (0xae/256 per step, x scale).
 * Like the original, the y factor is derived from the x factor (the 0xeb
 * series is computed and then overwritten). */
void ParticleSystem_setFalloff(ParticleSystem *ps, int n)
{
	if (n < 2) {
		ps->falloffY = ps->scale;
		ps->falloffX = ps->scale;
		return;
	}
	ps->falloffX = 0xae;
	ps->falloffY = 0xeb;
	for (int i = 1; i < n; i++) {
		ps->falloffX = ps->falloffX * 0xae >> 8;
		ps->falloffY = ps->falloffY * 0xeb >> 8;
	}
	ps->falloffX = ps->falloffX * ps->scale >> 8;
	ps->falloffY = ps->falloffX * ps->scale >> 8;
}

/* 32-bit multiply that wraps like the ARM's MUL */
static int mul32(int a, int b)
{
	return (int32_t)(uint32_t)((int64_t)a * b);
}

/* random value in [lo, hi] from the low 16 bits of a Random_nextInt draw */
static int rand_range(Random *rnd, int lo, int hi)
{
	int r = Random_nextInt(rnd) & 0xffff;
	return r % (hi - lo + 1) + lo;
}

/* 0x0003d20c  Emit n dot particles plus nImages gib-image particles (their
 * frame cycles through `range` frames from a random start). Positions are
 * 24.8 fixed point; velocity is per 1/0x100 of the update step. */
void ParticleSystem_spawn(ParticleSystem *ps, int n, int nImages, int range)
{
	Random *rnd = &g_app->random;
	int originX = 0, originY = 0, firstFrame, total = n + nImages;

	if (ps->singleOrigin != 0) {
		/* (when min == max the original keeps the raw value, not << 8) */
		originX = ps->xMax;
		if (ps->xMax != ps->xMin)
			originX = rand_range(rnd, ps->xMin, ps->xMax) << 8;
		originY = ps->yMax;
		if (ps->yMax != ps->yMin)
			originY = rand_range(rnd, ps->yMin, ps->yMax) << 8;
	}
	firstFrame = Random_nextInt(rnd) % range;

	for (int i = 0; i < total; i++) {
		Particle *node = ps->freeList.next;
		int life;
		if (node == &ps->freeList)
			return;
		if (ps->singleOrigin != 0) {
			node->x = originX;
			node->y = originY;
		} else {
			node->x = rand_range(rnd, ps->xMin, ps->xMax) << 8;
			node->y = rand_range(rnd, ps->yMin, ps->yMax) << 8;
		}
		if (ps->aimRadius > 0) {
			/* fly towards the aim point */
			int dx = (ps->aimX << 8) - node->x;
			int dy = (ps->aimY << 8) - node->y;
			Random_nextInt(rnd);
			node->vx = (dx << 8) / 0x2800;
			node->vy = (dy << 8) / 0x2800;
		} else {
			/* (when min == max the original stores the raw value, unscaled) */
			node->vx = ps->vxMax;
			if (ps->vxMax != ps->vxMin)
				node->vx = (rand_range(rnd, ps->vxMin, ps->vxMax) << 16) / 0x6400;
			node->vy = ps->vyMax;
			if (ps->vyMax != ps->vyMin)
				node->vy = (rand_range(rnd, ps->vyMin, ps->vyMax) << 16) / 0x6400;
		}
		if (nImages > 0) {
			nImages--;
			life = -((firstFrame + nImages) % range + 1); /* image frame */
		} else {
			life = ps->lifeMax;
			if (ps->lifeMax != ps->lifeMin)
				life = rand_range(rnd, ps->lifeMin, ps->lifeMax);
		}
		node->life = life;
		if (life != 0) {
			node->vyAccum = 0;
			ParticleSystem_linkActive(ps, node);
		}
	}
}

/* 0x00037aa4  Scale the emitter by distance `dist`, clip to the view, emit. */
void ParticleSystem_scaledSpawn(ParticleSystem *ps, int count, int nImages, int dist, int range)
{
	Canvas *cv = g_app->canvas;
	int fx, fy, n;

	ParticleSystem_setFalloff(ps, dist);
	fy = ps->falloffY;
	n = ((fy * count * 0x100 >> 8) + 0x80) >> 8;
	if (n == 0)
		return;
	fx = ps->falloffX;
	ps->vxMin = (fx * ps->vxMin * 0x100 + 0x8000) >> 16;
	ps->vxMax = (fx * ps->vxMax * 0x100 + 0x8000) >> 16;
	ps->vyMin = (fx * ps->vyMin * 0x100 + 0x8000) >> 16;
	ps->vyMax = (fx * ps->vyMax * 0x100 + 0x8000) >> 16;
	ps->gravity = (fx * ps->gravity * 0x100 + 0x8000) >> 16;
	ps->clipX2 = cv->viewRect[2];
	switch (dist) {
	case 2:
		ps->clipY2 = cv->renderRect[1] + cv->renderRect[3] - ((ps->heightFactor * 0x2e00 + 0x8000) >> 16);
		break;
	case 3:
		ps->clipY2 = cv->renderRect[1] + cv->renderRect[3] - ((ps->heightFactor * 0x3800 + 0x8000) >> 16);
		break;
	case 4:
		ps->clipY2 = cv->renderRect[1] + cv->renderRect[3] - ((ps->heightFactor * 0x3d00 + 0x8000) >> 16);
		break;
	default:
		ps->clipY2 = cv->viewRect[3] - 0xf;
		break;
	}
	ps->xMin = (fx * ps->xMin * 0x100 + 0x8000) >> 16;
	ps->xMax = (fx * ps->xMax * 0x100 + 0x8000) >> 16;
	ps->yMin = (fx * ps->yMin * 0x100 + 0x8000) >> 16;
	ps->yMax = (fx * ps->yMax * 0x100 + 0x8000) >> 16;
	ps->lifeMin = (fy * ps->lifeMin * 0x100 + 0x8000) >> 16;
	ps->lifeMax = (fy * ps->lifeMax * 0x100 + 0x8000) >> 16;
	if (ps->lifeMax == 0)
		ps->lifeMax = 1;
	ParticleSystem_spawn(ps, n, nImages, range);
}

/* 0x00037a84 */
void ParticleSystem_scaledSpawn8(ParticleSystem *ps, int count, int nImages, int dist)
{
	ParticleSystem_scaledSpawn(ps, count, nImages, dist, 8);
}

/* 0x0002e37c  Move all particles by the elapsed time; with `draw`, draw them
 * (dots as filled circles, gibs as image frames) and retire those that left
 * the clip rectangle or reached the aim point. */
void ParticleSystem_update(ParticleSystem *ps, int draw)
{
	Applet *app = g_app;
	Canvas *cv = app->canvas;
	Particle *node, *next;
	int step;

	if (ps->numActive == 0)
		return;
	step = ps->lastTime < app->time ? ((app->time - ps->lastTime) << 16) / 0xa00 : 0;
	if (app->render->greenBlood != 0 && ps->color == (int)0xffbb0000)
		ps->color = (int)0xff00bb00; /* red blood turns green */
	Graphics_setColor(cv->graphics, ps->color);
	Graphics_setClip(cv->graphics, (short)ps->clipX1, (short)ps->clipY1, (short)((short)ps->clipX2 - (short)ps->clipX1),
			 (short)((short)ps->clipY2 - (short)ps->clipY1));

	for (node = ps->activeList.next; node != &ps->activeList; node = next) {
		int oldX = node->x, oldY = node->y, vx = node->vx;
		int sx, sy, half, life;
		next = node->next;

		node->x = oldX + (mul32(step, vx) >> 8);
		node->vyAccum += mul32(mul32(step, step) >> 8, ps->gravity) >> 8;
		node->y = node->vyAccum + (mul32(step, node->vy) >> 8) + oldY;
		if (draw == 0)
			continue;

		cv = app->canvas;
		sx = cv->screenCenterX + (ps->scale * node->x >> 16);
		sy = cv->screenCenterY + (ps->heightFactor * node->y >> 16);
		life = node->life;
		half = life < 0 ? ps->particleSize >> 1 : life >> 1;

		if (sx - half > ps->clipX2 || sx + half < ps->clipX1 || sy - half > ps->clipY2 || sy + half < ps->clipY1) {
			ParticleSystem_linkFree(ps, node);
			continue;
		}
		if (ps->aimRadius > 0) {
			int aimSX = cv->screenCenterX + ps->aimX, aimSY = cv->screenCenterY + ps->aimY;
			int dx = sx - aimSX, dy = sy - aimSY;
			int oldDx = cv->screenCenterX + (ps->scale * oldX >> 16) - aimSX;
			int oldDy = cv->screenCenterY + (ps->heightFactor * oldY >> 16) - aimSY;
			/* passed the target on either axis, or arrived within the radius */
			if ((dx < 0) != (oldDx < 0) || (dy < 0) != (oldDy < 0) ||
			    (Math_abs(dx) <= ps->aimRadius && Math_abs(dy) <= ps->aimRadius)) {
				ParticleSystem_linkFree(ps, node);
				continue;
			}
		}
		if (life < 1) {
			int size = ps->particleSize;
			int frame = (app->time / 0xaf) & 3;
			int w = size, h = size;
			if (vx > 0)
				frame = 3 - frame;
			if (sx + half > ps->clipX2) {
				w = ps->clipX2 - sx + half;
			} else if (sx - half < ps->clipX1) {
				w = sx - ps->clipX1 + half;
				sx = ps->clipX1 + half;
			}
			if (sy + half > ps->clipY2) {
				h = ps->clipY2 - sy + half;
			} else if (sy - half < ps->clipY1) {
				h = sy - ps->clipY1 + half;
				sy = ps->clipY1 + half;
			}
			if (w > 0 && h > 0)
				Canvas_drawRegion(cv, particle_image(ps, ps->imageType), frame * size, -(life + 1) * size, w, h,
						  0, sx - half, sy - half, 0, 0, 0);
		} else {
			Graphics_fillCircle(cv->graphics, sx, sy, life >> 1);
		}
	}
	cv = app->canvas;
	Graphics_setClip(cv->graphics, 0, 0, cv->screenRect[2], cv->screenRect[3]);
	ps->lastTime = app->time;
	if (draw != 0)
		ps->f8fc = app->time;
}

/* ------------------------------------------------------------ emitter presets */

/* 0x000306b4  Blood/gibs for the combat target at distance `dist`. count -1 =
 * Combat_calcGibScale; imgType -1 = pick the gib image from the target. */
void Particles_spawnBlood(Combat *combat, int dist, int color, int count, int yOff, int imgType)
{
	Applet *app = g_app;
	ParticleSystem *ps = app->particles;
	Entity *target = combat->target;
	int effect = combat->effectMode != 0;

	if (target != NULL && target->monster != NULL)
		target->monster->f60 = app->time + 100;
	if (count == -1)
		count = Combat_calcGibScale(combat, dist);
	ps->vxMin = -0x96;
	ps->vxMax = 100;
	ps->vyMin = effect ? -0xaf : -0xa0;
	ps->vyMax = effect ? -0x4b : -0x3c;
	ps->xMin = -6;
	ps->xMax = 6;
	ps->yMin = yOff - 9;
	ps->yMax = yOff + 0xb;
	ps->gravity = 10;
	ps->lifeMin = 1;
	ps->lifeMax = 4;
	ps->color = color;
	ps->clipX1 = 0;
	ps->clipY1 = 0x14;
	ps->clipX2 = app->canvas->renderRect[2];
	ps->aimRadius = 0;
	ps->clipY2 = app->canvas->renderRect[3];
	ps->singleOrigin = 0;

	if (imgType != -1) {
		ps->imageType = (uint8_t)imgType;
	} else if (target != NULL) {
		if (target->monster == NULL) {
			if (target->def->eType == 9 && target->def->eSubType == 0x10) {
				count /= 2;
				ps->imageType = 3;
			}
		} else if (target->monster->monsterEffects & 2) {
			ps->imageType = 2;
		} else {
			int sub = target->def->eSubType;
			if (sub == 2 || sub == 3 || sub == 0xf) {
				ps->imageType = 1;
			} else if (sub == 8) {
				count /= 2;
				ps->imageType = 3;
			} else {
				ps->imageType = 0;
			}
		}
	}
	ParticleSystem_scaledSpawn(ps, count, effect ? 5 : 0, dist, 8);
}

/* 0x00030688 */
void Particles_spawnBloodDefault(Combat *combat, int dist, int color, int count)
{
	Particles_spawnBlood(combat, dist, color, count, 0, -1);
}

/* 0x00030880  Wood debris (b == 1 adds 10 splinter images). */
void Particles_spawnPreset1(void *a, int b)
{
	Applet *app = g_app;
	ParticleSystem *ps = app->particles;
	(void)a;
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
	ps->clipY1 = 0x14;
	ps->clipX1 = 0;
	ps->clipX2 = app->canvas->viewRect[2];
	ps->clipY2 = app->canvas->viewRect[3] - 0xf;
	ps->aimRadius = 0;
	ps->singleOrigin = 1;
	ps->imageType = 1;
	ParticleSystem_scaledSpawn8(ps, 0x19, b == 1 ? 10 : 0, b);
}

/* 0x0003096c  Radial burst. */
void Particles_spawnBurst(void *a, int b, int color, int count)
{
	Applet *app = g_app;
	ParticleSystem *ps = app->particles;
	(void)a;
	ps->vxMin = -300;
	ps->vxMax = 300;
	ps->vyMin = -300;
	ps->xMin = -8;
	ps->vyMax = 300;
	ps->xMax = 8;
	ps->yMin = -8;
	ps->yMax = 8;
	ps->gravity = 1;
	ps->color = color;
	ps->lifeMin = 1;
	ps->lifeMax = 4;
	ps->clipY1 = 0x14;
	ps->clipX1 = 0;
	ps->clipX2 = app->canvas->renderRect[2];
	ps->singleOrigin = 1;
	ps->clipY2 = app->canvas->renderRect[3];
	ps->aimRadius = 0;
	ParticleSystem_scaledSpawn8(ps, count, 0, b);
}

/* 0x00030a40 */
void Particles_spawnPreset3(void *a, int b, int color)
{
	Applet *app = g_app;
	ParticleSystem *ps = app->particles;
	(void)a;
	ps->vxMin = -0x55;
	ps->vxMax = 0x55;
	ps->vyMin = -0xa0;
	ps->vyMax = -100;
	ps->xMin = -0x10;
	ps->xMax = 0x10;
	ps->yMin = -9;
	ps->yMax = 0xb;
	ps->gravity = 1;
	ps->color = color;
	ps->lifeMin = 1;
	ps->lifeMax = 4;
	ps->clipX1 = 0;
	ps->clipY1 = 0x14;
	ps->clipX2 = app->canvas->viewRect[2];
	ps->clipY2 = app->canvas->viewRect[3] - 0xf;
	ps->aimRadius = 0;
	ps->singleOrigin = 0;
	ParticleSystem_scaledSpawn8(ps, 0xf, 0, b);
}

/* 0x00030b1c  45 particles converging on a point near the view centre. */
void Particles_spawnToTarget(void *a, int color, int b)
{
	Applet *app = g_app;
	ParticleSystem *ps = app->particles;
	(void)a;
	ps->xMax = 0x2c;
	ps->xMin = -0x2c;
	ps->yMax = 0x2c;
	ps->yMin = -0x2c;
	ps->lifeMin = 1;
	ps->gravity = 0;
	ps->lifeMax = 4;
	ps->color = color;
	ps->clipX1 = 0;
	ps->clipY1 = 0x14;
	ps->clipX2 = app->canvas->viewRect[2];
	ps->clipY2 = app->canvas->viewRect[3] - 0xf;
	ps->aimRadius = 6;
	ps->aimX = 0;
	ps->singleOrigin = 0;
	ps->aimY = 0xf;
	ParticleSystem_scaledSpawn8(ps, 0x2d, 0, b);
}

/* 0x00030be0  Wood image particles at distance 1. */
void Particles_spawnImage1(void *a, int color)
{
	Applet *app = g_app;
	ParticleSystem *ps = app->particles;
	(void)a;
	ps->vxMin = -0x14;
	ps->vxMax = 0x14;
	ps->vyMin = -0x14;
	ps->vyMax = -10;
	ps->xMin = -0x10;
	ps->xMax = 0x10;
	ps->yMax = 0x10;
	ps->yMin = -0x10;
	ps->gravity = 6;
	ps->lifeMax = 4;
	ps->color = color;
	ps->clipX1 = 0;
	ps->clipY1 = 0x14;
	ps->lifeMin = 1;
	ps->clipX2 = app->canvas->viewRect[2];
	ps->clipY2 = app->canvas->viewRect[3] - 0xf;
	ps->aimRadius = 0;
	ps->singleOrigin = 0;
	ps->imageType = 1;
	ParticleSystem_scaledSpawn8(ps, 0x1e, 10, 1);
}

/* 0x00030cac  Stone image particles. */
void Particles_spawnImage2(void *a, int b, int color)
{
	Applet *app = g_app;
	ParticleSystem *ps = app->particles;
	(void)a;
	ps->vxMin = -0x14;
	ps->vxMax = 0x14;
	ps->vyMin = -0x14;
	ps->xMin = -0x10;
	ps->vyMax = -10;
	ps->xMax = 0x10;
	ps->yMin = -0x10;
	ps->yMax = 0x10;
	ps->gravity = 6;
	ps->lifeMin = 1;
	ps->lifeMax = 4;
	ps->color = color;
	ps->clipX1 = 0;
	ps->clipY1 = 0x14;
	ps->clipX2 = app->canvas->viewRect[2];
	ps->clipY2 = app->canvas->viewRect[3] - 0xf;
	ps->aimRadius = 0;
	ps->singleOrigin = 0;
	ps->imageType = 2;
	ParticleSystem_scaledSpawn8(ps, 0x1e, 10, b);
}
