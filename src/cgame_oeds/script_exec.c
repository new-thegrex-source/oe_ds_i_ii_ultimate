/*
 * Tile-event script interpreter: ScriptThread_executeCommand (0x0000ea8c).
 *
 * Every map tile can carry an event word (Render.tileEvents) pointing at a
 * run of {opcode, arg, flags} commands in Render.commands. ScriptThread_runEvent
 * walks those and hands each one to ScriptThread_executeCommand. In the binary
 * the function body runs from 0xea8c to 0x10414 behind one jump table
 * (0xeb64, opcodes 0x00..0x65); Ghidra split a few of its shared tails off as
 * separate "functions" (0xf348 = yield-with-flag, 0xf4ec = the CHECKITEM
 * failure message, 0x10410 = "return 1").
 *
 * The 32-bit argument is a packed record; b0..b3 below are its bytes from
 * least to most significant. "str[n]" is Render.mapStrings[n]; "sprite n" is
 * Render.sprites[n].
 *
 * Return codes: 0 command did not run, 1 done, 2 yield (the thread resumes
 * later through ScriptThread_run; the same command runs again), 3 abort the
 * rest of the event, 4 restart it.
 */

#include <stdio.h>

#include "game.h"
#include "ds_local.h"
#include <stdlib.h>

/* Strings in the module's data area (read at runtime, never copied). */
#define MOD_STR_PAIN              0x2c878u
#define MOD_STR_EV_CHATBUBBLE     0x2c880u
#define MOD_STR_EV_NAMEENTITY     0x2c8b8u
#define MOD_STR_EV_UNKNOWNSTAT    0x2c8ecu
#define MOD_STR_EV_NOMONSTER      0x2c914u
#define MOD_STR_EV_MONSTERFLAGS   0x2c938u
#define MOD_STR_EV_DEACTIVATE     0x2c970u
#define MOD_STR_EV_DAMAGEENTITY   0x2c9a4u
#define MOD_STR_HEALTH_ENERGY_FULL 0x2c9dcu
#define MOD_STR_FOUND_SECRET      0x2c9f4u
#define MOD_STR_SECRETS_CLEARED   0x2ca04u

/* Sound resources played by the interpreter. */
enum {
	SND_LEVER = 0x13cb,          /* levers, secrets */
	SND_RELIQUARY = 0x13c4,
	SND_FOUNTAIN = 0x140a,
	SND_SECRETS_CLEARED = 0x141c,
	SND_MOVING_BLOCK = 0x143d,
	SND_PAY_GOLD = 0x1447,
};

enum {
	SCRIPT_NOT_RUN = 0,
	SCRIPT_DONE = 1,
	SCRIPT_YIELD = 2,
	SCRIPT_ABORT = 3,
	SCRIPT_RESTART = 4,
};

enum ScriptOpcode {
	EV_NOP = 0x00,
	EV_GOTO = 0x01,             /* warp to tile b0,b1; direction (arg>>16)&0x3ff, 0xff keeps it */
	EV_CHANGEMAP = 0x02,        /* Game.pendingMapChange = arg; bit30 = shake + fade first */
	EV_TRIGGER = 0x03,          /* run tile b0,b1's event nested; bit31 = random tile in [b0,b1)x[b2,b3&0x7f) */
	EV_MESSAGE = 0x04,          /* str[b0]; b1 selects HUD mode / cinematic caption line */
	EV_PAIN = 0x05,             /* "Pain!" + damage the player by the whole arg */
	EV_DOOR = 0x06,             /* 6, 0xf, 0x10, 0x11: Game_useDoor(op, line arg) */
	EV_SHOW = 0x07,             /* sprite lo16: frame b2, z b3 */
	EV_DIALOG = 0x08,           /* str[hi16]: style b0, flags b1 (4 = wand chat) */
	EV_REVEALAUTOMAP = 0x09,    /* Game_revealAutomapRect(b3, b2, b1, b0) */
	EV_PASSWORD = 0x0a,         /* combination str[b0] (+'P' per char), prompt str[b1] */
	EV_SETSTATE = 0x0b,         /* state of tile b0,b1's event := b2 */
	EV_LOCK = 0x0c,
	EV_UNLOCK = 0x0d,
	EV_TOGGLELOCK = 0x0e,
	EV_OPENLINE = 0x0f,
	EV_CLOSELINE = 0x10,
	EV_MOVELINE = 0x11,
	EV_HIDE = 0x12,             /* hide sprite arg; bit31 = sprites on tile b0,b1 (tile id b2, bit30 inverts) */
	EV_NEXTSTATE = 0x13,
	EV_PREVSTATE = 0x14,
	EV_INCSTAT = 0x15,
	EV_DECSTAT = 0x16,
	EV_REQSTAT = 0x17,          /* stat b0 >= b1, else dialog str[hi16] style 6 and abort */
	EV_FORCEMESSAGE = 0x18,     /* str[arg] into Hud.forcedMsg ("" clears) */
	EV_ANIM = 0x19,             /* one-shot animated sprite tile b2 at tile b0,b1 */
	EV_GIVEITEM = 0x1a,         /* Player_give(b0, b1, hi16) */
	EV_SETRESPAWN = 0x1b,       /* map 6000+b0, tile b1,b2, angle b3 */
	EV_ABORTMOVE = 0x1c,
	EV_SCREENSHAKE = 0x1d,      /* delay arg&0xfff, intensity (s8)b3, vibrate (arg>>12)&0xfff */
	EV_FLOORCOLOR = 0x1e,
	EV_CEILCOLOR = 0x1f,
	EV_ENABLEWEAPONS = 0x20,
	EV_OPENSTORE = 0x21,
	EV_CHANGESPRITE = 0x22,     /* retile the first prop on tile (arg&0x1f, (arg>>5)&0x1f) */
	EV_PARTICLES = 0x23,        /* trap effect (arg>>24)&0x1f one tile ahead, colour b0b1b2 */
	EV_REFRESHVIEW = 0x24,
	EV_WAIT = 0x25,
	EV_CHECKITEM = 0x26,        /* Player_requireItem(b0, b1, b2 range), else message str[b3] and abort */
	EV_NOTE = 0x28,
	EV_ENDGAME = 0x29,
	EV_PLAYSOUND = 0x2a,
	EV_WANDCHAT = 0x2c,
	EV_WAKEMONSTER = 0x2d,
	EV_LERPFLAT = 0x2e,         /* b0 0 floor / 1 ceiling height := b1, over hi16 ms (0 = now) */
	EV_LERPSPRITE = 0x30,       /* also 0x4a (no wait), 0x5e (to another sprite), 0x5f (relative) */
	EV_RANDSTATE = 0x31,        /* state := random in [b2, b3] */
	EV_SWITCHON = 0x32,
	EV_SWITCHOFF = 0x33,
	EV_SWITCHTOGGLE = 0x34,
	EV_STARTCINEMATIC = 0x36,   /* camera b0 from key b1-1 */
	EV_LERPFLAGS = 0x39,        /* flags of the lerp moving sprite (arg>>16)&0x3ff := (s8)b0 */
	EV_FIREPORTAL = 0x3b,       /* portal b0 on/off (bit 8) */
	EV_SHOWCHATBUBBLE = 0x3c,
	EV_SKIPPABLE = 0x3d,
	EV_ENDSKIPPABLE = 0x3e,
	EV_FLOORFLAT = 0x3f,
	EV_CEILFLAT = 0x40,
	EV_WOBBLE = 0x41,
	EV_NAMEENTITY = 0x42,
	EV_MONSTERSTAT = 0x43,
	EV_MONSTERFLAGS = 0x44,
	EV_LEAVECOMBAT = 0x45,
	EV_DEACTIVATE = 0x46,
	EV_GIVEWEAPON = 0x47,
	EV_MOVEBLOCK = 0x48,        /* also 0x60 (non-blocking) */
	EV_TAKEITEM = 0x49,
	EV_LERPSPRITE_NOWAIT = 0x4a,
	EV_TRIGGERTHREAD = 0x4b,
	EV_ENDWANDCHAT = 0x4d,
	EV_AIGOAL = 0x4e,
	EV_MAKECORPSE = 0x4f,
	EV_OPENRELIQUARY = 0x50,
	EV_SCALESPRITE = 0x51,      /* lerp sprite hi16 in place to scale b1 over b0*100 ms */
	EV_FOUNTAIN = 0x52,
	EV_AWARDSECRET = 0x53,
	EV_PAYGOLD = 0x54,
	EV_NPCPARAM = 0x55,
	EV_DAMAGE = 0x56,
	EV_SETMOOD = 0x57,
	EV_ADDMOOD = 0x58,
	EV_AISTATE = 0x59,
	EV_BOSSREINFORCEMENTS = 0x5a,
	EV_FADE = 0x5b,
	EV_PUSHPLAYER = 0x5c,
	EV_CHECKMOOD = 0x5d,
	EV_LERPSPRITE_TO = 0x5e,
	EV_LERPSPRITE_REL = 0x5f,
	EV_MOVEBLOCK_NB = 0x60,
	EV_TIMESTAMP = 0x61,
	EV_ENTITYFRAME = 0x62,
	EV_RESTART = 0x63,
	EV_SPRITEPARTICLES = 0x64,
	EV_ENDCOMBAT = 0x65,
	EV_COUNT = 0x66,
};

