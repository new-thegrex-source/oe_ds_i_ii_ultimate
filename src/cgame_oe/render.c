/*
 * Render: per-frame view setup, BSP traversal, wall clipping/projection and
 * the two wall rasterizers (textured walls, bitshape "sprite walls").
 *
 * View space (after Render_transformPoint): x = depth (16.16), y = lateral
 * (16.16). A segment (Line layout: {x1,y1,z1, x2,y2,z2, texture, flags}) holds
 * two view-space points whose z is the texture coordinate along the wall.
 * Projection turns a point into {screen x (16.16), 1/depth scale, z*scale};
 * the rasterizers then step linearly in screen space.
 *
 * Drawing is two-pass: Render_walkBSP walks front to back, marking the columns
 * closed by solid walls (occluder pass, LINE_OCCLUDER) so hidden nodes are
 * culled, and collects the visible leaves and their sprites (depth sorted);
 * Render_renderScene then draws the leaves' walls and the sorted sprites with a
 * per-column 1/z depth test (Render.columnZ).
 */

#include "render_local.h"
#include "oe_hw.h"

/* Port helper (see game.h): 1-based sprite access; "sprite 0" reads as zero like the original's memory. */
Sprite *Render_spriteAt(Render *render, int idx1)
{
	static Sprite scratch; /* "sprite 0": see game.h */
	if (idx1 >= 1 && idx1 <= (int)(sizeof(render->sprites) / sizeof(render->sprites[0])))
		return &render->sprites[idx1 - 1];
	scratch.scale = (uint8_t)render->numLines;
	return &scratch;
}

/* 0x0002aa68 World (x, y) -> view space with the 2x3 matrix Render.viewMatrix. */
void Render_transformPoint(Render *r, int *pt)
{
	int x = pt[0], y = pt[1];
	pt[1] = rmul(r->viewMatrix[3], x) + rmul(r->viewMatrix[4], y) + r->viewMatrix[5];
	pt[0] = rmul(r->viewMatrix[0], x) + rmul(r->viewMatrix[1], y) + r->viewMatrix[2];
}

/* 0x0001f7e8 Project a view-space point for sprite walls (scale Render.width16). */
void Render_projectX(Render *render, int *pt)
{
	int lateral = pt[1];
	int scale = rdiv64(rlsl64(render->width16, 16), pt[0]);
	pt[1] = scale;
	pt[0] = render->centerX16 + (int32_t)(rmul64(lateral, scale) >> 17);
	pt[2] = rmul(scale, pt[2]);
}

/* 0x0001f834 Project a view-space point for textured walls (scale Render.maxX16). */
void Render_projectY(Render *render, int *pt)
{
	int lateral = pt[1];
	int scale = rdiv64(rlsl64(render->maxX16, 16), pt[0]);
	pt[1] = scale;
	pt[0] = render->centerX16 + (int32_t)(rmul64(lateral, scale) >> 17);
	pt[2] = rmul(scale, pt[2]);
}

/* 0x000071c0 Replace the point `out` (one end of seg) by the crossing of seg
 * with a clip plane; d0/d1 are the signed plane distances (t = -d0/(d1-d0)). */
void Render_clipLerpVertex(Render *render, int *out, int *seg, int d0, int d1)
{
	int32_t negD0 = -d0;
	int64_t t = rlsl64(negD0, 16) / (int64_t)(int32_t)(d1 - d0); /* 16.16 */
	int64_t s = 0x10000 - t;

	out[0] = (int32_t)((rmul64(s, seg[0]) + rmul64(t, seg[3]) + 0x8000) >> 16);
	out[1] = (int32_t)((rmul64(s, seg[1]) + rmul64(t, seg[4]) + 0x8000) >> 16);
	out[2] = (int32_t)((rmul64(s, seg[2]) + rmul64(t, seg[5]) + 0x8000) >> 16);
}

/* 0x00007120 Clip a view-space segment {x1,y1,z1,x2,y2,z2} to the 90-degree
 * frustum (x + y >= 0, x - y >= 0) and the near plane x >= 4.0.
 * Returns 0 when nothing is left. */
