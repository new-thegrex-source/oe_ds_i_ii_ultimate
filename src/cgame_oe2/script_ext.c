/*
 * Tile-event interpreter, upper half: opcodes 0x3a..0x73 of
 * ScriptThread_executeCommand (0x15464). The lower half (0x00..0x39) and the
 * dispatcher live in script.c, which calls ScriptThread_executeExtCommand for
 * every opcode >= 0x3a.
 *
 * In the original all 116 opcodes are one switch; several cases in this range
 * jump into code shared with lower opcodes (0x4b with TRIGGER 0x03, 0x6a with
 * SHOW 0x07, 0x4a/0x5e/0x5f with LERPSPRITE 0x30, 0x50/0x67 into the
 * "wait for the loot dialog" tail). Those shared tails are re-implemented here
 * as static helpers.
 *
 * Script sprite ids index Render.sprites[] directly (0-based); an entity's
 * info low 16 bits hold its sprite index + 1.
 *
 * Return codes: 0 not run, 1 ok, 2 yield (thread suspended), 3 abort event,
 * 4 restart event. Unknown / no-op opcodes return 1 like the original switch.
 */

#include "game.h"

/* Debug error formats used by MARKTILE/UNMARKTILE (executable data area). */
#define MOD_STR_MARKTILE_ERROR   0x45e20u /* "Invalid location %d for EV_MARKTILE" */
#define MOD_STR_UNMARKTILE_ERROR 0x45e44u /* "Invalid location %d for EV_UNMARKTILE" */

int ScriptThread_executeExtCommand(ScriptThread *thread, ScriptFrame *frame, int opcode, int arg);

enum {
	SCRIPT_NOT_RUN = 0,
	SCRIPT_OK = 1,
	SCRIPT_YIELD = 2,
	SCRIPT_ABORT = 3,
	SCRIPT_RESTART = 4,
};

/* Opcodes handled in this file (names are ours; behaviour in comments below). */
enum {
	EV_NOP_3A = 0x3a,
	EV_NOP_3B = 0x3b,
	EV_SETNPCFLAG = 0x3c,         /* eType 3 entity: param = flag */
	EV_CINEMATIC_BEGIN = 0x3d,    /* open a skippable cinematic window */
	EV_CINEMATIC_END = 0x3e,
	EV_FLOORUNIFORM = 0x3f,
	EV_CEILUNIFORM = 0x40,
	EV_VIEWBOB = 0x41,
	EV_NAMEENTITY = 0x42,
	EV_SETMONSTERSTAT = 0x43,
	EV_MONSTERFLAGOP = 0x44,
	EV_CALMMONSTER = 0x45,        /* clear effects, drop flee goal, leave combat */
	EV_DEACTIVATEMONSTER = 0x46,
	EV_GIVETAKEWEAPON = 0x47,
	EV_SETENTITYPARAM = 0x48,     /* same code as 0x70 */
	EV_TAKEITEM = 0x49,
	EV_LERPSPRITE_ASYNC = 0x4a,
	EV_SPAWNTHREAD = 0x4b,        /* run a tile's event on a new thread */
	EV_ENDFAMILIAR = 0x4c,
	EV_KILLATTILE = 0x4d,         /* or wake all marked monsters */
	EV_AIGOAL_MOVE = 0x4e,
	EV_MAKECORPSE = 0x4f,
	EV_OPENRELIQUARY = 0x50,
	EV_LERPSCALE = 0x51,
	EV_QUESTFLAG = 0x52,
	EV_AWARDSECRET = 0x53,
	EV_PAYGOLD = 0x54,
	EV_NOP_55 = 0x55,
	EV_DAMAGE = 0x56,
	EV_ADDNOTE = 0x57,            /* NOTE (0x28) without the popup */
	EV_NOP_58 = 0x58,
	EV_SETAIGOAL = 0x59,
	EV_NOP_5A = 0x5a,
	EV_FADE = 0x5b,
	EV_KNOCKBACKPLAYER = 0x5c,
	EV_NOP_5D = 0x5d,
	EV_LERPSPRITE_TOSPRITE = 0x5e,
	EV_LERPSPRITE_RELATIVE = 0x5f,
	EV_NOP_60 = 0x60,
	EV_NOP_61 = 0x61,
	EV_SPRITEFRAME = 0x62,
	EV_RESTART = 0x63,
	EV_AIMEDEFFECT = 0x64,
	EV_ENDCOMBAT = 0x65,
	EV_NOP_66 = 0x66,
	EV_OPENBOOKSHELF = 0x67,
	EV_NOP_68 = 0x68,
	EV_STEALMSG = 0x69,
	EV_SHOWANDWAKE = 0x6a,
	EV_AUTOMAPFLAG = 0x6b,
	EV_MARKTILE = 0x6c,
	EV_UNMARKTILE = 0x6d,
	EV_TURNPLAYER = 0x6e,
	EV_CLEARBUFFS = 0x6f,
	EV_SETENTITYPARAM2 = 0x70,
	EV_CLEARENTITYFLAG = 0x71,
	EV_SETENTITYPARAMBIT = 0x72,
	EV_SETNODEATH = 0x73,
};