/* Everything a command handler needs, unpacked once like the original's prologue. */
typedef struct Cmd {
	ScriptThread *thread;
	ScriptFrame *frame;
	Applet *app;
	Game *game;
	Canvas *canvas;
	Render *render;
	Player *player;
	int opcode;
	int32_t arg;   /* signed view (arithmetic shifts) */
	uint32_t u;    /* unsigned view */
	uint32_t flags; /* the command's flags (DS: TRIGGERIF passes 0x2000000 on) */
	uint32_t b0, b1, b2, b3;
} Cmd;

/* ARM register shifts by >= 32 give 0. */
static uint32_t bit32(uint32_t n)
{
	return n < 32 ? 1u << n : 0;
}

static char *mapString(const Cmd *c, int index)
{
	return c->render->mapStrings[index];
}

static Sprite *scriptSprite(const Cmd *c, int index)
{
	return &c->render->sprites[index];
}

/* World coordinate of the centre of tile t. */
static int tileCenter(int t)
{
	return t * 64 + 32;
}

/* The binary computes some tile coordinates as (arg >> 8) << 6 without masking. */
static int shiftedTile(int32_t v)
{
	return (int)((uint32_t)v << 6);
}

static int yieldWithFlag(Cmd *c)
{
	c->game->yieldFlag = 1;
	return SCRIPT_YIELD;
}

/* Put the wand away before a dialog (repeated inline in the binary). */
static void lowerWand(Player *p)
{
	if (p->weapons & 1) {
		if (p->ce.weapon != 0)
			Player_selectWeapon(p, 0);
		else
			p->prevWeapon = 0;
	}
}

/* App_setErrorStr + App_setErrorCode(GETAPPINSTANCE(), -666), the EV_* error idiom. */
static void scriptError(const Cmd *c, uint32_t fmtOff, int index)
{
	App_setErrorStr(c->app, (char *)ModData_str(&g_mod, fmtOff), index);
	App_setErrorCode(g_app, -666);
}

/* ---- movement / map ---- */

static int opGoto(Cmd *c)
{
	Canvas *cv = c->canvas;
	uint32_t dir = (c->u >> 16) & 0x3ff;

	cv->destX = cv->viewX = tileCenter(c->b0);
	cv->destY = cv->viewY = tileCenter(c->b1);
	if (dir != 0xff) {
		int angle = Dir_toAngle(dir);
		cv->viewAngle = angle;
		cv->destAngle = angle;
		Canvas_finishRotation(cv);
	}
	Game_relinkEntity(c->thread->app->player);
	Game_setMoveDirMask(c->game, -1, -1, -1, -1);
	return SCRIPT_DONE;
}

static int opChangeMap(Cmd *c)
{
	c->game->pendingMapChange = c->u;
	if (c->u & 0x40000000) {
		c->game->pendingMapChange = c->u & ~0x40000000u;
		Canvas_startShake(c->canvas, 750, 2, 0);
		c->canvas->fadeStart = c->thread->app->tickTime;
		c->canvas->fadeDir = 0;
		Canvas_setState(c->canvas, 0x15);
		return SCRIPT_ABORT;
	}
	Game_processMapChange(c->game);
	return SCRIPT_ABORT;
}

static int opTrigger(Cmd *c)
{
	int x = c->b0;
	int y = c->b1;

	if (c->arg < 0) {
		/* random tile: x in [b0, b1), y in [b2, b3 & 0x7f) */
		int x1 = c->b1;
		int y0 = c->b2;
		int y1 = (c->u >> 24) & 0x7f;
		x = c->b0;
		y = y0;
		if (x1 != x) {
			int r = Random_nextByte(&c->app->random);
			x = r % (x1 - x) + x;
		}
		if (y1 != y) {
			int r = Random_nextByte(&c->thread->app->random);
			y = r % (y1 - y) + y;
		}
	}

	/* (DS executeEvent 0x0204d388 passes the command's flag 0x2000000 on, like TRIGGERIF) */
	int flags = (int)(c->flags & 0x2000000) | 0xf000;
	if (c->opcode == EV_TRIGGERTHREAD) {
		/* run it on a thread of its own; the caller does not wait for it */
		ScriptThread *t = Game_allocScriptThread(c->game);
		if (t != NULL)
			ScriptThread_executeTile(t, x, y, flags, 0);
		return SCRIPT_DONE;
	}

	c->frame->frameFlags |= 4;
	int rc = ScriptThread_executeTile(c->thread, x, y, flags, c->frame->frameFlags & 1);
	c->frame->frameFlags &= ~4;
	if (rc == SCRIPT_YIELD)
		return SCRIPT_YIELD;
	return SCRIPT_DONE;
}

static int opSetRespawn(Cmd *c)
{
	Game *g = c->game;
	g->respawnMap = (int16_t)(c->b0 + 6000);
	g->respawnX = tileCenter(c->b1);
	g->respawnY = tileCenter(c->b2);
	g->respawnAngle = c->b3;
	return SCRIPT_DONE;
}

/* 0x0b / 0x13 / 0x14 / 0x31: change the state bits (27..30) of tile b0,b1's event. */
static int opTileEventState(Cmd *c)
{
	int idx = Render_findTileEvent(c->render, c->b1 * 32 + c->b0);
	if (idx == -1)
		return SCRIPT_DONE;

	Render *r = c->thread->app->render;
	uint32_t ev = r->tileEvents[idx];
	int state = (int)((ev << 1) >> 28);

	if (c->opcode == EV_SETSTATE) {
		state = c->b2;
	} else if (c->opcode == EV_RANDSTATE) {
		int lo = c->b2;
		int hi = c->b3;
		int rnd = Random_nextByte(&c->thread->app->random);
		state = rnd % (hi - lo + 1) + lo;
	} else if (c->opcode == EV_NEXTSTATE) {
		if (state < 9)
			state++;
	} else if (c->opcode == EV_PREVSTATE) {
		if (state > 0)
			state--;
	}
	c->thread->app->render->tileEvents[idx] = ((uint32_t)state << 27) | (ev & ~0x78000000u);
	return SCRIPT_DONE;
}

/* ---- text ---- */

static int opMessage(Cmd *c)
{
	char *text = mapString(c, c->b0);

	if (c->canvas->state == 0x14) {
		/* cinematic caption: b1 == 1 bottom line, else top line */
		Hud *hud = c->app->hud;
		char *dst = (c->b1 == 1) ? hud->cinematicBottom : hud->cinematicTop;
		Text_wrap(text, 9999, dst, 0x20, 0, '|', NULL);
	} else {
		Hud_addMessage(c->thread->app->hud, text, c->b1 == 2 ? 3 : 0);
	}
	return SCRIPT_DONE;
}

static int opForceMessage(Cmd *c)
{
	char *text = mapString(c, c->arg);
	Hud *hud = c->app->hud;

	if (text[0] != '\0')
		Text_wrap(text, 9999, hud->forcedMsg, 0x40, 0, '|', NULL);
	else
		hud->forcedMsg[0] = '\0';
	return SCRIPT_DONE;
}

static int opDialog(Cmd *c)
{
	int style = c->b0;
	int flags = c->b1;
	uint32_t strIndex = c->u >> 16;
	Player *p = c->player;

	if (c->game->suppressDialog != 0)
		return SCRIPT_DONE;

	if ((flags & 4) && p->ce.weapon != 0) {
		/* the wand "wants to talk": the text is shown when the player asks */
		Player_setWandState(p, 1);
		c->thread->app->player->wandChatStr = (int32_t)strIndex;
		return SCRIPT_DONE;
	}

	if (p->weapons & 1) {
		if (style == 2)
			goto help;
		if (style == 1)
			goto wandDialog;
		if (style == 3)
			goto dialog;
		if (p->ce.weapon != 0)
			Player_selectWeapon(p, 0);
		else
			p->prevWeapon = 0;
	}
	if (style == 6 || style == 7 || style == 5 || style == 8)
		c->thread->app->player->attacked = 0;
	if (style == 2)
		goto help;
	if (style != 1)
		goto dialog;

wandDialog:
	lowerWand(c->thread->app->player);
	Canvas_showDialog(c->canvas, c->thread, mapString(c, strIndex), 1, 1, 1);
	Player_setWandState(c->thread->app->player, 0);
	c->thread->app->hud->numSubtitles = 0;
	return yieldWithFlag(c);

help:
	/* DS (FUN_0201b7e0): the help's title is the string, its text the next one; the BREW
	 * dialog takes "TITLE|text" in one string. Kept until shown (the queue holds 8) */
	{
		static char helpTexts[16][600];
		static int nextHelp;
		char *t = helpTexts[nextHelp++ & 15];
		int next = (int)strIndex + 1 < c->render->numMapStrings ? (int)strIndex + 1 : (int)strIndex;
		snprintf(t, sizeof helpTexts[0], "%s|%s", mapString(c, strIndex), mapString(c, next));
		if (!Canvas_addScriptHelp(c->canvas, t, c->thread))
			return SCRIPT_DONE;
	}
	return yieldWithFlag(c);

dialog:
	Canvas_showDialog(c->canvas, c->thread, mapString(c, strIndex), style, flags, 1);
	return yieldWithFlag(c);
}