int Render_clipSegmentNear(Render *render, int *seg)
{
	int *v1 = seg, *v2 = seg + 3;
	int a, b;

	/* left edge */
	a = seg[0] + seg[1];
	b = seg[3] + seg[4];
	if (a < 0) {
		if (b < 0)
			return 0;
		Render_clipLerpVertex(render, v1, seg, a, b);
	} else if (b < 0) {
		Render_clipLerpVertex(render, v2, seg, -a, -b);
	}

	/* right edge */
	a = seg[0] - seg[1];
	b = seg[3] - seg[4];
	if (b < 0) {
		if (a < 0)
			return 0;
		Render_clipLerpVertex(render, v2, seg, a, b);
	} else if (a < 0) {
		Render_clipLerpVertex(render, v1, seg, -a, -b);
	}

	/* near plane */
	a = seg[0] - NEAR_PLANE;
	b = seg[3] - NEAR_PLANE;
	if (b < 0) {
		if (a < 0)
			return 0;
		Render_clipLerpVertex(render, v2, seg, a, b);
	} else if (a < 0) {
		Render_clipLerpVertex(render, v1, seg, -a, -b);
	}
	return 1;
}

/* 0x0000757c Occlusion test of a BSP node's bounding box {x1,y1,x2,y2}: the
 * box edge facing the viewer is projected and the columns it covers are
 * checked. Returns 0 when the node may be visible (some column still empty),
 * 1 when it is hidden or off screen. */
int Render_isBoxOccluded(Render *render, short *bbox)
{
	Line *seg = &render->seg;
	int vx = render->viewX, vy = render->viewY;
	int b0 = bbox[0], b1 = bbox[1], b2 = bbox[2], b3 = bbox[3];
	int x;

	if (render->rSkipCull || Hw_freeView())   /* port: a free camera gets the whole map */
		return 0;
	/* viewer inside (or right next to) the box */
	if (b0 - 5 <= vx && vx <= b2 + 5 && b1 - 5 <= vy && vy <= b3 + 5)
		return 0;

	/* silhouette edge (corner to corner) seen from the viewer */
	if (vx < b0) {
		if (vy < b1) {
			seg->x1 = b2; seg->y1 = b1; seg->x2 = b0; seg->y2 = b3;
		} else if (vy < b3) {
			seg->x1 = b0; seg->y1 = b1; seg->x2 = b0; seg->y2 = b3;
		} else {
			seg->x1 = b0; seg->y1 = b1; seg->x2 = b2; seg->y2 = b3;
		}
	} else if (vx < b2) {
		if (vy < b1) {
			seg->x1 = b2; seg->y1 = b1; seg->x2 = b0; seg->y2 = b1;
		} else if (vy < b3) {
			return 0;
		} else {
			seg->x1 = b0; seg->y1 = b3; seg->x2 = b2; seg->y2 = b3;
		}
	} else {
		if (vy < b1) {
			seg->x1 = b2; seg->y1 = b3; seg->x2 = b0; seg->y2 = b1;
		} else {
			seg->x1 = (vy < b3) ? b2 : b0;
			seg->y1 = b3; seg->x2 = b2; seg->y2 = b1;
		}
	}

	Render_transformPoint(render, &seg->x1);
	Render_transformPoint(render, &seg->x2);
	if (Render_clipSegmentNear(render, &seg->x1)) {
		Render_projectY(render, &seg->x1);
		Render_projectY(render, &seg->x2);
		for (x = (seg->x1 + 0xffff) >> 16; x < (seg->x2 + 0xffff) >> 16; x++)
			if (render->columnZ[x] == DEPTH_EMPTY)
				return 0;
	}
	return 1;
}

/* 0x00021c8c Clip and project a view-space segment and send it to the right
 * rasterizer. Returns 1 when nothing was drawn. Map walls mark their line as
 * seen on the automap (0x80) and as visited this frame (0x800000). */
int Render_drawLineSeg(Render *render, void *segp)
{
	Line *seg = segp;
	int *p = &seg->x1;
	uint32_t flags;
	int result;

	if (!Render_clipSegmentNear(render, p))
		return 1;
	flags = seg->flags;
	if (flags & LINE_HIDDEN)
		return 1;
	if (flags & LINE_OCCLUDER) {
		Render_projectY(render, p);
		Render_projectY(render, p + 3);
		Render_occludeColumns(render, seg);
		return 0;
	}
	if (flags & LINE_SPRITE) {
		Render_projectX(render, p);
		Render_projectX(render, p + 3);
		return Render_rasterizeWall2(render, seg);
	}
	Render_projectY(render, p);
	Render_projectY(render, p + 3);
	result = Render_rasterizeWall(render, seg);
	if (render->curLine >= 0 && !Hw_freeView()) {   /* port: not seen by the player */
		if (result == 0)
			render->lines[render->curLine].flags |= 0x80;
		render->lines[render->curLine].flags |= LINE_DRAWN;
	}
	return result;
}

/* 0x000205cc Draw a world-space segment (in place: it gets transformed). Back
 * faces are skipped unless two-sided, which swaps the ends instead. The drunk
 * wobble shifts every wall except the occluders. */
