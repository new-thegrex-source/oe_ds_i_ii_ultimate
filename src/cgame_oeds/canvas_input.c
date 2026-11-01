/*
 * Canvas input: the key queue filled by App_handleEvent, the AVK -> action
 * mapping, and the per-state key handlers (menu, playing, combination lock).
 *
 * Keys are queued (4 slots) and replayed at the start of the next Canvas_run,
 * so every handler runs inside the game tick like on the phone.
 */

#include "oe_vm.h"
#include "game.h"
#include "ds_local.h"

/* ---- tables and strings in the executable's data area ---- */
#define MOD_KEY_DIGIT_ACTIONS 0x2d0b8u /* u8[10]: action for AVK_0..AVK_9 */
#define MOD_KEY_EXTRA_ACTIONS 0x2d0c4u /* {int avk, int action} x 3 (CLR, SOFT1, SOFT2) */
#define MOD_KEY_EXTRA_COUNT   3
#define MOD_STR_LOADING_DOTS  0x2d5bcu
#define MOD_STR_DRAGON_NO_WEAPONS 0x2d640u
#define MOD_STR_GAYA_CLOSER   0x2d6c8u
#define MOD_STR_GAYA_OAF      0x2d6f0u
#define MOD_STR_ITS_A_TRAP    0x2d7a8u
#define MOD_STR_DOOR_LOCKED   0x2d7b8u
#define MOD_STR_TURN_PASSED   0x2d7c8u
#define MOD_STR_HEALTH_MAX    0x2d7d8u
#define MOD_STR_NO_POTIONS    0x2d7ecu
#define MOD_STR_VOLUME        0x2d800u /* (menu item label) */
#define MOD_STR_FMT_PERCENT   0x2d808u

/* Canvas states (Canvas.state). */
enum {
	CS_LOGOS = 0,
	CS_MENU = 2,
	CS_PLAYING = 3,
	CS_COMBAT = 4,
	CS_AUTOMAP = 5,
	CS_LOADING = 6,
	CS_DIALOG = 7,
	CS_LOCK = 8,
	CS_INTRO = 9,
	CS_BENCHMARK = 0xa,
	CS_BENCHMARK_END = 0xb,
	CS_TRAVELMAP = 0xc,
	CS_EPILOGUE = 0xf,
	CS_CREDITS = 0x11,
	CS_ERROR = 0x13,
	CS_CINEMATIC = 0x14,
	CS_FADE = 0x15,
};

/* Key actions (Canvas_getKeyAction). */
enum {
	ACT_NONE = 0,
	ACT_FORWARD = 1,
	ACT_BACKWARD = 2,
	ACT_TURN_LEFT = 3,
	ACT_TURN_RIGHT = 4,
	ACT_MENU = 5,
	ACT_SELECT = 6,       /* use / attack / OK */
	ACT_AUTOMAP = 7,
	ACT_STRAFE_LEFT = 9,
	ACT_STRAFE_RIGHT = 10,
	ACT_PREV_WEAPON = 0xb,
	ACT_NEXT_WEAPON = 0xc,
	ACT_PASS_TURN = 0xe,
	ACT_BACK = 0xf,       /* CLR */
	ACT_SWAP_WEAPON = 0x10,
	ACT_QUICK_POTION = 0x11,
};

#define MAP_MENU        6012   /* DS map_menu (BREW 6009 ui.bsp) */
#define MAP_DRAGON_LAIR 6011   /* DS 6011 (BREW 6008): Gaya's lair, no weapons allowed */
#define MELEE_WEAPONS   0x40e  /* weapons 1, 2, 3, 10 */
#define TRACE_MASK_USE  0x76b5

static char *modStr(uint32_t off)
{
	return (char *)ModData_str(&g_mod, off);
}

/* 0x00018070 Queue a key press (from App_handleEvent EVT_KEY); 4 slots, extra keys dropped. */
void Canvas_keyPressed(Canvas *canvas, short key)
{
	uint8_t n = canvas->numKeys;
	if (n != 4) {
		canvas->numKeys = (uint8_t)(n + 1);
		canvas->keyQueue[n] = key;
	}
}

