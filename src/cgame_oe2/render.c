/*
 * Render: per-frame view setup, BSP traversal, wall clipping/projection,
 * the two wall rasterizers and sprite placement (Render_drawSpriteAt).
 *
 * View space (after Render_transformPoint): x = depth (16.16), y = lateral
 * (16.16). A Line holds two view-space points {x, y, z} where z is the
 * texture coordinate along the wall. Projection turns a point into
 * {screen x (16.16), 1/depth scale, z*scale}; rasterizers then step
 * linearly in screen space.
 *
 * Drawing is two-pass: Render_walkBSP walks front to back, marking the
 * columns closed by solid walls (occluder pass, LINE_OCCLUDER) so hidden
 * nodes are culled, and collects visible leaves and sprites; then
 * Render_renderScene draws the leaves' walls and the sorted sprites with a
 * per-column 1/z depth test (Render.columnDepth).
 */

#include "render_local.h"
#include "oe_hw.h"

/* 0x00042ed0 World (x, y) -> view space using the 2x3 matrix Render.viewPlane. */
void Render_transformPoint(Render *r, int *pt)
{
	int x = pt[0], y = pt[1];
	pt[1] = rmul(r->viewPlane[4], y) + rmul(r->viewPlane[3], x) + r->viewPlane[5];
	pt[0] = rmul(r->viewPlane[1], y) + rmul(r->viewPlane[0], x) + r->viewPlane[2];
}

/* 0x00030588 Project a view-space point for sprite walls (scale Render.projH). */
void Render_projectX(Render *render, int *pt)
{
	int lateral = pt[1];
	int scale = rdiv64(rmul64((int64_t)render->projH, 0x10000), pt[0]);
	pt[1] = scale;
	pt[0] = render->projX + (int32_t)(rmul64(lateral, scale) >> 17);
	pt[2] = rmul(scale, pt[2]);
}

/* 0x00030608 Project a view-space point for textured walls (scale Render.projW). */
void Render_projectY(Render *render, int *pt)
{
	int lateral = pt[1];
	int scale = rdiv64(rmul64((int64_t)render->projW, 0x10000), pt[0]);
	pt[1] = scale;
	pt[0] = render->projX + (int32_t)(rmul64(lateral, scale) >> 17);
	pt[2] = rmul(scale, pt[2]);
}

/* 0x00009cdc Replace the point `out` (one end of seg) by the crossing of seg
 * with a clip plane; d0/d1 are the signed plane distances (t = -d0/(d1-d0)). */
void Render_lerpClipVertex(Render *r, int *out, Line *seg, int d0, int d1)
{
	int64_t t = rmul64((int64_t)-d0, 0x10000) / (int64_t)(d1 - d0); /* 16.16 */
	int64_t s = 0x10000 - t;

	out[0] = (int32_t)((rmul64(s, seg->x1) + rmul64(t, seg->x2) + 0x8000) >> 16);
	out[1] = (int32_t)((rmul64(s, seg->y1) + rmul64(t, seg->y2) + 0x8000) >> 16);
	out[2] = (int32_t)((rmul64(s, seg->z1) + rmul64(t, seg->z2) + 0x8000) >> 16);
}

/* 0x00009bb4 Clip a view-space segment to the 90-degree view frustum
 * (x + y >= 0, x - y >= 0) and the near plane x >= 4.0. 0 = fully clipped. */
int Render_clipSegmentNear(Render *render, Line *seg)
{
	int *v1 = &seg->x1, *v2 = &seg->x2;
	int a, b;

	/* left edge */
	a = seg->x1 + seg->y1;
	b = seg->x2 + seg->y2;
	if (a < 0) {
		if (b < 0)
			return 0;
		Render_lerpClipVertex(render, v1, seg, a, b);
	} else if (b < 0) {
		Render_lerpClipVertex(render, v2, seg, -a, -b);
	}

	/* right edge */
	a = seg->x1 - seg->y1;
	b = seg->x2 - seg->y2;
	if (b < 0) {
		if (a < 0)
			return 0;
		Render_lerpClipVertex(render, v2, seg, a, b);
	} else if (a < 0) {
		Render_lerpClipVertex(render, v1, seg, -a, -b);
	}

	/* near plane */
	a = seg->x1 - NEAR_PLANE;
	b = seg->x2 - NEAR_PLANE;
	if (b < 0) {
		if (a < 0)
			return 0;
		Render_lerpClipVertex(render, v2, seg, a, b);
	} else if (a < 0) {
		Render_lerpClipVertex(render, v1, seg, -a, -b);
	}
	return 1;
}

/* 0x0000a4d0 Occlusion test of a BSP node's bounding box {x1,y1,x2,y2}:
 * project the box edge facing the viewer and look for any column that is
 * still uncovered. Returns 0 when the node may be visible, 1 when it is
 * hidden (or off screen). */