int Render_drawLine(Render *render, void *linep)
{
	Line *line = linep;

	if (rmul(line->y2 - line->y1, line->x1 - render->viewX) +
	    rmul(-(line->x2 - line->x1), line->y1 - render->viewY) <= 0) {
		int t;
		if (!(line->flags & LINE_TWO_SIDED))
			return 1;
		t = line->x1; line->x1 = line->x2; line->x2 = t;
		t = line->y1; line->y1 = line->y2; line->y2 = t;
		t = line->z1; line->z1 = line->z2; line->z2 = t;
	}
	if (render->drunkLevel != 0 && !(line->flags & LINE_OCCLUDER)) {
		int ofs = render->drunkOfs[0];
		line->x1 += ofs;
		line->y1 += ofs;
		line->x2 += ofs;
		line->y2 += ofs;
	}
	RenderHw_line(render, line);   /* port: for the GPU as well (render_hw.c) */
	Render_transformPoint(render, &line->x1);
	Render_transformPoint(render, &line->x2);
	return Render_drawLineSeg(render, line);
}

/* 0x0000a0bc Textured wall: one vertical span per column, 0x75 (117) texels
 * per 64 world units in both directions. Walls flagged 0xc0010000 run from
 * the floor height to the ceiling height in 64-unit slices, each slice
 * repeating the texture. Returns 1 if nothing was drawn. */
int Render_rasterizeWall(Render *render, void *segp)
{
	Line *seg = segp;
	int result = 1;
	int dx = seg->x2 - seg->x1;
	int dAdX, dUdX, x, xEnd, frac, u, a;
	int texOfs;
	void *span;

	if (dx <= 0)
		return 1;
	render->wallsDrawn++;

	/* per-column steps of the scale (1/z) and the texture coordinate */
	dAdX = rdiv64(rlsl64(seg->y2 - seg->y1, 16), dx);
	dUdX = rdiv64(rlsl64(seg->z2 - seg->z1, 16), dx);

	x = (seg->x1 + 0xffff) >> 16;
	xEnd = (seg->x2 + 0xffff) >> 16;
	if (render->clipX0 > x)
		x = render->clipX0;
	if (render->clipX1 < xEnd)
		xEnd = render->clipX1;
	frac = rshl(x, 16) - seg->x1;
	u = (int32_t)(rmul64(frac, dUdX) >> 16) + seg->z1;
	a = (int32_t)(rmul64(frac, dAdX) >> 16) + seg->y1;

	texOfs = render->wallMap[seg->texture].ofs;
	span = Render_setPalette(render, render->wallMap[seg->texture].palette, render->blendMode);

	for (; x < xEnd; x++) {
		int depth = (0x40000000 / a) << 2;
		int scale = rdiv64(0x1d4000000LL, a);   /* texels per screen pixel, 16.16 */
		int column = rsmulhi(scale, u) % 0x75;
		int du, top, bottom, screenY;
		int64_t tex;

		a += dAdX;
		u += dUdX;
		if (render->columnZ[x] < depth)
			continue;
		if (!(seg->flags & LINE_NO_DEPTH))
			render->columnZ[x] = depth;

		du = scale >> 3;
		tex = (int64_t)texOfs + rmul(0x75, column);
		bottom = render->floorZ;
		top = 0x40;
		if (seg->flags & 0xc0010000u) {
			top = render->ceilHeight;
			bottom = render->floorHeight;
			if (!(seg->flags & LINE_FLOORCEIL))
				bottom = 0x40;
		}

		screenY = -1;
		while (top > bottom) {
			int h = 0x40, topZ, len;
			int64_t uu;

			if (seg->flags & LINE_CLIP_CEIL) {
				int zOff = render->zOffset;
				int pieceTop = zOff + top - 0x20;
				if (pieceTop > render->ceilZ) {
					/* the slice pokes through a lowered ceiling: skip its top */
					h = 0x40 - (pieceTop - render->ceilHeight);
					tex += rmul(pieceTop - render->ceilZ, 0x75) >> 6;
					top = render->ceilHeight - (zOff - 0x20);
				}
			}
			topZ = render->zOffset + top - 0x20;
			if (topZ - h < bottom)
				h = topZ - bottom;
			if (h <= 0)
				break;
			len = rmul(a, h) >> 17;
			if (screenY == -1)
				screenY = render->halfH -
				          ((rmul(top - render->viewZ + render->zOffset - 0x20, a) + 0x8000) >> 17);

			uu = rlsl64(tex, 12);
			if (render->clipY0 > screenY) {
				int d = screenY - render->clipY0;
				uu -= rmul(du, d);
				len += d;
				screenY = render->clipY0;
			}
			if (screenY + len > render->clipY1)
				len = render->clipY1 - screenY;
			if (len > 0) {
				result = 0;
				SPAN_CALL(span, render, x, screenY, uu, du, len);
			}
			screenY += len;
			top -= 0x40;
		}
	}
	return result;
}