/* Entity.info bits touched here */
#define ENT_ACTIVE        0x00080000u /* on the active monster ring */
#define ENT_NAMED         0x00080000u /* same bit; NAMEENTITY sets it on doors / container type 1 */
#define ENT_NOT_LERPING   0x00400000u
#define ENT_LINKED        0x04000000u /* on a tile list */
#define ENT_LERPING       0x08000000u
#define ENT_ALERTED       0x10000000u
#define ENT_DIRTY         0x40000000u /* state differs from the map; saved */
#define ENT_FLAG31        0x80000000u
#define ENT_WAKE_BLOCKERS 0x200a0000u /* already awake / dead / ... */

/* Sprite.info bits */
#define SPR_TILE_MASK   0x000000ffu
#define SPR_FRAME_MASK  0x00000f00u
#define SPR_HIDDEN      0x00010000u
#define SPR_ANIMATING   0x80000000u

/* EntityDef.eType values used here */
#define ETYPE_MONSTER   2
#define ETYPE_NPC       3
#define ETYPE_CONTAINER 7
#define ETYPE_CORPSE    9
#define ETYPE_DOOR      0xc

/* ScriptFrame.frameFlags */
#define FRAME_NESTED    0x01

/* Resource ids */
#define SND_CHEST_OPEN  0x5a
#define SND_SECRET      0x54
#define SND_GOLD        0x11
#define SND_ROCK_SLIDE  0x13
#define TILE_BOULDER    0x77 /* EntityDef.tileIndex of the rolling boulder */

static Sprite *scriptSprite(unsigned idx)
{
	return &g_app->render->sprites[idx];
}

/* Unsigned bit-field extract: bits [lo, lo+n) of v */
static uint32_t bits(int v, int lo, int n)
{
	return ((uint32_t)v >> lo) & ((n >= 32) ? 0xffffffffu : ((1u << n) - 1));
}

/* Shared tail of 0x4a / 0x5e / 0x5f (and LERPSPRITE 0x30 in script.c): move a
 * sprite along a line over (arg & 0x7f) * 100 ms. */
static int lerpSpriteCommand(ScriptThread *thread, ScriptFrame *frame, int opcode, int arg)
{
	Applet *app = g_app;
	unsigned spriteIdx = bits(arg, 7, 10);
	int duration = arg & 0x7f;
	int tx, ty, z;

	if (opcode == EV_LERPSPRITE_TOSPRITE) {
		/* destination = another sprite's tile; z keeps the original's
		 * "z * 8 - 32" encoding applied to the raw sprite z */
		Sprite *target = scriptSprite(bits(arg, 17, 10));
		tx = target->x >> 6;
		ty = target->y >> 6;
		z = target->z;
	} else {
		if (opcode == EV_LERPSPRITE_RELATIVE) {
			tx = (int)bits(arg, 17, 5) - 10;
			ty = (int)bits(arg, 22, 5) - 10;
		} else {
			tx = (int)bits(arg, 17, 5);
			ty = (int)bits(arg, 22, 5);
		}
		z = (int)bits(arg, 27, 5);
	}
	int destZ = z * 8 - 0x20;
	int relative = (opcode == EV_LERPSPRITE_RELATIVE);

	Sprite *spr = scriptSprite(spriteIdx);
	LerpSprite *ls = Game_allocLerpSprite(app->game, thread, (int)spriteIdx);
	Entity *ent = spr->ent;

	/* The boulder: param 1 = rolls with a rumble, otherwise just marks it. */
	if (ent != NULL && (uint16_t)ent->def->tileIndex == TILE_BOULDER) {
		if (ent->param == 1) {
			Sound_play(app->sndSys, SND_ROCK_SLIDE, 0, 2, -1);
			Canvas_startShake(app->canvas, 500, 3, 200);
			spr->info &= ~(SPR_ANIMATING | SPR_FRAME_MASK);
		} else {
			spr->info |= SPR_ANIMATING;
			ent->info |= ENT_DIRTY;
		}
	}
	if (ls == NULL)
		return SCRIPT_NOT_RUN;

	if (!relative) {
		ls->dstX = 0x20 + (tx << 6);
		ls->dstY = 0x20 + (ty << 6);
	} else {
		ls->dstX = spr->x + (tx << 6);
		ls->dstY = spr->y + (ty << 6);
	}

	ent = spr->ent;
	if (ent != NULL) {
		uint32_t info = ent->info & ~ENT_NOT_LERPING;
		ent->info = info;
		info |= ENT_DIRTY;
		ent->info = info;
		if (duration > 0)
			ent->info = info | ENT_LERPING;
	}

	ls->dstZ = destZ;
	ls->srcX = spr->x;
	ls->srcY = spr->y;
	ls->srcZ = spr->z;
	ls->srcScale = spr->scale;
	ls->dstScale = spr->scale;
	ls->startTime = app->playTime;
	ls->travelTime = duration * 100;

	/* dist = sqrt((dx^2 + dy^2) << 8) >> 8 (64-bit fixed sqrt) */
	int dx = ls->dstX - ls->srcX;
	int dy = ls->dstY - ls->srcY;
	int distSq = (int)(((uint32_t)dy * (uint32_t)dy + (uint32_t)dx * (uint32_t)dx) << 8);
	ls->dist = (int)(Game_FixedSqrt(app->game, distSq, distSq >> 31) >> 8);

	int async = (opcode == EV_LERPSPRITE_ASYNC);
	ls->flags = async;

	if (duration == 0) {
		/* instant: finish the move now */
		return (Game_updateMover(app->game, ls) & 1) ? SCRIPT_OK : SCRIPT_NOT_RUN;
	}
	if (async)
		return SCRIPT_OK;
	if (!(frame->frameFlags & FRAME_NESTED))
		app->game->threadWaiting = 1;
	return SCRIPT_YIELD;
}