static int opPassword(Cmd *c)
{
	Game *g = c->game;
	const char *src = mapString(c, c->b0);
	size_t n = strlen(src);

	/* the original strcpy'd into the 8-byte buffer; combinations are 4 arrows */
	if (n >= sizeof g->password)
		n = sizeof g->password - 1;
	memcpy(g->password, src, n);
	g->password[n] = '\0';
	for (uint8_t i = 0; i < strlen(g->password); i++)
		g->password[i] += 'P';

	Canvas_showChatDialog(c->canvas, c->thread, mapString(c, c->b1));
	return yieldWithFlag(c);
}

/* ---- sprites and entities ---- */

static int opShow(Cmd *c)
{
	Game *g = c->game;
	Sprite *s = scriptSprite(c, c->u & 0xffff);

	s->z = (int32_t)c->b3;
	s->info = (s->info & 0xfffef0ffu) | (c->b2 << 8);
	if ((c->b3 != 0x20 || c->b2 != 0) && s->ent != NULL)
		s->ent->info |= 0x80000000u;

	if (s->ent != NULL) {
		/* anything standing where the sprite appears is killed (twice: two may share the tile) */
		Entity *e = Game_findEntityAt(g, s->x, s->y, 0x404);
		if (e != NULL)
			Entity_died(e, 1);
		e = Game_findEntityAt(g, s->x, s->y, 0x404);
		if (e != NULL)
			Entity_died(e, 1);
		if (s->ent->info & 0x4000000)
			Game_unlinkEntity_2ae8c(g, s->ent);
		Game_linkEntity(g, s->ent, s->x >> 6, s->y >> 6);
		/* DS 0x67: a monster shown turns on the player at once */
		if (c->opcode == DS_OP_SHOWWAKE && s->ent->def->eType == 2 && (s->ent->info & 0x400a0000) == 0) {
			s->ent->info |= 0x20000000;
			Game_activateEx(g, s->ent, 1, 0);
		}
	}
	return SCRIPT_DONE;
}

static int opHide(Cmd *c)
{
	Game *g = c->game;

	if (c->arg >= 0) {
		Sprite *s = scriptSprite(c, c->arg);
		s->info |= 0x10000;
		if (s->ent != NULL) {
			s->ent->info |= 0x80000000u;
			Game_unlinkEntity_2ae8c(g, s->ent);
		}
		Canvas_updateFacing(c->canvas);
		return SCRIPT_DONE;
	}

	/* every plain sprite on tile b0,b1; b2 != 0 filters by tile id (bit 30 inverts the filter) */
	int tx = c->b0;
	int ty = c->b1;
	uint32_t tileFilter = c->b2;
	int invert = (c->u & 0x40000000) != 0;

	for (int i = 0; i < c->thread->app->render->numSprites; i++) {
		Render *r = c->thread->app->render;
		Sprite *s = &r->sprites[i];
		uint32_t info = s->info;
		uint32_t tile = info & 0xff;
		if (info & 0x40000)
			tile += 0x101;

		if (tileFilter != 0) {
			if (invert ? tileFilter == tile : tileFilter != tile)
				continue;
		}
		if (tx == (s->x >> 6) && ty == (s->y >> 6) && ((info << 9) >> 28) == 0) {
			s->info = info | 0x10000;
			if (s->ent != NULL) {
				s->ent->info |= 0x80000000u;
				Game_unlinkEntity_2ae8c(g, s->ent);
			}
		}
	}
	return SCRIPT_DONE;
}

static int opChangeSprite(Cmd *c)
{
	Game *g = c->game;
	uint32_t newTile = (uint32_t)(c->arg >> 13); /* not masked: the binary ORs the shifted word in */
	uint32_t frame = (c->u >> 10) & 7;

	Entity *e = Game_getTileEntities(g, (c->u & 0x1f) << 6, ((c->u >> 5) & 0x1f) << 6);
	while (e != NULL) {
		if (!(e->info & 0x200000) && e->def->eType != 2) {
			Render *r = c->thread->app->render;
			Sprite *s = Render_spriteAt(r, (e->info & 0xffff));
			s->info = (s->info & 0xfffff000u) | newTile | (frame << 8) | 0x80000000u;
			Game_unlinkEntity_2ae8c(g, e);
		}
		/* read after the unlink, which clears nextOnTile: in effect only the first prop changes */
		e = e->nextOnTile;
	}
	return SCRIPT_DONE;
}

static int opWakeMonster(Cmd *c)
{
	Sprite *s = scriptSprite(c, c->arg);
	Entity *e = s->ent;

	if (e == NULL)
		return SCRIPT_NOT_RUN;
	if (e->def->eType == 2 && (e->info & 0x400a0000) == 0) {
		e->info |= 0x20000000;
		Game_activateEx(c->game, s->ent, 1, 0);
	}
	return SCRIPT_DONE;
}

/* 0x30 / 0x4a / 0x5e / 0x5f: move a sprite to a tile over (arg & 0x7f) * 100 ms. */
static int opLerpSprite(Cmd *c)
{
	uint32_t units = c->u & 0x7f;
	uint32_t sprIndex = (c->u >> 7) & 0x3ff;
	int dstTx, dstTy;
	int16_t dstZ;

	if (c->opcode == EV_LERPSPRITE_TO) {
		Sprite *target = scriptSprite(c, (c->u >> 17) & 0x3ff);
		dstTx = target->x >> 6;
		dstTy = target->y >> 6;
		dstZ = (int16_t)((uint16_t)target->z * 8 - 0x20);
	} else {
		dstTx = (c->u >> 17) & 0x1f;
		dstTy = (c->u >> 22) & 0x1f;
		dstZ = (int16_t)((c->u >> 27) * 8 - 0x20);
	}
	int relative = c->opcode == EV_LERPSPRITE_REL;

	Sprite *s = scriptSprite(c, sprIndex);
	LerpSprite *ls = Game_allocLerpSprite(c->game, c->thread, (short)sprIndex);
	if (ls == NULL)
		return SCRIPT_NOT_RUN;

	if (relative) {
		/* keep the sprite's offset inside its tile */
		ls->dstX = (int16_t)(s->x + (dstTx - (s->x >> 6)) * 64);
		ls->dstY = (int16_t)(s->y + (dstTy - (s->y >> 6)) * 64);
	} else {
		ls->dstX = (int16_t)tileCenter(dstTx);
		ls->dstY = (int16_t)tileCenter(dstTy);
	}
	if (s->ent != NULL) {
		uint32_t info = s->ent->info & ~0x400000u;
		s->ent->info = info;
		s->ent->info = info | 0x88000000u;
	}
	ls->dstZ = dstZ;
	ls->srcX = (int16_t)s->x;
	ls->srcY = (int16_t)s->y;
	ls->srcZ = (int16_t)s->z;
	ls->dstScale = s->scale;
	ls->srcScale = s->scale;
	ls->startTime = c->thread->app->gameTime;
	ls->duration = (int32_t)(units * 100);
	LerpSprite_calcDist(ls);
	ls->flags = c->opcode == EV_LERPSPRITE_NOWAIT;
	if (c->opcode == DS_OP_LERPSPRITE77)
		ls->flags |= 4;
	if (ls->flags & 1)
		return SCRIPT_DONE;
	return yieldWithFlag(c);
}

