#include <stdio.h>
/*
 * Canvas: key handling.
 *
 * Keys arrive through Canvas_queueKey (from the applet event handler) and are
 * replayed once per tick by Canvas_runInputEvents -> Canvas_handleEvent, which
 * dispatches on the canvas state. Keys are first mapped to game "actions" by
 * Canvas_getKeyAction (the digit and soft-key tables live in the .mod).
 *
 * Also here: the use/attack logic, the lock-pick minigame, quick weapon/potion
 * selection and pick-pocketing.
 */

#include "game.h"

/* Key -> action tables in the executable. */
#define MOD_KEY_DIGIT_ACTIONS 0x46214u /* uint8[10] action for AVK_0..AVK_9 */
#define MOD_KEY_PAIR_ACTIONS  0x46220u /* 8 x {int32 avk, int32 action} */
#define KEY_PAIR_COUNT        8
/* Squared tile distances {(64*k)^2, k = 1..15}. */
#define MOD_TILE_DIST_SQ      0x45ebcu /* int32[15] */
/* Fatal error text used by Canvas_setPassword. */
#define MOD_STR_NO_PASSWORD   0x47314u /* "Never found a password" */

/* Canvas states (Canvas.state). */
enum {
	ST_LOGOS = 0,
	ST_MENU = 2,
	ST_PLAYING = 3,
	ST_COMBAT = 4,
	ST_AUTOMAP = 5,
	ST_LOADING = 6,
	ST_DIALOG = 7,
	ST_INTRO = 8,
	ST_BENCHMARK = 9,
	ST_BENCHMARK_END = 10,
	ST_TRAVELMAP = 0xb,
	ST_DYING = 0xc,
	ST_ENDING = 0xd,
	ST_CREDITS = 0xe,
	ST_SAVING = 0xf,
	ST_ERROR = 0x10,
	ST_CINEMATIC = 0x11,
	ST_FADE = 0x12,
	ST_LOCKPICK = 0x13,
	ST_QUICK_WEAPON = 0x14,
	ST_QUICK_ITEM = 0x15,
};

/* Actions returned by Canvas_getKeyAction. */
enum {
	ACT_NONE = 0,
	ACT_FORWARD = 1,
	ACT_BACK = 2,
	ACT_TURN_LEFT = 3,
	ACT_TURN_RIGHT = 4,
	ACT_MENU = 5,
	ACT_USE = 6,
	ACT_AUTOMAP = 7,
	ACT_STRAFE_LEFT = 9,
	ACT_STRAFE_RIGHT = 10,
	ACT_PREV_WEAPON = 11,
	ACT_NEXT_WEAPON = 12,
	ACT_PASS = 14,
	ACT_CLEAR = 15,
};

/* Entity types (EntityDef.eType). */
enum {
	ET_WORLD = 0,
	ET_MONSTER = 2,
	ET_NPC = 3,
	ET_DOOR = 5,
	ET_DECOR = 7,
	ET_CORPSE = 8,
	ET_DESTRUCTIBLE = 9,
	ET_SIGN = 10,
	ET_BLOCK = 12,
	ET_SPIDERWEB = 13,
};

/* Script event flags for Game_executeTile. */
#define EV_ENTER_TILE  0x00000400
#define EV_FAMILIAR    0x00000800
#define EV_LEAVE_TILE  0x40000000
#define EV_SPECIAL     0xf000

/* Game_trace type mask used when attacking. */
#define TRACE_ATTACK_MASK 0x76bd

/* Player status effects. */
#define SE_TINT    4
#define SE_FROZEN  9

static int tileDistSq(int k)
{
	return ModData_s32(&g_mod, MOD_TILE_DIST_SQ + (uint32_t)k * 4u);
}

static Sprite *entitySprite(Entity *ent)
{
	return &g_app->render->sprites[(ent->info & 0xffff) - 1];
}

static int viewAtDest(Canvas *canvas)
{
	return canvas->viewX == canvas->destX && canvas->viewY == canvas->destY &&
	       canvas->viewAngle == canvas->destAngle;
}

/* Show the name of the newly selected weapon. */
static void announceWeapon(Canvas *canvas, int weapon)
{
	EntityDef *def = EntityDefs_find(canvas->entityDefs, 6, 1, weapon);
	Hud_addMessageStrFlags(g_app->hud, weapon == 4 ? &def->name : &def->longName, 1);
}

/* 0x0001bce8 Map a BREW key code to a game action. */
int Canvas_getKeyAction(Canvas *canvas, int avk)
{
	int i;

	if ((unsigned)(avk - AVK_0) <= 9)
		return ModData_u8(&g_mod, MOD_KEY_DIGIT_ACTIONS + (uint32_t)(avk - AVK_0));
	if (avk == AVK_STAR)
		return ACT_PREV_WEAPON;
	if (avk == AVK_POUND && (canvas->state == ST_PLAYING || canvas->state == ST_AUTOMAP))
		return ACT_AUTOMAP;
	if (avk == AVK_LEFT)
		return ACT_TURN_LEFT;
	if (avk == AVK_RIGHT)
		return ACT_TURN_RIGHT;
	if (avk == AVK_UP)
		return ACT_FORWARD;
	if (avk == AVK_DOWN)
		return ACT_BACK;
	if (avk == AVK_SELECT)
		return ACT_USE;
	for (i = 0; i < KEY_PAIR_COUNT; i++) {
		uint32_t entry = MOD_KEY_PAIR_ACTIONS + (uint32_t)i * 8u;
		if (ModData_s32(&g_mod, entry) == avk)
			return ModData_s32(&g_mod, entry + 4);
	}
	return ACT_NONE;
}

