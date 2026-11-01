/*
 * Free movement (port; oe_moveMode, see OE_TRAP_FREE_MOVE in oe_public.h).
 *
 * The game's rules stay on the tile grid: Canvas.view/dest hold the tile the
 * player (or the familiar) stands on and one of the four directions, as in
 * the original, and everything the game does works from them. Only the view
 * moves freely: the keys walk it around the tile and the mouse turns it.
 * Crossing into another tile is the game's own step
 * (Canvas_finishMovement_77f4, then arriving at once as when a key cuts a
 * step short), so walls, doors, monsters and tile scripts stop it as they
 * did; the nearest of the four directions to where it looks is the facing one.
 *
 * OE_MOVE_STEPS: each step passes the turn, as before. OE_MOVE_TIMER: steps
 * cost nothing and the turn passes on its own every turnMs while the game
 * waits for the player.
 */

#include "game.h"
#include "oe_vm.h"

#define TICK_MS          40
#define RADIUS           24      /* how near the view comes to a wall */
#define TRACE_MOVE_MASK  0x74bd  /* Canvas_finishMovement_77f4's */
#define ST_PLAYING       3
#define SE_FROZEN        9

static int mode;                 /* OE_MOVE_* from the engine's last answer */
static int synced;               /* fx/fy/fa follow the game's position */
static int fx, fy;               /* the view, world units * 256 */
static int fa;                   /* the view's angle, 1/64 angle units (0..0xffff) */
static int lastDestX, lastDestY, lastDestAngle;
static int lastTurns, turnStart;

int FreeMove_mode(void)
{
	return mode;
}

/* entering a tile passes the turn (not with turns on a timer) */
int FreeMove_stepCostsTurn(void)
{
	return mode != OE_MOVE_TIMER;
}

/* is the step from the player's tile to the next one (tx, ty) blocked? (the trace of Canvas_finishMovement_77f4) */
static int blocked(Canvas *cv, int tx, int ty)
{
	Applet *app = g_app;
	Game *game = app->game;
	int mask = app->player->noclip ? 0 : TRACE_MOVE_MASK, i;

	if (app->player->isFamiliar)
		mask |= 0x40;
	Game_trace(game, cv->destX, cv->destY, tx * 64 + 32, ty * 64 + 32, Player_getPlayerEnt(app->player), mask);
	for (i = 0; i < game->numTraceEntities; i++) {
		if (!Player_canUseEntity(app->player, game->traceEntities[i]))
			return 1;
	}
	return 0;
}

/* the game's step to the next tile, arriving at once; 1 if the player got there */
static int stepTo(Canvas *cv, int tx, int ty)
{
	int ox = cv->destX, oy = cv->destY;

	Canvas_finishMovement_77f4(cv, tx * 64 + 32, ty * 64 + 32);
	if (cv->destX == ox && cv->destY == oy)
		return 0;
	cv->viewX = cv->destX;
	cv->viewY = cv->destY;
	Canvas_finishMovement_1b1b4(cv);
	lastDestX = cv->destX;
	lastDestY = cv->destY;
	return 1;
}

/* move the view along one axis to *pos + d (world * 256), stepping or stopping at the tile's edges */
static void moveAxis(Canvas *cv, int *pos, int d, int yAxis)
{
	int cur = (yAxis ? cv->destY : cv->destX) >> 6;
	int other = (yAxis ? cv->destX : cv->destY) >> 6;
	int p = (*pos + d) >> 8, lim;

	if (d > 0 && (p + RADIUS) >> 6 > cur) {
		int nx = yAxis ? other : cur + 1, ny = yAxis ? cur + 1 : other;
		if (blocked(cv, nx, ny)) {
			/* a wall (or a monster) ahead: up to RADIUS from it, never pushed back */
			lim = ((cur + 1) * 64 - RADIUS - 1) << 8;
			*pos = *pos + d < lim ? *pos + d : *pos > lim ? *pos : lim;
			return;
		}
		if (p >> 6 > cur && !stepTo(cv, nx, ny)) {
			/* a script kept the player here */
			lim = ((cur + 1) * 64 - 1) << 8;
			*pos = *pos + d < lim ? *pos + d : lim;
			return;
		}
	} else if (d < 0 && (p - RADIUS) >> 6 < cur) {
		int nx = yAxis ? other : cur - 1, ny = yAxis ? cur - 1 : other;
		if (blocked(cv, nx, ny)) {
			lim = (cur * 64 + RADIUS) << 8;
			*pos = *pos + d > lim ? *pos + d : *pos < lim ? *pos : lim;
			return;
		}
		if (p >> 6 < cur && !stepTo(cv, nx, ny)) {
			lim = (cur * 64) << 8;
			*pos = *pos + d > lim ? *pos + d : lim;
			return;
		}
	}
	*pos += d;
}