/* 0x0000a384 Sprite drawn as a wall segment (billboards and wall-aligned
 * decorations): bitshape column runs scaled by 1/depth. Shapes are 0x58
 * units tall, runs stored bottom-up. Returns 1 if nothing was drawn. */
int Render_rasterizeWall2(Render *render, void *segp)
{
	Line *seg = segp;
	const int16_t *shape = render->shapeData;
	int result = 1;
	int dx = seg->x2 - seg->x1;
	int dAdX, dUdX, x, xEnd, frac, u, a;
	int ofs;
	int64_t texBase;
	void *span;

	if (dx <= 0)
		return 1;
	render->spriteWallsDrawn++;

	dAdX = rdiv64(rlsl64(seg->y2 - seg->y1, 16), dx);
	dUdX = rdiv64(rlsl64(seg->z2 - seg->z1, 16), dx);

	x = (seg->x1 + 0xffff) >> 16;
	xEnd = (seg->x2 + 0xffff) >> 16;
	if (render->clipX0 > x)
		x = render->clipX0;
	if (render->clipX1 < xEnd)
		xEnd = render->clipX1;
	frac = rshl(x, 16) - seg->x1;
	u = (int32_t)(rmul64(frac, dUdX) >> 16) + seg->z1;
	a = (int32_t)(rmul64(frac, dAdX) >> 16) + seg->y1;

	ofs = render->spriteMap[seg->texture].ofs;
	span = Render_setPalette(render, render->spriteMap[seg->texture].palette, render->blendMode);
	/* the shape header starts with its texel offset (two halves) */
	texBase = (int32_t)((uint32_t)(uint16_t)shape[ofs] | ((uint32_t)(uint16_t)shape[ofs + 1] << 16));

	for (; x < xEnd; x++) {
		int depth = (0x40000000 / a) << 2;
		int scale = rdiv64(rlsl64(render->spriteScale, 16), a);
		int column = rsmulhi(scale, u);
		int du, run, viewZ, eyeOfs, floorY, ceilY;

		a += dAdX;
		u += dUdX;
		if (render->columnZ[x] < depth && !(render->segFlags & 2))
			continue;

		du = scale >> 3;
		run = shape[ofs + 4 + column] + ofs + 2;
		viewZ = render->viewZ;
		eyeOfs = rmul(a, viewZ - (render->zOffset - 0x20)) >> 17;
		floorY = (rmul(viewZ - render->floorZ, a) >> 17) + render->halfH;
		ceilY = (rmul(viewZ - render->ceilZ, a) >> 17) + render->halfH;

		for (;;) {
			int word = shape[run];
			int start = word & 0xff;
			int runLen, len, top, k;
			uint32_t flags;
			int64_t uu;

			if (start == SHAPE_END)
				break;
			uu = texBase + shape[run + 1];
			runLen = word >> 8;
			run += 2;
			len = (rmul(runLen, a) / render->spriteScale) >> 1;
			flags = seg->flags;
			k = (flags & LINE_MIRROR) ? start + runLen : 0x58 - start;
			top = render->halfH - ((rmul(k, a) / render->spriteScale) >> 1) + eyeOfs;

			if (!(render->segFlags & 2)) {
				if ((flags & LINE_CLIP_CEIL) && top < ceilY)
					top = ceilY;
				if (!(render->segFlags & 1) && top + len > floorY)
					len = floorY - top;
			}
			uu = rlsl64(uu, 12);
			if (flags & LINE_MIRROR)
				uu += rmul(du, len - 1);
			if (render->clipY0 > top) {
				int d = top - render->clipY0;
				uu -= rmul(du, d);
				len += d;
				top = render->clipY0;
			}
			if (top + len > render->clipY1)
				len = render->clipY1 - top;
			if (len <= 0)
				continue;
			result = 0;
			SPAN_CALL(span, render, x, top, uu, (flags & LINE_MIRROR) ? -du : du, len);
		}
	}
	return result;
}

/* Copy a map line into the temp segment used by the drawers. */
static void loadSeg(Render *render, const Line *line)
{
	render->seg.x1 = line->x1;
	render->seg.y1 = line->y1;
	render->seg.z1 = line->z1;
	render->seg.x2 = line->x2;
	render->seg.y2 = line->y2;
	render->seg.z2 = line->z2;
}