/* 0x0001d524 Dispatch one key by canvas state. Returns 0 to keep the key queued. */
int Canvas_handleEvent(Canvas *canvas, int avk)
{
	Applet *app = g_app;
	int action;

	if (avk == AVK_SEND)
		return 1;
	if (getenv("OE2_KEYLOG")) /* developer aid: trace keys with state and action */
		fprintf(stderr, "key %x state %d action %d moveCount %d\n", avk, canvas->state, Canvas_getKeyAction(canvas, avk), canvas->moveCount);

	if (app->sndSys->enabled) {
		if (avk == AVK_VOLUME_UP)
			Sound_volumeUp(app->sndSys, 10);
		else if (avk == AVK_VOLUME_DOWN)
			Sound_volumeDown(app->sndSys, 10);
	}

	/* any key stops the benchmark */
	if (canvas->profiling) {
		canvas->profiling = 0;
		canvas->snapView = 0;
		app->menuSystem->animPeriod = 0x28;
	}

	switch (canvas->state) {
	case ST_LOGOS:
		action = Canvas_getKeyAction(canvas, avk);
		if (action == ACT_CLEAR)
			Applet_shutdown(app);
		else if (action == ACT_USE)
			canvas->f378 = 1;
		break;

	case ST_MENU:
		Canvas_handleMenuEvents(canvas, avk);
		break;

	case ST_PLAYING:
		return Canvas_handlePlayingEvents(canvas, avk, 0);

	case ST_COMBAT:
		/* the player may turn while a monster attacks */
		if (app->player->statusEffects[SE_FROZEN].turns == 0 && app->combat->attacker) {
			action = Canvas_getKeyAction(canvas, avk);
			if (action == ACT_TURN_RIGHT)
				canvas->destAngle -= 0x100;
			else if (action == ACT_TURN_LEFT)
				canvas->destAngle += 0x100;
		}
		if (canvas->combatDone && app->game->monstersMoving == 0) {
			ParticleSystem_freeAll(canvas->particles);
			Canvas_setState(canvas, ST_PLAYING);
			if (app->combat->attacker == NULL) {
				Game_advanceTurn(app->game);
				if (canvas->state == ST_PLAYING)
					return Canvas_handlePlayingEvents(canvas, avk, 0);
			}
		}
		break;

	case ST_AUTOMAP:
		canvas->automapFirstFrame = 0;
		return Canvas_handlePlayingEvents(canvas, avk, 0);

	case ST_DIALOG:
		Canvas_handleDialogEvents(canvas, avk);
		break;

	case ST_INTRO:
	case ST_ENDING:
		action = Canvas_getKeyAction(canvas, avk);
		if (action == ACT_USE || action == ACT_TURN_RIGHT || avk == AVK_SOFT2) {
			Canvas_fadeStep(canvas, 1);
		} else if (action == ACT_TURN_LEFT || avk == AVK_SOFT1) {
			Canvas_fadeStep(canvas, -1);
		} else if (action == ACT_CLEAR) {
			if (canvas->state == ST_INTRO && canvas->storyPage == 0) {
				Canvas_backToMain(canvas, 0);
				MenuSystem_setMenu(canvas->menuSystem, 0x10);
			} else {
				Canvas_fadeStep(canvas, -1);
			}
		}
		break;

	case ST_BENCHMARK:
	case ST_BENCHMARK_END:
		Canvas_setState(canvas, ST_PLAYING);
		Canvas_setAnimFrames(canvas, canvas->animFrames);
		break;

	case ST_TRAVELMAP: {
		int pathLen = Math_abs(canvas->travelTo - canvas->drawLoading);
		int start = canvas->travelStartTime;
		if ((app->time - start) / 200 <= pathLen) {
			/* first key press finishes the path animation */
			canvas->travelStartTime = start - 10000;
			break;
		}
		action = Canvas_getKeyAction(canvas, avk);
		if (action == ACT_USE || avk == AVK_SOFT2 || action == ACT_TURN_RIGHT || action == ACT_CLEAR) {
			Canvas_freeIntroImages(canvas);
			String_setLength(&canvas->scratch, 0);
			Text_getString(canvas, 0, 0x2f, &canvas->scratch);
			TextUtil_dehyphenate(app->textUtil, &canvas->scratch);
			Canvas_setState(canvas, ST_LOADING);
		}
		break;
	}

	case ST_DYING:
		action = Canvas_getKeyAction(canvas, avk);
		if (canvas->drawLoading > 0 && (action == ACT_USE || action == ACT_CLEAR))
			MenuSystem_setMenu(app->menuSystem, 0x74);
		break;

	case ST_CREDITS:
		action = Canvas_getKeyAction(canvas, avk);
		if (canvas->endGame) {
			if (action != ACT_CLEAR) {
				if (action != ACT_USE)
					break;
				/* USE only skips once every line has scrolled in */
				if ((app->time - canvas->scrollStartTime) / 750 <= canvas->scrollLines)
					break;
			}
			canvas->endGame = 0;
			Sound_requestStopAll(app->sndSys);
			MenuSystem_setMenu(app->menuSystem, 0x15);
		} else if (action == ACT_USE || action == ACT_CLEAR) {
			if (canvas->mapId == 0) {
				MenuSystem_setMenu(app->menuSystem, 4);
			} else {
				Sound_requestStopAll(app->sndSys);
				MenuSystem_setMenu(app->menuSystem, 0x67);
			}
		}
		break;

	case ST_ERROR:
		action = Canvas_getKeyAction(canvas, avk);
		if (action == ACT_CLEAR || action == ACT_USE)
			Applet_shutdown(app);
		break;

	case ST_CINEMATIC:
		action = Canvas_getKeyAction(canvas, avk);
		if (app->time > app->game->skipUntilTime &&
		    (action == ACT_USE || action == ACT_PASS || action == ACT_CLEAR || avk == AVK_SOFT2))
			Game_skipCinematic(app->game);
		break;

	case ST_FADE:
		if (!(canvas->fadeFlags & 4) && !(canvas->fadeFlags & 8)) {
			canvas->fadeStart = 0;
			if (canvas->fadeFlags & 1) {
				Canvas_setState(canvas, ST_PLAYING);
				return Canvas_handlePlayingEvents(canvas, avk, 0);
			}
		}
		break;

	case ST_LOCKPICK:
		Canvas_handleLockpickEvents(canvas, avk);
		break;

	case ST_QUICK_WEAPON:
	case ST_QUICK_ITEM:
		Canvas_handleQuickSelectEvents(canvas, avk);
		break;

	default: /* 1 (idle), 6 loading, 0xf saving, 0x16 */
		break;
	}
	return 1;
}