int Render_isSegmentVisible(Render *render, int *bbox)
{
	Line *seg = &render->tmpLine;
	int vx = render->viewX, vy = render->viewY;
	int x;

	if (render->dbg18824 != 0 || Hw_freeView())   /* port: a free camera gets the whole map */
		return 0;
	/* viewer inside (or next to) the box */
	if (bbox[0] - 5 <= vx && vx <= bbox[2] + 5 && bbox[1] - 5 <= vy && vy <= bbox[3] + 5)
		return 0;

	/* pick the silhouette edge (corner to corner) seen from the viewer */
	if (vx < bbox[0]) {
		if (vy < bbox[1]) {
			seg->x1 = bbox[2]; seg->y1 = bbox[1]; seg->x2 = bbox[0]; seg->y2 = bbox[3];
		} else if (vy < bbox[3]) {
			seg->x1 = bbox[0]; seg->y1 = bbox[1]; seg->x2 = bbox[0]; seg->y2 = bbox[3];
		} else {
			seg->x1 = bbox[0]; seg->y1 = bbox[1]; seg->x2 = bbox[2]; seg->y2 = bbox[3];
		}
	} else if (vx < bbox[2]) {
		if (vy < bbox[1]) {
			seg->x1 = bbox[2]; seg->y1 = bbox[1]; seg->x2 = bbox[0]; seg->y2 = bbox[1];
		} else if (vy < bbox[3]) {
			return 0;
		} else {
			seg->x1 = bbox[0]; seg->y1 = bbox[3]; seg->x2 = bbox[2]; seg->y2 = bbox[3];
		}
	} else {
		if (vy < bbox[1]) {
			seg->x1 = bbox[2]; seg->y1 = bbox[3]; seg->x2 = bbox[0]; seg->y2 = bbox[1];
		} else {
			seg->x1 = (vy < bbox[3]) ? bbox[2] : bbox[0];
			seg->y1 = bbox[3]; seg->x2 = bbox[2]; seg->y2 = bbox[1];
		}
	}

	Render_transformPoint(render, &seg->x1);
	Render_transformPoint(render, &seg->x2);
	if (Render_clipSegmentNear(render, seg)) {
		Render_projectY(render, &seg->x1);
		Render_projectY(render, &seg->x2);
		for (x = (seg->x1 + 0xffff) >> 16; x < (seg->x2 + 0xffff) >> 16; x++)
			if (render->columnDepth[x] == DEPTH_EMPTY)
				return 0;
	}
	return 1;
}

/* 0x00034504 Clip and project a view-space segment and send it to the right
 * rasterizer. Returns 1 when nothing was drawn. */
int Render_drawLineSeg(Render *render, Line *seg)
{
	if (!Render_clipSegmentNear(render, seg) || (seg->flags & LINE_HIDDEN))
		return 1;
	if (seg->flags & LINE_OCCLUDER) {
		Render_projectY(render, &seg->x1);
		Render_projectY(render, &seg->x2);
		Render_occludeColumns(render, seg);
		return 0;
	}
	if (seg->flags & LINE_SPRITE) {
		Render_projectX(render, &seg->x1);
		Render_projectX(render, &seg->x2);
		return Render_rasterizeWall2(render, seg);
	}
	Render_projectY(render, &seg->x1);
	Render_projectY(render, &seg->x2);
	return Render_rasterizeWall(render, seg);
}

/* 0x00032048 Draw a world-space line (in place: it is transformed). Back
 * faces are skipped unless two-sided (then the ends are swapped). */
int Render_drawLine(Render *render, Line *line)
{
	Canvas *canvas = g_app->canvas;
	int x1 = line->x1, y1 = line->y1;

	/* the camera standing exactly on the line's midpoint (doors) */
	if (canvas->viewX == (x1 + line->x2) >> 1 && canvas->viewY == (y1 + line->y2) >> 1)
		return 1;

	if (rmul(line->y2 - y1, x1 - render->viewX) + rmul(-(line->x2 - x1), y1 - render->viewY) <= 0) {
		int t;
		if (!(line->flags & LINE_TWO_SIDED))
			return 1;
		t = line->x1; line->x1 = line->x2; line->x2 = t;
		t = line->y1; line->y1 = line->y2; line->y2 = t;
		t = line->z1; line->z1 = line->z2; line->z2 = t;
	}

	/* drunk wobble moves the walls (not during cinematics, not the occluders) */
	if (canvas->state != 0x11 && render->drunkLevel != 0 && !(line->flags & LINE_OCCLUDER)) {
		line->x1 += render->oscOut[0];
		line->y1 += render->oscOut[0];
		line->x2 += render->oscOut[0];
		line->y2 += render->oscOut[0];
	}
	RenderHw_line(render, line);   /* port: for the GPU as well (render_hw.c) */
	Render_transformPoint(render, &line->x1);
	Render_transformPoint(render, &line->x2);
	return Render_drawLineSeg(render, line);
}