/* Animated wall textures: 4 frames, 256 ms each, phase-shifted per texture. */
static int16_t animTexture(Render *render, int tex)
{
	return (int16_t)((((uint32_t)(render->frameTime + tex * 3) >> 8) & 3) + render->wallBase[tex]);
}

/* 0x0000b044 Draw the walls of a visible leaf: its static lines (moving
 * lines are skipped here), then the dynamic lines linked to the leaf. */
void Render_drawLeafLines(Render *render, void *leafp)
{
	Node *leaf = leafp;
	int count = (int)(leaf->args2 >> 16);
	int i;

	for (i = 0; i < count; i++) {
		int idx = (int)(leaf->args2 & 0xffff) + i;
		Line *line = &render->lines[idx];

		render->curLine = idx;
		if (line->flags & LINE_MOVING)
			continue;
		loadSeg(render, line);
		render->seg.flags = line->flags;
		render->seg.texture = animTexture(render, line->texture);
		render->blendMode = 0;
		render->segFlags = 0;
		Render_drawLine(render, &render->seg);
	}

	if (!render->rSkipDynLines && (~leaf->args1 & 0xff000000u) != 0) {
		unsigned idx;
		int drawn;

		render->f1f198 = render->wallsDrawn;
		for (idx = (leaf->args1 >> 24) & 0xff; idx != 0xff; idx = render->movingLines[idx].next) {
			Line *line = render->movingLines[idx].line;

			loadSeg(render, line);
			render->seg.flags = line->flags;
			render->seg.texture = animTexture(render, line->texture);
			render->blendMode = 0;
			render->segFlags = 0;
			render->curLine = (int)(line - render->lines);
			Render_drawLine(render, &render->seg);
		}
		/* walls drawn by dynamic lines are counted separately */
		drawn = render->wallsDrawn - render->f1f198;
		render->f1f198 = drawn;
		render->wallsDrawn -= drawn;
	}
}

/* Sprite sort bias by tile when it has no entity (or a non-monster one):
 * effects drawn in front of what shares their spot, decorations behind. */
static int spriteDepthBias(uint32_t info, int depth)
{
	unsigned tile = info & 0xff;

	if (tile - 0xf4 < 8)
		return 0;
	if (tile == 0xeb)
		return depth - 4;
	if (tile - 0xf2 < 10 || tile == 0xdf || tile == 0xc0 || tile == 0xc6 || tile == 0xff || tile == 0xec)
		return depth - 3;
	if (info & 0x40000)
		return depth + 2;
	if (tile == 0x8d)
		return depth + 1;
	if (tile == 0x84 || tile == 0x99 || tile == 0xb8)
		return depth + 3;
	if (tile == 0x95 || tile == 0x89)
		return depth - 2;
	if (tile == 0x8a)
		return depth + 2;
	return depth;
}

/* 0x0002c398 Front-to-back BSP walk from `node`: culls hidden nodes, marks the
 * columns closed by each visible leaf's walls (occluder pass), queues the
 * leaf for Render_drawLeafLines and depth-sorts its sprites (far first) into
 * Render.sortedSprites. */