/* 0x0001d9ec Lock-picking: guess the lock's digit sequence with the d-pad. */
void Canvas_handleLockpickEvents(Canvas *canvas, int avk)
{
	Applet *app = g_app;
	int action = Canvas_getKeyAction(canvas, avk);
	int codeLen;

	if (canvas->lockDone)
		return;

	codeLen = TextUtil_strlenZ(app->textUtil, &canvas->game->lockCombination);
	if ((unsigned)(action - 1) <= 3) {
		/* lockpickTries: low 16 bits correct guesses, high 16 bits total tries */
		uint32_t tries = (uint32_t)app->player->lockpickTries;
		uint32_t correct = tries & 0xffff;
		uint32_t total = (uint32_t)((app->player->lockpickTries >> 16) + 1);
		String *entered = &canvas->lockEntered;

		String_setCharAt(entered, canvas->lockSlot, action & 0xff);
		if (String_length(entered) - 1 > canvas->lockSlot)
			String_setLength(entered, canvas->lockSlot + 1);

		if (String_charAt(entered, canvas->lockSlot) ==
		    String_charAt(&canvas->game->lockCombination, canvas->lockSlot)) {
			canvas->lockCorrectHits++;
			correct++;
			Sound_play(app->sndSys, 0x10, 0, 3, -1);
			/* each digit has to be hit twice */
			if (canvas->lockCorrectHits == 2) {
				canvas->lockCorrectHits = 0;
				canvas->lockSlot++;
			}
		} else {
			canvas->lockCorrectHits = 0;
			Sound_play(app->sndSys, 0xf, 0, 3, -1);
		}
		if (canvas->lockSlot == codeLen)
			canvas->lockDone = 1;
		app->player->lockpickTries = (int32_t)(correct | (total << 16));
		Game_snapAllMonsterSprites(app->game, 1);
		Game_advanceTurn(app->game);
	} else if (action == ACT_CLEAR || action == ACT_USE || action == ACT_MENU) {
		Hud_addMessageFlags(app->hud, 0x36, 1);
		if (canvas->state == ST_LOCKPICK) {
			Canvas_clearSoftKeys(canvas);
			Canvas_setState(canvas, ST_PLAYING);
		}
	}
}

