/*
 * Tile-event script interpreter, part 1 (opcodes 0x00..0x39).
 *
 * Every map tile can carry an event word that points at a run of
 * {opcode, arg, flags} triples in Render.byteCodes. ScriptThread_runEvent
 * walks those triples and hands each one to ScriptThread_executeCommand,
 * which is implemented here. Opcodes 0x01..0x2a keep the numbering of Doom
 * RPG's EV_* events; the later ones were added for Orcs & Elves (II).
 *
 * The 32-bit argument is usually a packed record; below b0..b3 are its bytes
 * from least to most significant.
 *
 * Return codes: 0 command did not run, 1 done, 2 yield (thread resumes later
 * through ScriptThread_run), 3 abort the rest of the event, 4 restart it.
 */

#include "game.h"

/* Opcodes 0x3a..0x73 of ScriptThread_executeCommand live in script_ext.c. Same return codes. */
int ScriptThread_executeExtCommand(ScriptThread *thread, ScriptFrame *frame, int opcode, int arg);

enum {
	SCRIPT_DONE = 1,
	SCRIPT_YIELD = 2,
	SCRIPT_ABORT = 3,
};

enum ScriptOpcode {
	EV_NOP = 0x00,
	EV_GOTO = 0x01,            /* warp player to tile b0,b1, angle dir b2 (0xff keep) */
	EV_CHANGEMAP = 0x02,       /* request a map change; bit30 = fade out first */
	EV_TRIGGER = 0x03,         /* run the event of tile b0,b1 nested; bit31 = random tile in rect */
	EV_MESSAGE = 0x04,         /* HUD message / cinematic caption (string lo16, mode b2) */
	EV_PAIN = 0x05,            /* "Pain!" and damage the player by arg */
	EV_MOVELINE = 0x06,        /* door / moving line ops, all through Game_useDoor */
	EV_SHOW = 0x07,            /* show sprite lo16 with frame b2, z = b3 - 0x20 */
	EV_DIALOG = 0x08,          /* dialog/help text hi16, style b0, flags b1; yields */
	EV_GIVEMAP = 0x09,         /* reveal automap rect (x b3, y b2, w b1, h b0) */
	EV_PASSWORD = 0x0a,        /* no-op in this game */
	EV_CHANGESTATE = 0x0b,     /* set state of tile b0,b1's event to b2 */
	EV_LOCK = 0x0c,
	EV_UNLOCK = 0x0d,
	EV_TOGGLELOCK = 0x0e,
	EV_OPENLINE = 0x0f,
	EV_CLOSELINE = 0x10,
	EV_MOVELINE2 = 0x11,
	EV_HIDE = 0x12,            /* hide sprite lo16; bit31 = every sprite on tile b0,b1 (filter tile b2) */
	EV_NEXTSTATE = 0x13,
	EV_PREVSTATE = 0x14,
	EV_INCSTAT = 0x15,
	EV_DECSTAT = 0x16,
	EV_REQSTAT = 0x17,         /* require stat b0 >= b1 else show text hi16 and abort */
	EV_FORCEMESSAGE = 0x18,
	EV_ANIM = 0x19,            /* one-shot animated sprite at tile/offset */
	EV_GIVEITEM = 0x1a,        /* Player_give(type b0, index b1, count hi16) */
	EV_SAVEGAME = 0x1b,        /* store map b0+1, tile b1,b2, angle b3 for the next save */
	EV_ABORTMOVE = 0x1c,
	EV_SCREENSHAKE = 0x1d,
	EV_CHANGEFLOORCOLOR = 0x1e,
	EV_CHANGECEILCOLOR = 0x1f,
	EV_ENABLEWEAPONS = 0x20,
	EV_OPENSTORE = 0x21,       /* store menu 0x12e+b0, price percent arg>>8; yields */
	EV_CHANGESPRITE = 0x22,    /* retile every prop on tile (arg&0x1f, (arg>>5)&0x1f) */
	EV_SPAWNPARTICLES = 0x23,
	EV_REFRESHVIEW = 0x24,
	EV_WAIT = 0x25,            /* wait arg ms; yields */
	EV_REQITEM = 0x26,         /* require inventory amount else show text hi16 and abort */
	EV_27 = 0x27,              /* no-op */
	EV_NOTE = 0x28,            /* add notebook entry lo16 and announce it */
	EV_ENDGAME = 0x29,
	EV_PLAYSOUND = 0x2a,
	EV_2B = 0x2b,              /* no-op */
	EV_SPEECHBUBBLE = 0x2c,
	EV_WAKEMONSTER = 0x2d,
	EV_LERPFLAT = 0x2e,        /* floor/ceiling height (b0 0 floor, 1 ceil) to b1 over hi16 ms */
	EV_CONTAINER = 0x2f,       /* container contents record, read by Game_openContainer */
	EV_LERPSPRITE = 0x30,      /* move sprite to absolute tile; duration (arg&0x7f)*100 ms */
	EV_RANDSTATE = 0x31,       /* set tile b0,b1's event state to random b2..b3 */
	EV_SWITCHON = 0x32,        /* set every "off" switch (eType 0xe sub 2) on tile on */
	EV_SWITCHOFF = 0x33,       /* set every "on" switch (sub 1) on tile off */
	EV_SWITCHTOGGLE = 0x34,
	EV_CAMERADELAY = 0x35,     /* camera key data, read by Game_loadCameras */
	EV_STARTCINEMATIC = 0x36,  /* play camera b0 from key b1; yields */
	EV_CAMERAKEY = 0x37,       /* camera key data, read by Game_loadCameras */
	EV_CAMERAFLAG = 0x38,      /* camera key data, read by Game_loadCameras */
	EV_LERPSPRITEFLAGS = 0x39, /* set flags b0 of the running lerp of sprite (arg>>16)&0x3ff */
	EV_FIRST_EXT = 0x3a,
};

