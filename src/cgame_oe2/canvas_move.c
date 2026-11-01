/*
 * Canvas: player movement, turning and the "what am I facing" probe.
 *
 * The player walks in whole tiles (64 units). A step first traces from the
 * current view position to the target tile (Canvas_finishMovement_77f4), runs
 * the "leave tile" scripts and then sets the destination; the per-frame
 * Canvas_updateMovement interpolates the view towards it and, on arrival,
 * Canvas_finishMovement_1b1b4 runs the "enter tile" scripts and ends the turn.
 * Turning works the same way with destAngle and Canvas_finishRotation.
 */

#include "game.h"

/* Table of squared tile distances {(64*k)^2, k = 1..15}: entry 0 is "adjacent". */
#define MOD_TILE_DIST_SQ 0x45ebcu /* int32[15] */

/* Canvas states used here (Canvas.state). */
enum {
	ST_PLAYING = 3,
	ST_COMBAT = 4,
	ST_AUTOMAP = 5,
	ST_DIALOG = 7,
};

/* Entity types (EntityDef.eType) that matter for the facing probe. */
enum {
	ET_MONSTER = 2,
	ET_NPC = 3,
	ET_DOOR = 5,
	ET_ITEM = 6,
	ET_DECOR = 7,
	ET_SIGN = 10,
	ET_PICKUP = 11,
	ET_BLOCK = 12,
};

/* Game_trace type masks (bit per entity type). */
#define TRACE_MOVE_MASK   0x74bd
#define TRACE_FACING_MASK 0x5dfd

/* Script event flags for Game_executeTile. */
#define EV_ENTER_TILE  0x00000400
#define EV_FAMILIAR    0x00000800
#define EV_LEAVE_TILE  0x40000000

/* Player status effect slot that speeds up walking. */
#define SE_HASTE 3

static int tileDistSq(int k)
{
	return ModData_s32(&g_mod, MOD_TILE_DIST_SQ + (uint32_t)k * 4u);
}

static Sprite *entitySprite(Entity *ent)
{
	return &g_app->render->sprites[(ent->info & 0xffff) - 1];
}

/* Step `cur` towards `target` by `step` without overshooting. */
static int32_t approach(int32_t cur, int32_t target, int step)
{
	if (cur < target) {
		cur += step;
		if (cur > target)
			cur = target;
	} else if (cur > target) {
		cur -= step;
		if (cur < target)
			cur = target;
	}
	return cur;
}

/* 0x000077f4 Try to step from the view position to (x, y). */
void Canvas_finishMovement_77f4(Canvas *canvas, int x, int y)
{
	Applet *app = g_app;
	Game *game;
	Player *player;
	int mask, i;
	uint32_t flags;

	if (canvas->snapView) {
		canvas->destX = x;
		canvas->destY = y;
		return;
	}

	player = app->player;
	mask = player->noclip ? 0 : TRACE_MOVE_MASK;
	if (player->isFamiliar)
		mask |= 0x40;
	Game_trace(app->game, canvas->viewX, canvas->viewY, x, y, Player_getPlayerEnt(player), mask);

	/* The first traced entity the player can't walk through blocks the step. */
	game = app->game;
	game->traceEntity = NULL;
	for (i = 0; i < game->numTraceEntities; i++) {
		Entity *ent = game->traceEntities[i];
		if (!Player_canUseEntity(app->player, ent)) {
			game->traceEntity = ent;
			break;
		}
	}

	if (game->traceEntity) {
		if (canvas->moveCount == 0 && canvas->state == ST_AUTOMAP)
			Game_advanceTurn(game);
		return;
	}

	{
		Hud *hud = app->hud;
		if (hud->numMessages > 0 && (hud->msgFlags[0] & 2))
			hud->msgTime = 0;
	}
	canvas->automapFlag = 1;
	Game_setMoveDirMask(game, canvas->viewX, canvas->viewY, x, y);
	canvas->abortMove = 0;

	flags = game->moveMask | EV_ENTER_TILE;
	if (app->player->isFamiliar)
		flags |= EV_FAMILIAR;
	Game_executeTile(game, canvas->viewX >> 6, canvas->viewY >> 6, (int)(flags | EV_LEAVE_TILE), 1);

	if (app->player->noclip == 0 && canvas->abortMove) {
		/* a "leave tile" script cancelled the step */
		if (canvas->moveCount)
			canvas->moveCount = 0;
	} else {
		canvas->destX = x;
		canvas->destY = y;
		canvas->prevX = canvas->viewX;
		canvas->prevY = canvas->viewY;
		Game_relinkEntity(app->player);
	}
}