/* 0x0001dbb8 Keys while a menu is open. */
int Canvas_handleMenuEvents(Canvas *canvas, int avk)
{
	Applet *app = g_app;
	MenuSystem *ms = app->menuSystem;
	int action;

	if (ms->menu == 0x135 || ms->menu == 0x97) {
		MenuSystem_handleNumberEntry(ms, avk);
		return 1;
	}
	if (ms->sliderEditing) {
		MenuSystem_handleVolumeKey(ms, avk);
		return 1;
	}
	if ((unsigned)(avk - AVK_0) <= 9) {
		int r = MenuSystem_enterDigit(ms, avk - AVK_0);
		if (r)
			return r;
	}

	action = Canvas_getKeyAction(canvas, avk);
	switch (action) {
	case ACT_FORWARD:
		MenuSystem_scrollUp(app->menuSystem);
		return 1;
	case ACT_BACK:
		MenuSystem_scrollDown(app->menuSystem);
		return 1;
	case ACT_TURN_LEFT:
		MenuSystem_scrollPageUp(app->menuSystem);
		return 1;
	case ACT_TURN_RIGHT:
		MenuSystem_scrollPageDown(app->menuSystem);
		return 1;
	case ACT_USE:
		return MenuSystem_activateItem(app->menuSystem);
	case ACT_AUTOMAP:
		return MenuSystem_select(app->menuSystem);
	case ACT_MENU:
		/* in menu 3 only the left soft key backs out */
		if (app->menuSystem->menu == 3 && avk != AVK_SOFT1)
			return 1;
		if (app->menu->f4c == 0 && app->menu->thread)
			Game_freeScriptThread(app->game, app->menu->thread);
		app->menu->thread = NULL;
		MenuSystem_back(app->menuSystem);
		return 1;
	case ACT_CLEAR:
		ms = app->menuSystem;
		if (ms->menu == 0x14) {
			Applet_shutdown(app);
			return 1;
		}
		if (ms->menu == 3) {
			MenuSystem_setMenu(ms, 0xd);
			return 1;
		}
		if (ms->menu == 1 || ms->menu == 0x12 || ms->menu == 0x15 || ms->menu == 0x13 || ms->menu == 0x16)
			return MenuSystem_activateItem(ms);
		MenuSystem_back(ms);
		return 1;
	default:
		return action;
	}
}

/* 0x0001dd64 Pick the pocket of the NPC in front of the player. */
void Canvas_attemptSteal(Canvas *canvas)
{
	Applet *app = g_app;
	Entity *npc = app->player->facingEntity;
	int alarm = 0;

	if (Entity_distSqFrom(npc, canvas->viewX, canvas->viewY) > tileDistSq(0))
		return;

	if (npc->def->eType == ET_NPC) {
		if (npc->info & 0x100000) {
			/* already robbed */
			Hud_addMessageEx(app->hud, 0, 0x32, 3);
		} else {
			int amount;
			if (npc->info & 0x80000000) {
				npc->info |= 0x40000000 | 0x100000;
			} else if ((Random_nextInt(&app->random) & 0xff) >= 0x24) {
				alarm = 1;
			}

			if (app->statSteals == 0)
				Hud_addTimedMsg(app->hud, 0xcd);
			else
				Hud_randomBark(app->hud, 10);
			app->statSteals++;

			amount = Random_nextInt(&app->random) % 4 + 1;
			if (!Player_give(app->player, 4, 0, amount, 0)) {
				Hud_addMessage(app->hud, 0x33);
			} else {
				if (amount == 1) {
					Hud_addMessageFlags(app->hud, 0x35, 3);
				} else {
					canvas->numTextArgs = 0;
					Canvas_addTextArgInt(canvas, amount);
					Hud_addMessageFlags(app->hud, 0x34, 3);
				}
				Sound_play(app->sndSys, 0xd, 0, 2, -1);
			}
			if (alarm)
				Game_executeTile(app->game, 5, 0, EV_SPECIAL, 1);
		}
	}
	canvas->dialogChoice = 0;
	Game_advanceTurn(app->game);
}

/* The USE key: interact with what is in front of the player or attack.
 * Returns 1 when the original returned straight away (skipping the redraw
 * at the end of Canvas_handlePlayingEvents). */