/* Width given to TextUtil_wrapText for "never wrap". */
#define WRAP_UNLIMITED 9999

static Sprite *script_sprite(int index)
{
	return &g_app->render->sprites[index];
}

/* Sprite of an entity: entity info keeps a 1-based sprite index. */
static Sprite *entity_sprite(Entity *ent)
{
	return &g_app->render->sprites[(ent->info & 0xffff) - 1];
}

/* (rnd & 0xff) % range, as the original __rt_sdiv remainder. */
static int random_offset(int range)
{
	int r = Random_nextInt(&g_app->random) & 0xff;
	return r % range;
}

/* Shared by EV_REQSTAT / EV_REQITEM: show the map string (if any) and abort. */
static int show_requirement_failed(int strId)
{
	Canvas *canvas = g_app->canvas;

	String_setLength(&canvas->scratch, 0);
	Text_getString(canvas, canvas->mapStringGroup, strId, &canvas->scratch);
	if (canvas->scratch.len > 0)
		Canvas_showDialogStr(canvas, NULL, &canvas->scratch, 5, 0);
	return SCRIPT_ABORT;
}

/* The script waits for a UI screen (dialog, store, menu) to resume it. */
static int wait_for_ui(ScriptThread *thread)
{
	thread->unpauseTime = -1;
	return SCRIPT_YIELD;
}

/* EV_GOTO: warp the player. */
static void op_goto(int arg)
{
	Applet *app = g_app;
	Canvas *canvas = app->canvas;
	int tx = arg & 0xff;
	int ty = (arg >> 8) & 0xff;
	int dir = (arg >> 16) & 0x3ff;

	canvas->gotoX = canvas->destX;
	canvas->gotoY = canvas->destY;
	canvas->gotoAngle = canvas->destAngle;
	if (app->game->monstersMoving)
		Game_snapAllMonsterSprites(app->game, 1);
	while (app->game->combatMonsters)
		Game_removeFromCombat(app->game->combatMonsters);
	/* The original left garbage in the last two argument registers. */
	Game_checkBobAfterWarp(app->game, 0, 0, 0);
	/* Leave the old tile (its exit triggers), then clear the destination. */
	Game_executeTile(app->game, canvas->viewX >> 6, canvas->viewY >> 6, 0x20000400, 1);
	/* Kill whatever monster stands on the destination (type mask 1<<2). */
	Game_removeEntitiesAtTile(app->game, tx, ty, 4);

	canvas->destX = canvas->viewX = tx * 64 + 32;
	canvas->destY = canvas->viewY = ty * 64 + 32;
	if (dir != 0xff) {
		int angle = Dir_toAngle(app->textUtil, dir);
		canvas->viewAngle = angle;
		canvas->destAngle = angle;
		Canvas_finishRotation(canvas, 1);
	}
	Game_relinkEntity(app->player);
	canvas->frameSkip = 2;
	app->game->arrived = 1;

	/* Close the doors the player was holding open. */
	if (!app->player->isFamiliar) {
		for (int i = 0; i < 4; i++) {
			if (app->game->trackedSlots[i] != -1)
				Game_useDoor(app->game, EV_CLOSELINE, app->game->trackedSlots[i], 1);
		}
	}
}