static int opScaleSprite(Cmd *c)
{
	int16_t sprIndex = (int16_t)(c->arg >> 16);
	Sprite *s = scriptSprite(c, sprIndex);
	LerpSprite *ls = Game_allocLerpSprite(c->game, c->thread, sprIndex);

	if (ls == NULL)
		return SCRIPT_NOT_RUN;
	ls->dstX = (int16_t)s->x;
	ls->dstY = (int16_t)s->y;
	ls->dstZ = (int16_t)s->z;
	ls->srcX = (int16_t)s->x;
	ls->srcY = (int16_t)s->y;
	ls->srcZ = (int16_t)s->z;
	ls->srcScale = s->scale;
	ls->dstScale = (uint8_t)c->b1;
	ls->startTime = c->thread->app->gameTime;
	ls->duration = (int32_t)(c->b0 * 100);
	ls->flags = 0;
	c->game->yieldFlag = 1;
	if (s->ent != NULL && s->ent->monster != NULL)
		s->ent->monster->flags |= 0x400;
	return SCRIPT_YIELD;
}

static int opLerpFlags(Cmd *c)
{
	uint32_t sprIndex = (c->u >> 16) & 0x3ff;

	for (int i = 0; i < 16; i++) {
		if ((uint32_t)(c->game->lerps[i].hSprite - 1) == sprIndex) {
			c->game->lerps[i].flags = (uint8_t)(int8_t)c->b0;
			break;
		}
	}
	return SCRIPT_DONE;
}

/* 0x32 / 0x33 / 0x34: levers (eType 0xe; subtype 1 on, 2 off) on tile b0, arg>>8. */
static int opSwitch(Cmd *c)
{
	Game *g = c->game;
	Entity *e = Game_getTileEntities(g, c->b0 << 6, shiftedTile(c->arg >> 8));

	for (; e != NULL; e = e->nextOnTile) {
		if (e->def->eType != 0xe)
			continue;
		int sub = e->def->eSubType;
		/* the 4th argument was a leftover register value; Game_setSwitch ignores it */
		if (c->opcode == EV_SWITCHON) {
			if (sub == 2)
				Game_setSwitch(g, e, 1, 0);
		} else if (c->opcode == EV_SWITCHOFF) {
			if (sub == 1)
				Game_setSwitch(g, e, 0, 0);
		} else {
			if (sub == 1)
				Game_setSwitch(g, e, 0, 0);
			else if (sub == 2)
				Game_setSwitch(g, e, 1, 0);
		}
	}
	if (c->opcode != EV_SWITCHTOGGLE)
		Sound_play(c->thread->app->sound, SND_LEVER, 0, 3, -1);
	return SCRIPT_DONE;
}

/* DS (0x43): the name is map string arg & 0xffff; the sprite is arg >> 16, or with bit 31 the
 * entity on tile (arg >> 16) & 0x1f, (arg >> 21) & 0x1f (BREW: sprite arg >> 8, string b0) */
static int opNameEntity(Cmd *c)
{
	int str = (int)(c->u & 0xffff);

	if (str >= c->render->numMapStrings)
		return SCRIPT_DONE;
	if (c->arg >= 0) {
		uint32_t sprIndex = (c->u >> 16) & 0x7fff;
		Entity *e = (int)sprIndex < c->render->numSprites ? scriptSprite(c, (int)sprIndex)->ent : NULL;
		if (e == NULL) {
			scriptError(c, MOD_STR_EV_NAMEENTITY, (int)sprIndex);
			return SCRIPT_DONE;
		}
		e->name = mapString(c, str);
		return SCRIPT_DONE;
	}
	Entity *e = Game_findEntityAt(c->game, ((c->u >> 16) & 0x1f) << 6, ((c->u >> 21) & 0x1f) << 6, -1);
	if (e != NULL)
		e->name = mapString(c, str);
	return SCRIPT_DONE;
}

static int opMonsterStat(Cmd *c)
{
	uint32_t sprIndex = (c->u >> 21) & 0x3ff;
	Sprite *s = scriptSprite(c, sprIndex);

	if (s->ent == NULL || s->ent->monster == NULL) {
		scriptError(c, MOD_STR_EV_NOMONSTER, (int)sprIndex);
		return SCRIPT_DONE;
	}
	CombatEntity *ce = &s->ent->monster->ce;
	uint32_t stat = (c->u >> 17) & 0xf;
	int value = (int)(c->u & 0xffff);

	switch (stat) {
	case 0:
		CombatEntity_setMaxHealth(ce, value);
		CombatEntity_setHealth(&s->ent->monster->ce, value);
		break;
	case 2:
		CombatEntity_setDefense(ce, value);
		break;
	case 3:
		CombatEntity_setStrength(ce, value);
		break;
	case 4:
		CombatEntity_setAccuracy(ce, value);
		break;
	default:
		App_setErrorStr(c->app, (char *)ModData_str(&g_mod, MOD_STR_EV_UNKNOWNSTAT), (int)stat, (int)sprIndex);
		App_setErrorCode(g_app, -666);
		break;
	}
	s->ent->info |= 0x80000000u;
	return SCRIPT_DONE;
}

static int opMonsterFlags(Cmd *c)
{
	uint32_t sprIndex = c->u & 0x3ff;
	Sprite *s = scriptSprite(c, sprIndex);

	if (s->ent == NULL || s->ent->monster == NULL) {
		scriptError(c, MOD_STR_EV_MONSTERFLAGS, (int)sprIndex);
		return SCRIPT_DONE;
	}
	EntityMonster *m = s->ent->monster;
	uint16_t bits = (uint16_t)(c->u >> 10);
	int mode = c->arg >> 26;
	if (mode == 1)
		m->flags |= bits;
	else if (mode == 2)
		m->flags &= (uint16_t)~bits;
	else
		m->flags = bits;
	s->ent->info |= 0x80000000u;
	return SCRIPT_DONE;
}

static int opLeaveCombat(Cmd *c)
{
	Sprite *s = scriptSprite(c, c->arg);

	if (s->ent != NULL && s->ent->monster != NULL) {
		s->ent->monster->petrifyTurns = 0;
		s->ent->monster->flags &= ~0x200;
		EntityMonster *m = s->ent->monster;
		if (m->goal.type == 4)
			MonsterGoal_reset(&m->goal);
		Game_removeFromCombat(s->ent);
	}
	return SCRIPT_DONE;
}

static int opDeactivate(Cmd *c)
{
	uint32_t sprIndex = c->u & 0x3ff;
	Entity *e = scriptSprite(c, sprIndex)->ent;

	if (e == NULL) {
		scriptError(c, MOD_STR_EV_DEACTIVATE, (int)sprIndex);
		return SCRIPT_DONE;
	}
	Game_deactivate(c->game, e);
	Canvas_updateFacing(c->canvas);
	return SCRIPT_DONE;
}

static int opMakeCorpse(Cmd *c)
{
	Sprite *s = scriptSprite(c, c->u & 0xffff);
	EntityDef *def = s->ent->def;

	s->ent->info |= 0xc0000000u;
	s->ent->def = EntityDefs_find(c->thread->app->entityDefs, 9, def->eSubType, def->parm);
	s->ent->monster->flags |= 0x80;
	s->info = (s->info & 0xfffef0ffu) + 0x800; /* death frame 8 */
	if (s->ent->info & 0x80000)
		Game_deactivate(c->game, s->ent);
	Game_linkEntity(c->game, s->ent, s->x >> 6, s->y >> 6);
	return SCRIPT_DONE;
}

static int opOpenReliquary(Cmd *c)
{
	Entity *e = Game_getTileEntities(c->game, c->b0 << 6, shiftedTile(c->arg >> 8));

	for (; e != NULL; e = e->nextOnTile) {
		if (e->def->eType == 7 && e->def->eSubType == 3) {
			Render *r = c->thread->app->render;
			Sprite *s = Render_spriteAt(r, (e->info & 0xffff));
			s->info = (s->info & 0xffffff00u) + 0x9b; /* opened reliquary tile */
			e->info |= 0x80000000u;
			Game_unlinkEntity_2ae8c(c->thread->app->game, e);
			Game_openContainer(e);
			Sound_play(c->thread->app->sound, SND_RELIQUARY, 0, 3, -1);
			break;
		}
	}
	return SCRIPT_DONE;
}

static int opDamage(Cmd *c)
{
	uint32_t target = (c->u >> 16) & 0xffff;
	int dmg = (int)(c->u & 0xffff);

	if (target == 0xffff) {
		Player_hurtFeedback(c->player, 0);
		Player_takeDamage(c->thread->app->player, dmg, NULL);
		return SCRIPT_DONE;
	}
	Sprite *s = scriptSprite(c, target);
	if (s->ent == NULL) {
		scriptError(c, MOD_STR_EV_DAMAGEENTITY, (int)target);
		return SCRIPT_DONE;
	}
	if (s->ent->monster != NULL) {
		Entity_takeDamage(s->ent, dmg);
		if (CombatEntity_getHealth(&s->ent->monster->ce) > 0)
			return SCRIPT_DONE;
	}
	Entity_died(s->ent, 1);
	return SCRIPT_DONE;
}