static int useOrAttack(Canvas *canvas)
{
	Applet *app = g_app;
	Game *game = app->game;
	Player *player = app->player;
	Entity *facing = player->facingEntity;
	Entity *target = NULL, *web = NULL;
	uint32_t flags;
	int range, mask, weaponBits, i;

	canvas->automapFlag = 1;

	/* a lever/trap decoration within reach */
	if (player->isFamiliar == 0 && facing && facing->def->eType == ET_DECOR && facing->def->eSubType == 4) {
		Sprite *spr = entitySprite(facing);
		int distSq = Entity_distSqFrom(facing, canvas->viewX, canvas->viewY);
		if (distSq <= tileDistSq(0) && !(spr->info & 0x40000)) {
			if (app->player->ce.weapon == 1) {
				int pos[2];
				Entity_calcPosition(facing, pos);
				Player_fireWeapon(app->player, facing, pos[0], pos[1]);
				return 0;
			}
			if (facing->param == 1) {
				Player_hurtFeedback(app->player, 0);
				Player_takeDamage(app->player, 2, NULL);
			}
		}
	}

	/* scripts on the tile in front win over attacking */
	flags = (uint32_t)Canvas_flagForFacingDir(canvas, 0xf000);
	if (app->player->isFamiliar)
		flags |= EV_FAMILIAR;
	if (Game_executeTile(game, (canvas->destX + canvas->viewStepX) >> 6, (canvas->destY + canvas->viewStepY) >> 6,
	                     (int)(flags | EV_LEAVE_TILE | EV_ENTER_TILE), 1)) {
		if (game->threadWaiting)
			return 0;
		Game_touchTile(game, canvas->destX, canvas->destY, 0);
		Game_snapAllMonsterSprites(app->game, 1);
		Game_advanceTurn(app->game);
		return 0;
	}

	/* trace along the attack line (1 tile for melee weapons, 6 otherwise) */
	mask = TRACE_ATTACK_MASK;
	range = 6;
	if (app->player->ce.weapon == 10)
		mask |= mask >> 5;
	if (arm_bit(app->player->ce.weapon) & 0x82)
		range = 1;
	Game_trace(game, canvas->viewX, canvas->viewY, canvas->viewX + canvas->viewStepX * range,
	           canvas->viewY + canvas->viewStepY * range, NULL, mask);

	for (i = 0; i < app->game->numTraceEntities; i++) {
		Entity *ent = app->game->traceEntities[i];
		int distSq = Entity_distSqFrom(ent, canvas->viewX, canvas->viewY);
		EntityDef *def = ent->def;
		int type = def->eType;

		weaponBits = arm_bit(app->player->ce.weapon);
		if (type == ET_NPC && def->eSubType == 1)
			continue;
		/* past a destructible only ranged weapons reach on */
		if (target && target->def->eType == ET_DESTRUCTIBLE && distSq > tileDistSq(0)) {
			if ((weaponBits & 0xc) || app->player->fc0 != 0)
				continue;
		}

		if (type == ET_WORLD || type == ET_BLOCK || type == 4) {
			if (!target)
				target = ent;
			break;
		}
		if (type == ET_SIGN) {
			if (def->eSubType == 1) {
				target = ent;
				break;
			}
			if (distSq > tileDistSq(0))
				continue;
			if (weaponBits & 0x82) {
				target = ent;
				break;
			}
			if ((weaponBits & 0xc) && def->eSubType == 3) {
				target = ent;
				break;
			}
			continue;
		}
		if (type == ET_DECOR) {
			if (def->eSubType == 1 && distSq <= tileDistSq(0)) {
				target = ent;
				break;
			}
		} else if (type == ET_SPIDERWEB) {
			if (weaponBits & 0x8e)
				web = ent;
			continue;
		}

		if (range > 1 && type == ET_NPC) {
			if (!(ent->info & 0x8000000) || (ent->info & 0x80000000)) {
				target = ent;
				break;
			}
			continue;
		}
		if (type == ET_MONSTER) {
			/* flying monsters are out of reach when high up */
			if ((ent->monster->flags & 0x200) && entitySprite(ent)->z > 0x20)
				continue;
			target = ent;
			break;
		}
		if (type == ET_DOOR) {
			target = ent;
			break;
		}
		if (type == ET_DECOR) {
			if (def->eSubType == 9) {
				target = ent;
				break;
			}
			if (!target)
				target = ent;
			continue;
		}
		if (type == ET_DESTRUCTIBLE) {
			if (!(weaponBits & 0x82) || tileDistSq(0) != distSq)
				continue;
			if (target && target->def->eType == ET_DESTRUCTIBLE && target->linkIndex >= ent->linkIndex)
				continue;
			target = ent;
			continue;
		}
		if (type == ET_CORPSE) {
			target = ent;
			break;
		}
		if (!target)
			target = ent;
	}

	if (web && (!target || (target->def->eType != ET_MONSTER && target->def->eType != ET_DESTRUCTIBLE)))
		target = web;

	if (!target) {
		/* swing at the air */
		Player_fireWeapon(app->player, &app->game->entities[0], canvas->viewX + canvas->viewStepX * 6,
		                  canvas->viewY + canvas->viewStepY * 6);
		return 0;
	}

	{
		int distSq = Entity_distSqFrom(target, canvas->viewX, canvas->viewY);
		EntityDef *def = target->def;
		int type = def->eType;
		int pos[2], weapon;
		WeaponInfo *wi;
		Entity *victim = target;

		if (type == ET_WORLD) {
			/* hit a wall */
			Player_fireWeapon(app->player, &app->game->entities[0], app->game->traceHitX, app->game->traceHitY);
			return 0;
		}

		if (app->player->isFamiliar == 0) {
			if (type == ET_MONSTER) {
				if (def->eSubType == 2 && !(target->info & 0x10000000) && distSq <= tileDistSq(0)) {
					/* wake a dormant monster */
					target->info |= 0x10000000;
					Game_activate_4168(app->game, target);
					Game_advanceTurn(app->game);
					Hud_addMessageFlags(app->hud, 0x38, 1);
					Canvas_startShake(canvas, 0, 2, 300);
					return 1;
				}
			} else if (type == ET_DOOR) {
				if (def->eSubType == 1 && distSq <= tileDistSq(0)) {
					/* locked door */
					Hud_addMessage(app->hud, 0x39);
					Canvas_setLockpick(canvas, target);
					return 1;
				}
			} else if (type == ET_DECOR) {
				if (def->eSubType == 9) {
					if (distSq <= tileDistSq(0)) {
						Sprite *spr = entitySprite(target);
						target->info |= 0x40000000;
						spr->info = (spr->info & ~0xf00u) | 0x80000000u | 0x400u;
						Game_unlinkEntity_43710(canvas->game, target);
						Canvas_updateFacing(canvas);
						Sound_play(app->sndSys, 0x5e, 0, 3, -1);
						return 1;
					}
				} else if (def->eSubType == 1 && distSq <= tileDistSq(0)) {
					/* chests: 0x8e/0x90 locked, 0x8f/0x91 closed */
					Sprite *spr = entitySprite(target);
					int tile = spr->info & 0xff;
					if (((spr->info >> 8) & 0xf) == 0) {
						if (tile == 0x8e || tile == 0x90) {
							Canvas_setLockpick(canvas, target);
						} else if (tile == 0x8f || tile == 0x91) {
							target->info |= 0x40000000;
							Sound_play(app->sndSys, 0xc, 0, 4, -1);
							app->game->threadWaiting = 1;
							Game_openContainer(target);
							spr->info = (spr->info & ~0xf00u) | 0x80000000u | 0x100u;
							Game_executeTile0(app->game, spr->x >> 6, spr->y >> 6, EV_SPECIAL);
							Game_advanceTurn(app->game);
						}
						return 1;
					}
				}
			}
		}

		Entity_calcPosition(target, pos);
		weapon = app->player->ce.weapon;
		wi = &canvas->combat->weapons[weapon];
		/* melee-only weapons can't reach a target further than one tile */
		if (wi->rangeMax == 1 && wi->rangeMin == 1 && weapon != 4 && tileDistSq(0) < distSq)
			victim = &app->game->entities[0];
		Player_fireWeapon(app->player, victim, pos[0], pos[1]);
	}
	return 0;
}