/* 0x0002279c Replay the queued keys; a handler returning 0 leaves the rest queued. */
void Canvas_runInputEvents(Canvas *canvas)
{
	int i, n;

	if (canvas->ignoreKeysUntil != 0) {
		if (canvas->ignoreKeysUntil < canvas->app->tickTime)
			canvas->ignoreKeysUntil = 0;
		canvas->numKeys = 0;
		return;
	}
	for (i = 0; i < canvas->numKeys; i++) {
		if (!Canvas_handleEvent(canvas, (uint16_t)canvas->keyQueue[i]))
			break;
	}
	for (n = 0; i < canvas->numKeys; i++)
		canvas->keyQueue[n++] = canvas->keyQueue[i];
	canvas->numKeys = (uint8_t)n;
}

/* 0x00012a48 AVK key code -> game action. */
int Canvas_getKeyAction(Canvas *canvas, int avk)
{
	int i;

	if ((unsigned)(avk - AVK_0) < 10)
		return ModData_u8(&g_mod, MOD_KEY_DIGIT_ACTIONS + (uint32_t)(avk - AVK_0));
	if (avk == AVK_STAR)
		return ACT_PREV_WEAPON;
	if (avk == AVK_POUND)
		return ACT_AUTOMAP;
	if (avk == AVK_LEFT)
		return ACT_TURN_LEFT;
	if (avk == AVK_RIGHT)
		return ACT_TURN_RIGHT;
	if (avk == AVK_UP)
		return ACT_FORWARD;
	if (avk == AVK_DOWN)
		return ACT_BACKWARD;
	if (avk == AVK_SELECT)
		return ACT_SELECT;
	for (i = 0; i < MOD_KEY_EXTRA_COUNT; i++) {
		if (ModData_s32(&g_mod, MOD_KEY_EXTRA_ACTIONS + (uint32_t)i * 8) == avk)
			return ModData_s32(&g_mod, MOD_KEY_EXTRA_ACTIONS + (uint32_t)i * 8 + 4);
	}
	return ACT_NONE;
}

/* 0x00012764 Script trigger flag for the facing direction: mask 0xf0000000 =
 * enter/face (E 0x20000000, N 0x10000000, W 0x80000000, S 0x40000000),
 * mask 0xf000 = use (E 0x8000, N 0x4000, W 0x2000, S 0x1000). */
int Canvas_getDirEventFlags(Canvas *canvas, int mask)
{
	int angle = canvas->destAngle & 0x3ff;

	if (mask == (int)0xf0000000) {
		switch (angle) {
		case 0x000: return 0x20000000;
		case 0x100: return 0x10000000;
		case 0x200: return (int)0x80000000;
		case 0x300: return 0x40000000;
		}
	} else if (mask == 0xf000) {
		switch (angle) {
		case 0x000: return 0x8000;
		case 0x100: return 0x4000;
		case 0x200: return 0x2000;
		case 0x300: return 0x1000;
		}
	}
	return 0;
}

/* Refresh the "Volume:" value of the options menu after a volume key. */
static void refreshVolumeItem(Canvas *canvas)
{
	MenuSystem *ms = canvas->menuSystem;

	if (canvas->state != CS_MENU)
		return;
	if (strcmp(ms->items[2].text, modStr(MOD_STR_VOLUME)) == 0)
		App_snprintf(canvas->menuSystem->items[2].text2, 0xe, modStr(MOD_STR_FMT_PERCENT),
		             canvas->app->sound->volume * 100 / 100);
}