/* Instead of Canvas_updateMovement while exploring (canMove) or in combat (look only):
 * returns 0 when the game moves the view itself (turn-based mode, forced steps, scripted turns). */
int FreeMove_update(Canvas *cv, int canMove)
{
	Applet *app = g_app;
	oeFreeMove_t in;
	int card, ang, s, c;

	mode = trap_FreeMove(&in);
	if (mode == OE_MOVE_TURNS || cv->snapView || Game_isCinematicActive(app->game) || cv->moveCount > 0 ||
	    app->game->arrived || cv->viewX != cv->destX || cv->viewY != cv->destY || cv->viewAngle != cv->destAngle) {
		synced = 0;
		return 0;
	}
	if (!synced || cv->destX != lastDestX || cv->destY != lastDestY) {
		/* a new place (start, teleport, a step of the game's own) */
		fx = cv->destX << 8;
		fy = cv->destY << 8;
		lastDestX = cv->destX;
		lastDestY = cv->destY;
	}
	if (!synced || cv->destAngle != lastDestAngle)
		fa = (cv->destAngle & 0x3ff) << 6;
	synced = 1;

	fa = (fa + in.yaw) & 0xffff;
	ang = fa >> 6;
	if (canMove && cv->state == ST_PLAYING) {
		/* the nearest of the four directions is the one the game faces */
		card = ((ang + 0x80) & 0x3ff) & 0x300;
		if (card != (cv->destAngle & 0x3ff)) {
			cv->destAngle = cv->viewAngle = card;
			cv->automapFlag = 1;
			Canvas_finishRotation(cv, 1);
		}
		if (app->player->statusEffects[SE_FROZEN].turns == 0 && (in.forward || in.side)) {
			int step = in.speed * TICK_MS * 256 / 1000;	/* world units * 256 per tick */
			int dx, dy;
			s = cv->render->sinTable[ang & 0x3ff];
			c = cv->render->sinTable[(ang + 0x100) & 0x3ff];
			/* forward is (cos, -sin), right is (sin, cos) (y points south); keys * 256, sin/cos 16.16 */
			dx = (int)(((int64_t)in.forward * c + (int64_t)in.side * s) * step >> 24);
			dy = (int)(((int64_t)-in.forward * s + (int64_t)in.side * c) * step >> 24);
			moveAxis(cv, &fx, dx, 0);
			if (cv->state == ST_PLAYING && cv->viewX == cv->destX && cv->viewY == cv->destY)
				moveAxis(cv, &fy, dy, 1);
		}
		/* turns on a timer: the turn passes on its own while the game waits for the player */
		if (mode == OE_MOVE_TIMER && cv->state == ST_PLAYING) {
			Game *game = app->game;
			if (app->statMoves != lastTurns) {
				lastTurns = app->statMoves;
				turnStart = app->time;
			} else if (app->time - turnStart >= in.turnMs && game->monstersTurn == 0 && game->monstersMoving == 0 &&
			           game->activePropagators == 0) {
				Game_touchTile(game, cv->destX, cv->destY, 0);
				Game_advanceTurn(game);			}
		}
	}
	lastDestAngle = cv->destAngle;

	cv->viewZ += cv->viewZ < cv->destZ ? 1 : cv->viewZ > cv->destZ ? -1 : 0;
	if (app->player->isFamiliar == 0) {
		app->player->ce.x = cv->viewX;
		app->player->ce.y = cv->viewY;
	}
	Canvas_renderView(cv, fx >> 8, fy >> 8, ang);
	return 1;
}