static int opAiGoal(Cmd *c)
{
	Entity *e = scriptSprite(c, c->u & 0xffff)->ent;

	if (e != NULL) {
		MonsterGoal *goal = &e->monster->goal;
		MonsterGoal_reset(goal);
		goal->type = 1;
		goal->x = (uint8_t)c->b2;
		goal->y = (uint8_t)c->b3;
	}
	return SCRIPT_DONE;
}

static int opAiState(Cmd *c)
{
	Sprite *s = scriptSprite(c, c->u & 0xffff);

	if (s->ent == NULL)
		return SCRIPT_DONE;
	/* as DS executeEvent 0x0204d388 (EV_AIGOAL, 0x5a): the monster leaves the fight it is in, only
	 * goals 3 and 10 think at once (4, fleeing, lasts 20 turns), and a fight that starts pauses the
	 * thread until the monsters' turn is over (FRAME_COMBAT, Canvas_combatTurnDone) */
	c->game->yieldFlag = 1;
	MonsterGoal *goal = &s->ent->monster->goal;
	MonsterGoal_reset(goal);
	goal->type = (uint8_t)c->b2;
	if (c->b2 == 4)
		goal->param = 20;
	Game_removeFromCombat(s->ent);
	if (goal->type == 3 || goal->type == 10)
		Entity_aiThink(s->ent);
	goal->flags &= 0xb7;

	Entity *attacker = c->thread->app->game->combatMonsters;
	if (attacker == NULL)
		return SCRIPT_DONE;
	c->frame->frameFlags |= 8;		/* FRAME_COMBAT */
	Combat_start(c->thread->app->combat, attacker, attacker->monster->target, 0, 0);
	return SCRIPT_YIELD;
}

static int opEntityFrame(Cmd *c)
{
	Sprite *s = scriptSprite(c, c->u & 0xffff);

	if (s->ent == NULL)
		return SCRIPT_DONE;
	if (c->b2 == 0xff)
		s->info &= 0x7ffff0ffu;
	else
		s->info = (s->info & ~0xf00u) | (c->b2 << 8) | 0x80000000u;
	return SCRIPT_DONE;
}

/* 0x23 / 0x64: trap effect at a world position, colour 0xff000000 | rgb. */
static int opTrapEffect(Cmd *c)
{
	int kind, x, y;
	uint32_t color;

	if (c->opcode == EV_PARTICLES) {
		Canvas *cv = c->canvas;
		color = (c->b0 << 16) | (c->b1 << 8) | c->b2;
		y = cv->viewY + cv->stepY * 64;
		x = cv->viewX + cv->stepX * 64;
		kind = (c->u >> 24) & 0x1f;
	} else {
		/* RGB565-style packed colour: 5 bits red, 6 green, 5 blue */
		kind = (c->u >> 10) & 0xf;
		color = (((c->u >> 14) & 0x1f) << 19) | (((c->u >> 19) & 0x3f) << 10) | (((c->u >> 25) & 0x1f) << 3);
		Sprite *s = scriptSprite(c, c->u & 0x3ff);
		x = s->x;
		y = s->y;
	}
	Game_trapAttack(c->game, kind, x, y, (int)(color | 0xff000000u));
	return SCRIPT_DONE;
}

/* ---- player ---- */

static int opReqStat(Cmd *c)
{
	if (Player_requireStat(c->player, c->b0, c->b1) != 0)
		return SCRIPT_DONE;

	char *text = mapString(c, c->arg >> 16);
	if (text[0] != '\0') {
		lowerWand(c->thread->app->player);
		Canvas_showDialog(c->canvas, NULL, text, 6, 0, 0);
	}
	return SCRIPT_ABORT;
}

static int opCheckItem(Cmd *c)
{
	/* b2: bit0 set = at least b2>>1, clear = at most b2>>1 */
	int bound = (int)(c->b2 >> 1);
	int lo, hi;
	if (c->b2 & 1) {
		lo = bound;
		hi = 9999999;
	} else {
		lo = 0;
		hi = bound;
	}
	if (Player_requireItem(c->player, c->b0, c->b1, lo, hi) != 0)
		return SCRIPT_DONE;

	char *text = c->thread->app->render->mapStrings[c->b3];
	if (text[0] != '\0')
		Hud_addMessage(c->thread->app->hud, text, 1);
	return SCRIPT_ABORT;
}

static int opGiveWeapon(Cmd *c)
{
	int w = c->b0;

	if (c->b1 != 0) {
		Player_give(c->player, 1, w, 1, 0);
		Player_selectWeapon(c->thread->app->player, w);
		return SCRIPT_DONE;
	}
	c->player->weapons &= ~bit32(w);
	Player *p = c->thread->app->player;
	if (p->ce.weapon == w)
		Player_selectNextWeapon(p);
	if (w == 0)
		Player_showHelp(c->thread->app->player, 0x10, 1);
	return SCRIPT_DONE;
}

static int opFountain(Cmd *c)
{
	uint8_t energyBefore = c->player->ammo[1];

	Player_give(c->player, 3, 1, 5, 0);
	Player *p = c->thread->app->player;
	if (p->ammo[1] == energyBefore
	    && CombatEntity_getHealth(&p->ce) == CombatEntity_getMaxHealth(&c->thread->app->player->ce)) {
		Hud_addMessage(c->thread->app->hud, (char *)ModData_str(&g_mod, MOD_STR_HEALTH_ENERGY_FULL), 0);
		c->thread->app->game->yieldFlag = 1;
		return SCRIPT_DONE;
	}
	Player_addHealth(c->thread->app->player, 20);
	Sound_play(c->thread->app->sound, SND_FOUNTAIN, 0, 2, -1);
	return SCRIPT_DONE;
}

static int opAwardSecret(Cmd *c)
{
	Applet *app = c->thread->app;
	Canvas *cv = c->canvas;
	int found, total;

	Hud_addMessage(c->app->hud, (char *)ModData_str(&g_mod, MOD_STR_FOUND_SECRET), 0);
	Sound_play(app->sound, SND_LEVER, 0, 3, -1);
	Player_countSecrets(app->player, &found, &total);

	if (found + 1 == total && !(app->player->secretsDoneMaps & bit32((uint32_t)(cv->mapId - 6000) & 0xff))) {
		/* last secret of the map: bonus of 1/16 of the current level's XP span */
		Sound_play(app->sound, SND_SECRETS_CLEARED, 2, 5, -1);
		Player *p = app->player;
		int xpHere = Player_xpForLevel(p, p->level);
		int bonus = (xpHere - Player_xpForLevel(app->player, app->player->level - 1)) >> 4;

		int slot = cv->helpBufIdx;
		char *buf = cv->helpBuf + slot * 600;
		cv->helpBufIdx = (slot + 1) % 2;
		App_snprintf(buf, 600, (char *)ModData_str(&g_mod, MOD_STR_SECRETS_CLEARED), app->render->mapName, bonus);
		Canvas_addHelpMessage(cv, buf, 2);
		Player_addXP(app->player, bonus);
		app->player->secretsDoneMaps |= bit32((uint32_t)(cv->mapId - 6000) & 0xff);
	} else {
		Player_addXP(app->player, 5);
	}
	Hud_randomBark(app->hud, 8, 0);
	return SCRIPT_DONE;
}

static int opPayGold(Cmd *c)
{
	Applet *app = c->thread->app;
	int goldBefore = c->player->gold;

	Player_give(c->player, 4, 0, -(int)(c->u & 0xffff), 0);
	int paid = goldBefore - app->player->gold;
	/* Player_give counted the payment as gold spent/gained; undo both */
	app->statGoldSpent -= paid;
	app->statGoldGained -= paid;
	if (paid <= 0)
		return SCRIPT_DONE;

	app->game->goldPaid = (int16_t)(app->game->goldPaid + paid);
	int msg = c->arg >> 16;
	if (msg >= 0) {
		char *buf = Hud_getMessageBuffer(app->hud, 3);
		const char *fmt = app->render->mapStrings[msg];
		snprintf(buf, 0x40, fmt, paid);
		Hud_finishMessageBuffer(app->hud);
	}
	Sound_play(app->sound, SND_PAY_GOLD, 0, 3, -1);
	Canvas_startShake(c->canvas, 0, 2, 400);
	return SCRIPT_DONE;
}

static int opCheckMood(Cmd *c)
{
	int mood = c->player->mood;

	if (mood > (int)c->b1 || mood < (int)c->b0) {
		char *text = mapString(c, c->u >> 16);
		if (text[0] != '\0')
			Hud_addMessage(c->app->hud, text, 1);
		return SCRIPT_ABORT;
	}
	return SCRIPT_DONE;
}

/* ---- world ---- */