/* 0x00013880 Handle one key. Returns 0 to keep the remaining keys queued. */
int Canvas_handleEvent(Canvas *canvas, int avk)
{
	int action;
	static int keylog = -1; /* developer hook OE_KEYLOG=1: log every key with the state it arrives in */

	if (keylog < 0)
		keylog = getenv("OE_KEYLOG") != NULL;
	if (keylog)
		Oe_log("key 0x%04x state %d action %d menu %d time %d", avk, canvas->state, Canvas_getKeyAction(canvas, avk),
		       canvas->app->menuSystem->menuId, canvas->app->gameTime);

	if (avk == AVK_VOLUME_UP) {
		Sound_volumeUp(canvas->app->sound, 10);
		Game_saveConfig(canvas->app->game);
		refreshVolumeItem(canvas);
	} else if (avk == AVK_VOLUME_DOWN) {
		Sound_volumeDown(canvas->app->sound, 10);
		Game_saveConfig(canvas->app->game);
		refreshVolumeItem(canvas);
	}

	/* any key ends the render benchmark */
	if (canvas->benchmark) {
		canvas->benchmark = 0;
		canvas->cameraMode = 0;
		canvas->app->menuSystem->spinPeriod = 0x28;
	}

	/* DS port: the spell circle (Q) takes the keys while it is up */
	if (canvas->state == CS_PLAYING) {
		if (DsSpells_open())
			return DsSpells_key(canvas, avk);
		if (avk == AVK_SOFT3) {
			DsSpells_toggle(canvas);
			return 1;
		}
	}

	switch (canvas->state) {
	case CS_LOGOS:
		if (Canvas_getKeyAction(canvas, avk) == ACT_BACK) {
			Applet_shutdown(canvas->app);
			return 1;
		}
		if (avk != AVK_SEND)
			canvas->keySkip = 1;
		return 1;

	case CS_MENU:
		Canvas_handleMenuEvents(canvas, avk);
		return 1;

	case CS_PLAYING:
		return Canvas_handlePlayingEvents(canvas, avk);

	case CS_COMBAT:
		/* the player may turn while a monster attacks */
		if (canvas->player->effects[0].turns == 0 && canvas->combat->attacker != NULL) {
			action = Canvas_getKeyAction(canvas, avk);
			if (action == ACT_TURN_RIGHT)
				canvas->destAngle -= 0x100;
			else if (action == ACT_TURN_LEFT)
				canvas->destAngle += 0x100;
		}
		if (canvas->turnDone && canvas->game->monstersMoving == 0) {
			ParticleSystem_freeAll(canvas->particles);
			Canvas_setState(canvas, CS_PLAYING);
			if (canvas->combat->attacker == NULL) {
				Game_advanceTurn(canvas->game);
				if (canvas->state == CS_PLAYING)
					return Canvas_handlePlayingEvents(canvas, avk);
			}
		}
		return 1;

	case CS_AUTOMAP:
		canvas->f249 = 0;
		return Canvas_handlePlayingEvents(canvas, avk);

	case CS_DIALOG:
		Canvas_handleDialogEvents(canvas, avk);
		return 1;

	case CS_LOCK:
		Canvas_handleLockpickEvents(canvas, avk);
		return 1;

	case CS_INTRO:
	case CS_EPILOGUE: {
		/* story pages: forward / back */
		int step;
		action = Canvas_getKeyAction(canvas, avk);
		if (action == ACT_SELECT || action == ACT_TURN_RIGHT || avk == AVK_SOFT2)
			step = 1;
		else if (action == ACT_BACK || action == ACT_TURN_LEFT || avk == AVK_SOFT1)
			step = -1;
		else
			return 1;
		Canvas_fadeStep(canvas, step);
		return 1;
	}

	case CS_BENCHMARK:
	case CS_BENCHMARK_END:
		Canvas_setState(canvas, CS_PLAYING);
		Canvas_setAnimFrames(canvas, canvas->animFrames);
		return 1;

	case CS_TRAVELMAP:
		action = Canvas_getKeyAction(canvas, avk);
		if (action == ACT_SELECT || avk == AVK_SOFT2) {
			Canvas_freeLoadImages(canvas);
			strcpy(canvas->printMsg, modStr(MOD_STR_LOADING_DOTS));
			Canvas_setState(canvas, CS_LOADING);
		}
		return 1;

	case CS_CREDITS: {
		int menu;
		action = Canvas_getKeyAction(canvas, avk);
		if (canvas->creditsRunning == 0) {
			if (action != ACT_SELECT && action != ACT_BACK)
				return 1;
			if (canvas->mapId == MAP_MENU) {
				menu = 2;
			} else {
				Sound_clearChannelFlags(canvas->app->sound);
				menu = 0x67;
			}
		} else {
			/* end-game credits: OK only once every line has scrolled in (750 ms each) */
			if (action != ACT_SELECT)
				return 1;
			if ((canvas->app->tickTime - canvas->scrollStart) / 750 <= canvas->scrollLines)
				return 1;
			canvas->creditsRunning = 0;
			Sound_clearChannelFlags(canvas->app->sound);
			menu = 0xf;
		}
		MenuSystem_setMenu(canvas->app->menuSystem, menu);
		return 1;
	}

	case CS_ERROR:
		action = Canvas_getKeyAction(canvas, avk);
		if (action == ACT_BACK || action == ACT_SELECT)
			Applet_shutdown(canvas->app);
		return 1;

	case CS_CINEMATIC:
		action = Canvas_getKeyAction(canvas, avk);
		if (canvas->game->skipUntil < canvas->app->tickTime &&
		    (action == ACT_SELECT || action == ACT_PASS_TURN || action == ACT_BACK || avk == AVK_SOFT2))
			Game_skipCinematic(canvas->game);
		return 1;

	case CS_FADE:
		canvas->fadeStart = 0;
		if (canvas->fadeDir) {
			Canvas_setState(canvas, CS_PLAYING);
			return Canvas_handlePlayingEvents(canvas, avk);
		}
		return 1;

	default:
		return 1;
	}
}