/* 0x0001b1b4 The view has reached its destination tile. */
void Canvas_finishMovement_1b1b4(Canvas *canvas)
{
	Applet *app = g_app;
	uint32_t flags = (uint32_t)Canvas_flagForFacingDir(canvas, 0xf000000);

	if (app->player->isFamiliar)
		flags |= EV_FAMILIAR;
	Game_executeTile(app->game, canvas->destX >> 6, canvas->destY >> 6,
	                 (int)(app->game->eventFlags | flags | EV_LEAVE_TILE | EV_ENTER_TILE), 1);
	Canvas_updateFacing(canvas);
	Canvas_revealAutomap(canvas);
	Game_touchTile(app->game, canvas->destX, canvas->destY, 1);

	if (app->player->isFamiliar) {
		/* the familiar walks low; returning onto the body ends familiar mode */
		canvas->destZ = 0xf;
		if (app->player->isFamiliar && canvas->destX == canvas->savedViewX && canvas->destY == canvas->savedViewY)
			Canvas_resetPlayerPos(canvas, canvas->destX, canvas->destY);
	}

	if (canvas->moveCount > 0) {
		/* multi-step move (e.g. pushed/falling): bob the camera up each step */
		canvas->moveCount--;
		canvas->destZ += 0xc;
		if (canvas->moveCount == 0)
			canvas->destZ = 0x24;
		return;
	}
	if ((!app->player->isFamiliar || canvas->viewZ == canvas->destZ) &&
	    FreeMove_stepCostsTurn())   /* port: not with turns on a timer (freemove.c) */
		Game_advanceTurn(app->game);
}

/* 0x0001b2f8 The view has turned to destAngle: rebuild the direction vectors. */
void Canvas_finishRotation(Canvas *canvas, int execTile)
{
	Applet *app = g_app;
	const int32_t *sinTable = app->render->sinTable;
	int angle = canvas->destAngle;
	int s = sinTable[angle & 0x3ff];
	int c = sinTable[(angle + 0x100) & 0x3ff];

	canvas->viewSin = s;
	canvas->viewCos = c;
	canvas->viewStepY = (-(s * 64)) >> 16;
	canvas->viewStepX = (c * 64) >> 16;
	canvas->viewRightStepY = (-(sinTable[(angle + 0x100) & 0x3ff] * 64)) >> 16;
	canvas->viewRightStepX = (sinTable[(angle + 0x200) & 0x3ff] * 64) >> 16;

	if (execTile) {
		Hud *hud = app->hud;
		if (hud->numMessages > 0 && (hud->msgFlags[0] & 2))
			hud->msgTime = 0;
	}

	if (canvas->state != ST_COMBAT) {
		uint32_t flags = (uint32_t)Canvas_flagForFacingDir(canvas, 0xf000000);
		if (app->player->isFamiliar)
			flags |= EV_FAMILIAR;
		Game_executeTile(app->game, canvas->destX >> 6, canvas->destY >> 6,
		                 (int)(flags | EV_LEAVE_TILE | EV_ENTER_TILE), 1);
	}
	Canvas_updateFacing(canvas);
	canvas->lastMoveTime = App_getUpTimeMS();
}

/* 0x0001b710 Script event flag for the direction the player is facing. */
int Canvas_flagForFacingDir(Canvas *canvas, int mask)
{
	int angle = canvas->destAngle & 0x3ff;

	if (mask == 0xf000000) {
		if (angle == 0x000) return 0x2000000;
		if (angle == 0x100) return 0x1000000;
		if (angle == 0x200) return 0x8000000;
		if (angle == 0x300) return 0x4000000;
	} else if (mask == 0xf000) {
		if (angle == 0x000) return 0x8000;
		if (angle == 0x100) return 0x4000;
		if (angle == 0x200) return 0x2000;
		if (angle == 0x300) return 0x1000;
	}
	return 0;
}