/* 0x0000e7dc Textured wall: one vertical span per column, 117x117 texels per
 * 64x64 world units. Walls flagged 0xc0010000 run from floorHeight to
 * ceilHeight in 64-unit pieces; above a raised ceiling a second texture
 * (texture2) is used for the top piece. Returns 1 if nothing was drawn. */
int Render_rasterizeWall(Render *render, Line *seg)
{
	int result = 1;
	int dx = seg->x2 - seg->x1;
	int dYdX, dZdX, x, xEnd, frac, z, y;
	int multiPiece, dual;
	int texOfs1, texOfs2 = 0;
	uint8_t *otherTexels = NULL;
	uint16_t *otherPalette = NULL;
	void *drawer;
	const TexMap *tm;

	if (dx <= 0)
		return 1;
	render->cntWalls++;

	y = seg->y1;
	dYdX = rdiv64(rmul64((int64_t)(seg->y2 - y), 0x10000), dx);
	z = seg->z1;
	dZdX = rdiv64(rmul64((int64_t)(seg->z2 - z), 0x10000), dx);

	x = (seg->x1 + 0xffff) >> 16;
	xEnd = (seg->x2 + 0xffff) >> 16;
	if (render->clipX1 > x)
		x = render->clipX1;
	if (render->clipX2 < xEnd)
		xEnd = render->clipX2;
	frac = rshl(x, 16) - seg->x1;
	z += (int32_t)(rmul64(frac, dZdX) >> 16);
	y += (int32_t)(rmul64(frac, dYdX) >> 16);

	multiPiece = (seg->flags & 0xc0010000u) != 0;
	dual = render->ceilHeight > 0x40 && multiPiece &&
	       (uint16_t)seg->texture != (uint16_t)seg->texture2;

	tm = &render->wallMap[seg->texture];
	texOfs1 = tm->texelOfs;
	drawer = Render_setPalette(render, tm->palette, render->shade);
	render->curTexels = render->texelPages + tm->page * TEXEL_PAGE_SIZE;
	if (dual) {
		/* remember texture 1, start with texture 2 (the top piece) */
		otherTexels = render->curTexels;
		otherPalette = render->curPalette;
		tm = &render->wallMap[seg->texture2];
		texOfs2 = tm->texelOfs;
		render->curTexels = render->texelPages + tm->page * TEXEL_PAGE_SIZE;
		drawer = Render_setPalette(render, tm->palette, render->shade);
	}

	for (; x < xEnd; x++, y += dYdX, z += dZdX) {
		int depth = (0x40000000 / y) << 2;
		int vStep = rdiv64(0x1d4000000LL, y);   /* 0x75 texels per 64 units, 16.16 */
		int top = 0x40, bottom = render->effFloor;
		int column, texA, texB, screenY, piece, du;

		if (multiPiece) {
			top = render->ceilHeight;
			bottom = render->floorHeight;
			if (!(seg->flags & LINE_FLOORCEIL))
				bottom = 0x40;
		}
		column = rsmulhi(vStep, z) % 0x75;
		if (render->columnDepth[x] < depth)
			continue;
		du = vStep >> 3;
		if (!(seg->flags & LINE_NO_DEPTH))
			render->columnDepth[x] = depth;
		texA = 0x75 * column + texOfs1;
		texB = 0x75 * column + texOfs2;
		screenY = -1;
		piece = 0;

		while (top > bottom) {
			int h = 0x40, topZ, len, u;

			if (seg->flags & LINE_CLIP_CEIL) {
				int pieceTop = render->projZ + top - 0x20;
				if (pieceTop > render->effCeil) {
					int skip = (0x75 * (pieceTop - render->effCeil)) >> 6;
					texA += skip;
					texB += skip;
					h = 0x40 - (pieceTop - render->ceilHeight);
					top = render->ceilHeight - (render->projZ - 0x20);
				}
			}
			topZ = render->projZ + top - 0x20;
			if (topZ - h < bottom)
				h = topZ - bottom;
			if (h <= 0)
				break;
			len = rmul(y, h) >> 17;
			if (screenY == -1)
				screenY = render->centerY - ((rmul(y, top - render->viewZ + render->projZ - 0x20) + 0x8000) >> 17) + 1;

			u = rshl(texA, 12);
			if (dual && piece == 0)
				u = rshl(texB, 12);
			if (render->clipY1 > screenY) {
				int d = screenY - render->clipY1;
				screenY = render->clipY1;
				len += d;
				u -= rmul(du, d);
			}
			if (screenY + len > render->clipY2)
				len = render->clipY2 - screenY;
			if (len > 0) {
				result = 0;
				SPAN_DRAW(drawer, x, screenY, u, du, len);
			}
			screenY += len;
			piece++;
			top -= 0x40;

			if (dual) {
				/* switch to the other texture for the next piece */
				uint16_t *p = render->curPalette;
				uint8_t *t = render->curTexels;
				render->curPalette = otherPalette;
				render->curTexels = otherTexels;
				otherPalette = p;
				otherTexels = t;
			} else if (!multiPiece) {
				break;
			}
		}
	}
	return result;
}