/* 0x00013e0c State 2: keys for the MenuSystem. */
void Canvas_handleMenuEvents(Canvas *canvas, int avk)
{
	MenuSystem *ms = canvas->app->menuSystem;
	int action;

	if (ms->menuId == 0x133) { /* Gaya's store: offer entry */
		MenuSystem_handleNumberEntry(ms, avk);
		return;
	}
	if (ms->sliderEdit != 0) {
		MenuSystem_handleVolumeKey(ms, avk);
		return;
	}
	if ((unsigned)(avk - AVK_0) < 10 && MenuSystem_enterDigit(ms, avk - AVK_0))
		return; /* cheat code digit */

	action = Canvas_getKeyAction(canvas, avk);
	switch (action) {
	case ACT_FORWARD:
		MenuSystem_scrollUp(canvas->app->menuSystem);
		return;
	case ACT_BACKWARD:
		MenuSystem_scrollDown(canvas->app->menuSystem);
		return;
	case ACT_TURN_LEFT:
		MenuSystem_scrollPageUp(canvas->app->menuSystem);
		return;
	case ACT_TURN_RIGHT:
		MenuSystem_scrollPageDown(canvas->app->menuSystem);
		return;
	case ACT_SELECT:
		MenuSystem_activateItem(canvas->app->menuSystem);
		return;
	case ACT_AUTOMAP:
		MenuSystem_select(canvas->app->menuSystem);
		return;
	case ACT_MENU:
		MenuSystem_back(canvas->app->menuSystem);
		return;
	case ACT_BACK:
		ms = canvas->app->menuSystem;
		if (ms->menuId == 0xe) {          /* "Enable sounds?" at startup */
			Applet_shutdown(canvas->app);
		} else if (ms->menuId == 1) {     /* main menu -> exit confirmation */
			MenuSystem_setMenu(ms, 5);
		} else if (ms->menuId == 10 || ms->menuId == 0xc) {
			MenuSystem_activateItem(ms); /* level-end / completed screens */
		} else {
			MenuSystem_back(ms);
		}
		return;
	default:
		return;
	}
}