/* 0x00034ed0 Leave familiar mode: jump back to the body at (x, y). */
void Canvas_resetPlayerPos(Canvas *canvas, int x, int y)
{
	Applet *app = g_app;

	canvas->viewAngle = canvas->savedViewAngle;
	canvas->destAngle = canvas->savedViewAngle;
	canvas->viewX = x;
	canvas->destX = x;
	canvas->viewY = y;
	canvas->destZ = 0x24;
	canvas->destY = y;
	Game_gspriteDestroy(app->game, app->player->familiarSprite);
	app->player->familiarSprite = NULL;
	Game_unlinkEntityById(app->player);
	Player_setFamiliar(app->player, 0, 0, 0);
	Canvas_finishRotation(canvas, 1);
	Game_relinkEntity(app->player);
}

/* 0x00043ae8 Find the entity in front of the player and offer context help. */
int Canvas_updateFacing(Canvas *cv)
{
	Applet *app = g_app;
	Game *game;
	Entity *facing;
	Player *player;
	int x0, y0, mask, i;

	/* trace from just behind the front edge of the tile, six steps ahead */
	x0 = cv->destX + ((cv->viewCos * 31) >> 16);
	y0 = cv->destY + ((-(cv->viewSin * 31)) >> 16);
	mask = TRACE_FACING_MASK;
	if (app->player->isFamiliar)
		mask |= mask >> 4;
	Game_trace(cv->game, x0, y0, x0 + cv->viewStepX * 6, y0 + cv->viewStepY * 6, NULL, mask);

	game = cv->game;
	facing = game->traceEntity;
	if (facing) {
		int firstType = facing->def->eType;
		/* things you can see past: prefer something more interesting behind them */
		if (firstType == ET_ITEM || firstType == ET_PICKUP || firstType == ET_BLOCK || firstType == ET_SIGN) {
			for (i = 0; i < game->numTraceEntities; i++) {
				Entity *ent = game->traceEntities[i];
				EntityDef *def = ent->def;
				int type = def->eType;

				if (type == ET_MONSTER) {
					if (def->eSubType == 2 && !(ent->info & 0x10000000))
						continue;
					if (firstType != ET_BLOCK)
						facing = ent;
					break;
				}
				if (type == ET_DOOR || type == 4 || type == 0)
					break;
				if (type == ET_DECOR) {
					if (firstType == ET_BLOCK)
						facing = ent;
					break;
				}
				if (type == ET_SIGN) {
					if (def->eSubType == 1) {
						facing = ent;
						break;
					}
				} else if (type == ET_BLOCK) {
					Sprite *spr = entitySprite(ent);
					if (spr->z == 0x20 && (spr->info & 0x40000)) {
						facing = ent;
						break;
					}
				}
			}
		}
	}

	player = app->player;
	player->facingEntity = facing;
	if (facing) {
		int distSq = Entity_distSqFrom(facing, cv->viewX, cv->viewY);
		EntityDef *def = facing->def;
		int type = def->eType;
		int helpDone = 0;

		if (type != ET_MONSTER && tileDistSq(2) < distSq) {
			/* too far away to interact with */
			app->player->facingEntity = NULL;
		} else if (tileDistSq(0) >= distSq) {
			if (type == ET_NPC) {
				Player_showHelp(app->player, 0, 0);
			} else if (type == ET_DECOR) {
				if (def->eSubType == 7) {
					Player_showHelp(app->player, 10, 0);
				} else if (def->eSubType == 1) {
					int tile = entitySprite(facing)->info & 0xff;
					if (tile != 0x8e && tile != 0x90)
						helpDone = 1;
					else
						Player_showHelp(app->player, 1, 0);
				} else {
					helpDone = 1;
				}
			} else if (type == ET_SIGN) {
				if (def->eSubType == 1)
					Player_showHelp(app->player, 3, 0);
				else if (def->eSubType != 3)
					Player_showHelp(app->player, 7, 0);
				else
					helpDone = 1;
			} else if (type == ET_DOOR) {
				if (def->eSubType == 1)
					Player_showHelp(app->player, 1, 0);
				Player_showHelp(app->player, 8, 0);
			}
		}

		if (!helpDone && facing->def->eType == ET_MONSTER && app->player->isFamiliar == 0 &&
		    (facing->monster->flags & 0x7000))
			Player_showHelp(app->player, 0x10, 0);

		facing = app->player->facingEntity;
		if (facing && facing->def->eType != 0)
			return 0;
	}

	/* nothing to interact with: remind about worn-out armour */
	player = app->player;
	if (player->inCombat == 0) {
		int armor = CombatEntity_getArmorPoints(&player->ce);
		int maxArmor = CombatEntity_getMaxArmorPoints(&app->player->ce);
		if (armor <= maxArmor / 4 && app->player->inventory[10] != 0)
			Player_showHelp(app->player, 9, 0);
	}
	return 0;
}