void Render_walkBSP(Render *r, int node)
{
	for (;;) {
		Node *n = &r->nodes[node];
		uint32_t args1;
		int count, i;
		Sprite *sp;

		if (Render_isBoxOccluded(r, (short *)n->bbox))
			return;
		r->nodesVisited++;
		args1 = n->args1;
		if ((args1 & 0x3ffff) >> 16 != 0) {
			int split = (int)(args1 & 0xffff);
			/* near child first, then continue with the far one */
			if (((args1 & 0x20000) && split < r->viewY) || ((args1 & 0x10000) && split < r->viewX)) {
				Render_walkBSP(r, n->args2 & 0xffff);
				node = n->args2 >> 16;
			} else {
				Render_walkBSP(r, n->args2 >> 16);
				node = n->args2 & 0xffff;
			}
			continue;
		}

		/* leaf: link it at the head of the visible-leaf list */
		r->leavesDrawn++;
		n->visPrev = &r->leafSentinel;
		n->visNext = r->leafSentinel.visNext;
		n->visPrev->visNext = n;
		n->visNext->visPrev = n;

		count = (int)(n->args2 >> 16);
		for (i = 0; i < count; i++) {
			int idx = (int)(n->args2 & 0xffff) + i;
			Line *line = &r->lines[idx];

			r->curLine = idx;
			if (line->flags & 0x1080000)
				continue;
			r->linesDrawn++;
			loadSeg(r, line);
			r->seg.flags = line->flags | LINE_OCCLUDER;
			r->blendMode = 0;
			r->segFlags = 0;
			r->zOffset = 0x20;
			Render_drawLine(r, &r->seg);
		}

		if (!r->rSkipDynLines && (~n->args1 & 0xff000000u) != 0) {
			unsigned idx;
			for (idx = (n->args1 >> 24) & 0xff; idx != 0xff; idx = r->movingLines[idx].next) {
				Line *line = r->movingLines[idx].line;

				loadSeg(r, line);
				r->seg.flags = line->flags | LINE_OCCLUDER;
				r->blendMode = 0;
				r->segFlags = 0;
				r->zOffset = 0x20;
				r->curLine = (int)(line - r->lines);
				Render_drawLine(r, &r->seg);
				r->dynLinesDrawn++;
			}
		}

		for (sp = n->sprites; sp != NULL; sp = sp->nodeNext) {
			uint32_t info = sp->info;
			int depth;

			if (info & 0x800000) {
				depth = DEPTH_EMPTY;
			} else {
				Entity *ent = sp->ent;
				depth = rmul(r->viewMatrix[0], sp->x) + rmul(r->viewMatrix[1], sp->y) + r->viewMatrix[2];
				if (ent == NULL)
					depth = spriteDepthBias(info, depth);
				else if (ent->info & 0x40020000)
					depth += 1;   /* corpses / removed */
				else if (ent->def->eType == 2)
					depth -= 1;   /* live monsters */
				else
					depth = spriteDepthBias(info, depth);
			}
			sp->depth = depth;
			if (!(info & 0x8000000) && !Hw_freeView())
				sp->info = info | 0x8000000;   /* seen (port: by the player, not a free camera) */

			/* insertion sort, largest depth first */
			if (r->sortedSprites == NULL) {
				sp->sortNext = NULL;
				r->sortedSprites = sp;
			} else if (r->sortedSprites->depth <= depth) {
				sp->sortNext = r->sortedSprites;
				r->sortedSprites = sp;
			} else {
				Sprite *prev = r->sortedSprites;
				while (prev->sortNext != NULL && depth < prev->sortNext->depth)
					prev = prev->sortNext;
				sp->sortNext = prev->sortNext;
				prev->sortNext = sp;
			}
		}
		return;
	}
}

/* 0x00020218 BSP scene: walk (occluders + collection), then the visible
 * leaves' walls, then the sorted sprites; keeps the peak counters. x, y and
 * angle are unused. */
void Render_renderScene(Render *render, int x, int y, int angle)
{
	Node *sentinel = &render->leafSentinel;
	Applet *app = render->app;
	Node *leaf;
	Sprite *sprite;

	render->nodesVisited = 0;
	render->leavesDrawn = 0;
	render->linesDrawn = 0;
	render->wallsDrawn = 0;
	render->spritesDrawn = 0;
	render->spriteWallsDrawn = 0;
	render->dynLinesDrawn = 0;
	render->f1f198 = 0;
	render->sortedSprites = NULL;
	sentinel->visNext = sentinel;
	sentinel->visPrev = sentinel;

	Render_walkBSP(render, 0);

	if (!render->rSkipLines)
		for (leaf = sentinel->visNext; leaf != sentinel; leaf = leaf->visNext)
			Render_drawLeafLines(render, leaf);

	if (!render->rSkipSprites)
		for (sprite = render->sortedSprites; sprite != NULL; sprite = sprite->sortNext) {
			render->spritesDrawn++;
			Render_drawSprite(render, sprite);
		}

	if (app->statPeakSprites < render->spritesDrawn)
		app->statPeakSprites = render->spritesDrawn;
	if (app->statPeakLines < render->linesDrawn)
		app->statPeakLines = render->linesDrawn;
	if (app->statPeakNodes < render->nodesVisited)
		app->statPeakNodes = render->nodesVisited;
}

/* 0x000204c0 Debug path (r_skipBSP): every line, then every map sprite,
 * with no culling or sorting. */
void Render_drawAll(Render *render)
{
	int i;

	render->nodesVisited = 0;
	render->linesDrawn = 0;
	render->wallsDrawn = 0;
	render->spritesDrawn = 0;
	render->spriteWallsDrawn = 0;
	render->dynLinesDrawn = 0;
	render->f1f198 = 0;

	if (!render->rSkipLines)
		for (i = 0; i < render->numLines; i++) {
			Line *line = &render->lines[i];

			render->linesDrawn++;
			loadSeg(render, line);
			render->seg.texture = render->wallBase[line->texture];
			render->seg.flags = line->flags;
			render->blendMode = 0;
			render->segFlags = 0;
			render->curLine = i;
			Render_drawLine(render, &render->seg);
		}

	if (!render->rSkipSprites)
		for (i = 0; i < render->numMapSprites; i++) {
			render->spritesDrawn++;
			render->curLine = i;
			Render_drawSprite(render, &render->sprites[i]);
		}
}