/* 0x0000ed30 Sprite drawn as a wall segment (billboards and wall-aligned
 * decorations): bitshape column runs scaled by 1/depth. Returns 1 if
 * nothing was drawn. */
int Render_rasterizeWall2(Render *render, Line *seg)
{
	int result = 1;
	int dx = seg->x2 - seg->x1;
	int dYdX, dZdX, x, xEnd, frac, z, y;
	int ofs, texBase, maxCol;
	const int16_t *shape = (const int16_t *)render->shapeSpans;
	void *drawer;

	if (dx <= 0)
		return 1;
	render->cnt1884c[1]++;

	y = seg->y1;
	dYdX = rdiv64(rmul64((int64_t)(seg->y2 - y), 0x10000), dx);
	z = seg->z1;
	dZdX = rdiv64(rmul64((int64_t)(seg->z2 - z), 0x10000), dx);

	x = (seg->x1 + 0xffff) >> 16;
	xEnd = (seg->x2 + 0xffff) >> 16;
	if (render->clipX1 > x)
		x = render->clipX1;
	if (render->clipX2 < xEnd)
		xEnd = render->clipX2;
	frac = rshl(x, 16) - seg->x1;
	z += (int32_t)(rmul64(frac, dZdX) >> 16);
	y += (int32_t)(rmul64(frac, dYdX) >> 16);

	ofs = render->spriteMap[seg->texture].ofs;
	drawer = Render_setPalette(render, render->spriteMap[seg->texture].palette, render->shade);
	render->curTexels = render->texelPages + ((render->shapeSpans[ofs + 1] >> 14) & 3) * TEXEL_PAGE_SIZE;
	texBase = (int)(render->shapeSpans[ofs] | ((uint32_t)(render->shapeSpans[ofs + 1] & ~0xc000u) << 16));
	maxCol = shape[ofs + 3] - shape[ofs + 2];

	for (; x < xEnd; x++, y += dYdX, z += dZdX) {
		int depth = (0x40000000 / y) << 2;
		int scale = rdiv64(rmul64((int64_t)render->projScale, 0x10000), y);
		int column = rsmulhi(scale, z);
		int du, run, eyeOfs, floorY, ceilY;

		if (column > maxCol)
			column = maxCol;
		if (render->columnDepth[x] < depth && !(render->effectFlags & 2))
			continue;

		du = scale >> 3;
		run = shape[ofs + 4 + column] + ofs + 2;
		eyeOfs = rmul(y, render->viewZ - (render->projZ - 0x20)) >> 17;
		floorY = render->centerY + (rmul(y, render->viewZ - render->effFloor) >> 17);
		ceilY = render->centerY + (rmul(y, render->viewZ - render->effCeil) >> 17);

		for (;; run += 2) {
			int word = shape[run];
			int start = word & 0xff;
			int runLen = word >> 8;
			int texOfs, top, len, u;
			uint32_t flags;

			if (start == SHAPE_END)
				break;
			texOfs = shape[run + 1] + texBase;
			flags = seg->flags;
			/* shapes are 0x58 (88) units tall, stored bottom-up */
			top = render->centerY -
			      ((rmul((flags & LINE_MIRROR) ? start + runLen : 0x58 - start, y) / render->projScale) >> 1) +
			      eyeOfs;
			len = (rmul(runLen, y) / render->projScale) >> 1;
			u = rshl(texOfs, 12);
			if (flags & LINE_MIRROR)
				u = rmul(len - 1, du) + u;

			if (!(render->effectFlags & 2)) {
				if ((flags & LINE_CLIP_CEIL) && top < ceilY) {
					int d;
					if (top + len <= ceilY)
						continue;
					d = top - ceilY;
					u -= rmul(du, d);
					len += d;
					top = ceilY;
				}
				if (!(render->effectFlags & 1) && top + len > floorY) {
					if (top > floorY)
						continue;
					len = floorY - top;
				}
			}
			if (render->clipY1 > top) {
				int d = top - render->clipY1;
				u -= rmul(du, d);
				top = render->clipY1;
				len += d;
			}
			if (top + len > render->clipY2)
				len = render->clipY2 - top;
			if (len <= 0)
				continue;
			result = 0;
			SPAN_DRAW(drawer, x, top, u, (flags & LINE_MIRROR) ? -du : du, len);
		}
	}
	return result;
}

/* 0x000341c4 Place a sprite in the world. Billboards (flags & 0x780000 == 0)
 * become a view-facing segment drawn with Render_rasterizeWall2; the
 * wall-aligned kinds (0x80000/0x100000 along x, 0x200000/0x400000 along y)
 * become a world line drawn with Render_drawLine. mode = blend mode (shade),
 * extra = effect flags, scale 16.16. Returns 1 if nothing was drawn. */