/* 0x0001df28 Keys while exploring (states 3 and 5, and after a fade). */
int Canvas_handlePlayingEvents(Canvas *canvas, int avk, int unused)
{
	Applet *app = g_app;
	int action = Canvas_getKeyAction(canvas, avk);
	int redraw = 0;

	(void)unused;

	if (canvas->hideHud) {
		/* the controls help screen only reacts to a few keys */
		if (action != ACT_MENU && action != ACT_AUTOMAP && action != ACT_USE && action != ACT_CLEAR)
			return 1;
		canvas->hideHud = 0;
		if (action == ACT_USE || action == ACT_CLEAR)
			action = ACT_MENU;
	}

	if (canvas->forceInterpolation && !viewAtDest(canvas))
		return 0;
	if (canvas->moveCount != 0)
		return 0;

	if (canvas->snapView) {
		const int32_t *sinTable = canvas->render->sinTable;
		int s, c;
		canvas->viewX = canvas->destX;
		canvas->viewY = canvas->destY;
		canvas->viewZ = canvas->destZ;
		canvas->viewAngle = canvas->destAngle;
		s = sinTable[canvas->destAngle & 0x3ff];
		c = sinTable[(canvas->destAngle + 0x100) & 0x3ff];
		canvas->viewSin = s;
		canvas->viewCos = c;
		canvas->viewStepX = (c * 64) >> 16;
		canvas->viewStepY = (-(s * 64)) >> 16;
	} else {
		/* finish any animation still in progress before acting */
		if (canvas->viewX != canvas->destX || canvas->viewY != canvas->destY) {
			redraw = 1;
			canvas->viewX = canvas->destX;
			canvas->viewY = canvas->destY;
			canvas->viewZ = canvas->destZ;
			Canvas_finishMovement_1b1b4(canvas);
		} else if (canvas->viewAngle != canvas->destAngle) {
			redraw = 1;
			canvas->viewAngle = canvas->destAngle;
			Canvas_finishRotation(canvas, 1);
		}
		if (action != ACT_PREV_WEAPON && action != ACT_NEXT_WEAPON && action != ACT_TURN_LEFT &&
		    action != ACT_TURN_RIGHT && !Game_snapAllMonsterSprites(app->game, canvas->state == ST_AUTOMAP))
			return 1;
		if (canvas->state != ST_PLAYING && canvas->state != ST_AUTOMAP) {
			/* the finished move changed the state (combat, dialog...): replay the key there */
			Canvas_renderView(canvas, canvas->viewX, canvas->viewY, canvas->viewAngle);
			Hud_draw(app->hud);
			Canvas_handleEvent(canvas, avk);
			return 1;
		}
	}

	/* port: with free movement the keys and the mouse walk and turn (freemove.c) */
	if (FreeMove_mode() != OE_MOVE_TURNS && canvas->state == ST_PLAYING &&
	    (action == ACT_FORWARD || action == ACT_BACK || action == ACT_TURN_LEFT || action == ACT_TURN_RIGHT ||
	     action == ACT_STRAFE_LEFT || action == ACT_STRAFE_RIGHT))
		action = ACT_NONE;
	if (app->player->statusEffects[SE_FROZEN].turns == 0 || action == ACT_MENU) {
		switch (action) {
		case ACT_FORWARD:
			Canvas_finishMovement_77f4(canvas, canvas->viewX + canvas->viewStepX, canvas->viewY + canvas->viewStepY);
			break;
		case ACT_BACK:
			Canvas_finishMovement_77f4(canvas, canvas->viewX - canvas->viewStepX, canvas->viewY - canvas->viewStepY);
			break;
		case ACT_TURN_LEFT:
		case ACT_TURN_RIGHT:
			app->hud->damageDirUntil = 0;
			canvas->destAngle += action == ACT_TURN_RIGHT ? -0x100 : 0x100;
			canvas->automapFlag = 1;
			break;
		case ACT_USE:
			if (useOrAttack(canvas))
				return 1;
			break;
		case ACT_AUTOMAP:
			if (app->player->isFamiliar == 0)
				Canvas_setState(canvas, canvas->state == ST_AUTOMAP ? ST_PLAYING : ST_AUTOMAP);
			break;
		case ACT_STRAFE_LEFT:
		case ACT_STRAFE_RIGHT: {
			const int32_t *sinTable = canvas->render->sinTable;
			int angle = canvas->viewAngle + (action == ACT_STRAFE_RIGHT ? -0x100 : 0x100);
			int x = canvas->viewX + ((sinTable[(angle + 0x100) & 0x3ff] * 64) >> 16);
			int y = canvas->viewY + ((-(sinTable[angle & 0x3ff] * 64)) >> 16);
			Canvas_finishMovement_77f4(canvas, x, y);
			break;
		}
		case ACT_PREV_WEAPON:
		case ACT_NEXT_WEAPON:
			if (action == ACT_PREV_WEAPON && canvas->weaponCycleKey) {
				/* '*' opens the quick-select menus instead */
				if (app->player->isFamiliar)
					break;
				if (canvas->state == ST_PLAYING)
					Canvas_setState(canvas, ST_QUICK_WEAPON);
				else if (canvas->state == ST_QUICK_WEAPON)
					Canvas_setState(canvas, ST_QUICK_ITEM);
				else
					Canvas_setState(canvas, ST_PLAYING);
				break;
			} else {
				Player *player = app->player;
				int oldWeapon = player->ce.weapon;
				if (action == ACT_PREV_WEAPON)
					Player_selectPrevWeapon(player);
				else
					Player_selectNextWeapon(player);
				if (app->player->ce.weapon != oldWeapon)
					announceWeapon(canvas, app->player->ce.weapon);
				canvas->automapFlag = 1;
				app->player->helpBitmask |= 0x8000;
			}
			break;
		case ACT_PASS:
			Hud_addMessage(app->hud, 0x3a);
			Game_touchTile(app->game, canvas->destX, canvas->destY, 0);
			Game_advanceTurn(app->game);
			break;
		case ACT_CLEAR:
			if (canvas->state == ST_AUTOMAP) {
				Canvas_setState(canvas, ST_PLAYING);
				break;
			}
			/* fall through */
		case ACT_MENU:
			app->hud->numMessages = 0;
			MenuSystem_setMenu(app->menuSystem, 100);
			break;
		default:
			break;
		}
	}

	if (redraw && canvas->state != ST_PLAYING && canvas->state != ST_AUTOMAP) {
		Canvas_renderView(canvas, canvas->viewX, canvas->viewY, canvas->viewAngle);
		Hud_draw(app->hud);
	}
	if ((action == ACT_STRAFE_LEFT || action == ACT_STRAFE_RIGHT || action == ACT_TURN_LEFT ||
	     action == ACT_TURN_RIGHT) && !viewAtDest(canvas))
		app->player->facingEntity = NULL;
	return 1;
}