/* Shared with SHOW (0x07): place sprite (arg & 0xffff) with frame (arg >> 16) & 0xff
 * and z (arg >> 24) - 32, link its entity to the tile; 0x6a also wakes monsters. */
static void showSpriteCommand(int opcode, int arg)
{
	Applet *app = g_app;
	Sprite *spr = scriptSprite(arg & 0xffff);
	uint32_t frame = bits(arg, 16, 8);
	int z = (int)((uint32_t)arg >> 24) - 0x20;

	spr->z = z;
	spr->info = (spr->info & ~(SPR_HIDDEN | SPR_FRAME_MASK)) | (frame << 8);

	Entity *ent = spr->ent;
	if (ent == NULL)
		return;
	if (z != 0x20 || frame != 0)
		ent->info |= ENT_DIRTY;
	if (spr->ent->def->eType == ETYPE_MONSTER) {
		spr->ent->monster->frameTime = (int)GETUPTIMEMS();
		spr->ent->monster->flags |= 0x8000;
	}
	/* crush whatever stands on the destination tile (mask 0x404) */
	Game_removeEntitiesAtTile(app->game, spr->x >> 6, spr->y >> 6, 0x404);
	if (spr->ent->info & ENT_LINKED)
		Game_unlinkEntity_43710(app->game, spr->ent);
	Game_linkEntity(app->game, spr->ent, spr->x >> 6, spr->y >> 6);

	if (opcode == EV_SHOWANDWAKE) {
		ent = spr->ent;
		if (ent->def->eType == ETYPE_MONSTER && !(ent->info & ENT_WAKE_BLOCKERS)) {
			ent->info |= ENT_ALERTED;
			Game_activate_4168(app->game, spr->ent);
		}
	}
}

/* Shared tail of 0x50 / 0x67: open the first container of the given subtype on
 * tile (arg & 0xff, (arg >> 8)), then suspend the thread until the loot
 * dialog closes (unpauseTime = -1 is resumed by the UI). */
static int openContainerCommand(ScriptThread *thread, int arg, int subType)
{
	Applet *app = g_app;
	int x = (arg & 0xff) << 6;
	int y = (int)((uint32_t)(arg >> 8) << 6); /* original uses the whole arg >> 8 */

	for (Entity *ent = Game_getEntitiesAt(app->game, x, y); ent != NULL; ent = ent->nextOnTile) {
		if (ent->def->eType != ETYPE_CONTAINER || ent->def->eSubType != subType)
			continue;
		Sprite *spr = &app->render->sprites[(ent->info & 0xffff) - 1];
		int unlink = 1;
		if (subType == 3) {
			/* reliquary: switch to the open tile */
			spr->info = (spr->info & ~SPR_TILE_MASK) | 0x9b;
			ent->info |= ENT_DIRTY;
		} else {
			/* bookshelf: param 1 stays in place showing frame 1 */
			ent->info |= ENT_DIRTY;
			if (ent->def->parm == 1) {
				spr->info = (spr->info & ~SPR_FRAME_MASK) | 0x100;
				unlink = 0;
			}
		}
		if (unlink)
			Game_unlinkEntity_43710(app->game, ent);
		Game_openContainer(ent);
		Sound_play(app->sndSys, SND_CHEST_OPEN, 0, 2, -1);
		break;
	}
	thread->unpauseTime = -1;
	return SCRIPT_YIELD;
}