int Render_drawSpriteAt(Render *render, int x, int y, int z, int tile, int frame, int flags, int mode, int scale, int extra)
{
	Line *line = &render->tmpLine;
	int idx, left, right;

	if (tile == 0x7fff)
		return 1;
	idx = tile + frame;

	if (!(flags & 0x780000)) {
		line->x1 = x;
		line->y1 = y;
		Render_transformPoint(render, &line->x1);
		line->x1 -= 0x100000;
		if (line->x1 < NEAR_PLANE)
			return 1;
	}
	if (render->drunkLevel != 0 && !(flags & 0x780000)) {
		x += render->oscOut[2];
		y += render->oscOut[0];
		line->x1 = x;
		line->y1 = y;
		Render_transformPoint(render, &line->x1);
		line->x1 -= 0x100000;
	}
	line->z1 = 0;

	/* horizontal extent (world units) from the shape's first/last column */
	right = scale >> 11;
	left = -(scale >> 11);
	if (flags & 0x40000) {
		line->flags = LINE_NO_DEPTH;
	} else {
		const uint16_t *shape = &render->shapeSpans[render->spriteMap[idx].ofs];
		if (!(shape[2] == 0 && shape[3] == 0x57 && scale == 0x10000)) {
			int half = scale >> 11;
			left = ((rshl(rmul(scale, (int16_t)shape[2]), 6) / 0x58) >> 16) - half;
			right = ((rshl(rmul(scale, (int16_t)shape[3]), 6) / 0x58) >> 16) - half;
		}
		line->flags = LINE_SPRITE;
	}
	if (flags & 0x1000000) { /* mirrored */
		int t = left;
		left = -right;
		right = -t;
	}

	line->texture = (int16_t)idx;
	line->texture2 = (int16_t)idx;
	render->shade = mode;
	render->effectFlags = (uint32_t)extra;
	render->curLineIdx = -1;
	render->projScale = rdiv64(0x160000000LL, scale);
	render->projZ = z;
	line->z2 = right - left;
	if (flags & 0x40000000)
		line->flags = 0x40000000;
	if (flags & 0x4000000)
		line->flags |= LINE_TWO_SIDED;
	if (flags & 0x20000000)
		line->flags |= LINE_MIRROR;
	if (flags & 0x2000000)
		render->effectFlags |= 2;

	if (!(flags & 0x780000)) {
		/* billboard: already in view space, spread across y */
		line->x2 = line->x1;
		if (flags & 0x1000000) {
			int t = line->z1;
			line->z1 = line->z2;
			line->z2 = t;
		}
		line->y2 = line->y1 + rshl(right, 16);
		line->y1 = line->y1 + rshl(left, 16);
		RenderHw_billboard(render, x, y, left, right, line->z1, line->z2);   /* port: for the GPU as well */
		return Render_drawLineSeg(render, line);
	}

	line->flags |= LINE_CLIP_CEIL;
	if (flags & 0x80000) {
		if (flags & 0x1000000)
			goto along_x_neg;
		goto along_x_pos;
	} else if (flags & 0x100000) {
		if (flags & 0x1000000)
			goto along_x_pos;
		goto along_x_neg;
	} else if (flags & 0x400000) {
		if (flags & 0x1000000)
			goto along_y_neg;
		goto along_y_pos;
	} else if (flags & 0x200000) {
		if (flags & 0x1000000)
			goto along_y_pos;
		goto along_y_neg;
	}
	goto draw;

along_x_pos:
	line->x1 = x + left;
	line->x2 = x + right;
	line->y1 = y;
	line->y2 = y;
	goto draw;
along_x_neg:
	line->x1 = x - left;
	line->x2 = x - right;
	line->y1 = y;
	line->y2 = y;
	goto draw;
along_y_pos:
	line->y1 = y + left;
	line->y2 = y + right;
	line->x1 = x;
	line->x2 = x;
	goto draw;
along_y_neg:
	line->y1 = y - left;
	line->y2 = y - right;
	line->x1 = x;
	line->x2 = x;
draw:
	return Render_drawLine(render, line);
}

/* 0x00009924 Is any of the four edges of tile (tx, ty) drawn (visible)?
 * Probes with invisible wall-aligned sprites (mode 12 = no drawer). */
int Render_tileTouchesWall(Render *render, int tx, int ty)
{
	int x = tx * 64, y = ty * 64;

	if (Render_drawSpriteAt(render, x + 0x20, y, 0x20, 0, 0, 0x140000, 12, 0x10000, 0) == 0 ||
	    Render_drawSpriteAt(render, x + 0x20, y + 0x40, 0x20, 0, 0, 0xc0000, 12, 0x10000, 0) == 0 ||
	    Render_drawSpriteAt(render, x + 0x40, y + 0x20, 0x20, 0, 0, 0x240000, 12, 0x10000, 0) == 0 ||
	    Render_drawSpriteAt(render, x, y + 0x20, 0x20, 0, 0, 0x440000, 12, 0x10000, 0) == 0)
		return 1;
	return 0;
}

/* ======================================================================
 * BSP
 * ==================================================================== */