/* EV_TRIGGER: run another tile's event inside this thread. */
static int op_trigger(ScriptThread *thread, ScriptFrame *frame, int arg)
{
	int tx = arg & 0xff;
	int ty = (arg >> 8) & 0xff;
	int result;

	if (arg & 0x80000000) {
		/* Random tile in the rectangle b0..b1 x b2..b3 (bit31 cleared). */
		int x1 = (arg >> 8) & 0xff;
		int y0 = (arg >> 16) & 0xff;
		int y1 = (arg >> 24) & 0x7f;

		ty = y0;
		if (x1 != tx)
			tx += random_offset(x1 - tx);
		if (y1 != y0)
			ty += random_offset(y1 - y0);
	}

	frame->frameFlags |= 4;
	result = ScriptThread_executeTile(thread, tx, ty, 0xf000, frame->frameFlags & 1);
	frame->frameFlags &= ~4;
	return result == SCRIPT_YIELD ? SCRIPT_YIELD : SCRIPT_DONE;
}

/* EV_MESSAGE */
static int op_message(int arg)
{
	Applet *app = g_app;
	Canvas *canvas = app->canvas;
	int mode = (arg >> 16) & 0xff;

	if (app->game->skippingCinematic)
		return SCRIPT_DONE;
	String_setLength(&canvas->scratch, 0);
	Text_getString(canvas, canvas->mapStringGroup, (int16_t)arg, &canvas->scratch);

	if (canvas->state == 0x11) {
		/* Cinematic: caption 1 is a two-line wrap, caption 2 unwrapped. */
		if (mode == 1) {
			String_setLength(&app->hud->caption1, 0);
			TextUtil_wrapTextLines(app->textUtil, &canvas->scratch, canvas->textWidths[7],
			                       &app->hud->caption1, 2, '\n', NULL);
		} else {
			String_setLength(&app->hud->caption2, 0);
			TextUtil_wrapText(app->textUtil, &canvas->scratch, WRAP_UNLIMITED, &app->hud->caption2);
		}
	} else if (mode == 2) {
		Hud_addMessageStrFlags(app->hud, &canvas->scratch, 3);
	} else {
		Hud_addMessageStr(app->hud, &canvas->scratch);
	}
	return SCRIPT_DONE;
}

/* EV_SHOW: place a sprite (frame b2, height b3) and relink its entity. */
static void op_show(int arg)
{
	Applet *app = g_app;
	Sprite *spr = script_sprite(arg & 0xffff);
	int frame = (arg >> 16) & 0xff;
	int z = (int)((uint32_t)arg >> 24) - 0x20;
	Entity *ent;

	spr->z = z;
	spr->info = (spr->info & 0xfffef0ff) | ((uint32_t)frame << 8);
	ent = spr->ent;
	if (!ent)
		return;

	if (z != 0x20 || frame != 0)
		ent->info |= 0x40000000;
	if (spr->ent->def->eType == 2) {
		spr->ent->monster->frameTime = (int32_t)GETUPTIMEMS();
		spr->ent->monster->flags |= 0x8000;
	}
	Game_removeEntitiesAtTile(app->game, spr->x >> 6, spr->y >> 6, 0x404);
	if (spr->ent->info & 0x4000000)
		Game_unlinkEntity_43710(app->game, spr->ent);
	Game_linkEntity(app->game, spr->ent, spr->x >> 6, spr->y >> 6);

	/* 0x6a (SHOW + wake monster, script_ext.c) continues here in the original. */
}