/* 0x0000e93c amp * sin(2pi * (t/1000 s * freq Hz) + phase), rounded; amp
 * and freq are 16.16, phase in 1/1024 turns. Exact 64-bit fixed-point steps
 * of the original (16.16 x 16.16 product split into halves). */
int Render_sinWave(Render *render, int timeMs, int amp, int freq, int phase)
{
	int64_t secs = rlsl64(timeMs, 32) / 0x3e80000;     /* t / 1000 as 16.16 */
	int32_t secsLo = (int32_t)(secs & 0xffff);
	int64_t secsHi = secs >> 16;
	int32_t turns = (int32_t)(rlsl64(freq, 26) >> 16); /* freq * 1024 as 16.16 */
	int32_t turnsHi = turns >> 16;
	int32_t turnsLo = turns & 0xffff;
	int64_t hiHi = rmul64(secsHi, turnsHi);
	int64_t cross1 = rmul64(rlsl64(turnsHi, 16), secsLo) >> 16;
	int64_t cross2 = rmul64(rlsl64(secsHi, 16), turnsLo) >> 16;
	int64_t sum = cross2 + cross1 + (rmul(secsLo, turnsLo) >> 16);
	int64_t angle = (sum >> 16) + hiHi + phase;
	int idx = (int)(angle % 0x400);

	/* the original indexes with the signed remainder; never negative for the
	 * (non-negative) times the game passes */
	if (idx < 0)
		idx &= 0x3ff;
	return (int32_t)((((int64_t)render->sinTable[idx] * amp >> 16) + 0x8000) >> 16);
}

/* 0x00010b08 Reset the per-column depth buffer. */
void Render_clearColumnZ(Render *render)
{
	int x = render->viewW;
	while (--x >= 0)
		render->columnZ[x] = DEPTH_EMPTY;
}

/* 0x0001cb00 Occluder pass: close the columns covered by a projected solid wall. */
void Render_occludeColumns(Render *render, void *segp)
{
	const int *seg = segp;
	int x2 = seg[3];
	int x;

	if (x2 - seg[0] <= 0)
		return;
	for (x = (seg[0] + 0xffff) >> 16; x < (x2 + 0xffff) >> 16; x++)
		render->columnZ[x] = DEPTH_SOLID;
}

/* One floor/ceiling height lerp (script op LERPFLAT); returns 1 while running. */
static void lerpFlat(Render *render, FlatLerp *lerp, uint8_t *height)
{
	int now, elapsed;

	if (lerp->duration == 0)
		return;
	now = (int)GETUPTIMEMS();
	elapsed = now - lerp->startTime;
	if (elapsed < lerp->duration) {
		int t = rshl(elapsed, 16) / rshl(lerp->duration, 8);
		*height = (uint8_t)(((uint32_t)rmul(rmul(t, lerp->to - lerp->from), 0x100) >> 16) + lerp->from);
		render->lastLerpTime = now;
	} else {
		*height = (uint8_t)lerp->to;
		lerp->duration = 0;
	}
}

/* 0x00021d40 Render the 3D view from (viewX, viewY, viewZ) facing `angle`
 * (10-bit): view matrix, screen shake, floor/ceiling height lerps and wobble,
 * the drunk effect, then floors/ceilings and the scene. */