/* 0x00045940 Front-to-back BSP walk with occlusion culling. Leaves: solid
 * walls are drawn in occluder mode, the leaf is queued for the wall pass,
 * and its sprites are inserted into Render.visibleSprites sorted far to
 * near by squared distance (with small per-type biases so decals and
 * effects sort in front of/behind what they sit on). */
void Render_walkBSP(Render *r, int node)
{
	Node *n = &r->nodes[node];
	uint32_t args1;
	Sprite *s;
	int i, count;

	if (Render_isSegmentVisible(r, &n->x1) != 0)
		return;
	r->cntNodes++;
	args1 = n->args1;

	if (args1 & 0x30000) {
		int split = (int)(args1 & 0xffff);
		if (((args1 & 0x20000) && r->viewY > split) || ((args1 & 0x10000) && r->viewX > split)) {
			Render_walkBSP(r, n->args2 & 0xffff);
			Render_walkBSP(r, n->args2 >> 16);
		} else {
			Render_walkBSP(r, n->args2 >> 16);
			Render_walkBSP(r, n->args2 & 0xffff);
		}
		return;
	}

	r->cntLeaves++;
	/* push the leaf on the visible-leaf list (drawn by Render_renderScene) */
	n->visPrev = &r->leafSentinel;
	n->visNext = r->leafSentinel.visNext;
	n->visPrev->visNext = n;
	n->visNext->visPrev = n;

	count = (int)(n->args2 >> 16);
	for (i = 0; i < count; i++) {
		int li = (int)(n->args2 & 0xffff) + i;
		Line *src = &r->lines[li];
		r->curLineIdx = li;
		if (src->flags & 0x1000000)
			continue;
		r->cntLines++;
		r->tmpLine.x1 = src->x1;
		r->tmpLine.y1 = src->y1;
		r->tmpLine.z1 = src->z1;
		r->tmpLine.x2 = src->x2;
		r->tmpLine.y2 = src->y2;
		r->tmpLine.z2 = src->z2;
		r->tmpLine.flags = src->flags | LINE_OCCLUDER;
		r->shade = 0;
		r->projZ = 0x20;
		r->effectFlags = 0;
		Render_drawLine(r, &r->tmpLine);
	}

	for (s = n->sprites; s != NULL; s = s->nodeNext) {
		int dx = s->x - r->viewX, dy = s->y - r->viewY;
		uint32_t dist = (uint32_t)rmul(dx, dx) + (uint32_t)rmul(dy, dy);
		uint32_t info = s->info;
		int tile = (int)(info & 0xff);
		int biased = 0;

		if (s->ent != NULL) {
			if (s->ent->info & 0x20020000) {
				dist += 1;
				biased = 1;
			} else if (s->ent->def->eType == 2) {
				dist -= 1;
				biased = 1;
			}
		}
		if (!biased) {
			if ((unsigned)(tile - 0xf6) <= 5)
				dist = 0;
			else if ((unsigned)(tile - 0xf2) <= 0xb || tile == 0xdf || tile == 0xc0 || tile == 0xc6 || tile == 0xff)
				dist -= 3;
			else if (info & 0x40000)
				dist += ((unsigned)(tile - 0x10) <= 0xf) ? 0x1000 : 2;
			else if (tile == 0x8d)
				dist += 1;
			else if (tile == 0x95)
				dist -= 2;
			else if (tile == 0x8a)
				dist += 2;
			else if (tile == 0xf || tile == 0x96 || tile == 0xb5)
				dist -= 2;
			else if (tile == 0x7b)
				dist -= 0x800;
		}
		s->f14 = (int32_t)dist;
		if (!(info & 0x8000000) && !Hw_freeView())   /* port: seen by the player, not a free camera */
			s->info = info | 0x8000000;

		/* insert, farthest first (unsigned compare like the original) */
		if (r->visibleSprites == NULL) {
			s->sortNext = NULL;
			r->visibleSprites = s;
		} else if (!((uint32_t)r->visibleSprites->f14 > dist)) {
			s->sortNext = r->visibleSprites;
			r->visibleSprites = s;
		} else {
			Sprite *p = r->visibleSprites;
			while (p->sortNext != NULL && (uint32_t)p->sortNext->f14 > dist)
				p = p->sortNext;
			s->sortNext = p->sortNext;
			p->sortNext = s;
		}
	}
}

/* 0x0001000c Wall pass for one visible leaf: draw its lines with animated
 * textures (4 frames, switching every 256 ms) and mark them seen. */