/* 0x00044f58 Per-frame interpolation of the view towards its destination. */
void Canvas_updateMovement(Canvas *cv, int unused1, int unused2)
{
	Applet *app = g_app;
	int posDone, angleDone;
	int destX, destY, destZ, destAngle;
	int moveStep, turnStep, zStep;

	(void)unused1;
	(void)unused2;

	if (Game_isCinematicActive(app->game)) {
		Camera_apply(app->game->activeCamera);
		ParticleSystem_update(cv->particles, 1);
		if (cv->state != ST_DIALOG || (cv->dialogStyle != 1 && cv->dialogStyle != 8)) {
			/* letterbox bars */
			Graphics_setColor(cv->graphics, (int)0xff000000);
			Graphics_fillRect(cv->graphics, cv->viewRect[0], cv->viewRect[1], cv->viewRect[2], 0x24);
			Graphics_fillRect(cv->graphics, cv->viewRect[0], cv->viewRect[1] + cv->viewRect[3] - 0x24,
			                  cv->viewRect[2], 0x24);
			Hud_drawCinematicText(app->hud);
		}
		Hud_drawSpeechBubble(app->hud, 0, 0);
		return;
	}

	/* forced multi-tile move: queue the next step once the previous one landed */
	if (cv->moveCount > 0 && cv->viewX == cv->destX && cv->viewY == cv->destY)
		Canvas_finishMovement_77f4(cv, cv->viewX + cv->moveDirX * 64, cv->viewY + cv->moveDirY * 64);

	destX = cv->destX;
	posDone = cv->viewX == destX && cv->viewY == cv->destY;
	destAngle = cv->destAngle;
	angleDone = cv->viewAngle == destAngle;

	moveStep = cv->moveStep;
	turnStep = cv->turnStep;
	zStep = (cv->animFrames + 11) / cv->animFrames;
	if (app->player->statusEffects[SE_HASTE].turns > 0) {
		moveStep += moveStep / 2;
		turnStep += moveStep / 2;
	}

	cv->viewX = approach(cv->viewX, destX, moveStep);
	destY = cv->destY;
	cv->viewY = approach(cv->viewY, destY, moveStep);
	destZ = cv->destZ;
	cv->viewZ = approach(cv->viewZ, destZ, zStep);
	cv->viewAngle = approach(cv->viewAngle, destAngle, turnStep);

	if (cv->state == ST_AUTOMAP) {
		/* the automap snaps instead of animating */
		cv->viewY = destY;
		cv->viewZ = destZ;
		cv->viewAngle = destAngle;
		cv->viewX = destX;
	}

	if (app->player->isFamiliar == 0) {
		app->player->ce.x = cv->viewX;
		app->player->ce.y = cv->viewY;
	}
	Canvas_renderView(cv, cv->viewX, cv->viewY, cv->viewAngle);

	if (app->game->arrived) {
		/* a script teleported the player: run the arrival events now */
		uint32_t flags;
		app->game->arrived = 0;
		Game_touchTile(app->game, cv->destX, cv->destY, 1);
		flags = (uint32_t)Canvas_flagForFacingDir(cv, 0xf000000);
		if (app->player->isFamiliar)
			flags |= EV_FAMILIAR;
		Game_setMoveDirMask(app->game, -1, -1, -1, -1);
		Game_executeTile(app->game, cv->destX >> 6, cv->destY >> 6,
		                 (int)(app->game->eventFlags | flags | EV_LEAVE_TILE | EV_ENTER_TILE), 1);
		Canvas_updateFacing(cv);
	} else if (!posDone && cv->viewX == cv->destX && cv->viewY == cv->destY) {
		Canvas_finishMovement_1b1b4(cv);
	}

	if (!angleDone && cv->viewAngle == cv->destAngle)
		Canvas_finishRotation(cv, 1);
}
