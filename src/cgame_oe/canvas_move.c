/*
 * Canvas movement and the exploring/combat states.
 *
 * The player moves tile by tile: a key sets Canvas.dest*, and every tick
 * Canvas_updateMovement steps the view towards it by moveStep/turnStep
 * (Canvas_setAnimFrames). Arriving runs Canvas_finishMovement (tile scripts,
 * pickups, the monsters' turn), finishing a turn runs Canvas_finishRotation.
 */

#include "game.h"

/* ---- tables and strings in the executable's data area ---- */
#define MOD_AMBIENT_DIST_TABLE 0x2d0dcu /* int[16] squared distances for the ambient monster sound volume */
#define MOD_STR_RENDER_AVG     0x2d564u
#define MOD_STR_HORIZ_AVG      0x2d54cu
#define MOD_STR_TOTAL_AVG      0x2d534u

/* Canvas states (Canvas.state). */
enum {
	CS_PLAYING = 3,
	CS_COMBAT = 4,
	CS_AUTOMAP = 5,
	CS_DIALOG = 7,
	CS_DYING = 0xd,
	CS_CINEMATIC = 0x14,
};

#define MAP_DRAGON_LAIR 0x1778
#define VIEW_EYE_Z      0x24
#define TRACE_MASK_MOVE   0x74b5
#define TRACE_MASK_FACING 0xddfd
#define COLOR_BLACK     ((int)0xff000000)
#define SE_HASTE        4 /* Player.effects[4]: steps 4/3 larger */