/* Set or clear one of the 7 MARKTILE location bits in Render.mapFlags. */
static int markTileCommand(int arg, int set)
{
	Applet *app = g_app;
	int loc = arg >> 16;
	uint16_t bit;

	switch (loc) {
	case 0: bit = 0x40; break;
	case 1: bit = 0x08; break;
	case 2: bit = 0x80; break;
	case 3: bit = 0x100; break;
	case 4: bit = 0x200; break;
	case 5: bit = 0x400; break;
	case 6: bit = 0x800; break;
	default:
		App_setErrorStr(app, (char *)ModData_str(&g_mod, set ? MOD_STR_MARKTILE_ERROR : MOD_STR_UNMARKTILE_ERROR), loc);
		App_setErrorCode(g_app, 1);
		return SCRIPT_NOT_RUN;
	}
	uint16_t *flags = &app->render->mapFlags[arg & 0xffff];
	if (set)
		*flags |= bit;
	else
		*flags &= (uint16_t)~bit;
	return SCRIPT_OK;
}

/* Opcodes 0x3a..0x73 of ScriptThread_executeCommand (0x15464, upper half). Same return codes. */
int ScriptThread_executeExtCommand(ScriptThread *thread, ScriptFrame *frame, int opcode, int arg)
{
	Applet *app = g_app;
	Canvas *canvas = app->canvas;
	Render *render = app->render;
	Game *game = app->game;
	Player *player = app->player;
	int lo16 = arg & 0xffff;
	int hi16s = arg >> 16;            /* signed upper half */
	int b0 = arg & 0xff;
	int b1 = (int)bits(arg, 8, 8);
	int b2 = (int)bits(arg, 16, 8);
	Entity *ent;
	Sprite *spr;

	switch (opcode) {
	case EV_NOP_3A:
	case EV_NOP_3B:
		break;

	case EV_SETNPCFLAG:
		/* NPC (eType 3) sprite lo16: param = (b2 != 0) */
		ent = scriptSprite(lo16)->ent;
		if (ent == NULL || ent->def->eType != ETYPE_NPC)
			return SCRIPT_OK;
		ent->param = (arg & 0xff0000) != 0;
		scriptSprite(lo16)->ent->info |= ENT_DIRTY;
		break;

	case EV_CINEMATIC_BEGIN:
		/* arg 0: input may skip after 1 s; otherwise never */
		game->skippingCinematic = 0;
		canvas->moveCount = 0;
		game->skipUntilTime = (arg == 0) ? (int)GETUPTIMEMS() + 1000 : 0x7fffffff;
		game->cinematicActive = 1;
		break;

	case EV_CINEMATIC_END:
		game->skippingCinematic = 0;
		game->skipUntilTime = 0;
		game->cinematicActive = 0;
		Canvas_setState(canvas, 3);
		Canvas_updateFacing(app->canvas);
		break;

	case EV_FLOORUNIFORM:
		render->floorUniform = (arg == 1);
		break;
	case EV_CEILUNIFORM:
		render->ceilUniform = (arg == 1);
		break;
	case EV_VIEWBOB:
		render->bobEnable = (arg == 1);
		break;

	case EV_NAMEENTITY: {
		/* name = map string (short)arg; target = entity at tile (bits 16..20, 21..25)
		 * when bit 31 is set, else sprite (arg >> 16). Doors and container type 1
		 * additionally get ENT_DIRTY | ENT_NAMED. */
		int strId = (int16_t)arg;
		String_setLength(&canvas->scratch, 0);
		if ((uint32_t)arg & 0x80000000u) {
			ent = Game_findEntityAt(game, (int)bits(arg, 16, 5) << 6, (int)bits(arg, 21, 5) << 6, -1);
			if (ent == NULL)
				return SCRIPT_OK;
		} else {
			ent = scriptSprite((uint32_t)arg >> 16)->ent;
			if (ent == NULL)
				return SCRIPT_OK;
		}
		String_setLength(&ent->name, 0);
		Text_getString(app->canvas, app->canvas->mapStringGroup, strId, &ent->name);
		uint8_t type = ent->def->eType;
		if (type == ETYPE_DOOR || (type == ETYPE_CONTAINER && ent->def->eSubType == 1))
			ent->info |= ENT_DIRTY | ENT_NAMED;
		break;
	}

	case EV_SETMONSTERSTAT: {
		/* sprite bits 21..30, stat bits 17..20 (0 health, 2 defense, 3 strength,
		 * 4 accuracy), value lo16 */
		spr = scriptSprite(bits(arg, 21, 10));
		ent = spr->ent;
		if (ent == NULL || ent->monster == NULL)
			return SCRIPT_OK;
		CombatEntity *ce = &ent->monster->ce;
		switch (bits(arg, 17, 4)) {
		case 0:
			CombatEntity_setMaxHealth(ce, lo16);
			CombatEntity_setHealth(&spr->ent->monster->ce, lo16);
			break;
		case 2: CombatEntity_setDefense(ce, lo16); break;
		case 3: CombatEntity_setStrength(ce, lo16); break;
		case 4: CombatEntity_setAccuracy(ce, lo16); break;
		default: break;
		}
		spr->ent->info |= ENT_DIRTY;
		break;
	}

	case EV_MONSTERFLAGOP: {
		/* sprite arg & 0x3ff; flags bits 10..25; op arg >> 26: 1 or, 2 clear, else set.
		 * Flag 0x200 also mirrors into bit 0. */
		spr = scriptSprite(arg & 0x3ff);
		ent = spr->ent;
		if (ent == NULL || ent->monster == NULL)
			return SCRIPT_OK;
		EntityMonster *mon = ent->monster;
		int op = arg >> 26;
		uint16_t value = (uint16_t)bits(arg, 10, 16);
		if (op == 1)
			mon->flags |= value;
		else if (op == 2)
			mon->flags &= (uint16_t)~value;
		else
			mon->flags = value;
		if (value & 0x200) {
			if (op == 2)
				spr->ent->monster->flags &= 0xfffe;
			else
				spr->ent->monster->flags |= 1;
		}
		spr->ent->info |= ENT_DIRTY;
		break;
	}

	case EV_CALMMONSTER:
		/* whole arg is the sprite index */
		spr = scriptSprite((uint32_t)arg);
		ent = spr->ent;
		if (ent != NULL && ent->monster != NULL) {
			EntityMonster_clearEffects(ent->monster);
			EntityMonster *mon = spr->ent->monster;
			if (mon->goal.type == 4)
				MonsterGoal_reset(&mon->goal);
			Game_removeFromCombat(spr->ent);
		}
		break;

	case EV_DEACTIVATEMONSTER:
		ent = scriptSprite(arg & 0x3ff)->ent;
		if (ent == NULL)
			return SCRIPT_OK;
		Game_deactivate(game, ent);
		Canvas_updateFacing(app->canvas);
		break;

	case EV_GIVETAKEWEAPON:
		/* b1 != 0: give and select weapon b0; else take it away */
		if (b1 != 0) {
			Player_give(player, 1, b0, 1, 0);
			Player_selectWeapon(app->player, b0);
		} else {
			app->player->weapons &= ~arm_bit(b0);
			if (app->player->ce.weapon == b0)
				Player_selectNextWeapon(app->player);
		}
		break;

	case EV_SETENTITYPARAM:
	case EV_SETENTITYPARAM2:
		/* sprite arg & 0x3ff: info |= bit31, param high half = bits 10..14 */
		ent = scriptSprite(arg & 0x3ff)->ent;
		ent->info |= ENT_FLAG31;
		ent = scriptSprite(arg & 0x3ff)->ent;
		ent->param = (int)(((uint32_t)ent->param & 0xffff) | (bits(arg, 10, 5) << 16));
		break;

	case EV_TAKEITEM:
		Player_give(player, b0, b1, -hi16s, 0);
		break;

	case EV_LERPSPRITE_ASYNC:
	case EV_LERPSPRITE_TOSPRITE:
	case EV_LERPSPRITE_RELATIVE:
		return lerpSpriteCommand(thread, frame, opcode, arg);

	case EV_SPAWNTHREAD: {
		/* tile (b0, b1); bit 31: random tile in [b0, b1) x [b2, b3 & 0x7f) */
		int tx = b0, ty = b1;
		if ((uint32_t)arg & 0x80000000u) {
			uint32_t a = (uint32_t)arg & 0x7fffffffu;
			int xLo = (int)(a & 0xff), xHi = (int)((a >> 8) & 0xff);
			int yLo = (int)((a >> 16) & 0xff), yHi = (int)(a >> 24);
			tx = xLo;
			if (xHi != xLo)
				tx = (Random_nextInt(&app->random) & 0xff) % (xHi - xLo) + xLo;
			ty = yLo;
			if (yHi != yLo)
				ty = (Random_nextInt(&app->random) & 0xff) % (yHi - yLo) + yLo;
		}
		ScriptThread *child = Game_allocScriptThread(app->game);
		if (child != NULL) /* all 16 busy: the original ran on a NULL thread (undefined); skip it */
			ScriptThread_executeTile0(child, tx, ty, 0xf000);
		return SCRIPT_OK;
	}

	case EV_ENDFAMILIAR:
		/* leave familiar mode; arg 0 also returns the view to the body */
		if (player->isFamiliar != 0) {
			Player_setFamiliar(player, 0, arg != 0, 0);
			if (arg == 0)
				Canvas_resetPlayerPos(app->canvas, app->canvas->savedViewX, app->canvas->savedViewY);
		}
		break;

	case EV_KILLATTILE:
		if (arg & 0x10000000)
			Game_activateMonsters();
		else
			Game_removeEntitiesAtTile(game, arg & 0x1f, (int)bits(arg, 5, 5), 4);
		break;

	case EV_AIGOAL_MOVE:
		/* sprite lo16 walks to tile (b2, b3) */
		ent = scriptSprite(lo16)->ent;
		if (ent != NULL) {
			EntityMonster *mon = ent->monster;
			MonsterGoal_reset(&mon->goal);
			mon->goal.type = 1;
			mon->goal.x = b2;
			mon->goal.y = (int)((uint32_t)arg >> 24);
		}
		break;

	case EV_MAKECORPSE: {
		spr = scriptSprite(lo16);
		spr->ent->info |= 0x60000000u;
		EntityDef *def = spr->ent->def;
		spr->ent->def = EntityDefs_find(app->entityDefs, ETYPE_CORPSE, def->eSubType, def->parm);
		spr->ent->monster->flags |= 0x80;
		spr->info = (spr->info & ~(SPR_HIDDEN | SPR_FRAME_MASK)) | 0x800;
		if (spr->ent->info & ENT_ACTIVE)
			Game_deactivate(app->game, spr->ent);
		Game_linkEntity(app->game, spr->ent, spr->x >> 6, spr->y >> 6);
		if (app->game->difficulty != 4)
			return SCRIPT_OK;
		ent = spr->ent;
		if (ent->def->eSubType > 10)
			return SCRIPT_OK;
		/* hardest difficulty: the corpse will rise again */
		ent->monster->monsterEffects |= 0x6000;
		spr->ent->monster->monsterEffects |= 0x6010;
		break;
	}

	case EV_OPENRELIQUARY:
		return openContainerCommand(thread, arg, 3);

	case EV_LERPSCALE: {
		/* sprite arg >> 16 scales to b1 over b0 * 100 ms */
		uint32_t idx = (uint32_t)arg >> 16;
		spr = scriptSprite(idx);
		ent = spr->ent;
		if (ent != NULL && ent->monster != NULL)
			ent->monster->flags |= 0x400;
		LerpSprite *ls = Game_allocLerpSprite(app->game, thread, (int)idx);
		if (ls == NULL)
			return SCRIPT_NOT_RUN;
		ls->dstX = spr->x;
		ls->dstY = spr->y;
		ls->dstZ = spr->z;
		ls->srcX = spr->x;
		ls->srcY = spr->y;
		ls->srcZ = spr->z;
		ls->srcScale = spr->scale;
		ls->dstScale = b1;
		ls->startTime = app->playTime;
		ls->travelTime = b0 * 100;
		ls->flags = 0;
		if (!(frame->frameFlags & FRAME_NESTED))
			app->game->threadWaiting = 1;
		if (b0 != 0)
			return SCRIPT_YIELD;
		return (Game_updateMover(app->game, ls) & 1) ? SCRIPT_OK : SCRIPT_NOT_RUN;
	}

	case EV_QUESTFLAG:
		Player_setQuestFlag(player, lo16, b2);
		break;

	case EV_AWARDSECRET: {
		int counts[2];
		Hud_addMessage(app->hud, 0xcb); /* "Found Secret!" */
		Render_countSecrets(app->player, counts);
		uint32_t levelBit = arm_bit(app->canvas->mapId - 1);
		if (counts[0] + 1 == counts[1] && !(app->player->foundSecretsLevels & levelBit)) {
			/* last secret of the level: bonus of 1/16 of the current level's XP span */
			Sound_play(app->sndSys, SND_SECRET, 2, 5, -1);
			int xp = (Player_xpForLevel(app->player, app->player->level) -
			          Player_xpForLevel(app->player, app->player->level - 1)) >> 4;
			Canvas *cv = app->canvas;
			String *msg = &cv->msgRing[cv->msgRingIdx];
			cv->msgRingIdx = (cv->msgRingIdx + 1) % 2;
			String_setLength(msg, 0);
			Canvas_resetEvents(app->canvas);
			Canvas_addTextArgStr(app->canvas, &app->render->mapName);
			Canvas_addTextArgInt(app->canvas, xp);
			Text_getString(app->canvas, 0, 0xd, msg);
			/* help entry type 2 = String* */
			Canvas_addHelpMessage(app->canvas, msg, 2, NULL);
			Player_addXP(app->player, xp);
			app->player->foundSecretsLevels |= arm_bit(app->canvas->mapId - 1);
		} else {
			Player_addXP(app->player, 5);
		}
		Hud_randomBark(app->hud, 7);
		break;
	}

	case EV_PAYGOLD: {
		/* take lo16 gold; message string hi16 (signed, < 0 = none) */
		int before = player->gold;
		Player_give(player, 4, 0, -lo16, 0);
		int paid = before - app->player->gold;
		app->statGoldSpent -= paid;
		app->statGoldGained -= paid;
		if (paid < 1)
			return SCRIPT_OK;
		app->game->goldPaid += paid;
		Sound_play(app->sndSys, SND_GOLD, 0, 4, -1);
		if (hi16s >= 0) {
			String_setLength(&canvas->scratch, 0);
			Canvas_resetEvents(app->canvas);
			Canvas_addTextArgInt(app->canvas, paid);
			Text_getString(app->canvas, app->canvas->mapStringGroup, hi16s, &canvas->scratch);
			Hud_addMessageStrFlags(app->hud, &canvas->scratch, 3);
		}
		Canvas_startShake(app->canvas, 0, 2, 400);
		break;
	}

	case EV_NOP_55:
		break;

	case EV_DAMAGE:
		if (hi16s != -1) {
			/* monster sprite hi16 takes lo16 damage; non-monsters just die */
			spr = scriptSprite(hi16s);
			ent = spr->ent;
			if (ent == NULL)
				return SCRIPT_OK;
			if (ent->monster != NULL) {
				Entity_takeDamage(ent, lo16);
				if (CombatEntity_getHealth(&spr->ent->monster->ce) > 0)
					return SCRIPT_OK;
				ent = spr->ent;
			}
			Entity_diedDefault(ent);
			return SCRIPT_OK;
		}
		/* player: armor loss (lo16 >> 8), health damage (arg & 0xff) */
		Player_hurtFeedback(player, 0);
		CombatEntity_addArmor(&app->player->ce, -(lo16 >> 8));
		Player_takeDamage(app->player, arg & 0xff, NULL);
		break;

	case EV_ADDNOTE:
		Player_addNotebookIndex(player, (int16_t)arg);
		break;

	case EV_NOP_58:
		break;

	case EV_SETAIGOAL:
		/* sprite lo16 gets goal type b2, thinks immediately; restart combat */
		spr = scriptSprite(lo16);
		ent = spr->ent;
		if (ent != NULL) {
			EntityMonster *mon = ent->monster;
			MonsterGoal_reset(&mon->goal);
			mon->goal.type = (uint8_t)b2;
			Entity_aiThink(spr->ent);
			mon->goal.flags &= 0xf7;
			Entity *attacker = app->game->combatMonsters;
			if (attacker != NULL)
				Combat_start(app->combat, attacker, attacker->monster->target, 0, 0);
		}
		break;

	case EV_NOP_5A:
		break;

	case EV_FADE: {
		/* duration lo16; bit 31 fade in (5) vs out (4); bit 30 adds 0x10 */
		int mode = ((uint32_t)arg & 0x80000000u) ? 5 : 4;
		if (arg & 0x40000000)
			mode |= 0x10;
		Canvas_requestState12(canvas, lo16, mode);
		break;
	}

	case EV_KNOCKBACKPLAYER:
		/* push the player away from tile (b0, b1) by b2 + 1 */
		Entity_knockback(&app->game->entities[1], b0 * 64 + 32, ((arg >> 2) & 0x3fc0) + 0x20, b2 + 1);
		break;

	case EV_NOP_5D:
	case EV_NOP_60:
	case EV_NOP_61:
		break;

	case EV_SPRITEFRAME:
		/* sprite lo16: frame b2 and animate, or 0xff = stop and reset */
		spr = scriptSprite(lo16);
		if (b2 == 0xff)
			spr->info &= ~(SPR_ANIMATING | SPR_FRAME_MASK);
		else
			spr->info = (spr->info & ~SPR_FRAME_MASK) | ((uint32_t)b2 << 8) | SPR_ANIMATING;
		break;

	case EV_RESTART:
		return SCRIPT_RESTART;

	case EV_AIMEDEFFECT: {
		/* Combat effect of kind bits 10..13, colour RGB 5:6:5 in bits 14..29,
		 * only if sprite (arg & 0x3ff) lies straight ahead of the view on a row
		 * or column. */
		if (app->game->skippingCinematic != 0)
			return SCRIPT_OK;
		spr = scriptSprite(arg & 0x3ff);
		int kind = (int)bits(arg, 10, 4);
		uint32_t color = (bits(arg, 14, 5) << 19) | (bits(arg, 19, 6) << 10) | (bits(arg, 25, 5) << 3) | 0xff000000u;
		int angle, vx, vy;
		if (Game_isCinematicActive(app->game) == 0) {
			angle = app->canvas->viewAngle;
			vx = app->canvas->viewX;
			vy = app->canvas->viewY;
		} else {
			Camera *cam = app->game->activeCamera;
			angle = cam->angle;
			vx = cam->x;
			vy = cam->y;
		}
		int fwdX = (app->render->sinTable[(angle + 0x100) & 0x3ff] * 64) >> 16;
		int fwdY = (-(app->render->sinTable[angle & 0x3ff] * 64)) >> 16;
		int dx = spr->x - vx;
		int dy = spr->y - vy;
		if (dx != 0 && dy != 0)
			return SCRIPT_OK;
		if (dx == 0 && dy == 0)
			return SCRIPT_OK;
		if (fwdX < 0) {
			if (dx >= 0)
				return SCRIPT_OK;
		} else if (fwdX > 0 && dx <= 0) {
			return SCRIPT_OK;
		}
		if (fwdY < 0) {
			if (dy >= 0)
				return SCRIPT_OK;
		} else if (fwdY > 0 && dy <= 0) {
			return SCRIPT_OK;
		}
		int dist = Combat_WorldDistToTileDist(app->combat, (int)((uint32_t)dx * (uint32_t)dx + (uint32_t)dy * (uint32_t)dy));
		Script_combatEffect(0, kind, (int)color, dist);
		break;
	}

	case EV_ENDCOMBAT:
		while (app->game->combatMonsters != NULL)
			Game_removeFromCombat(app->game->combatMonsters);
		if (app->canvas->state == 4)
			Canvas_setState(app->canvas, 3);
		break;

	case EV_NOP_66:
		break;

	case EV_OPENBOOKSHELF:
		return openContainerCommand(thread, arg, 6);

	case EV_NOP_68:
		break;

	case EV_STEALMSG:
		Hud_addMessageFlags(app->hud, 0xcc, 3);
		Hud_randomBark(app->hud, 10);
		break;

	case EV_SHOWANDWAKE:
		showSpriteCommand(opcode, arg);
		break;

	case EV_AUTOMAPFLAG:
		game->automapFlag = (arg != 0);
		break;

	case EV_MARKTILE:
		return markTileCommand(arg, 1);
	case EV_UNMARKTILE:
		return markTileCommand(arg, 0);

	case EV_TURNPLAYER: {
		/* face direction arg, turning the short way round */
		app->canvas->destAngle = Dir_toAngle(app->textUtil, arg);
		app->canvas->viewAngle &= 0x3ff;
		Canvas *cv = app->canvas;
		int view = cv->viewAngle, dest = cv->destAngle;
		int dPlus = abs(view - (dest + 0x400));
		int d = abs(view - dest);
		if (dPlus < d) {
			cv->destAngle = cv->destAngle + 0x400;
		} else {
			int dMinus = abs(view - (dest - 0x400));
			if (dMinus < d)
				cv->destAngle = cv->destAngle - 0x400;
		}
		break;
	}

	case EV_CLEARBUFFS:
		app->player->numBuffs = 0;
		for (int i = 0; i < 12; i++) {
			app->player->buffs[i].turns = 0;
			app->player->buffs[i].amount = 0;
		}
		for (int i = 0; i < 14; i++)
			Player_removeBuff(app->player, i);
		break;

	case EV_CLEARENTITYFLAG:
		scriptSprite(arg & 0x3ff)->ent->info &= ~ENT_FLAG31;
		break;

	case EV_SETENTITYPARAMBIT:
		/* param = bit 10 of arg (the original's mask keeps a single bit).
		 * The original stores through a NULL entity unchecked. */
		ent = scriptSprite(arg & 0x3ff)->ent;
		if (ent != NULL)
			ent->param = (int)bits(arg, 10, 1);
		break;

	case EV_SETNODEATH:
		app->player->noDeathFlag = (arg > 0);
		break;

	default:
		break;
	}
	return SCRIPT_OK;
}