/* 0x00013c28 State 8: arrows enter the combination, SELECT/CLR give up. */
void Canvas_handleLockpickEvents(Canvas *canvas, int avk)
{
	int action = Canvas_getKeyAction(canvas, avk);
	int typed, need, i;

	if (canvas->lockDone)
		return;
	typed = (int)strlen(canvas->lockTyped);
	need = (int)strlen(canvas->game->password);

	if ((unsigned)(action - 1) < 4) {
		/* an arrow sprite flies up in front of the player */
		int x = canvas->stepX / 2 + canvas->viewX;
		int y = canvas->stepY / 2 + canvas->viewY;
		int side = 0, lift = 0;
		uint32_t wallFlag = 0;
		GameSprite *gs;

		canvas->lockTyped[typed] = (char)(action - 0x80); /* glyphs 0x81..0x84 */
		typed++;
		switch (action) {
		case ACT_FORWARD:    side = -2;    lift = 0xd;  break;
		case ACT_BACKWARD:   side = -2;    lift = 0x30; break;
		case ACT_TURN_LEFT:  side = -0x13; lift = 0x1e; break;
		case ACT_TURN_RIGHT: side = 0x10;  lift = 0x1e; break;
		}
		if (canvas->stepX > 0) {
			y += side;
			wallFlag = 0x400000;
		} else if (canvas->stepX < 0) {
			y -= side;
			wallFlag = 0x200000;
		} else if (canvas->stepY > 0) {
			x -= side;
			wallFlag = 0x100000;
		} else if (canvas->stepY < 0) {
			x += side;
			wallFlag = 0x80000;
		}
		gs = (GameSprite *)Game_gspriteAllocAnim(canvas->game, 0xf5, x, y, 0x40 - lift);
		if (gs && gs->sprite) {
			gs->sprite->info |= wallFlag | 0x2000000;
			Render_relinkSpriteAt(canvas->render, (int *)gs->sprite, canvas->viewX, canvas->viewY);
		}
		Sound_play(canvas->app->sound, 5067, 0, 3, -1);
		if (typed == need)
			canvas->lockDone = 1;
	} else if (action == ACT_SELECT || action == ACT_BACK) {
		canvas->lockDone = 1;
	} else {
		return;
	}

	for (i = 0; i < need; i++)
		canvas->lockShown[i] = i < typed ? canvas->lockTyped[i] : '_';
	canvas->lockShown[i] = '\0';
	if (!canvas->lockDone)
		canvas->lockTime = canvas->app->tickTime + 600;
	else
		canvas->lockTime = canvas->app->tickTime + (typed == need ? 300 : 0);
}

/* Use/attack: pick the target in front of the player and fire. Returns 1 when
 * the key was fully handled here (no redraw). */