/* EV_DIALOG */
static int op_dialog(ScriptThread *thread, int arg)
{
	Applet *app = g_app;
	Player *player = app->player;
	Canvas *canvas = app->canvas;
	int style = arg & 0xff;
	int flags = (arg >> 8) & 0xff;
	int strId = (int16_t)((uint32_t)arg >> 16);

	if (app->game->f9be8)
		return SCRIPT_DONE;
	if (style == 6 || style == 7 || style == 5)
		player->inCombat = 0;

	if (style == 1) {
		/* NPC talk: put the weapon away for the conversation. */
		if ((player->weapons & 1) && player->ce.weapon != 0) {
			player->prevWeapon = player->ce.weapon;
			app->player->tempWeapon = 1;
			Player_selectWeapon(app->player, 0);
		}
		Canvas_showDialogText(canvas, thread, canvas->mapStringGroup, strId, 1, 1, 1);
		app->hud->numLines = 0;
	} else if (style == 2) {
		player->prevWeapon = player->ce.weapon;
		Canvas_addHelpText(canvas, canvas->mapStringGroup, strId, thread);
	} else {
		if (style == 4)
			player->prevWeapon = player->ce.weapon;
		Canvas_showDialogText(canvas, thread, canvas->mapStringGroup, strId, style, flags, 1);
	}
	app->game->threadWaiting = 1;
	return SCRIPT_YIELD;
}

/* EV_HIDE */
static void op_hide(int arg)
{
	Applet *app = g_app;
	Render *render = app->render;

	if (arg & 0x80000000) {
		/* Every unlocked sprite on tile b0,b1; b2 != 0 filters by tile id
		 * (bit30 inverts the filter). */
		int tx = arg & 0xff;
		int ty = (arg >> 8) & 0xff;
		int tile = (arg >> 16) & 0xff;
		int invert = (arg & 0x40000000) != 0;

		for (int i = 0; i < render->numSprites; i++) {
			Sprite *spr = &render->sprites[i];
			uint32_t info = spr->info;
			int sprTile = info & 0xff;

			if (info & 0x40000)
				sprTile += 0x101;
			if (tile != 0) {
				if (!invert && tile != sprTile)
					continue;
				if (invert && tile == sprTile)
					continue;
			}
			if (tx != spr->x >> 6 || ty != spr->y >> 6 || (info & 0x780000))
				continue;
			spr->info = info | 0x10000;
			if (spr->ent) {
				spr->ent->info |= 0x40000000;
				Game_unlinkEntity_43710(app->game, spr->ent);
			}
		}
		return;
	}

	Sprite *spr = script_sprite(arg); /* the whole arg (bit 31 is clear here), as at 0x15ec8 */
	spr->info |= 0x10000;
	if (spr->ent) {
		int tx, ty;
		uint16_t flags;

		spr->ent->info |= 0x40000000;
		Game_unlinkEntity_43710(app->game, spr->ent);
		tx = spr->x >> 6;
		ty = spr->y >> 6;
		flags = app->render->mapFlags[tx + ty * 32];
		if ((flags & 1) && (flags & 2))
			Game_unlinkPlayerFromTile(app->game, tx, ty);
	}
	Canvas_updateFacing(app->canvas);
}

/* EV_CHANGESTATE / NEXTSTATE / PREVSTATE / RANDSTATE: event word bits 27..29. */
static void op_change_state(int opcode, int arg)
{
	Render *render = g_app->render;
	int tx = arg & 0xff;
	int ty = (arg >> 8) & 0xff;
	uint32_t state = (arg >> 16) & 0xff;
	int idx = Render_findTileEvent(render, tx + ty * 32);
	uint32_t event;

	if (idx == -1)
		return;
	event = render->tileEvents[idx];
	if (opcode != EV_CHANGESTATE) {
		uint32_t cur = (event >> 27) & 7;
		if (opcode == EV_RANDSTATE) {
			int hi = (int)((uint32_t)arg >> 24);
			state = (uint32_t)(random_offset(hi - (int)state + 1) + (int)state);
		} else {
			state = cur;
			if (opcode == EV_NEXTSTATE) {
				if (cur < 7)
					state = cur + 1;
			} else if (opcode == EV_PREVSTATE) {
				if (cur > 0)
					state = cur - 1;
			}
		}
	}
	render->tileEvents[idx] = (event & 0xc7ffffff) | (state << 27);
}

/* EV_ANIM: one-shot sprite animation at a tile, offset by (dx,dy)*8 units. */
static void op_anim(int arg)
{
	int tileIdx = arg & 0x3ff;
	int tile = (arg >> 10) & 0xff;
	int dx = ((arg >> 18) & 0xf) - 4;
	int dy = ((arg >> 22) & 0xf) - 4;
	/* Offsets at the extremes are pulled one unit back inside the tile. */
	int adjX = dx == -4 ? 1 : (dx == 4 ? -1 : 0);
	int adjY = dy == -4 ? 1 : (dy == 4 ? -1 : 0);
	int x = (tileIdx % 32) * 64 + dx * 8 + adjX + 0x20;
	int y = (tileIdx / 32) * 64 + dy * 8 + adjY + 0x20;

	Game_gspriteAllocAnim(g_app->game, tile, x, y, 0x20);
}