static int opLerpFlat(Cmd *c)
{
	int duration = c->arg >> 16;
	int layer = c->b0; /* 0 floor, 1 ceiling */

	if (duration == 0) {
		if (layer == 0)
			c->app->render->floorHeight = (uint8_t)c->b1;
		else
			c->app->render->ceilHeight = (uint8_t)c->b1;
		return SCRIPT_DONE;
	}
	if (layer > 1) {
		Oe_warn("EV_LERPFLAT: layer %d out of range", layer);
		return SCRIPT_DONE;
	}
	Render *r = c->thread->app->render;
	r->flatLerps[layer].duration = duration;
	r->flatLerps[layer].startTime = c->thread->app->tickTime;
	r->flatLerps[layer].to = c->b1;
	r->flatLerps[layer].from = layer == 0 ? r->floorZ : r->ceilZ;
	return SCRIPT_DONE;
}

static int opWait(Cmd *c)
{
	ScriptThread *t = c->thread;

	t->waitTime = (int32_t)GETUPTIMEMS() + c->arg;
	if (c->frame->frameFlags & 1) {
		/* inside a cinematic: the camera clock waits too */
		c->game->cameraStart += c->arg;
		if (c->canvas->state != 0x14)
			c->canvas->ignoreKeysUntil = t->waitTime;
	}
	return SCRIPT_YIELD;
}

static int opStartCinematic(Cmd *c)
{
	Game *g = c->game;
	int8_t key = (int8_t)c->b1;

	if (key <= 0)
		return SCRIPT_DONE;
	if (c->b0 >= g->numCameras) {
		Oe_print("EV_STARTCINEMATIC: camera %u out of range\n", (unsigned)c->b0);
		return SCRIPT_DONE;
	}
	if (key == 1) {
		/* DS: keys marked so are where the player is when the camera starts */
		Camera *cam = &g->cameras[c->b0];
		for (int i = 0; i < cam->numKeys; i++) {
			CameraKey *k = &cam->keys[i];
			if (k->dsFlags & 1)
				k->x = c->canvas->viewX;
			if (k->dsFlags & 2)
				k->y = c->canvas->viewY;
			if (k->dsFlags & 4)
				k->z = c->canvas->viewZ;
			if (k->dsFlags & 8)
				k->angle = c->canvas->viewAngle & 0x3ff;
		}
	}
	g->cameraKey = (int8_t)(key - 1);
	g->activeCamera = &g->cameras[c->b0];
	g->cameras[c->b0].x = g->cameras[c->b0].keys[(int8_t)g->cameraKey].x;
	g->activeCamera->y = g->activeCamera->keys[(int8_t)g->cameraKey].y;
	g->activeCamera->z = g->activeCamera->keys[(int8_t)g->cameraKey].z;
	g->activeCamera->angle = g->activeCamera->keys[(int8_t)g->cameraKey].angle;
	g->activeCamera->thread = c->thread;
	g->cameraStart = (int32_t)GETUPTIMEMS();
	if (c->canvas->state != 2 && c->canvas->state != 0x14)
		Canvas_setState(c->canvas, 0x14);
	return yieldWithFlag(c);
}

static int opMoveBlock(Cmd *c)
{
	int duration = (c->arg >> 20) * 100;
	int srcX = tileCenter((c->u >> 15) & 0x1f);
	int srcY = tileCenter((c->u >> 10) & 0x1f);
	int dstX = tileCenter((c->u >> 5) & 0x1f);
	int dstY = tileCenter(c->u & 0x1f);

	Game_startMovingBlock(c->game, srcX, srcY, dstX, dstY, duration, c->opcode == EV_MOVEBLOCK_NB);
	int dx = c->canvas->viewX - srcX;
	int dy = c->canvas->viewY - srcY;
	if (dx * dx + dy * dy < 0x19000)
		Sound_play(c->thread->app->sound, SND_MOVING_BLOCK, 0, 3, -1);
	return SCRIPT_DONE;
}

static int opOpenStore(Cmd *c)
{
	Canvas *cv = c->canvas;

	if (c->arg == 0) {
		MenuSystem_setMenu(c->app->menuSystem, 0x12e);
	} else if (cv->helpCount > 0) {
		Canvas_dequeueHelpDialog(cv, 0);
		cv->fec8 = 1;
	} else {
		Canvas_setState(cv, 2);
	}
	return SCRIPT_DONE;
}

static int opEndGame(Cmd *c)
{
	Game *g = c->game;

	c->app->player->gameWon = 1;
	c->canvas->creditsRunning = 1;
	g->cinematicActive = 0;
	g->skipUntil = 0;
	g->skippable = 0;
	Canvas_setState(c->canvas, 0xf);
	return SCRIPT_ABORT;
}

static int opSkippable(Cmd *c)
{
	Game *g = c->game;

	g->cinematicActive = 0;
	if (c->opcode == EV_SKIPPABLE) {
		g->skipUntil = c->arg != 0 ? 0x7fffffff : (int32_t)GETUPTIMEMS() + 1000;
		g->skippable = 1;
		return SCRIPT_DONE;
	}
	g->skipUntil = 0;
	g->skippable = 0;
	Canvas_setState(c->canvas, 3);
	return SCRIPT_DONE;
}

static int opEndCombat(Cmd *c)
{
	while (c->thread->app->game->combatMonsters != NULL)
		Game_removeFromCombat(c->thread->app->game->combatMonsters);
	if (c->canvas->state == 4)
		Canvas_setState(c->canvas, 3);
	return SCRIPT_DONE;
}

/* ---- Orcs & Elves DS: its own commands (DS ScriptThread_executeEvent 0x0204d388, cases
 * 0x69..0x75; DsScript_brewOp keeps their numbers) ---- */

static void dsLog(const Cmd *c, const char *what)
{
	char tr[8];
	trap_Cvar_VariableStringBuffer("oe_dsTrace", tr, sizeof tr);
	if (tr[0] == '1')
		Oe_print("script: DS %s 0x%08x not done yet\n", what, (unsigned)c->u);
}

/* EV_RESPAWNMONSTER (DS case 0x6d) */
static int dsRespawnMonster(Cmd *c)
{
	int sprIndex = (int)(c->u & 0x3ff), tile = (int)((c->u >> 10) & 0x3ff);
	int x = (tile & 31) * 64 + 32, y = (tile >> 5) * 64 + 32;
	uint32_t opts = c->u >> 20;
	Entity *ent, *there;

	if (sprIndex >= c->render->numSprites || (ent = scriptSprite(c, sprIndex)->ent) == NULL) {
		Oe_print("EV_RESPAWNMONSTER on non entity at %d\n", sprIndex);
		return SCRIPT_NOT_RUN;
	}
	if (!(ent->info & 0x40020000u))		/* not dead */
		return (opts & 2) ? SCRIPT_ABORT : SCRIPT_DONE;
	there = Game_findEntityAt(c->game, x, y, 0x406);
	if (there != NULL && !(there->def->eType == 2 && there->def->eSubType == 0xe))
		return (opts & 2) ? SCRIPT_ABORT : SCRIPT_DONE;
	Entity_resurrect(ent, x, y, 0x20);
	if (opts & 1) {
		ent->info |= 0x20000000u;
		Game_activateEx(c->game, ent, 1, 0);
	}
	return SCRIPT_DONE;
}

/* EV_LERPSPRITETOOPENTILE (DS case 0x71) */
static int dsLerpToOpenTile(Cmd *c)
{
	int sprIndex = (int)((c->u >> 7) & 0x3ff), tile, ox, oy, open;
	Sprite *s;
	Entity *ent, *list;
	LerpSprite *ls;

	if (sprIndex >= c->render->numSprites)
		return SCRIPT_DONE;
	s = scriptSprite(c, sprIndex);
	ent = s->ent;
	tile = (s->y >> 6) * 32 + (s->x >> 6);
	if (tile < 0 || tile >= 32 * 32)
		return SCRIPT_DONE;
	list = c->game->tileLists[tile];	/* (its own tile is left out of the search) */
	c->game->tileLists[tile] = NULL;
	open = Entity_findOpenAdjacentTile(c->game, s->x, s->y, -1, ent, &ox, &oy);
	c->game->tileLists[tile] = list;
	if (!open)
		return SCRIPT_DONE;
	ls = Game_allocLerpSprite(c->game, c->thread, (short)sprIndex);
	if (ls == NULL)
		return SCRIPT_DONE;
	ls->dstX = (int16_t)ox;
	ls->dstY = (int16_t)oy;
	ls->dstZ = (int16_t)s->z;
	if (ent != NULL) {
		ent->info = (ent->info & ~0x400000u) | 0x88000000u;
		if (ent->monster != NULL) {
			ent->monster->destX = ox;
			ent->monster->destY = oy;
		}
	}
	ls->srcX = (int16_t)s->x;
	ls->srcY = (int16_t)s->y;
	ls->srcZ = (int16_t)s->z;
	ls->dstScale = s->scale;
	ls->srcScale = s->scale;
	ls->startTime = c->thread->app->gameTime;
	ls->duration = (int32_t)((c->u & 0x7f) * 100);
	LerpSprite_calcDist(ls);
	ls->flags = 0;
	if (ent != NULL) {
		/* the entity is on its new tile at once (flag 2: the move leaves its tile alone) */
		ls->flags = 2;
		Game_unlinkEntity_2ae8c(c->game, ent);
		Game_linkEntity(c->game, ent, ox >> 6, oy >> 6);
		Canvas_updateFacing(c->canvas);
	}
	return yieldWithFlag(c);
}