void Render_render(Render *render, int viewX, int viewY, int viewZ, int angle)
{
	/* port: or from the engine's free camera (oe_idtech3freecam), as it is */
	int freeView = Hw_queryFreeView(&viewX, &viewY, &viewZ, &angle);
	int s = render->sinTable[angle & 0x3ff];
	int c = render->sinTable[(angle + 0x100) & 0x3ff];
	Canvas *canvas;
	int elapsed, shakeX, shakeY;

	Render_syncScreenDib(render);
	render->frameTime = (int32_t)GETTIMEMS();
	render->profStart = (int32_t)GETUPTIMEMS();

	/* the eye sits 16 units behind the player's position */
	if (!render->noViewOffset && !freeView) {
		viewX -= rshl(c, 4) >> 16;
		viewY += rshl(s, 4) >> 16;
	}
	render->viewX = viewX;
	render->viewY = viewY;
	render->viewZ = viewZ;
	render->viewAngle = angle;
	render->viewMatrix[0] = c;
	render->viewMatrix[1] = -s;
	render->viewMatrix[2] = -(rmul(viewX, c) + rmul(viewY, -s));
	render->viewMatrix[3] = s;
	render->viewMatrix[4] = c;
	render->viewMatrix[5] = -(rmul(viewX, s) + rmul(viewY, c));

	/* the view moves with the screen shake (game pixels); the clip rect keeps it inside */
	canvas = render->canvas;
	shakeX = canvas->shakeX * Brew_screenScale();
	shakeY = canvas->shakeY * Brew_screenScale();
	render->framebuffer = (uint16_t *)(render->dib->pBmp +
	                                   (ptrdiff_t)render->dib->nPitch * (render->viewportY + shakeY) +
	                                   (ptrdiff_t)(render->viewportX + shakeX) * 2);
	render->clipX0 = 0;
	render->clipY0 = 0;
	render->clipX1 = render->viewW;
	render->clipY1 = render->viewH;
	if (shakeX < 0)
		render->clipX0 = -shakeX;
	else if (shakeX > 0)
		render->clipX1 = render->viewW - shakeX;
	if (shakeY < 0)
		render->clipY0 = -shakeY;
	else if (shakeY > 0)
		render->clipY1 = render->viewH - shakeY;

	lerpFlat(render, &render->flatLerps[0], &render->floorHeight);
	lerpFlat(render, &render->flatLerps[1], &render->ceilHeight);

	/* floor wobble (water) while no floor lerp runs */
	render->floorZ = render->floorHeight;
	render->wobbleOfs = 0;
	if (render->flatLerps[0].duration == 0 && render->wobble) {
		int w = Render_sinWave(render, (int)GETUPTIMEMS() - render->lastLerpTime, 0x20000, 0x5555, 0);
		render->wobbleOfs = w;
		if (w > 3)
			render->wobbleOfs = 3;
		else if (w < -3)
			render->wobbleOfs = -3;
		render->floorZ = (int16_t)((render->floorZ + render->wobbleOfs) & 0xff);
	}
	if (render->viewZ < render->floorZ)
		render->viewZ = render->floorZ;
	render->ceilZ = render->ceilHeight;

	/* drunk effect: ring 8 keeps it at 40 or more; the level eases toward the target */
	if (render->app->player->ring == 8 && render->drunkTarget < 0x28)
		render->drunkTarget = 0x28;
	if (render->drunkLevel < render->drunkTarget)
		render->drunkLevel++;
	else if (render->drunkLevel > render->drunkTarget)
		render->drunkLevel--;
	if (render->drunkLevel > 0x4b)
		render->drunkLevel = 0x4b;
	if (render->drunkLevel != 0) {
		int amp, ceil;

		elapsed = (int)GETUPTIMEMS() - render->drunkStart;
		amp = (int32_t)(rlsl64(rshl(render->drunkLevel, 16), 16) / 0xa0000);
		render->drunkOfs[0] = Render_sinWave(render, elapsed, amp, 0x5555, 0xc);
		render->drunkOfs[1] = Render_sinWave(render, elapsed, amp, 0x5555, 0x7f);
		render->drunkOfs[2] = Render_sinWave(render, elapsed, amp, 0x5555, 0x181);
		render->drunkOfs[3] = Render_sinWave(render, elapsed, amp / 2, 0x5555, 0);
		render->drunkOfs[4] = Render_sinWave(render, elapsed, amp / 2, 0x5555, 0x13f);
		render->floorZ = (int16_t)(render->floorZ + render->drunkOfs[3]);
		ceil = rshl((uint16_t)render->ceilZ + render->drunkOfs[4], 16);
		render->ceilZ = (int16_t)(ceil >> 16);
		if ((ceil >> 16) > 0x80)
			render->ceilZ = 0x80;
	}

	RenderHw_beginFrame(render);   /* port: the frame for the GPU as well (render_hw.c) */
	Render_clearColumnZ(render);
	if (!Hw_fastPass())   /* port: the GPU draws them */
		Render_drawFloorCeiling(render);
	render->profFlats = (int32_t)GETUPTIMEMS();

	if (render->rShowLines) {
		int i;
		for (i = 0; i < render->numLines; i++)
			render->lines[i].flags &= ~(uint32_t)LINE_DRAWN;
	}
	if (!render->rSkipBSP)
		Render_renderScene(render, render->viewX, render->viewY, angle);
	else
		Render_drawAll(render);

	render->profEnd = (int32_t)GETUPTIMEMS();
	elapsed = render->profEnd - render->profStart;
	if (render->app->statRenderPeak < elapsed)
		render->app->statRenderPeak = (int32_t)(((uint32_t)elapsed & 0xffff) |
		                                        (uint32_t)rshl(render->viewX >> 6, 16) |
		                                        (uint32_t)rshl(render->viewY >> 6, 24));
	Hw_endScene();
}