void Render_drawLeafLines(Render *render, Node *leaf)
{
	int automap = g_app->game->automapFlag;
	int count = (int)(leaf->args2 >> 16);
	int i;

	for (i = 0; i < count; i++) {
		int li = (int)(leaf->args2 & 0xffff) + i;
		Line *src = &render->lines[li];
		int t = render->frameTime;

		render->curLineIdx = li;
		render->tmpLine.x1 = src->x1;
		render->tmpLine.y1 = src->y1;
		render->tmpLine.z1 = src->z1;
		render->tmpLine.x2 = src->x2;
		render->tmpLine.y2 = src->y2;
		render->tmpLine.z2 = src->z2;
		render->tmpLine.flags = src->flags;
		render->tmpLine.texture = (int16_t)(render->wallRemap[src->texture] +
		                                    (((uint32_t)(t + src->texture * 3) << 22) >> 30));
		render->tmpLine.texture2 = (int16_t)(render->wallRemap[src->texture2] +
		                                     (((uint32_t)(t + src->texture2 * 3) << 22) >> 30));
		render->shade = 0;
		render->effectFlags = 0;
		if (Render_drawLine(render, &render->tmpLine) == 0 && !Hw_freeView()) {
			/* something was drawn: reveal it on the automap (port: unless a free camera saw it) */
			if ((automap == 0 && !(src->flags & 0x20)) || (src->flags & 8))
				src->flags |= 0x80;
		}
		if (!Hw_freeView())
			src->flags |= 0x800000;
	}
}

/* 0x00031c40 Walk the BSP, then draw the queued leaves' walls and the
 * sorted sprites. */
void Render_renderScene(Render *render)
{
	Node *leaf;
	Sprite *s;

	render->cntNodes = 0;
	render->cntLeaves = 0;
	render->cntLines = 0;
	render->cntWalls = 0;
	render->cnt1884c[0] = 0;
	render->cnt1884c[1] = 0;
	render->cnt1884c[2] = 0;
	render->cntSpriteWalls = 0;
	render->visibleSprites = NULL;
	render->leafSentinel.visNext = &render->leafSentinel;
	render->leafSentinel.visPrev = &render->leafSentinel;

	Render_walkBSP(render, 0);

	if (render->dbgSkipWalls == 0)
		for (leaf = render->leafSentinel.visNext; leaf != &render->leafSentinel; leaf = leaf->visNext)
			Render_drawLeafLines(render, leaf);
	if (render->dbgSkipSprites2 == 0) {
		for (s = render->visibleSprites; s != NULL; s = s->sortNext) {
			render->cnt1884c[0]++;
			Render_drawSprite(render, s);
		}
	}

	if (g_app->statPeakSprites < render->cnt1884c[0])
		g_app->statPeakSprites = render->cnt1884c[0];
	if (g_app->statPeakWalls < render->cntLines)
		g_app->statPeakWalls = render->cntLines;
	if (g_app->statPeak518 < render->cntNodes)
		g_app->statPeak518 = render->cntNodes;
}

/* ======================================================================
 * Frame
 * ==================================================================== */

/* Advance a LERPFLAT height animation (floor or ceiling). */
static void lerp_flat(Render *render, FlatLerp *lerp, int16_t *height)
{
	int now, elapsed;

	if (lerp->duration == 0)
		return;
	now = App_getUpTimeMS();
	elapsed = now - lerp->startTime;
	if (lerp->duration <= elapsed) {
		*height = (int16_t)lerp->to;
		lerp->duration = 0;
		return;
	}
	{
		int t = rshl(elapsed, 16) / rshl(lerp->duration, 8);
		*height = (int16_t)(lerp->from + (rmul(t, rshl(lerp->to - lerp->from, 8)) >> 16));
	}
	/* the bob oscillator restarts from the end of the lerp */
	render->bobTime[0] = now;
	render->bobTime[1] = now >> 31;
}

/* 0x000345bc Render one frame of the 3D view from (viewX, viewY, viewZ)
 * looking at `angle` (10-bit): camera shake, view matrix, floor/ceiling
 * height animation, idle bob, drunk wobble, then flats and the scene. */