/* EV_CHANGESPRITE */
static void op_change_sprite(int arg)
{
	Applet *app = g_app;
	int tx = arg & 0x1f;
	int ty = (arg >> 5) & 0x1f;
	int frame = (arg >> 10) & 7;
	int tile = arg >> 13;
	Entity *ent;

	/* Tiles above 0x100 use the second sprite bank (info bit 0x40000). */
	if (tile >= 0x101)
		tile = (tile - 0x101) | 0x40000;

	for (ent = (Entity *)Game_getEntitiesAt(app->game, tx << 6, ty << 6); ent; ent = ent->nextOnTile) {
		int type = ent->def->eType;
		Sprite *spr;

		if (type == 1 || (ent->info & 0x200000) || type == 2 || type == 0)
			continue;
		spr = entity_sprite(ent);
		spr->info = (spr->info & 0xfffff000) | (uint32_t)tile | ((uint32_t)frame << 8) | 0x80000000;
		ent->info |= 0x40000000;
	}
}

/* EV_WAIT */
static int op_wait(ScriptThread *thread, ScriptFrame *frame, int arg)
{
	Applet *app = g_app;
	Canvas *canvas = app->canvas;

	thread->unpauseTime = (int32_t)GETUPTIMEMS() + arg;
	if (!(frame->frameFlags & 1))
		return SCRIPT_YIELD;

	/* Inside a cinematic: the camera timeline pauses too. */
	app->game->cameraStartTime += arg;
	if (canvas->state == 0x11)
		return SCRIPT_YIELD;
	canvas->waitTime = thread->unpauseTime;
	if (canvas->state == 5)
		Canvas_setState(canvas, 3);
	if (canvas->state == 3) {
		String left, right;

		String_ctor(&left);
		String_ctor(&right);
		Canvas_drawSoftKeys_10c68(canvas, &left, &right, 1);
		String_dtor(&right, 0);
		String_dtor(&left, 0);
	}
	return SCRIPT_YIELD;
}

/* EV_SPEECHBUBBLE */
static void op_speech_bubble(int arg)
{
	Applet *app = g_app;
	int strId = (int16_t)arg;

	if (app->game->skippingCinematic)
		return;
	if (arg < 0) {
		Hud_clearLines(app->hud);
		app->hud->bubbleMode = 1;
	}
	if (strId == 0) {
		Hud_clearLines(app->hud);
		app->hud->bubbleMode = 0;
	} else {
		Hud_addTimedMsgFile(app->hud, app->canvas->mapStringGroup, strId);
	}
}

/* EV_WAKEMONSTER */
static int op_wake_monster(int arg)
{
	Entity *ent = script_sprite(arg)->ent;

	if (!ent)
		return 0;
	if (ent->def->eType == 2 && (ent->info & 0x200a0000) == 0) {
		ent->info |= 0x10000000;
		Game_activate_4168(g_app->game, script_sprite(arg)->ent);
	}
	return SCRIPT_DONE;
}

/* EV_LERPFLAT */
static void op_lerp_flat(int arg)
{
	Applet *app = g_app;
	Render *render = app->render;
	int which = arg & 0xff;   /* 0 floor, else ceiling */
	int height = (arg >> 8) & 0xff;
	int duration = arg >> 16;

	if (duration == 0) {
		if (which != 0)
			render->ceilHeight = (int16_t)height;
		else
			render->floorHeight = (int16_t)height;
		return;
	}
	render->flatLerps[which].duration = duration;
	render->flatLerps[which].startTime = app->time;
	render->flatLerps[which].to = height;
	render->flatLerps[which].from = which != 0 ? render->effCeil : render->effFloor;
}

/* EV_LERPSPRITE (absolute destination). 0x4a/0x5e/0x5f are variants handled
 * in script_ext.c. */