static int useOrAttack(Canvas *canvas)
{
	Game *game = canvas->game;
	Player *player = canvas->player;
	Entity *target = NULL, *spriteWall = NULL;
	int melee = (arm_bit(player->ce.weapon) & MELEE_WEAPONS) != 0;
	int range = melee ? 1 : 6;
	int i, x, y, dist;

	Game_trace(canvas->game, canvas->viewX, canvas->viewY, canvas->viewX + range * canvas->stepX,
	           canvas->viewY + range * canvas->stepY, NULL, TRACE_MASK_USE);
	for (i = 0; i < canvas->game->numTraceHits; i++) {
		Entity *hit = canvas->game->traceHits[i];
		EntityDef *def;
		dist = Entity_distSqFrom(hit, canvas->viewX, canvas->viewY);
		if (target && target->def->eType == 9 && dist > 0x1000)
			break;
		def = hit->def;
		switch (def->eType) {
		case 0:   /* world */
		case 0xc: /* sprite wall */
		case 4:   /* magic seal */
			if (!target)
				target = hit;
			goto traced;
		case 0xa: /* breakable / chest */
			if (def->eSubType == 1 || (melee && dist <= 0x1000)) {
				target = hit;
				goto traced;
			}
			break;
		case 0xd: /* non-obstructing sprite wall: only for melee */
			if (melee)
				spriteWall = hit;
			break;
		case 2:   /* monster */
		case 5:   /* door */
			target = hit;
			goto traced;
		case 9:   /* corpse: only adjacent, for melee */
			if (melee && dist == 0x1000 && def->eSubType != 4 &&
			    (!target || target->def->eType != 9 || target < hit))
				target = hit;
			break;
		default:
			if (!target)
				target = hit;
			break;
		}
	}
traced:
	if (spriteWall && (!target || (target->def->eType != 2 && target->def->eType != 9)))
		target = spriteWall;

	/* Gaya's lair: attacking towards the dragon only gets a lecture */
	if (canvas->mapId == MAP_DRAGON_LAIR) {
		int tx = canvas->destX >> 6;
		if ((tx == 10 || tx == 11 || tx == 12) && (canvas->destAngle & 0x3ff) == 0x300 &&
		    (canvas->destY >> 6) > 12 && (!target || target->def->eType != 2)) {
			Player *pl = canvas->app->player;
			if (pl->helpShown & 0x20000) {
				Canvas_showDialog(canvas, NULL, modStr(MOD_STR_GAYA_CLOSER), 1, 0, 0);
				return 1;
			}
			Canvas_showDialog(canvas, NULL, modStr(MOD_STR_GAYA_OAF), 1, 1, 0);
			canvas->app->player->helpShown |= 0x20000;
			return 1;
		}
	}

	if (!target) {
		x = canvas->viewX + canvas->stepX * 6;
		y = canvas->viewY + canvas->stepY * 6;
		target = &game->entities[0];
	} else {
		dist = Entity_distSqFrom(target, canvas->viewX, canvas->viewY);
		if (target->def->eType == 0) {
			target = &canvas->game->entities[0];
			x = canvas->game->traceX;
			y = canvas->game->traceY;
		} else {
			Weapon *w;
			int wi;
			if (target->def->eType == 2) {
				if (target->def->eSubType == 2 && !(target->info & 0x20000000) && dist <= 0x1000) {
					/* a disguised monster (mimic) */
					target->info |= 0x20000000;
					Game_activateEx(canvas->game, target, 1, 0);
					Game_advanceTurn(canvas->game);
					Hud_addMessage(canvas->hud, modStr(MOD_STR_ITS_A_TRAP), 1);
					Canvas_startShake(canvas, 0, 2, 300);
					return 1;
				}
			} else if (target->def->eType == 5 && dist <= 0x1000) {
				Hud_addMessage(canvas->hud, modStr(MOD_STR_DOOR_LOCKED), 0);
				Sound_play(canvas->app->sound, 5143, 0, 3, -1);
				return 1;
			}
			Entity_calcPosition(target, &x, &y);
			wi = canvas->player->ce.weapon;
			w = &canvas->combat->weapons[wi];
			/* a melee weapon swung at something out of reach hits the air */
			if (w->rangeMax == 1 && w->rangeMin == 1 && wi != 8 && dist > 0x1000)
				target = &canvas->game->entities[0];
		}
	}
	Player_fireWeapon(canvas->player, target, x, y);
	return 0;
}

/* the attack ahead, as the use/attack key: the DS's wand spells fire this way (DS menu action 0x2a) */
void Canvas_attackAhead(Canvas *canvas)
{
	useOrAttack(canvas);
}