/* 0x0001ebb0 Digit keys in the quick weapon (0x14) / potion (0x15) menus. */
int Canvas_handleQuickSelectEvents(Canvas *canvas, int avk)
{
	Applet *app = g_app;
	int action = Canvas_getKeyAction(canvas, avk);

	if (action == ACT_CLEAR || avk == AVK_SELECT || action == ACT_MENU) {
		Canvas_clearSoftKeys(canvas);
	} else if (action == ACT_PREV_WEAPON) {
		/* '*' cycles weapon list -> potion list -> closed */
		if (canvas->state == ST_QUICK_WEAPON) {
			Canvas_setState(canvas, ST_QUICK_ITEM);
			return 1;
		}
		if (canvas->state != ST_QUICK_ITEM)
			return 1;
	} else {
		int idx = avk - AVK_1; /* keys 1..9 */
		int next = idx + 1;
		if ((unsigned)idx > 8)
			return 1;
		if (canvas->state == ST_QUICK_WEAPON) {
			Player *player;
			/* key -> weapon slot; keys 3 and 5 pick the upgraded weapon if owned */
			if (idx == 2) {
				if (app->player->weapons & 8)
					idx = 3;
			} else {
				if (idx > 2)
					idx = next;
				if (idx > 5)
					idx++;
				else if (idx == 5 && (app->player->weapons & 0x40))
					idx = 6;
			}
			player = app->player;
			if (player->ce.weapon != idx && (player->weapons & arm_bit(idx))) {
				Player_selectWeapon(player, idx);
				announceWeapon(canvas, idx);
			}
		} else {
			Player *player;
			int item = next;
			if (idx <= 0) {
				/* key 1: small potion unless badly hurt or out of small ones */
				int missing = CombatEntity_getMaxHealth(&app->player->ce) - CombatEntity_getHealth(&app->player->ce);
				player = app->player;
				if (player->inventory[1] == 0 || (missing <= 0x28 && player->inventory[0] != 0))
					item = idx;
			}
			Player_useItem(app->player, item, 0, 0);
		}
	}
	Canvas_setState(canvas, ST_PLAYING);
	String_setLength(&app->hud->forcedMsg, 0);
	return 1;
}