static int op_lerp_sprite(ScriptThread *thread, ScriptFrame *frame, int arg)
{
	Applet *app = g_app;
	int index = (arg >> 7) & 0x3ff;
	int duration = arg & 0x7f;
	int tx = (arg >> 17) & 0x1f;
	int ty = (arg >> 22) & 0x1f;
	int tz = (int)((uint32_t)arg >> 27);
	Sprite *spr = script_sprite(index);
	LerpSprite *ls = Game_allocLerpSprite(app->game, thread, index);
	Entity *ent = spr->ent;

	/* Tile 0x77 is a two-state prop: param 1 rumbles and resets its frame. */
	if (ent && ent->def->tileIndex == 0x77) {
		if (ent->param == 1) {
			Sound_play(app->sndSys, 0x13, 0, 2, -1);
			Canvas_startShake(app->canvas, 500, 3, 200);
			spr->info &= 0x7ffff0ff;
		} else {
			spr->info |= 0x80000000;
			ent->info |= 0x40000000;
		}
	}
	if (!ls)
		return 0;

	ls->dstX = tx * 64 + 0x20;
	ls->dstY = ty * 64 + 0x20;
	if (spr->ent) {
		uint32_t info = spr->ent->info & ~0x400000u;
		spr->ent->info = info;
		info |= 0x40000000;
		spr->ent->info = info;
		if (duration > 0)
			spr->ent->info = info | 0x08000000;
	}
	ls->dstZ = tz * 8 - 0x20;
	ls->srcX = spr->x;
	ls->srcY = spr->y;
	ls->srcZ = spr->z;
	ls->srcScale = ls->dstScale = spr->scale;
	ls->startTime = app->playTime;
	ls->travelTime = duration * 100;
	LerpSprite_calcDist(ls);
	ls->flags = 0;

	if (duration == 0)
		return (Game_updateMover(app->game, ls) & 1) ? SCRIPT_DONE : 0;
	if (!(frame->frameFlags & 1))
		app->game->threadWaiting = 1;
	return SCRIPT_YIELD;
}

/* EV_SWITCHON / OFF / TOGGLE: switches (eType 0xe) on the tile. The y
 * coordinate is the whole arg>>8 like the original (scripts keep b2,b3 zero). */
static void op_switches(int opcode, int arg)
{
	Applet *app = g_app;
	int x = (arg & 0xff) << 6;
	int y = (arg >> 8) << 6;

	for (Entity *ent = (Entity *)Game_getEntitiesAt(app->game, x, y); ent; ent = ent->nextOnTile) {
		if (ent->def->eType != 0xe)
			continue;
		int sub = ent->def->eSubType;
		if (opcode == EV_SWITCHON) {
			if (sub == 2)
				Game_setSwitch(app->game, ent, 1);
		} else if (opcode == EV_SWITCHOFF) {
			if (sub == 1)
				Game_setSwitch(app->game, ent, 0);
		} else if (sub == 1) {
			Game_setSwitch(app->game, ent, 0);
		} else if (sub == 2) {
			Game_setSwitch(app->game, ent, 1);
		}
	}
	if (opcode != EV_SWITCHTOGGLE)
		Sound_play(app->sndSys, 0x15, 0, 3, -1);
}

/* EV_STARTCINEMATIC */
static int op_start_cinematic(ScriptThread *thread, int arg)
{
	Applet *app = g_app;
	Game *game = app->game;
	int camIdx = arg & 0xff;
	int startKey = (arg >> 8) & 0xff;
	Camera *cam;
	CameraKey *key;

	if (startKey <= 0)
		return SCRIPT_DONE;
	game->cameraKeyIdx = startKey - 1;
	game->activeCamera = &game->cameras[camIdx];
	cam = game->activeCamera;

	if (startKey == 1) {
		/* Keys flagged 1/2/4/8 start from the current view x/y/z/angle. */
		for (int i = 0; i < cam->numKeys; i++) {
			CameraKey *k = &game->cameraKeys[cam->firstKey + i];
			uint8_t flags = k->flags;

			if (flags & 1)
				k->x = app->canvas->viewX;
			if (flags & 2)
				k->y = app->canvas->viewY;
			if (flags & 4)
				k->z = app->canvas->viewZ;
			if (flags & 8)
				k->angle = app->canvas->viewAngle & 0x3ff;
		}
	}

	key = &game->cameraKeys[cam->firstKey + game->cameraKeyIdx];
	cam->keyDone = 0;
	cam->x = key->x;
	cam->y = key->y;
	cam->z = key->z;
	cam->angle = key->angle;
	cam->thread = thread;
	game->cameraStartTime = (int32_t)GETUPTIMEMS();
	if (app->canvas->state != 2 && app->canvas->state != 0x11)
		Canvas_setState(app->canvas, 0x11);
	game->threadWaiting = 1;
	return SCRIPT_YIELD;
}