static int dsCommand(Cmd *c)
{
	Render *r = c->render;
	Canvas *cv = c->canvas;
	int tile, loc;

	switch (c->opcode) {
	case DS_OP_MARKTILE:		/* tile lo16 at location b2: 0 flag 0x80, 1 seen (automap), 2 flag 0x100 */
	case DS_OP_UNMARKTILE: {
		int set = c->opcode == DS_OP_MARKTILE;
		tile = (int)(c->u & 0xffff) & 0x3ff;
		loc = (int)((c->u >> 16) & 0xffff);
		switch (loc) {
		case 0:
		case 2: {
			uint16_t bit = loc == 0 ? 0x80 : 0x100;
			r->dsTileMarks[tile] = set ? (uint16_t)(r->dsTileMarks[tile] | bit) : (uint16_t)(r->dsTileMarks[tile] & ~bit);
			break;
		}
		case 1:
			if (set)
				r->mapFlags[tile] |= 0x10;
			else
				r->mapFlags[tile] &= (uint8_t)~0x10;
			break;
		default:
			dsLog(c, set ? "EV_MARKTILE (monster clip)" : "EV_UNMARKTILE (monster clip)");
			break;
		}
		return SCRIPT_DONE;
	}

	case DS_OP_TOUCHENTITIES:	/* what lies on the player's tile is touched */
		Game_touchTile(c->game, cv->destX, cv->destY, 1);
		return SCRIPT_DONE;

	case DS_OP_TURNPLAYERTO: {	/* face direction code arg, turning the short way */
		int a = Dir_toAngle((int)c->u);
		int v = cv->viewAngle & 0x3ff;
		cv->viewAngle = v;
		if (abs(v - (a + 0x400)) < abs(v - a))
			a += 0x400;
		else if (abs(v - (a - 0x400)) < abs(v - a))
			a -= 0x400;
		cv->destAngle = a;
		return SCRIPT_DONE;
	}

	case DS_OP_SET:				/* player flag arg & 0x1f set if arg >> 5 is not 0, else cleared */
		if (c->u >> 5)
			c->player->dsFlags |= 1u << (c->u & 0x1f);
		else
			c->player->dsFlags &= ~(1u << (c->u & 0x1f));
		return SCRIPT_DONE;

	case DS_OP_TRIGGERIF: {		/* run tile b0, b1's event if player flag b2 (& 0x1f) is set
								 * (bit 21 set) or clear (bit 21 clear) */
		int want = (c->u >> 21) & 1;
		int has = (c->player->dsFlags >> ((c->u >> 16) & 0x1f)) & 1;
		int rc;
		if (want != has)
			return SCRIPT_DONE;
		c->frame->frameFlags |= 4;
		rc = ScriptThread_executeTile(c->thread, c->b0, c->b1, (int)(c->flags & 0x2000000) | 0xf000,
			c->frame->frameFlags & 1);
		c->frame->frameFlags &= ~4;
		return rc == SCRIPT_YIELD ? SCRIPT_YIELD : SCRIPT_DONE;
	}

	case DS_OP_ABORTUNLESSTILEOPEN: {	/* stop the event if tile b0 & 0x1f, (arg >> 5) & 0x1f is
										 * taken; with bit 18: unless a tile next to it is free */
		int tx = (int)(c->u & 0x1f), ty = (int)((c->u >> 5) & 0x1f);
		int x = tx * 64 + 32, y = ty * 64 + 32, ox, oy, open;
		if (c->u & 0x40000) {
			/* (the tile's own entities are left out of the search) */
			Entity *list = c->game->tileLists[ty * 32 + tx];
			c->game->tileLists[ty * 32 + tx] = NULL;
			open = Entity_findOpenAdjacentTile(c->game, x, y, -1, NULL, &ox, &oy);
			c->game->tileLists[ty * 32 + tx] = list;
			return open ? SCRIPT_DONE : SCRIPT_ABORT;
		}
		Game_trace(c->game, x, y, x, y, NULL, 0xfdaf);
		return c->game->numTraceHits != 0 ? SCRIPT_ABORT : SCRIPT_DONE;
	}

	case DS_OP_EMITTER:			/* the map's ambient sound emitter b0 on / off (bit 16): DS Sound_setEmitter */
		dsLog(c, "emitter");	/* (the DS's ambient sounds are not done yet) */
		return SCRIPT_DONE;

	case DS_OP_LAUNCHFLAMES:	/* flames from tile b0, b1 to tile b2, b3 (DS 0x020222a0) */
		Combat_launchFlames(c->app->combat, c->b0 * 64 + 32, c->b1 * 64 + 32, c->b2 * 64 + 32, c->b3 * 64 + 32);
		return SCRIPT_DONE;

	case DS_OP_RESPAWNMONSTER:	/* the dead monster of sprite arg & 0x3ff comes back on tile (arg >> 10) & 0x3ff;
								 * bit 20: angry and active, bit 21: abort the event when it cannot */
		return dsRespawnMonster(c);

	case DS_OP_LERPSPRITETOOPENTILE:	/* sprite (arg >> 7) & 0x3ff moves to a free tile next to it in
										 * (arg & 0x7f) * 100 ms, the event waiting */
		return dsLerpToOpenTile(c);

	case DS_OP_PLAYER75:		/* the player gets collectible arg - 1 (of 15) */
		c->player->dsCollectibles |= 1u << ((c->u - 1) & 0xff);
		return SCRIPT_DONE;

	default:					/* 0x73: the state of an entry of a list the player keeps (+0x100) */
		dsLog(c, "player command");
		return SCRIPT_DONE;
	}
}