/* 0x0003a3c8 Start picking a lock (or open it outright with the cheat). */
void Canvas_setLockpick(Canvas *canvas, Entity *entity)
{
	Applet *app = g_app;
	int len;

	if (Entity_getEventParam68(entity) > app->player->lockLevel) {
		Sound_play(app->sndSys, 0xf, 0, 3, -1);
		Hud_addMessageEx(app->hud, 0, 0x2c, 3);
		return;
	}
	canvas->lockEntity = entity;
	if (canvas->showCodes) {
		Canvas_finishUse(canvas);
		Player_addXP(app->player, 5);
		return;
	}
	Sound_play(app->sndSys, 4, 0, 3, -1);
	len = Entity_getEventParam68(canvas->lockEntity) + 3;
	Canvas_setPassword(canvas->lockEntity, len);
	if (String_length(&canvas->lockEntered) > len)
		String_setLength(&app->game->lockCombination, len);
	Canvas_setState(canvas, ST_LOCKPICK);
	Player_showHelp(app->player, 2, 0);
}

/* 0x0003acc0 Load the lock code of `entity` (a map string of digits 1-4). */
void Canvas_setPassword(Entity *entity, int len)
{
	Applet *app = g_app;
	Render *render = app->render;
	int ev = Render_findTileEvent(render, entity->linkIndex);
	int numOps, first, i, j;

	if (ev < 0)
		return;

	render = app->render;
	numOps = (int)((render->tileEvents[ev] >> 20) & 0x7f) * 3;
	first = (int)((render->tileEvents[ev] >> 9) & 0x7ff) * 3;
	for (i = 0; i < numOps; i += 3) {
		int op = render->byteCodes[first + i];
		int arg = render->byteCodes[first + i + 1];
		Canvas *cv;
		String *text;

		if (op != 10)
			continue;
		/* opcode 10 holds the id of the password string */
		cv = app->canvas;
		text = &cv->scratch;
		String_setLength(text, 0);
		Text_getString(app->canvas, app->canvas->mapStringGroup, (int16_t)arg, text);
		if (String_length(text) != len)
			return;
		String_setLength(&app->game->lockCombination, 0);
		for (j = 0; j < len; j++) {
			int ch = String_charAt(text, j);
			if (ch < '1' || ch > '4')
				return;
			String_setCharAt(&app->game->lockCombination, j, ch - '0');
		}
		return;
	}
	App_fatalError(app, (char *)ModData_str(&g_mod, MOD_STR_NO_PASSWORD));
}

/* 0x0002b38c The lock was picked: open the door or chest. */
void Canvas_finishUse(Canvas *canvas)
{
	Applet *app = g_app;
	Entity *ent;
	EntityDef *def;

	Sound_play(app->sndSys, 0x15, 0, 3, -1);
	Hud_addMessageFlags(canvas->hud, 0x3e, 1);
	ent = canvas->lockEntity;
	def = ent->def;
	if (def->eType == ET_DOOR) {
		if (def->eSubType == 1) {
			int line = Render_getLineIndex(&app->render->lines[(ent->info & 0xffff) - 1]);
			Game_setLineLocked(app->game, line, 0, 1);
		}
	} else if (def->eType == ET_DECOR && def->eSubType == 1) {
		/* locked chest tile -> closed chest tile */
		Sprite *spr = entitySprite(ent);
		uint32_t tile = (spr->info & 0xff) == 0x8e ? 0x8f : 0x91;
		spr->info = (spr->info & 0xffffff00u) | tile;
		canvas->lockEntity->info |= 0x40000000;
	}
}

/* 0x00034f74 Re-equip the weapon that was swapped out for a temporary one. */
void Canvas_restorePrevWeapon(Canvas *canvas)
{
	Applet *app = g_app;
	Player *player;

	if (canvas->state == ST_DIALOG)
		return;
	player = app->player;
	if (player->tempWeapon == 0 || player->prevWeapon == player->ce.weapon || player->ce.weapon != 0)
		return;
	Player_selectWeapon(player, player->prevWeapon);
	app->player->tempWeapon = 0;
}