void Render_render(Render *render, int viewX, int viewY, int viewZ, int angle)
{
	Applet *app = g_app;
	int now, x, y, z, cosA, sinA;
	int fx, fy, fz, freeView;

	Render_syncScreenDib(render);

	/* port: or from the engine's free camera (oe_idtech3freecam), as it is */
	freeView = Hw_queryFreeView(&fx, &fy, &fz, &angle);

	render->viewSin = render->sinTable[angle & 0x3ff];
	render->viewCos = render->sinTable[(angle + 0x100) & 0x3ff];
	now = App_getUpTimeMS();
	render->frameTime = now;
	render->time98 = now;
	render->frameCount++;

	/* the eye sits 16 units behind the player position */
	x = viewX - (rshl(render->viewCos, 4) >> 16);
	y = viewY + (rshl(render->viewSin, 4) >> 16);
	z = viewZ;
	render->shakeX = 0;
	render->shakeY = 0;
	if (now < render->shakeEndTime) {
		int intensity = render->shakeIntensity;
		render->shakeX = Random_nextInt(&app->random) % (intensity * 2) - intensity;
		intensity = render->shakeIntensity;
		render->shakeY = Random_nextInt(&app->random) % (intensity * 2) - intensity;
		y -= rmul(render->sinTable[(angle + 0x100) & 0x3ff], render->shakeX) >> 16;
		z += render->shakeY;
		x += rmul(render->sinTable[(angle + 0x200) & 0x3ff], render->shakeX) >> 16;
	}
	if (freeView) {
		x = fx;
		y = fy;
		z = fz;
	}
	render->viewX = x;
	render->viewY = y;
	render->viewZ = z;
	render->viewAngle = angle;

	cosA = render->viewCos;
	sinA = render->viewSin;
	render->viewPlane[0] = cosA;
	render->viewPlane[1] = -sinA;
	render->viewPlane[2] = -(rmul(x, cosA) + rmul(-sinA, y));
	render->viewPlane[3] = sinA;
	render->viewPlane[4] = cosA;
	render->viewPlane[5] = -(rmul(x, sinA) + rmul(cosA, y));

	render->framebuffer = (uint16_t *)(render->fbDib->pBmp + rmul(render->fbDib->nPitch, render->fbOffsetY) +
	                                   render->fbOffsetX * 2);
	render->clipX1 = 0;
	render->clipX2 = render->width;
	render->clipY1 = 0;
	render->clipY2 = render->height;

	lerp_flat(render, &render->flatLerps[0], &render->floorHeight);
	lerp_flat(render, &render->flatLerps[1], &render->ceilHeight);

	render->effFloor = render->floorHeight;
	render->bobOffset = 0;
	if (render->flatLerps[0].duration == 0 && render->bobEnable != 0) {
		int t = App_getUpTimeMS();
		int64_t dt = (int64_t)t - (((int64_t)render->bobTime[1] << 32) | (uint32_t)render->bobTime[0]);
		int bob = Render_sinWave(render, (int32_t)dt, (int32_t)(dt >> 32), 0x20000, 0x5555, 0);
		render->bobOffset = bob;
		if (bob > 3)
			render->bobOffset = 3;
		else if (bob < -3)
			render->bobOffset = -3;
		render->effFloor = (render->effFloor + render->bobOffset) & 0xff;
	}
	if (render->viewZ < render->effFloor)
		render->viewZ = render->effFloor;
	render->effCeil = render->ceilHeight;

	/* drunk wobble: ramps one step per frame toward the target */
	if (app->player->equippedRing == 8 && render->drunkTarget < 0x28)
		render->drunkTarget = 0x28;
	if (render->drunkLevel < render->drunkTarget)
		render->drunkLevel++;
	else if (render->drunkLevel > render->drunkTarget)
		render->drunkLevel--;
	if (render->drunkLevel > 0x4b)
		render->drunkLevel = 0x4b;

	if (app->player->isFamiliar != 0 || app->canvas->state == 0x11 || app->menuSystem->menu == 1) {
		render->drunkLevel = 0;
	} else if (render->drunkLevel != 0) {
		int t = App_getUpTimeMS() - render->oscTime;
		int tHi = t >> 31;
		int amp = (int32_t)(rlsl64((int64_t)(int16_t)render->drunkLevel, 32) / 0xa0000);
		render->oscOut[0] = Render_sinWave(render, t, tHi, amp, 0x5555, 0xc);
		render->oscOut[1] = Render_sinWave(render, t, tHi, amp, 0x5555, 0x7f);
		render->oscOut[2] = Render_sinWave(render, t, tHi, amp, 0x5555, 0x181);
		amp /= 2;
		render->oscOut[3] = Render_sinWave(render, t, tHi, amp, 0x5555, 0);
		render->oscOut[4] = Render_sinWave(render, t, tHi, amp, 0x5555, 0x13f);
		render->effFloor = (int16_t)(render->effFloor + render->oscOut[3]);
		render->effCeil = (int16_t)(render->effCeil + render->oscOut[4]);
		if (render->effCeil > 0x80)
			render->effCeil = 0x80;
	}

	RenderHw_beginFrame(render);   /* port: the frame for the GPU as well (render_hw.c) */
	Render_clearColumnDepth(render);
	render->timeA4 = App_getUpTimeMS();
	if (render->dbgSkipFlats == 0 && !Hw_fastPass())   /* port: the GPU draws them */
		Render_drawFloorCeiling(render);
	render->time9c = App_getUpTimeMS();
	if (render->dbgClearLineFlag != 0) {
		int i;
		for (i = 0; i < render->numLines; i++)
			render->lines[i].flags &= ~0x800000u;
	}
	if (render->dbgSkipSprites == 0)
		Render_renderScene(render);
	now = App_getUpTimeMS();
	render->timeA0 = now;
	{
		int frameMs = now - render->time98;
		if (app->statWorstFrame < frameMs)
			app->statWorstFrame = (int32_t)(((uint32_t)frameMs & 0xffff) |
			                                ((uint32_t)(render->viewX >> 6) << 16) |
			                                ((uint32_t)(render->viewY >> 6) << 24));
	}
	Hw_endScene();
}