/* 0x00013f18 States 3 and 5: movement, turning, use/attack, menus. */
int Canvas_handlePlayingEvents(Canvas *canvas, int avk)
{
	int action = Canvas_getKeyAction(canvas, avk);
	int snapped = 0;

	{
		/* DS port: oe_dsTrace 1 prints the keys in play */
		char tr[8];
		trap_Cvar_VariableStringBuffer("oe_dsTrace", tr, sizeof tr);
		if (tr[0] == '1')
			Oe_print("key: avk 0x%x action %d state %d swallow %d view %d,%d,%d dest %d,%d,%d stun %d\n", avk, action,
				canvas->state, canvas->swallowKey, canvas->viewX, canvas->viewY, canvas->viewAngle, canvas->destX,
				canvas->destY, canvas->destAngle, canvas->player->effects[0].turns);
	}

	/* .bar option 9: ignore keys while the view is still moving */
	if (canvas->barOpt9 && (canvas->viewX != canvas->destX || canvas->viewY != canvas->destY ||
	                        canvas->viewAngle != canvas->destAngle))
		return 0;

	if (!canvas->cameraMode) {
		/* finish the current step at once */
		if (canvas->viewX == canvas->destX && canvas->viewY == canvas->destY) {
			if (canvas->viewAngle != canvas->destAngle) {
				snapped = 1;
				canvas->viewAngle = canvas->destAngle;
				Canvas_finishRotation(canvas);
			}
		} else {
			snapped = 1;
			canvas->viewX = canvas->destX;
			canvas->viewY = canvas->destY;
			canvas->viewZ = canvas->destZ;
			Canvas_finishMovement(canvas);
		}
		if (canvas->swallowKey) {
			/* the controls help is up: only SELECT/menu/automap/CLR close it */
			if (action != ACT_SELECT && action != ACT_MENU && action != ACT_AUTOMAP && action != ACT_BACK)
				return 1;
			canvas->swallowKey = 0;
			if (action != ACT_MENU && action != ACT_AUTOMAP)
				return 1;
		}
		if (action != ACT_PREV_WEAPON && action != ACT_NEXT_WEAPON && action != ACT_TURN_LEFT &&
		    action != ACT_TURN_RIGHT && !Game_snapAllMovers(canvas->game, canvas->state == CS_AUTOMAP))
			return 1;
		if (canvas->state != CS_PLAYING && canvas->state != CS_AUTOMAP) {
			/* finishing the step changed the state: redraw and handle the key there */
			Canvas_renderView(canvas, canvas->viewX, canvas->viewY, canvas->viewAngle);
			Canvas_updateShake(canvas);
			Hud_draw(canvas->hud);
			Canvas_handleEvent(canvas, avk);
			return 1;
		}
	} else {
		/* free camera (debug): jump to the destination */
		int angle, s, c;
		canvas->viewX = canvas->destX;
		canvas->viewY = canvas->destY;
		canvas->viewZ = canvas->destZ;
		angle = canvas->destAngle;
		canvas->viewAngle = angle;
		s = canvas->render->sinTable[angle & 0x3ff];
		canvas->viewSin = s;
		c = canvas->render->sinTable[(angle + 0x100) & 0x3ff];
		canvas->viewCos = c;
		canvas->stepX = (c * 64) >> 16;
		canvas->stepY = (s * -0x40) >> 16;
	}

	/* status effect 0 (stunned?) blocks everything but the menu */
	if (canvas->player->effects[0].turns != 0 && action != ACT_MENU)
		goto done;
	/* port: with free movement the keys and the mouse walk and turn (freemove.c) */
	if (FreeMove_mode() != OE_MOVE_TURNS && canvas->state == CS_PLAYING &&
	    (action == ACT_FORWARD || action == ACT_BACKWARD || action == ACT_TURN_LEFT || action == ACT_TURN_RIGHT ||
	     action == ACT_STRAFE_LEFT || action == ACT_STRAFE_RIGHT))
		goto done;

	switch (action) {
	case ACT_FORWARD:
		Canvas_attemptMove(canvas, canvas->viewX + canvas->stepX, canvas->viewY + canvas->stepY);
		break;

	case ACT_BACKWARD:
		Canvas_attemptMove(canvas, canvas->viewX - canvas->stepX, canvas->viewY - canvas->stepY);
		break;

	case ACT_TURN_LEFT:
	case ACT_TURN_RIGHT:
		canvas->hud->arrowUntil = 0;
		canvas->destAngle += action == ACT_TURN_RIGHT ? -0x100 : 0x100;
		canvas->usedThisTurn = 1;
		break;

	case ACT_SELECT:
		canvas->usedThisTurn = 1;
		if (Game_executeTile(canvas->game, (canvas->destX + canvas->stepX) >> 6,
		                     (canvas->destY + canvas->stepY) >> 6,
		                     Canvas_getDirEventFlags(canvas, 0xf000) | 0x400, 1) == 0) {
			if (useOrAttack(canvas))
				return 1;
			break;
		}
		/* a "use" event ran: the turn passes unless the script yielded */
		if (canvas->game->yieldFlag != 0)
			break;
		Game_touchTile(canvas->game, canvas->destX, canvas->destY, 0);
		Game_advanceTurn(canvas->game);
		break;

	case ACT_AUTOMAP:
		Canvas_setState(canvas, canvas->state == CS_AUTOMAP ? CS_PLAYING : CS_AUTOMAP);
		break;

	case ACT_STRAFE_LEFT:
	case ACT_STRAFE_RIGHT: {
		int angle = canvas->viewAngle + (action == ACT_STRAFE_RIGHT ? -0x100 : 0x100);
		int dy = (canvas->render->sinTable[angle & 0x3ff] * -0x40) >> 16;
		int dx = (canvas->render->sinTable[(angle + 0x100) & 0x3ff] * 64) >> 16;
		Canvas_attemptMove(canvas, canvas->viewX + dx, canvas->viewY + dy);
		break;
	}

	case ACT_PREV_WEAPON:
	case ACT_NEXT_WEAPON: {
		Player *player = canvas->player;
		int old;
		if (canvas->mapId == MAP_DRAGON_LAIR) {
			Canvas_showDialog(canvas, NULL, modStr(MOD_STR_DRAGON_NO_WEAPONS), 1, 0, 0);
			break;
		}
		old = player->ce.weapon;
		if (action == ACT_PREV_WEAPON)
			Player_selectPrevWeapon(player);
		else
			Player_selectNextWeapon(player);
		if (canvas->player->ce.weapon != old) {
			EntityDef *def = (EntityDef *)EntityDefs_find(canvas->entityDefs, 6, 1, canvas->player->ce.weapon);
			Hud_addMessage(canvas->hud, def->longName, 1);
		}
		canvas->usedThisTurn = 1;
		break;
	}

	case ACT_PASS_TURN:
		Hud_addMessage(canvas->hud, modStr(MOD_STR_TURN_PASSED), 0);
		Game_touchTile(canvas->game, canvas->destX, canvas->destY, 0);
		Game_advanceTurn(canvas->game);
		break;

	case ACT_BACK:
		if (canvas->state == CS_AUTOMAP) {
			Canvas_setState(canvas, CS_PLAYING);
			break;
		}
		/* CLR while exploring opens the game menu */
		/* fall through */
	case ACT_MENU:
		canvas->hud->numMessages = 0;
		MenuSystem_setMenu(canvas->app->menuSystem, 100);
		break;

	case ACT_SWAP_WEAPON: {
		Player *player = canvas->app->player;
		Player_selectWeapon(player, player->ce.weapon == 0 ? player->prevWeapon : 0);
		break;
	}

	case ACT_QUICK_POTION: {
		Player *player = canvas->app->player;
		int maxHp = CombatEntity_getMaxHealth(&player->ce);
		int hp = CombatEntity_getHealth(&canvas->app->player->ce);
		if (maxHp == hp) {
			Hud_addMessage(canvas->hud, modStr(MOD_STR_HEALTH_MAX), 0);
		} else if (maxHp - hp > 40 && canvas->app->player->items[1] != 0) {
			Player_useItem(canvas->app->player, 1, 0); /* large potion */
		} else if (canvas->app->player->items[0] != 0) {
			Player_useItem(canvas->app->player, 0, 0);
		} else {
			Hud_addMessage(canvas->hud, modStr(MOD_STR_NO_POTIONS), 0);
		}
		break;
	}

	default:
		break;
	}

done:
	if (snapped && canvas->state != CS_PLAYING && canvas->state != CS_AUTOMAP) {
		Canvas_renderView(canvas, canvas->viewX, canvas->viewY, canvas->viewAngle);
		Canvas_updateShake(canvas);
		Hud_draw(canvas->hud);
	}
	if ((action == ACT_STRAFE_LEFT || action == ACT_STRAFE_RIGHT || action == ACT_TURN_LEFT ||
	     action == ACT_TURN_RIGHT) &&
	    (canvas->viewX != canvas->destX || canvas->viewY != canvas->destY || canvas->viewAngle != canvas->destAngle))
		canvas->player->facing = NULL;
	return 1;
}