static char *modStr(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

/* IDISPLAY_DrawRect(..., RGB_NONE, RGB_NONE, IDF_RECT_FILL): black (the user
 * background colour set at EVT_APP_START), absolute screen coordinates. */
static void displayFillBlack(int x, int y, int w, int h)
{
	AEERect rc;
	IBitmap *screen = Brew_screen();

	if (!screen)
		return;
	rc.x = (int16_t)x;
	rc.y = (int16_t)y;
	rc.dx = (int16_t)w;
	rc.dy = (int16_t)h;
	IBITMAP_FillRect(screen, &rc, 0, AEE_RO_COPY);
}

/* 0x00005714 Try to step to (x, y); blocked moves may still pass the turn. */
void Canvas_attemptMove(Canvas *canvas, int x, int y)
{
	Game *game;
	int mask;

	if (canvas->cameraMode) {
		canvas->destX = x;
		canvas->destY = y;
		return;
	}
	mask = canvas->player->noclip ? 0 : TRACE_MASK_MOVE;
	Game_trace(canvas->game, canvas->viewX, canvas->viewY, x, y, &canvas->game->entities[1], mask);
	game = canvas->game;
	if (game->traceEntity == NULL) {
		canvas->usedThisTurn = 1;
		Game_setMoveDirMask(game, canvas->viewX, canvas->viewY, x, y);
		canvas->abortMove = 0;
		/* "leave tile" scripts may cancel the move (op 0x1c) */
		Game_executeTile(canvas->game, canvas->viewX >> 6, canvas->viewY >> 6,
		                 canvas->game->moveDirMaskA | 0x400, 1);
		if (canvas->player->noclip || !canvas->abortMove) {
			canvas->destX = x;
			canvas->destY = y;
			canvas->prevX = canvas->viewX;
			canvas->prevY = canvas->viewY;
			Game_relinkEntity(canvas->player);
		}
	} else if (canvas->pushSteps == 0) {
		/* bumping into things on the automap, or into a wererat, costs a turn */
		EntityDef *def = game->traceEntity->def;
		if (canvas->state == CS_AUTOMAP || (def->eType == 2 && def->eSubType == 10))
			Game_advanceTurn(game);
	}
}

/* 0x000124ec Arrived on a tile. */
void Canvas_finishMovement(Canvas *canvas)
{
	int flags = Canvas_getDirEventFlags(canvas, (int)0xf0000000);

	Game_executeTile(canvas->game, canvas->destX >> 6, canvas->destY >> 6,
	                 flags | (int)canvas->game->moveDirMaskB | 0x400, 1);
	Canvas_updateFacing(canvas);
	Canvas_revealAutomap(canvas);
	Game_touchTile(canvas->game, canvas->destX, canvas->destY, 1);
	if (canvas->pushSteps < 1) {
		if (FreeMove_stepCostsTurn())   /* port: not with turns on a timer */
			Game_advanceTurn(canvas->game);
	} else {
		/* knocked back: hop one tile per step, no turn in between */
		canvas->pushSteps--;
		canvas->destZ += 0xc;
		if (canvas->pushSteps == 0)
			canvas->destZ = VIEW_EYE_Z;
	}
	canvas->fedd = 0;
}

/* 0x00012578 Finished turning: new direction vectors, facing scripts / spin attack. */
void Canvas_finishRotation(Canvas *canvas)
{
	Render *render = canvas->render;
	int angle = canvas->destAngle;
	int s = render->sinTable[angle & 0x3ff];
	int c = render->sinTable[(angle + 0x100) & 0x3ff];

	canvas->viewSin = s;
	canvas->viewCos = c;
	canvas->stepX = (c * 64) >> 16;
	canvas->stepY = (s * -0x40) >> 16;
	c = render->sinTable[(angle + 0x100) & 0x3ff];
	canvas->rightX = (render->sinTable[(angle + 0x200) & 0x3ff] * 64) >> 16;
	canvas->rightY = (c * -0x40) >> 16;

	if (canvas->state == CS_COMBAT) {
		/* weapon 10's spin attack: each quarter turn that faces a monster strikes again */
		int *charge;
		Canvas_updateFacing(canvas);
		charge = &canvas->app->combat->chargeLevel;
		if (*charge > 0) {
			Entity *target;
			(*charge)--;
			target = canvas->player->facing;
			if (target && target->def->eType == 2 && *charge != 0)
				Combat_start(canvas->combat, NULL, target, 0, 0);
		}
	} else {
		int flags = Canvas_getDirEventFlags(canvas, (int)0xf0000000);
		Game_executeTile(canvas->game, canvas->destX >> 6, canvas->destY >> 6, flags | 0x400, 1);
		Canvas_updateFacing(canvas);
	}
	canvas->lastTurnTime = (int32_t)GETUPTIMEMS();
}

/* Step `cur` towards `dest` by `step`, without overshooting. */
static int32_t approach(int32_t cur, int32_t dest, int step)
{
	if (cur < dest) {
		cur += step;
		if (cur > dest)
			cur = dest;
	} else if (cur > dest) {
		cur -= step;
		if (cur < dest)
			cur = dest;
	}
	return cur;
}

/* 0x0002bc08 Advance the view one tick towards its destination and render it. */
void Canvas_updateMovement(Canvas *cv)
{
	int wasStill, oldAngle, destAngle, moveStep, turnStep, zStep;
	int32_t destX, destY, destZ;

	if (Game_isCinematicActive(cv->game)) {
		/* scripted camera with letterbox bars */
		Camera_apply(cv->game->activeCamera);
		ParticleSystem_update(cv->particles, 1);
		if (cv->state != CS_DIALOG || cv->dialogStyle != 1) {
			int bar;
			Gfx_setColor(cv->app, 0);
			Gfx_fillRect(cv->app, cv->originX, cv->originY, cv->viewW, cv->hud->topBarH * 2 - 4);
			bar = cv->hud->topBarH;
			Gfx_fillRect(cv->app, cv->originX, cv->originY + cv->viewH - bar * 2 + 4, cv->viewW, bar * 2 - 4);
			Hud_drawCinematicText(cv->app->hud);
		}
		Hud_drawSpeechBubble(cv->app->hud);
		return;
	}

	/* knockback continues tile by tile */
	if (cv->pushSteps > 0 && cv->viewX == cv->destX && cv->viewY == cv->destY)
		Canvas_attemptMove(cv, cv->viewX + cv->pushDx * 0x40, cv->viewY + cv->pushDy * 0x40);

	destX = cv->destX;
	wasStill = cv->viewX == destX && cv->viewY == cv->destY;
	oldAngle = cv->viewAngle;
	destAngle = cv->destAngle;
	moveStep = cv->moveStep;
	turnStep = cv->turnStep;
	zStep = (cv->animFrames + 0xb) / cv->animFrames;
	if (cv->app->player->effects[SE_HASTE].turns != 0) {
		moveStep += moveStep / 3;
		turnStep += turnStep / 3;
	}

	cv->viewX = approach(cv->viewX, destX, moveStep);
	destY = cv->destY;
	cv->viewY = approach(cv->viewY, destY, moveStep);
	destZ = cv->destZ;
	cv->viewZ = approach(cv->viewZ, destZ, zStep);
	if (oldAngle < destAngle) {
		cv->viewAngle = oldAngle + turnStep;
		if (cv->viewAngle > destAngle)
			cv->viewAngle = destAngle;
	} else if (oldAngle > destAngle) {
		cv->viewAngle = oldAngle - turnStep;
		if (cv->viewAngle < destAngle)
			cv->viewAngle = destAngle;
	}
	if (cv->state == CS_AUTOMAP) {
		/* the automap does not animate */
		cv->viewY = destY;
		cv->viewZ = destZ;
		cv->viewAngle = destAngle;
		cv->viewX = destX;
	}
	cv->player->ce.x = cv->viewX;
	cv->player->ce.y = cv->viewY;
	Canvas_renderView(cv, cv->viewX, cv->viewY, cv->viewAngle);

	if (!wasStill && cv->viewX == cv->destX && cv->viewY == cv->destY)
		Canvas_finishMovement(cv);
	if (oldAngle != destAngle && cv->viewAngle == cv->destAngle)
		Canvas_finishRotation(cv);
}

/* "%d.%02dms" average of `sum` over the benchmark frames. */
static void drawAverage(Canvas *canvas, uint32_t fmt, int sum, int y)
{
	int frames = canvas->benchFrames;
	int hundredths = sum * 100 / frames;
	int ms = sum / frames;

	App_snprintf(canvas->printMsg, 0x80, modStr(fmt), ms, hundredths - ms * 100);
	Canvas_drawString(canvas, canvas->printMsg, 0, y, 0, 0);
}

/* 0x00020678 Free camera (Canvas.cameraMode): step without game logic; render benchmark. */
void Canvas_updateCamera(Canvas *canvas)
{
	int angle = canvas->viewAngle;

	if (canvas->benchmark) {
		canvas->viewAngle = (angle + canvas->turnStep) & 0x3ff;
	} else {
		if (canvas->viewX == canvas->destX && canvas->viewY == canvas->destY && angle == canvas->destAngle)
			return;
		canvas->viewX = approach(canvas->viewX, canvas->destX, canvas->moveStep);
		canvas->viewY = approach(canvas->viewY, canvas->destY, canvas->moveStep);
		canvas->viewZ = approach(canvas->viewZ, canvas->destZ, 1);
		if (angle < canvas->destAngle) {
			angle += canvas->turnStep;
			canvas->viewAngle = angle;
			if (angle > canvas->destAngle)
				canvas->viewAngle = canvas->destAngle;
		} else if (angle > canvas->destAngle) {
			angle -= canvas->turnStep;
			canvas->viewAngle = angle;
			if (angle < canvas->destAngle)
				canvas->viewAngle = canvas->destAngle;
		}
	}

	canvas->lastRenderTime = canvas->app->tickTime;
	Render_render(canvas->render, canvas->viewX, canvas->viewY, canvas->viewZ, canvas->viewAngle);
	Canvas_updateShake(canvas);
	if (canvas->benchmark) {
		int y = canvas->app->hud->topBarH;
		drawAverage(canvas, MOD_STR_RENDER_AVG, canvas->benchRender, y);
		drawAverage(canvas, MOD_STR_HORIZ_AVG, canvas->benchHoriz, canvas->app->hud->topBarH + 0xc);
		drawAverage(canvas, MOD_STR_TOTAL_AVG, canvas->benchTotal, canvas->app->hud->topBarH + 0x18);
	}
	canvas->tickEnd = (int32_t)GETUPTIMEMS();
}

/* 0x000208a4 Render the 3D view and the weapon (bobbed by the shake). */
void Canvas_renderView(Canvas *canvas, int x, int y, int angle)
{
	canvas->lastRenderTime = canvas->app->tickTime;
	canvas->renderStart = (int32_t)GETUPTIMEMS();
	Render_render(canvas->render, x, y, canvas->viewZ, angle);
	Combat_drawWeapon(canvas->combat, canvas->shakeX, canvas->shakeY);
	canvas->renderEnd = (int32_t)GETUPTIMEMS();
}

/* Ambient monster sound: a random active monster growls, quieter with distance. */
static void playAmbientSound(Canvas *canvas)
{
	Entity *ent = canvas->game->activeMonsters;
	int k = 0, count = 0, r, sound;

	/* walk the active ring once to count it, then back up a random amount */
	do {
		k++;
		ent = ent->monster->nextOnList;
		if (canvas->game->activeMonsters == ent) {
			count = k;
			k = -Random_nextByte(&canvas->app->random) % k;
		}
	} while (k != 0);

	r = Random_nextByte(&canvas->app->random) * 0x40 - count * 1000;
	canvas->nextAmbientTime = r < 0 ? 0 : r;
	canvas->nextAmbientTime += canvas->app->gameTime + 6000;

	sound = EntityMonster_getSound(ent->monster, 0);
	if (sound != 0) {
		int dist = Entity_distSqFrom(ent, canvas->viewX, canvas->viewY);
		int volume = 0xff, i;
		Sound *snd;
		for (i = 15; i >= 0; i--) {
			if (ModData_s32(&g_mod, MOD_AMBIENT_DIST_TABLE + (uint32_t)i * 4) <= dist) {
				volume = ((16 - i) << 16) / 4096;
				break;
			}
		}
		snd = canvas->app->sound;
		Sound_play(snd, sound, 0, 1, (volume * (snd->volume << 8)) >> 16);
	}
}

/* 0x0001f068 States 3 and 10: one exploring tick. */
void Canvas_updatePlaying(Canvas *canvas)
{
	Game *game;
	Sprite *hub;

	if (canvas->cameraMode) {
		Canvas_updateCamera(canvas);
		return;
	}

	/* status effect 0 (paralysis?): the turn passes on its own every 1.5 s */
	if (canvas->player->effects[0].turns != 0 && canvas->app->tickTime - canvas->game->phaseTime > 1500) {
		Game_touchTile(canvas->game, canvas->destX, canvas->destY, 0);
		Game_advanceTurn(canvas->game);
	}
	if (CombatEntity_getHealth(&canvas->player->ce) < 1) {
		Player_died(canvas->player);
		return;
	}

	game = canvas->game;
	if (game->numMovingGSprites == 0 && game->monsterTurnMode != 0 && Game_isIdle(game)) {
		/* the monsters' turn */
		Game_snapMovingWalls(canvas->game, 0);
		Game_endPlayerTurn(canvas->game);
	} else {
		/* the hub's special monster (wererat) comes back on its own timer */
		hub = canvas->app->render->hubSprite;
		if (hub && canvas->hubEventTime < canvas->app->tickTime) {
			Entity_updateWererat(hub->ent, 1);
			Game_endPlayerTurn(canvas->game);
			Canvas_updateFacing(canvas);
		}
	}
	Player_updateWandChat(canvas->player);
	Game_updateLerpSprites(canvas->game);
	if (canvas->viewX != canvas->destX || canvas->viewY != canvas->destY || canvas->viewAngle != canvas->destAngle)
		ParticleSystem_freeAll(canvas->particles);
	if (!FreeMove_update(canvas, 1))   /* port: free movement (freemove.c) */
		Canvas_updateMovement(canvas);
	Canvas_updateShake(canvas);
	ParticleSystem_update(canvas->particles, 1);

	/* the wand's idle chatter, once a minute */
	if (canvas->app->gameTime - canvas->hud->nextBarkTime > 60000) {
		canvas->hud->nextBarkTime = canvas->app->gameTime + 60000;
		Hud_randomBark(canvas->hud, 10, 0);
	}

	if (canvas->swallowKey) {
		/* controls help over a black view */
		Gfx_setColor(canvas->app, COLOR_BLACK);
		displayFillBlack(0, 0, canvas->viewW, canvas->viewH);
		Hud_draw(canvas->hud);
		Hud_drawControlsHelp(canvas->hud);
	} else if (!Game_isCinematicActive(canvas->game)) {
		Hud_draw(canvas->hud);
	}

	if (canvas->game->activeMonsters != NULL && canvas->nextAmbientTime < canvas->app->gameTime)
		playAmbientSound(canvas);

	/* random map event (tile 1,0 scripts) every 6..25.5 s */
	if (canvas->nextRandomEvent < canvas->app->gameTime) {
		int t = Random_nextByte(&canvas->app->random) * 100;
		canvas->nextRandomEvent = t < 6000 ? 6000 : t;
		canvas->nextRandomEvent += canvas->app->gameTime;
		Game_executeTile(canvas->game, 1, 0, 0xf000, 0);
	}

	if (!Game_isCinematicActive(canvas->game))
		Canvas_dequeueHelpDialog(canvas, 0);
}

/* 0x0002b214 What is the player looking at (Player.facing), plus context help. */
void Canvas_updateFacing(Canvas *cv)
{
	Game *game;
	Entity *first, *facing;
	Player *player;
	int x0, y0, i, helpId;

	x0 = cv->destX + ((cv->viewCos * 0x1f) >> 16);
	y0 = cv->destY + ((cv->viewSin * -0x1f) >> 16);
	Game_trace(cv->game, x0, y0, cv->stepX * 6 + x0, cv->stepY * 6 + y0, NULL, TRACE_MASK_FACING);
	game = cv->game;
	first = game->traceEntity;
	facing = first;
	if (first && (first->def->eType == 6 || first->def->eType == 0xb)) {
		/* an item on the floor: a monster behind it takes precedence */
		for (i = 0; i < game->numTraceHits; i++) {
			Entity *hit = game->traceHits[i];
			int type = hit->def->eType;
			if (type == 2) {
				if (hit->def->eSubType != 2 || (hit->info & 0x20000000)) {
					facing = hit;
					break;
				}
			} else if (type == 5 || type == 7 || type == 4) {
				break;
			}
		}
	}
	cv->player->facing = facing;

	if (facing && facing->def->eType != 2) {
		int dist = Entity_distSqFrom(facing, cv->viewX, cv->viewY);
		EntityDef *def = facing->def;
		if (dist > 0x9000) {
			cv->player->facing = NULL;
		} else {
			helpId = -1;
			switch (def->eType) {
			case 3: /* NPC */
				if (dist <= 0x1000)
					helpId = 0;
				break;
			case 7: /* decoration: rune lock / healing crystal */
				if (def->eSubType == 1) {
					if (dist <= 0x1000)
						helpId = 3;
				} else if (def->eSubType == 4 && dist <= 0x2400) {
					helpId = 0x13;
				}
				break;
			case 0xa: /* barricade / destructible */
				if (def->eSubType == 1) {
					if (dist <= 0x1000)
						helpId = 4;
				} else if (def->eSubType == 2 && dist <= 0x1000) {
					helpId = 0xb;
				}
				break;
			case 5: /* door */
				if (dist <= 0x1000)
					helpId = 0x12;
				break;
			case 0xf: /* fire portal (Dragon's Lair) */
				if (cv->mapId == MAP_DRAGON_LAIR && dist <= 0x1000) {
					int n = facing->param & 0xff; /* ARM shifts by >= 32 give 0 */
					uint32_t bit = n < 32 ? 1u << n : 0;
					helpId = (cv->app->player->firePortals & bit) ? 0xd : 0xe;
				}
				break;
			}
			if (helpId >= 0)
				Player_showHelp(cv->player, helpId, 0);
		}
	}

	/* armour running low and a kit in the bag */
	facing = cv->player->facing;
	player = cv->app->player;
	if ((facing == NULL || facing->def->eType == 0) && !player->attacked) {
		unsigned armor = (unsigned)CombatEntity_getArmorPoints(&player->ce);
		unsigned maxArmor = (unsigned)CombatEntity_getMaxArmorPoints(&cv->app->player->ce);
		if (armor <= maxArmor >> 2 && cv->app->player->items[12] != 0)
			Player_showHelp(cv->player, 0x14, 0);
	}
}

/* 0x0000733c State 4: run the combat animation, then hand the turn on. */
void Canvas_combatTurnDone(Canvas *canvas)
{
	Game_updateMonsterLerps(canvas->game);
	if (!FreeMove_update(canvas, 0))   /* port: free movement looks around */
		Canvas_updateMovement(canvas);
	Canvas_updateShake(canvas);
	ParticleSystem_update(canvas->app->particles, 1);

	if (!canvas->turnDone) {
		Combat *combat;
		Entity *attacker;

		if (Combat_runFrame(canvas->app->combat))
			goto draw;
		if (canvas->state == CS_DYING) {
			while (canvas->game->combatMonsters != NULL)
				Game_removeFromCombat(canvas->game->combatMonsters);
			return;
		}
		combat = canvas->app->combat;
		attacker = combat->attacker;
		if (attacker == NULL) {
			/* the player's attack is over (spin attack: keep turning) */
			if (combat->chargeLevel > 0) {
				if (canvas->viewAngle == canvas->destAngle)
					canvas->destAngle += 0x100;
				goto draw;
			}
			Game_touchTile(canvas->game, canvas->destX, canvas->destY, 0);
		} else {
			EntityMonster *mon = attacker->monster;
			Entity *next;
			if (mon->goal.flags & 0x08) {
				/* hit and run */
				MonsterGoal_reset(&mon->goal);
				mon->goal.type = 5;
				mon->goal.param = 1;
				Entity_aiThink(attacker);
			} else if (mon->goal.flags & 0x40) {
				/* attack then teleport */
				MonsterGoal_reset(&mon->goal);
				mon->goal.type = 8;
				Entity_aiThink(attacker);
			}
			next = attacker->monster->nextAttacker;
			if (next != NULL) {
				Combat_start(canvas->app->combat, next, next->monster->target, 0, 0);
				goto draw;
			}
			canvas->game->combatMonsters = NULL;
			if (canvas->game->monstersMoving != 0) {
				Canvas_setState(canvas, CS_PLAYING);
				goto draw;
			}
			Game_clearMonsterTurn(canvas->game);
			Canvas_drawPlayingSoftKeys(canvas);
		}
		canvas->turnDone = 1;
	} else {
		if (canvas->game->monstersMoving != 0)
			goto draw;
		if (canvas->app->combat->attacker == NULL)
			Game_advanceTurn(canvas->game);
		if (Game_isCinematicActive(canvas->game)) {
			Canvas_setState(canvas, CS_CINEMATIC);
			ScriptThread_run(canvas->game->activeCamera->thread);
		} else {
			Canvas_setState(canvas, CS_PLAYING);
		}
	}

draw:
	Game_updateLerpSprites(canvas->game);
	if (!Game_isCinematicActive(canvas->game)) {
		Hud_draw(canvas->app->hud);
		Hud_drawSpeechBubble(canvas->app->hud);
	}
}