/* 0x0000ea8c */
int ScriptThread_executeCommand(ScriptThread *thread, void *framePtr, int opcode, int arg, uint32_t cmdFlags)
{
	Cmd c;
	c.flags = cmdFlags;
	c.thread = thread;
	c.frame = framePtr;
	c.app = thread->app;
	c.game = c.app->game;
	c.canvas = c.app->canvas;
	c.render = c.app->render;
	c.player = c.app->player;
	c.opcode = opcode;
	c.arg = arg;
	c.u = (uint32_t)arg;
	c.b0 = c.u & 0xff;
	c.b1 = (c.u >> 8) & 0xff;
	c.b2 = (c.u >> 16) & 0xff;
	c.b3 = c.u >> 24;

	if ((unsigned)opcode >= EV_COUNT && (opcode < DS_OP_MARKTILE || opcode > DS_OP_LERPSPRITE77))
		return SCRIPT_DONE;

	switch (opcode) {
	case EV_GOTO:
		return opGoto(&c);
	case EV_CHANGEMAP:
		return opChangeMap(&c);
	case EV_TRIGGER:
	case EV_TRIGGERTHREAD:
		return opTrigger(&c);
	case EV_MESSAGE:
		return opMessage(&c);
	case EV_PAIN:
		Hud_addMessage(c.app->hud, (char *)ModData_str(&g_mod, MOD_STR_PAIN), 0);
		Player_hurtFeedback(thread->app->player, 0);
		Player_takeDamage(thread->app->player, arg, NULL);
		return SCRIPT_DONE;
	case EV_DOOR:
	case EV_OPENLINE:
	case EV_CLOSELINE:
	case EV_MOVELINE:
		return Game_useDoor(c.game, opcode, arg, 1) != 0 ? SCRIPT_DONE : SCRIPT_NOT_RUN;
	case EV_SHOW:
	case DS_OP_SHOWWAKE:
		return opShow(&c);
	case EV_DIALOG:
		return opDialog(&c);
	case EV_REVEALAUTOMAP:
		Game_revealAutomapRect(c.game, c.b3, c.b2, c.b1, c.b0);
		return SCRIPT_DONE;
	case EV_PASSWORD:
		return opPassword(&c);
	case EV_SETSTATE:
	case EV_NEXTSTATE:
	case EV_PREVSTATE:
	case EV_RANDSTATE:
		return opTileEventState(&c);
	case EV_LOCK:
		Game_setLineLocked(c.game, arg, 1, 0);
		return SCRIPT_DONE;
	case EV_UNLOCK:
		Game_setLineLocked(c.game, arg, 0, 0);
		return SCRIPT_DONE;
	case EV_TOGGLELOCK:
		Game_setLineLocked(c.game, arg, (c.render->lines[arg].flags & 0x400) ? 0 : 1, 0);
		return SCRIPT_DONE;
	case EV_HIDE:
		return opHide(&c);
	case EV_INCSTAT:
	case EV_DECSTAT:
		/* the 4th argument is the opcode, left in r3 by the dispatch */
		Player_modifyStat(c.player, c.b0, opcode == EV_INCSTAT ? (int)c.b1 : -(int)c.b1, opcode);
		return SCRIPT_DONE;
	case EV_REQSTAT:
		return opReqStat(&c);
	case EV_FORCEMESSAGE:
		return opForceMessage(&c);
	case EV_ANIM:
		Game_gspriteAllocAnim(c.game, c.b2, tileCenter(c.b0), tileCenter(c.b1), 0x20);
		return SCRIPT_DONE;
	case EV_GIVEITEM:
		Player_give(c.player, c.b0, c.b1, arg >> 16, 0);
		return SCRIPT_DONE;
	case EV_TAKEITEM:
		Player_give(c.player, c.b0, c.b1, -(arg >> 16), 0);
		return SCRIPT_DONE;
	case EV_SETRESPAWN:
		return opSetRespawn(&c);
	case EV_ABORTMOVE:
		c.canvas->abortMove = 1;
		return SCRIPT_DONE;
	case EV_SCREENSHAKE:
		Canvas_startShake(c.canvas, c.u & 0xfff, (int8_t)c.b3, (c.u >> 12) & 0xfff);
		return SCRIPT_DONE;
	case EV_FLOORCOLOR:
		thread->app->render->floorColor = (uint16_t)Color_rgb888to565(c.render, arg);
		return SCRIPT_DONE;
	case EV_CEILCOLOR:
		thread->app->render->ceilColor = (uint16_t)Color_rgb888to565(c.render, arg);
		return SCRIPT_DONE;
	case EV_ENABLEWEAPONS:
		if (arg != 0)
			Player_refreshWeapons(c.player);
		else
			Player_holsterWeapon(c.player);
		return SCRIPT_DONE;
	case EV_OPENSTORE:
		return opOpenStore(&c);
	case EV_CHANGESPRITE:
		return opChangeSprite(&c);
	case EV_PARTICLES:
	case EV_SPRITEPARTICLES:
		return opTrapEffect(&c);
	case EV_REFRESHVIEW:
		Canvas_updateFacing(c.canvas);
		return SCRIPT_DONE;
	case EV_WAIT:
		return opWait(&c);
	case EV_CHECKITEM:
		return opCheckItem(&c);
	case EV_NOTE:
		Player_addNotebookIndex(c.player, (int16_t)arg);
		Player_showHelp(thread->app->player, 10, 0);
		return SCRIPT_DONE;
	case EV_ENDGAME:
		return opEndGame(&c);
	case EV_PLAYSOUND:
		Sound_play(c.app->sound, c.u & 0xffff, 0, 3, -1);
		return SCRIPT_DONE;
	case EV_WANDCHAT:
		Hud_addTimedMsg(c.app->hud, mapString(&c, arg));
		return SCRIPT_DONE;
	case EV_WAKEMONSTER:
		return opWakeMonster(&c);
	case EV_LERPFLAT:
		return opLerpFlat(&c);
	case EV_LERPSPRITE:
	case EV_LERPSPRITE_NOWAIT:
	case EV_LERPSPRITE_TO:
	case EV_LERPSPRITE_REL:
	case DS_OP_LERPSPRITE77:
		return opLerpSprite(&c);
	case EV_SWITCHON:
	case EV_SWITCHOFF:
	case EV_SWITCHTOGGLE:
		return opSwitch(&c);
	case EV_STARTCINEMATIC:
		return opStartCinematic(&c);
	case EV_LERPFLAGS:
		return opLerpFlags(&c);
	case EV_FIREPORTAL: {
		int on = (c.u & 0x100) != 0;
		if (on)
			c.player->firePortals |= bit32(c.b0);
		else
			c.player->firePortals &= ~bit32(c.b0);
		Game_setFirePortalEnabled(c.game, c.b0, on);
		return SCRIPT_DONE;
	}
	case EV_SHOWCHATBUBBLE: {
		uint32_t sprIndex = c.u & 0xffff;
		Entity *e = scriptSprite(&c, sprIndex)->ent;
		if (e != NULL && e->def->eType == 3) {
			e->param = (int32_t)c.b2;
			e->info |= 0x80000000u;
		} else {
			scriptError(&c, MOD_STR_EV_CHATBUBBLE, (int)sprIndex);
		}
		return SCRIPT_DONE;
	}
	case EV_SKIPPABLE:
	case EV_ENDSKIPPABLE:
		return opSkippable(&c);
	case EV_FLOORFLAT:
		c.render->floorFlat = arg == 1;
		return SCRIPT_DONE;
	case EV_CEILFLAT:
		c.render->ceilFlat = arg == 1;
		return SCRIPT_DONE;
	case EV_WOBBLE:
		c.render->wobble = arg == 1;
		return SCRIPT_DONE;
	case EV_NAMEENTITY:
		return opNameEntity(&c);
	case EV_MONSTERSTAT:
		return opMonsterStat(&c);
	case EV_MONSTERFLAGS:
		return opMonsterFlags(&c);
	case EV_LEAVECOMBAT:
		return opLeaveCombat(&c);
	case EV_DEACTIVATE:
		return opDeactivate(&c);
	case EV_GIVEWEAPON:
		return opGiveWeapon(&c);
	case EV_MOVEBLOCK:
	case EV_MOVEBLOCK_NB:
		return opMoveBlock(&c);
	case EV_ENDWANDCHAT:
		Player_setWandState(c.player, 0);
		thread->app->hud->numSubtitles = 0;
		return SCRIPT_DONE;
	case EV_AIGOAL:
		return opAiGoal(&c);
	case EV_MAKECORPSE:
		return opMakeCorpse(&c);
	case EV_OPENRELIQUARY:
		return opOpenReliquary(&c);
	case EV_SCALESPRITE:
		return opScaleSprite(&c);
	case EV_FOUNTAIN:
		return opFountain(&c);
	case EV_AWARDSECRET:
		return opAwardSecret(&c);
	case EV_PAYGOLD:
		return opPayGold(&c);
	case EV_NPCPARAM: {
		Entity *e = Game_findEntityAt(c.game, c.b0 << 6, c.b1 << 6, 0x80);
		if (e != NULL)
			e->param = (int32_t)c.b2;
		return SCRIPT_DONE;
	}
	case EV_DAMAGE:
		return opDamage(&c);
	case EV_SETMOOD:
		if (c.u <= 0xff)
			c.player->mood = (uint8_t)c.u;
		return SCRIPT_DONE;
	case EV_ADDMOOD:
		Player_addMood(c.player, arg);
		return SCRIPT_DONE;
	case EV_AISTATE:
		return opAiState(&c);
	case EV_BOSSREINFORCEMENTS:
		Game_updateBossReinforcements(c.game, c.b0, (c.u >> 8) & 0xffff);
		return SCRIPT_DONE;
	case EV_FADE:
		c.canvas->fadeStart = (int32_t)GETUPTIMEMS();
		c.canvas->fadeDuration = arg;
		c.canvas->fadeDir = 0;
		return SCRIPT_DONE;
	case EV_PUSHPLAYER:
		Entity_knockback(&c.game->entities[1], tileCenter(c.b0), tileCenter(c.b1), tileCenter(c.b2));
		return SCRIPT_DONE;
	case EV_CHECKMOOD:
		return opCheckMood(&c);
	case EV_TIMESTAMP:
		c.player->timestamp = (arg & 1) ? c.app->gameTime : 0;
		return SCRIPT_DONE;
	case EV_ENTITYFRAME:
		return opEntityFrame(&c);
	case EV_RESTART:
		return SCRIPT_RESTART;
	case EV_ENDCOMBAT:
		return opEndCombat(&c);
	default:
		/* the DS's own commands (their numbers kept by DsScript_brewOp) */
		if (opcode >= DS_OP_MARKTILE && opcode <= DS_OP_PLAYER75)
			return dsCommand(&c);
		/* 0x00, 0x27, 0x2b, 0x2f (container record), 0x35/0x37/0x38 (camera data),
		 * 0x3a (fire-pit destination), 0x4c, DS_OP_DATA: data for other code, nothing to do */
		return SCRIPT_DONE;
	}
}