/* 0x00015464 */
int ScriptThread_executeCommand(ScriptThread *thread, ScriptFrame *frame, int opcode, int arg)
{
	Applet *app = g_app;
	Canvas *canvas = app->canvas;
	int b0 = arg & 0xff;
	int b1 = (arg >> 8) & 0xff;
	int b2 = (arg >> 16) & 0xff;
	int b3 = (int)((uint32_t)arg >> 24);

	/* The jump table covers 0..0x73 (unsigned compare); anything else is a no-op. */
	if ((uint32_t)opcode > 0x73)
		return SCRIPT_DONE;
	if (opcode >= EV_FIRST_EXT)
		return ScriptThread_executeExtCommand(thread, frame, opcode, arg);

	switch (opcode) {
	case EV_NOP:
	case EV_PASSWORD:
	case EV_27:
	case EV_2B:
	case EV_CONTAINER:
	case EV_CAMERADELAY:
	case EV_CAMERAKEY:
	case EV_CAMERAFLAG:
		break;

	case EV_GOTO:
		op_goto(arg);
		break;

	case EV_CHANGEMAP:
		app->game->pendingChangeMap = (uint32_t)arg;
		if (!(arg & 0x40000000)) {
			Game_doChangeMap(app->game);
			return SCRIPT_ABORT;
		}
		/* Fade out first; the change happens when the fade completes. */
		app->game->pendingChangeMap &= ~0x40000000u;
		Canvas_requestState12(canvas, 1000, 2);
		Canvas_setState(app->canvas, 0x12);
		return SCRIPT_ABORT;

	case EV_TRIGGER:
		return op_trigger(thread, frame, arg);

	case EV_MESSAGE:
		return op_message(arg);

	case EV_PAIN:
		Hud_addMessage(app->hud, 0xca); /* "Pain!" */
		Player_hurtFeedback(app->player, 0);
		Player_takeDamage(app->player, arg, NULL);
		break;

	case EV_MOVELINE:
	case EV_OPENLINE:
	case EV_CLOSELINE:
	case EV_MOVELINE2:
		if (!Game_useDoor(app->game, opcode, arg, 1))
			return 0;
		break;

	case EV_SHOW:
		op_show(arg);
		break;

	case EV_DIALOG:
		return op_dialog(thread, arg);

	case EV_GIVEMAP:
		Game_revealAutomapRect(app->game, b3, b2, b1, b0);
		break;

	case EV_CHANGESTATE:
	case EV_NEXTSTATE:
	case EV_PREVSTATE:
	case EV_RANDSTATE:
		op_change_state(opcode, arg);
		break;

	case EV_LOCK:
		Game_setLineLocked(app->game, arg, 1, 0);
		break;
	case EV_UNLOCK:
		Game_setLineLocked(app->game, arg, 0, 0);
		break;
	case EV_TOGGLELOCK:
		Game_setLineLocked(app->game, arg, (app->render->lines[arg].flags & 0x400) ? 0 : 1, 0);
		break;

	case EV_HIDE:
		op_hide(arg);
		break;

	case EV_INCSTAT:
	case EV_DECSTAT:
		Player_modifyStat(app->player, b0, opcode == EV_INCSTAT ? b1 : -b1);
		break;

	case EV_REQSTAT:
		if (Player_requireStat(app->player, b0, b1))
			return SCRIPT_DONE;
		return show_requirement_failed(arg >> 16);

	case EV_FORCEMESSAGE:
		String_setLength(&canvas->scratch, 0);
		Text_getString(canvas, canvas->mapStringGroup, (int16_t)arg, &canvas->scratch);
		String_setLength(&app->hud->forcedMsg, 0);
		if (canvas->scratch.len > 0)
			TextUtil_wrapText(app->textUtil, &canvas->scratch, WRAP_UNLIMITED, &app->hud->forcedMsg);
		break;

	case EV_ANIM:
		op_anim(arg);
		break;

	case EV_GIVEITEM:
		Player_give(app->player, b0, b1, arg >> 16, 0);
		if (app->canvas->state != 7)
			return SCRIPT_DONE;
		/* Given from inside a dialog: continue when it closes. */
		return wait_for_ui(thread);

	case EV_SAVEGAME:
		app->game->savedMapId = b0 + 1;
		app->game->changeMapX = b1 * 64 + 32;
		app->game->changeMapY = b2 * 64 + 32;
		app->game->changeMapAngle = b3;
		break;

	case EV_ABORTMOVE:
		canvas->abortMove = 1;
		break;

	case EV_SCREENSHAKE:
		if (app->game->skippingCinematic)
			return SCRIPT_DONE;
		Canvas_startShake(canvas, arg & 0xfff, arg >> 24, (arg >> 12) & 0xfff);
		break;

	case EV_CHANGEFLOORCOLOR:
		app->render->floorColor = arg;
		break;
	case EV_CHANGECEILCOLOR:
		app->render->ceilColor = arg;
		break;

	case EV_ENABLEWEAPONS:
		if (arg == 0)
			Player_resetWeapons(app->player);
		else
			Player_refreshWeapons(app->player);
		break;

	case EV_OPENSTORE:
		app->menu->pricePercent = arg >> 8;
		app->menu->fromStore = 0;
		MenuSystem_setMenu(app->menuSystem, b0 + 0x12e);
		app->menu->f4c = 0;
		app->menu->thread = thread;
		return wait_for_ui(thread);

	case EV_CHANGESPRITE:
		op_change_sprite(arg);
		break;

	case EV_SPAWNPARTICLES:
		if (app->game->skippingCinematic)
			return SCRIPT_DONE;
		Script_combatEffect(0, (arg >> 24) & 0x1f,
		                    (int)(0xff000000u | ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | (uint32_t)b2),
		                    arg >> 29);
		break;

	case EV_REFRESHVIEW:
		Canvas_updateFacing(canvas);
		break;

	case EV_WAIT:
		return op_wait(thread, frame, arg);

	case EV_REQITEM: {
		/* b1 bit0 set: need at least b1>>1; clear: at most b1>>1. */
		int amountMin, amountMax;
		if (b1 & 1) {
			amountMin = b1 >> 1;
			amountMax = 9999999;
		} else {
			amountMin = 0;
			amountMax = b1 >> 1;
		}
		if (Player_requireItem(app->player, arg & 0xf, b0 >> 4, amountMin, amountMax))
			return SCRIPT_DONE;
		return show_requirement_failed((int16_t)((uint32_t)arg >> 16));
	}

	case EV_NOTE:
		Player_addNotebookIndex(app->player, (int16_t)arg);
		Canvas_showDialogText0(app->canvas, thread, 0x12, 3, 0);
		Player_showHelp(app->player, 6, 0);
		break;

	case EV_ENDGAME:
		app->player->gameCompleted = 1;
		app->canvas->endGame = 1;
		app->game->skippingCinematic = 0;
		app->game->skipUntilTime = 0;
		app->game->cinematicActive = 0;
		Canvas_setState(app->canvas, 0xd);
		return SCRIPT_ABORT;

	case EV_PLAYSOUND:
		if (app->game->skippingCinematic)
			return SCRIPT_DONE;
		Sound_play(app->sndSys, (int16_t)arg, 0, arg >> 16, -1);
		break;

	case EV_SPEECHBUBBLE:
		op_speech_bubble(arg);
		break;

	case EV_WAKEMONSTER:
		return op_wake_monster(arg);

	case EV_LERPFLAT:
		op_lerp_flat(arg);
		break;

	case EV_LERPSPRITE:
		return op_lerp_sprite(thread, frame, arg);

	case EV_SWITCHON:
	case EV_SWITCHOFF:
	case EV_SWITCHTOGGLE:
		op_switches(opcode, arg);
		break;

	case EV_STARTCINEMATIC:
		return op_start_cinematic(thread, arg);

	case EV_LERPSPRITEFLAGS: {
		uint32_t sprite = (arg >> 16) & 0x3ff;
		for (int i = 0; i < 16; i++) {
			if ((uint32_t)(app->game->lerpSprites[i].hSprite - 1) == sprite) {
				app->game->lerpSprites[i].flags = b0;
				break;
			}
		}
		break;
	}

	default:
		break;
	}
	return SCRIPT_DONE;
}
