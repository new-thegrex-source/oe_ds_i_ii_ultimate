/*
 * Entity.cpp / EntityDef.cpp / EntityMonster.cpp: map entities, their
 * definitions (entities.bin), death, loot, damage, knockback, save/load.
 *
 * Entity.info bits: low 16 sprite (1-based) or line index (0x200000),
 * 0x10000 attacking, 0x20000 gibbed/removed, 0x40000 alive/interactive,
 * 0x80000 active, 0x100000 loot dropped, 0x400000 needs lerp near doors,
 * 0x800000 summoned helper, 0x1000000 undead, 0x2000000 raise target,
 * 0x4000000 linked in tile grid, 0x10000000 in combat, 0x20000000 corpse,
 * 0x40000000 counts as a kill.
 */
#include "game.h"

#define MOD_MONSTER_SOUNDS  0x4716cu /* s16 [type*12 + slot], -1 = none; type 16 = weakened */
#define MOD_CORPSE_NAME_IDS 0x461b4u /* s16 [subtype*3 + parm]: name string id of an undead monster */
#define MOD_TILE_DIST_TABLE 0x45ebcu /* 15 x s32 squared distances (see Combat_WorldDistToTileDist) */

#define MAX_ENTITY_DEFS 200

static Sprite *entSprite(Entity *e) { return &g_app->render->sprites[(e->info & 0xffff) - 1]; }
static Line *entLine(Entity *e) { return &g_app->render->lines[(e->info & 0xffff) - 1]; }
static int32_t mul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static void setSpriteFrame(Sprite *s, int frame) { s->info = (s->info & ~0xf00u) | ((uint32_t)frame << 8); }

/* ------------------------------------------------------------------ directions */

/* 0x000010c4: view angle (multiple of 0x80) to the map's direction code */
int Dir_fromAngle(TextUtil *textUtil, int angle)
{
	(void)textUtil;
	if (angle & 0x7f)
		return 0;
	switch (angle >> 7) {
	case 0: return 1;
	case 1: return 4;
	case 2: return 2;
	case 3: return 5;
	case 5: return 7;
	case 6: return 3;
	case 7: return 6;
	default: return 0;
	}
}

/* 0x00001b30 */
int Dir_toAngle(TextUtil *textUtil, int dir)
{
	(void)textUtil;
	switch (dir) {
	case 0: return 0x200;
	case 2: return 0x100;
	case 3: return 0x300;
	case 4: return 0x80;
	case 5: return 0x180;
	case 6: return 0x380;
	case 7: return 0x280;
	default: return 0;
	}
}

/* 0x00008534: side of the viewer (ax,ay looking along angle) that b is on:
 * 0 front, 1 right, 2 back, 3 left (drives the HUD damage arrow) */
int Dir_relativeQuadrant(int x, int ax, int ay, int angle, int bx, int by)
{
	(void)x;
	switch (angle & 0x3ff) {
	case 0:
		if (by > ay)
			return 1;
		if (by < ay)
			return 3;
		return bx < ax ? 2 : 0;
	case 0x100:
		if (by > ay)
			return 2;
		if (by < ay)
			return 0;
		if (bx < ax)
			return 3;
		if (bx > ax)
			return 1;
		return 0;
	case 0x200:
		if (by > ay)
			return 3;
		if (by < ay)
			return 1;
		return bx > ax ? 2 : 0;
	case 0x300:
		if (by > ay)
			return 0;
		if (by < ay)
			return 2;
		if (bx < ax)
			return 1;
		if (bx > ax)
			return 3;
		return 0;
	default:
		return 0;
	}
}

/* ------------------------------------------------------------------ definitions */

/* 0x0001a0b4: parm -1 matches any */
EntityDef *EntityDefs_find(EntityDefManager *defs, int eType, int eSubType, int parm)
{
	for (int i = 0; i < defs->numDefs; i++) {
		EntityDef *d = &defs->list[i];
		if (d->eType == eType && d->eSubType == eSubType && (parm == -1 || d->parm == parm))
			return d;
	}
	return NULL;
}

/* 0x0001a118 */
EntityDef *EntityDefs_findType(EntityDefManager *defs, int eType, int eSubType)
{
	return EntityDefs_find(defs, eType, eSubType, -1);
}

/* 0x0002b628 */
EntityDef *EntityDef_findByTile(EntityDefManager *defs, int tile)
{
	for (int i = 0; i < defs->numDefs; i++) {
		if (defs->list[i].tileIndex == tile)
			return &defs->list[i];
	}
	return NULL;
}

/* 0x0003dd98: entities.bin (resource 1000): u16 count, then 13-byte records
 * {s16 tile, u8 subId, u8 eType, u8 eSubType, s16 parm, s16 name, s16 longName,
 * s16 description}; the texts are ids into string group 1. */
int EntityDef_startup(EntityDefManager *ed)
{
	Applet *app = g_app;
	InputStream is;
	int ok = 0;

	ed->numDefs = 0;
	if (!Text_loadStringGroup(app->canvas, 1))
		return 0;

	InputStream_ctor(&is);
	if (Resource_open(&is, 1000, 1)) {
		DataBuffer *buf = app->dataBuffer;
		Resource_bufferFill(buf, &is);
		int count = Resource_shiftShort(buf);
		ed->numDefs = count;
		if (count <= MAX_ENTITY_DEFS) {
			for (int i = 0; i < ed->numDefs; i++) {
				EntityDef *d = &ed->list[i];
				Resource_bufferEnsure(buf, &is, 0xd);
				d->tileIndex = (int16_t)Resource_shiftShort(buf);
				d->subId = (uint8_t)Resource_shiftByte(buf);
				d->eType = (uint8_t)Resource_shiftByte(buf);
				d->eSubType = (uint8_t)Resource_shiftByte(buf);
				d->parm = Resource_shiftShort(buf);
				Text_getString(app->canvas, 1, Resource_shiftShort(buf), &d->name);
				Text_getString(app->canvas, 1, Resource_shiftShort(buf), &d->longName);
				Text_getString(app->canvas, 1, Resource_shiftShort(buf), &d->description);
			}
			Canvas_unloadStringGroup(app->canvas, 1);
			ok = 1;
		} else {
			App_fatalError(app, "MAX_ENTITY_DEFS");
		}
	}
	InputStream_dtor(&is, 0);
	return ok;
}

/* 0x0003c160 */
void EntityDefManager_shutdown(EntityDefManager *edm)
{
	for (int i = 0; i < edm->numDefs; i++) {
		String_release(&edm->list[i].description);
		String_release(&edm->list[i].longName);
		String_release(&edm->list[i].name);
	}
}

/* ------------------------------------------------------------------ monster records */

/* 0x0003503c */
void MonsterGoal_reset(MonsterGoal *goal)
{
	goal->type = 0;
	goal->flags = 0;
	goal->turns = 0;
	goal->y = 0;
	goal->x = 0;
	goal->param = 0;
}

/* 0x00036e40 */
void MonsterGoal_saveState(MonsterGoal *goal, OutputStream *os)
{
	OutStream_writeBool(os, (int8_t)goal->type);
	OutStream_writeBool(os, (int8_t)goal->flags);
	OutStream_writeBool(os, (int8_t)goal->turns);
	OutStream_writeBool(os, (int8_t)goal->x);
	OutStream_writeBool(os, (int8_t)goal->y);
	OutStream_writeShort(os, (int16_t)goal->param);
}

/* 0x00028f80 */
void MonsterGoal_loadState(MonsterGoal *goal, InputStream *stream)
{
	goal->type = (uint8_t)Stream_readUByte(stream);
	goal->flags = (uint8_t)Stream_readUByte(stream);
	goal->turns = (uint8_t)Stream_readUByte(stream);
	goal->x = Stream_readUByte(stream);
	goal->y = Stream_readUByte(stream);
	goal->param = Stream_readShort(stream);
}

/* 0x00009a8c */
void EntityMonster_clearEffects(EntityMonster *mon)
{
	if (mon->monsterEffects & 4) /* weakened: restore the halved stats */
		CombatEntity_scaleStats(&mon->ce, 0);
	mon->monsterEffects = 0;
}

/* 0x00034ff8 */
void EntityMonster_reset(EntityMonster *monster)
{
	monster->prevOnList = NULL;
	monster->nextOnList = NULL;
	monster->target = NULL;
	monster->nextAttacker = NULL;
	monster->destY = 0;
	monster->destX = 0;
	monster->frameTime = 0;
	monster->f60 = 0;
	monster->flags = 0;
	EntityMonster_clearEffects(monster);
	MonsterGoal_reset(&monster->goal);
}

/* 0x0001c270 */
int EntityMonster_getSound(EntityMonster *mon, int slot)
{
	if ((g_app->render->mapFlagsByte & 1) && (unsigned)slot <= 3)
		return -1; /* quiet map: no idle/alert sounds */
	int type = (mon->monsterEffects & 4) ? 0x10 : mon->ce.type;
	return ModData_s16(&g_mod, MOD_MONSTER_SOUNDS + (slot + type * 12) * 2);
}

/* 0x0001c08c: one of up to three alternatives starting at slot */
int EntityMonster_getRandomSound(EntityMonster *mon, int slot)
{
	int type = mon->ce.type;
	if (mon->monsterEffects & 4)
		type = 0x10;
	if ((g_app->render->mapFlagsByte & 1) && (unsigned)slot <= 3)
		return -1;

	int base = slot + type * 12;
	int count = 0;
	while (count < 3 && ModData_s16(&g_mod, MOD_MONSTER_SOUNDS + (base + count) * 2) != -1)
		count++;
	if (count == 0)
		return -1;
	int pick = (Random_nextInt(&g_app->random) & 0xff) % count;
	return ModData_s16(&g_mod, MOD_MONSTER_SOUNDS + (base + pick) * 2);
}

/* ------------------------------------------------------------------ entity basics */

/* 0x0003505c */
void Entity_reset(Entity *entity)
{
	String zero;

	entity->def = NULL;
	entity->prevOnTile = NULL;
	entity->nextOnTile = NULL;
	entity->linkIndex = 0;
	entity->info = 0;
	entity->param = 0;
	entity->monster = NULL;
	String_ctorInt(&zero, 0);
	String_assign(&entity->name, &zero);
	String_dtor(&zero, 0);
}

/* 0x0001bf94 */
void Entity_calcPosition(Entity *ent, int *outXY)
{
	Applet *app = g_app;
	int x, y;

	switch (ent->def->eType) {
	case 0: /* the world */
		x = 0;
		y = 0;
		break;
	case 1: /* player (its body stays behind while playing the familiar) */
		if (ent->def->eSubType == 0 && app->player->isFamiliar) {
			x = app->canvas->savedViewX;
			y = app->canvas->savedViewY;
		} else {
			x = app->canvas->destX;
			y = app->canvas->destY;
		}
		break;
	case 2:
		x = ent->monster->destX;
		y = ent->monster->destY;
		break;
	default:
		if (ent->info & 0x200000) {
			Line *ln = entLine(ent);
			x = ln->x1 + (ln->x2 - ln->x1) / 2;
			y = ln->y1 + (ln->y2 - ln->y1) / 2;
		} else {
			Sprite *spr = entSprite(ent);
			x = spr->x;
			y = spr->y;
		}
		break;
	}
	outXY[0] = x;
	outXY[1] = y;
}

/* 0x0000c188 */
int Entity_distSqFrom(Entity *self, int x, int y)
{
	int pos[2];
	Entity_calcPosition(self, pos);
	return mul32(y - pos[1], y - pos[1]) + mul32(x - pos[0], x - pos[0]);
}

/* 0x000246bc */
int Entity_isBoss(Entity *ent)
{
	return ent->def->eType == 2 && ent->def->eSubType > 10;
}

/* 0x00024704: one of the 8 reserved dropped-item slots */
int Entity_isDroppedEntity(Entity *ent)
{
	int idx = Game_getEntityIndex(ent);
	int first = g_app->game->firstDropEntity;
	return idx >= first && idx < first + 8;
}

/* 0x00024764 */
int Entity_isMonsterType13or14(Entity *ent)
{
	return ent->def->eType == 2 && (ent->def->eSubType == 14 || ent->def->eSubType == 13);
}

/* 0x0002458c: entities whose state is a single on/off bit in the save.
 * state[0] receives the bit; a locked door also sets 0x8000 in state[1]
 * (the save handle that Entity_getSaveHandle keeps next to it). */
int Entity_isBinaryEntity(Entity *ent, int *state)
{
	if (ent->def == NULL || Entity_isDroppedEntity(ent))
		return 0;

	int on;
	switch (ent->def->eType) {
	case 5: { /* door */
		uint32_t flags = entLine(ent)->flags;
		on = (flags & 0x40) != 0;
		if (flags & 0x400) {
			if (state == NULL)
				return 1;
			state[1] |= 0x8000;
		}
		break;
	}
	case 6:
	case 10:
	case 11:
		on = (ent->info & 0x4000000) != 0;
		break;
	case 7:
		if (ent->def->eSubType != 4)
			return 0;
		on = ent->param != 0;
		break;
	case 14:
		on = ent->def->eSubType == 1;
		break;
	default:
		return 0;
	}
	if (state)
		state[0] = on;
	return 1;
}

/* 0x000247ec: entity renamed by a script; strIdx = its id in the map's string group */
int Entity_isNamedEntity(Entity *ent, int *strIdx)
{
	if (ent->def == NULL || String_equals_40fc(&ent->name, &ent->def->name))
		return 0;
	int t = ent->def->eType;
	if (t == 9 || (ent->info & 0x1000000) || (t == 2 && ent->def->eSubType == 8))
		return 0;
	*strIdx = Text_findString(g_app->canvas, g_app->canvas->mapStringGroup, &ent->name);
	return *strIdx != -1;
}

/* 0x0001c168: save-record handle: index | 0x400 on | 0x800 named | 0x1000 visible
 * | 0x2000 not a kill | 0x4000 corpse | 0x8000 locked door */
int Entity_getSaveHandle(Entity *ent, int *out)
{
	if ((ent->info & 0xffff) == 0 || ent->def == NULL)
		return 0;

	int st[2]; /* {on/off state, handle}: Entity_isBinaryEntity may OR 0x8000 into st[1] */
	int strIdx;
	st[0] = 1;
	st[1] = Game_getEntityIndex(ent);
	int binary = Entity_isBinaryEntity(ent, st);
	int h = st[1];
	if (binary && st[0])
		h |= 0x400;
	if (Entity_isNamedEntity(ent, &strIdx))
		h |= 0x800;

	uint32_t info = ent->info;
	if (!(info & 0x200000) && !(entSprite(ent)->info & 0x10000))
		h |= 0x1000;
	if (!(info & 0x40000000))
		h |= 0x2000;
	if (info & 0x20000000)
		h |= 0x4000;
	if (info & 0x20000) {
		if (ent->def->eSubType != 2)
			h &= ~0x1000;
		h |= 0x4000;
	}
	*out = h;
	return 1;
}

/* 0x0001bdcc: argument of the first opcode 0x68 in the entity's tile event */
int Entity_getEventParam68(Entity *ent)
{
	Render *render = g_app->render;
	int ev = Render_findTileEvent(render, ent->linkIndex);
	if (ev == 0)
		return 0;

	uint32_t word;
	if (ev >= 0) {
		word = render->tileEvents[ev];
	} else {
		/* not found (-1): the original reads the word just before the table,
		 * i.e. the last two entries of eventTiles */
		word = (uint16_t)render->eventTiles[398] | ((uint32_t)(uint16_t)render->eventTiles[399] << 16);
	}
	int count = (int)((word >> 20) & 0x7f) * 3;
	int first = (int)((word >> 9) & 0x7ff) * 3;
	for (int i = 0; i < count; i += 3) {
		if (render->byteCodes[first + i] == 0x68)
			return render->byteCodes[first + i + 1];
	}
	return 0;
}

/* 0x00024218: per-type setup when an entity is spawned from the map */
void Entity_initspawn(Entity *ent)
{
	Applet *app = g_app;
	EntityDef *def = ent->def;

	String_assign(&ent->name, &def->name);

	switch (def->eType) {
	case 2: { /* monster */
		EntityMonster *mon = ent->monster;
		Sprite *spr = entSprite(ent);
		CombatEntity_copy(&mon->ce, &app->combat->monsterStats[def->eSubType * 3 + def->parm]);
		int diff = app->game->difficulty;
		if (diff == 4 || (diff == 2 && ent->def->eSubType < 11)) {
			int hp = CombatEntity_getMaxHealth(&mon->ce);
			hp += hp >> 2;
			CombatEntity_setMaxHealth(&mon->ce, hp);
			CombatEntity_setHealth(&mon->ce, hp);
		}
		mon->destX = spr->x;
		mon->ce.x = spr->x;
		mon->destY = spr->y;
		mon->ce.y = spr->y;
		spr->renderMode = 0;

		int sub = ent->def->eSubType;
		if (sub == 4) { /* ghost */
			spr->renderMode = 8;
			spr->info |= 0x20000000;
		} else if (sub == 12) {
			spr->scale = 0x48;
			mon->flags |= 0x400;
			ent->info |= 0x40000;
			return;
		} else if (sub == 1) {
			int parm = ent->def->parm;
			if (parm == 0 || (parm == 2 && (spr->x & 0x40)))
				spr->scale = 0x2c;
			ent->info |= 0x40000000;
		} else if (sub == 14 || sub == 13) {
			mon->flags |= 8;
			mon->monsterEffects &= ~(0x6000 | 0x10);
		}
		if (spr->scale != 0x40)
			mon->flags |= 0x400; /* scale is saved */
		ent->info |= 0x40000;
		return;
	}
	case 7:
		if (def->parm != 0) {
			ent->info |= 0x40000;
			return;
		}
		if (def->eSubType == 4)
			ent->param = 1;
		return;
	case 8:
	case 10:
		ent->info |= 0x40000;
		return;
	case 9:
		if (def->eSubType == 16)
			ent->info |= 0x40040000;
		return;
	case 3:
		ent->param = 1;
		return;
	default:
		return;
	}
}

/* 0x00007794: queue the monster for the combat phase */
void Entity_attack(Entity *self)
{
	if (self->info & 0x10000)
		return;
	self->info |= 0x10000;
	self->monster->nextAttacker = g_app->game->combatMonsters;
	g_app->game->combatMonsters = self;
}

/* 0x0000c4a8: blood splats on the walls around a dying monster */
void Entity_spawnBloodFX(Entity *self)
{
	if (arm_bit(self->def->eSubType) & 0x7ce3) {
		Sprite *spr = entSprite(self);
		Game_spawnWallSpritesAround(g_app->game, spr->x >> 6, spr->y >> 6, 0xa1, 0);
	}
}

/* ------------------------------------------------------------------ damage and death */

/* 0x0002ce80 */
void Entity_takeDamage(Entity *entity, int dmg)
{
	Applet *app = g_app;
	EntityMonster *mon = entity->monster;
	int maxHp = CombatEntity_getMaxHealth(&mon->ce);
	int hp = CombatEntity_getHealth(&mon->ce) - dmg;

	if ((mon->flags & 0x800) && hp < 1)
		hp = 1; /* scripted: cannot die */
	CombatEntity_setHealth(&mon->ce, hp);
	Sprite *spr = entSprite(entity);
	entity->monster->flags &= 0x8fff;

	/* the split boss: all its parts share one health pool */
	if (entity->def->eSubType == 12 && !(entity->monster->flags & 0x100) && app->game->activeMonsters) {
		Entity *e = app->game->activeMonsters;
		int last = 0;
		do {
			EntityMonster *m = e->monster;
			Entity *next = m->nextOnList;
			if (app->game->activeMonsters == next)
				last = 1;
			if (e->def->eSubType == 12 && (m->flags & 0x100)) {
				CombatEntity_setHealth(&m->ce, hp);
				if (hp <= 0)
					Entity_died(e, 1);
			}
			e = next;
		} while (app->game->activeMonsters != NULL && !last);
	}

	mon = entity->monster;
	if (hp <= 0) {
		if (!(mon->monsterEffects & 2)) {
			setSpriteFrame(spr, 7);
			entity->monster->frameTime = app->time + 200;
		}
		return;
	}

	if (mon->goal.type != 6) {
		MonsterGoal_reset(&mon->goal);
		if (!(entity->monster->monsterEffects & 2)) {
			setSpriteFrame(spr, 6); /* pain */
			entity->monster->frameTime = app->time + 250;
		}
	}

	int sub = entity->def->eSubType;
	if (sub >= 11 && sub <= 15) {
		/* bosses: scripted events when health crosses 100/75/50/25 % */
		if (sub == 12 && (entity->monster->flags & 0x100)) {
			CombatEntity_setHealth(&entity->monster->ce, 0);
			return;
		}
		int isSplit = sub == 12;
		int ev = Render_findTileEvent(app->render, 0x20 + isSplit);
		if (ev == -1)
			return;
		int kind = (int)((app->render->tileEvents[ev] >> 27) & 7);
		int before = hp + dmg;
		int half = maxHp >> 1;
		int quarter = half >> 1;
		int threeQ = maxHp - quarter;
		int fire = (before == maxHp && kind == 1) || (kind == 2 && hp <= threeQ && before > threeQ) ||
		           (kind == 3 && hp <= half && before > half) || (kind == 4 && hp <= quarter && before > quarter);
		if (fire) {
			Game_executeTile(app->game, isSplit, 1, 0xf000, 1);
			if (!(entity->monster->monsterEffects & 2))
				entity->monster->monsterEffects &= 0xfe7d;
		}
		return;
	}

	if (hp >= maxHp >> 1)
		return;

	if (sub == 1) { /* goblins may run away when badly hurt */
		if (entity->monster->monsterEffects & 2)
			return;
		if (app->player->ce.level <= entity->def->parm * 3 + 3)
			return;
		if (!(Random_nextInt(&app->random) & 3)) {
			Canvas_resetEvents(app->canvas);
			Canvas_addTextArgStr(app->canvas, &entity->name);
			Hud_addMessage(app->hud, 0x93);
			MonsterGoal_reset(&entity->monster->goal);
			entity->monster->goal.type = 4;
			entity->monster->goal.param = (uint32_t)(Random_nextInt(&app->random) & 0xff) % 3 + 2;
			return;
		}
	}

	if (entity->def->eSubType == 5 && !(entity->monster->monsterEffects & 2) && !(entity->info & 0x800000)) {
		/* summoner calls a helper sprite once */
		entity->info |= 0x800000;
		GameSprite *gs = Game_gspriteAlloc(app->game, 0, 0x95, 0, 2);
		if (entity->info & 0x1000000)
			gs->sprite->renderMode = 8;
		if (entity->monster->monsterEffects & 4)
			gs->sprite->scale = spr->scale - (spr->scale >> 2);
		gs->sprite->ent = entity;
		gs->sprite->x = spr->x;
		gs->sprite->y = spr->y;
		gs->sprite->z = spr->z;
		Render_relinkSprite(app->render, gs->sprite);
		Hud_randomBark(app->hud, 4);
	}
}

/* 0x00013b38: random drop by monster subtype */
void Entity_dropLoot(Entity *entity)
{
	Applet *app = g_app;
	Random *rnd = &app->random;

	if (entity->info & 0x100000)
		return;

	Sprite *spr = entSprite(entity);
	int sprFlags, subtype, parm, count;

	switch (entity->def->eSubType) {
	case 1: /* goblin: gold, or what the player paid it */
		if ((entity->monster->flags & 8) && app->game->goldPaid > 0) {
			count = app->game->goldPaid;
			app->game->goldPaid = 0;
		} else {
			if ((Random_nextInt(rnd) & 0xff) < 0x33)
				return;
			int p = entity->def->parm;
			if (p == 2)
				count = Random_nextInt(rnd) % 4 + 8;
			else if (p == 1)
				count = Random_nextInt(rnd) % 4 + 4;
			else
				count = Random_nextInt(rnd) % 4 + 2;
		}
		sprFlags = 0x72;
		subtype = 4;
		parm = 0;
		break;
	case 2:
		if (entity->def->parm != 0 && (Random_nextInt(rnd) & 0xff) < 0x66)
			return;
		if ((Random_nextInt(rnd) & 0xff) >= 0x32) {
			int p = entity->def->parm;
			if (p == 2)
				count = Random_nextInt(rnd) % 4 + 8;
			else if (p == 1)
				count = Random_nextInt(rnd) % 4 + 4;
			else
				count = Random_nextInt(rnd) % 4 + 2;
			sprFlags = 0x72;
			subtype = 4;
			parm = 0;
		} else {
			int p = entity->def->parm;
			int big = p == 2 || (p == 1 && (Random_nextInt(rnd) & 1));
			sprFlags = big ? 0x63 : 0x62;
			subtype = 0;
			parm = 0;
			count = 0;
		}
		break;
	case 3:
		if (entity->def->parm != 2)
			return;
		if ((Random_nextInt(rnd) & 0xff) < 0x3d)
			return;
		count = Random_nextInt(rnd) % 2 + 2;
		sprFlags = 0xf;
		subtype = 3;
		parm = 4;
		break;
	case 6:
		if ((Random_nextInt(rnd) & 0xff) < 0x99)
			return;
		sprFlags = 0x69;
		subtype = 0;
		parm = 7;
		count = 0;
		break;
	case 7: {
		if ((Random_nextInt(rnd) & 0xff) < 0x80)
			return;
		int p = entity->def->parm;
		if (p == 2 || (p == 1 && (Random_nextInt(rnd) & 1))) {
			sprFlags = 0x63;
			parm = 1;
		} else {
			sprFlags = 0x62;
			parm = 0;
		}
		subtype = 0;
		count = 0;
		break;
	}
	case 12:
		if (entity->monster && (entity->monster->flags & 0x100))
			return;
		/* fall through */
	case 11:
		sprFlags = 0x71;
		subtype = 4;
		parm = 2;
		count = 1;
		break;
	case 15:
		sprFlags = 0xf;
		subtype = 3;
		parm = 4;
		count = 5;
		break;
	default:
		entity->info |= 0x100000;
		return;
	}
	Game_spawnDropItem(app->game, spr->x, spr->y, sprFlags, 6, subtype, parm, count);
	entity->info |= 0x100000;
}

/* Death of a non-monster entity (NPC, corpse, breakable). */
static void diedObject(Entity *self)
{
	Applet *app = g_app;

	switch (self->def->eType) {
	case 3:
		Game_unlinkEntity_31bb8(app->game, self);
		break;
	case 9: { /* corpse gibbed */
		entSprite(self)->info |= 0x10000;
		self->info |= 0x40020000;
		app->statGibs++;
		int sub = self->def->eSubType;
		Sound_play(app->sndSys, (sub == 16 || sub == 8) ? 8 : 7, 0, 4, -1);
		Game_unlinkEntity_43710(app->game, self);
		Hud_randomBark(app->hud, 3);
		break;
	}
	case 10: { /* breakable */
		Canvas_resetEvents(app->canvas);
		Canvas_addTextArgStr(app->canvas, &self->name);
		Hud_addMessage(app->hud, 0x94);
		Sprite *spr = entSprite(self);
		int sub = self->def->eSubType;
		if (sub == 3) {
			Game_openContainer(self);
			Game_executeTile0(app->game, spr->x >> 6, spr->y >> 6, 0xf000);
			break;
		}
		if (sub == 2)
			Player_addXP(app->player, 1);
		else if (sub == 0 && app->canvas->mapId < 7)
			Hud_addTimedMsg(app->hud, 0xcd);
		Game_executeTile(app->game, spr->x >> 6, spr->y >> 6, 0xf000, 1);
		Game_unlinkEntity_31bb8(app->game, self);
		break;
	}
	default:
		break;
	}
	Canvas_updateFacing(app->canvas);
}

/* 0x0000b75c */
void Entity_died(Entity *self, int byPlayer)
{
	Applet *app = g_app;
	uint32_t info = self->info;

	if (!(info & 0x40000))
		return;
	if (self->monster && (self->monster->flags & 0x800))
		return;
	self->info = info & ~0x40000u;
	if (self->def->eType != 2) {
		diedObject(self);
		return;
	}

	EntityMonster *mon = self->monster;
	Sprite *spr = &app->render->sprites[(info & 0xffff) - 1];
	self->info = (info & ~0x40000u) | 0x40000000;
	app->statKillsBySubtype[self->def->eSubType]++;
	MonsterGoal_reset(&self->monster->goal);
	Game_snapMonsterSprite(self);
	setSpriteFrame(spr, 8);
	self->monster->frameTime = app->time;

	if (!(self->info & 0x20000)) {
		self->info |= 0x20040000;
		mon = self->monster;
		if (byPlayer && !(mon->monsterEffects & 2)) {
			int snd;
			if (self->def->eSubType == 12 && (mon->flags & 0x100))
				snd = 0x12;
			else
				snd = EntityMonster_getRandomSound(mon, 8);
			if (snd != -1)
				Sound_play(app->sndSys, snd, 0, 4, -1);
		}
		Game_hideExcessCorpses(app->game, spr->x, spr->y);
	} else {
		/* gibbed */
		spr->info |= 0x10000;
		if (byPlayer) {
			int snd;
			if (self->monster->monsterEffects & 2)
				snd = 9;
			else if (self->def->eSubType == 2)
				snd = 10;
			else if (self->def->eSubType == 8)
				snd = 8;
			else
				snd = 7;
			Sound_play(app->sndSys, snd, 0, 4, -1);
		}
		Entity_spawnBloodFX(self);
		if (self->monster->flags & 0x10)
			Game_executeTile(app->game, 7, 0, 0xf000, 1);
	}

	mon = self->monster;
	if (!(mon->monsterEffects & 2) && !(mon->monsterEffects & 1))
		mon->monsterEffects = (mon->monsterEffects & 0x801f) | 0x2aa0;
	else
		EntityMonster_clearEffects(mon);

	if (app->game->difficulty == 4 && !(self->monster->flags & 0x100)) {
		/* hard: corpses may rise again */
		int r = Random_nextInt(&app->random) % 3;
		self->monster->monsterEffects |= (uint16_t)((r + 2) * 0x2000);
		self->monster->monsterEffects |= 0x10;
	}

	Game_deactivate(app->game, self);
	Game_removeFromCombat(self);
	if (byPlayer && !(self->info & 0x1000000) && !(self->monster->flags & 0x80))
		Entity_dropLoot(self);

	int sub = self->def->eSubType;
	if (sub < 11 || sub > 15) {
		if (sub == 9) {
			Game_gspriteAllocAnim(app->game, 0xc0, spr->x, spr->y, 0x20);
			self->info = (self->info & ~0x20040000u) | 0x20000;
		} else if (sub == 4 || (self->info & 0x1000000)) {
			Game_gspriteAllocAnim(app->game, 0xc6, spr->x, spr->y, 0x20);
			self->info = (self->info & ~0x20040000u) | 0x20000;
			spr->info |= 0x10000;
		}
	} else if (sub == 12 && (self->monster->flags & 0x100)) {
		Game_gspriteAllocAnim(app->game, 0xf4, spr->x, spr->y, 0x20);
		self->info = (self->info & ~0x20040000u) | 0x20000;
		spr->info |= 0x10000;
	} else {
		/* a boss died */
		Player *player = app->player;
		player->inCombat = 0;
		if (self->def->eSubType == 14 || self->def->eSubType == 13) {
			Player_removeBuff(player, 9);
			Player_removeBuff(player, 10);
			Player_removeBuff(player, 11);
			Player_removeBuff(player, 13);
			if (player->statusEffects[4].turns > 0)
				Player_removeBuff(player, 4);
		}
		Game_executeTile(app->game, self->def->eSubType == 12, 1, 0xf000, 1);
	}

	int counts[2] = { 0, 1 }; /* {dead, total} level monsters */
	if (!(self->monster->flags & 0x80))
		Game_countMonsters(NULL, counts);

	if (byPlayer) {
		int xp = CombatEntity_calcXP(&self->monster->ce);
		if (!(self->info & 0x1000000) && self->def->eSubType > 10) {
			if (self->def->eSubType == 12 && (self->monster->flags & 0x100))
				xp >>= 2;
			else
				xp += 0x82;
		}
		Player_addXP(app->player, xp);
	}

	Player *player = app->player;
	int levelShift = (app->canvas->mapId - 1) & 0xff; /* ARM register shift: >= 32 gives 0 */
	uint32_t levelBit = arm_bit(levelShift);
	if (counts[0] == counts[1] && !(player->killedMonstersLevels & levelBit)) {
		/* every monster of the level is dead */
		Sound_play(app->sndSys, 0x54, 2, 5, -1);
		int bonus = (Player_xpForLevel(player, player->level) - Player_xpForLevel(player, player->level - 1)) >> 4;
		Canvas *cv = app->canvas;
		if (cv->mapId < 7 && (cv->mapId != 6 || byPlayer)) {
			String *s = &cv->msgRing[cv->msgRingIdx];
			cv->msgRingIdx = (cv->msgRingIdx + 1) % 2;
			String_setLength(s, 0);
			Canvas_resetEvents(cv);
			Canvas_addTextArgStr(cv, &app->render->mapName);
			Canvas_addTextArgInt(cv, bonus);
			Text_getString(cv, 0, 0xe, s);
			Canvas_addHelpMessage(cv, s, 2, NULL);
		}
		Player_addXP(player, bonus);
		player->killedMonstersLevels |= levelBit;
	}

	if ((self->info & 0x1020000) == 0 && self->def->eSubType != 2 && self->def->eSubType != 4 &&
	    self->def->eSubType != 9) {
		self->def = EntityDefs_find(app->entityDefs, 9, self->def->eSubType, self->def->parm); /* corpse */
	} else {
		self->info = (self->info & ~0x20040000u) | 0x20000;
		Game_unlinkEntity_43710(app->game, self);
	}
	self->info &= ~0x1000000u;
	String_assign(&self->name, &self->def->name);
	Canvas_updateFacing(app->canvas);
}

/* 0x0000bf90 */
void Entity_diedDefault(Entity *self)
{
	Entity_died(self, 1);
}

/* ------------------------------------------------------------------ movement helpers */

/* 0x000248c0: clear line from tile (sx,sy) to (dx,dy) for a monster move */
int Entity_checkLineOfSight(Entity *ent, int sx, int sy, int dx, int dy, int flags)
{
	Applet *app = g_app;

	if (app->render->mapFlags[dx + dy * 32] & 0x100)
		return 0;
	Game_trace(app->game, sx * 64 + 32, sy * 64 + 32, dx * 64 + 32, dy * 64 + 32, ent, flags);
	Game *game = app->game;
	Entity *hit = game->traceEntity;
	if (hit == NULL)
		return 1;

	int t = hit->def->eType;
	if (t == 5) {
		/* a door on our own tile does not block, unless something else is there */
		Line *ln = entLine(hit);
		int cx = ln->x1 + (ln->x2 - ln->x1) / 2;
		int cy = ln->y1 + (ln->y2 - ln->y1) / 2;
		if (sx != cx >> 6 || sy != cy >> 6)
			return 0;
		if (game->numTraceEntities == 1)
			return 1;
		game->traceEntity = game->traceEntities[1];
	} else if (t == 13) {
		/* wall decoration on our tile: passable when it spans both sides */
		Sprite spr = *entSprite(hit);
		if (sx != spr.x >> 6 || sy != spr.y >> 6)
			return 0;
		if ((spr.info & 0x80000) && (spr.info & 0x100000))
			return 1;
		if ((spr.info & 0x200000) && (spr.info & 0x400000))
			return 1;
		return 0;
	}
	/* anything else in the way (including the player) blocks */
	return 0;
}

/* 0x00024bcc: push ent away from (srcX,srcY) by up to dist tiles */
void Entity_knockback(Entity *ent, int srcX, int srcY, int dist)
{
	Applet *app = g_app;
	int x, y, mask;

	if (app->player->isFamiliar)
		return;
	if (ent->def->eType == 1) {
		mask = 0x74bd;
		x = app->game->destX;
		y = app->game->destY;
	} else {
		if (ent->info & 0x200000)
			return;
		Sprite *spr = entSprite(ent);
		x = spr->x;
		y = spr->y;
		mask = 0x7daf;
	}

	int dir[2] = { x - srcX, y - srcY };
	int reach[2] = { 0, 0 };
	for (int axis = 0; axis < 2; axis++) {
		if (dir[axis] == 0)
			continue;
		dir[axis] = dir[axis] < 0 ? -1 : 1;
		int ex = x, ey = y;
		if (axis == 0)
			ex = x + dist * dir[0] * 64;
		else
			ey = y + dist * dir[1] * 64;
		Game_trace(app->game, x, y, ex, ey, ent, mask);
		Game *game = app->game;
		Entity *hit = game->traceEntity;
		if (hit == NULL) {
			reach[axis] = dist;
			continue;
		}
		if (hit->def->eType == 0) {
			int hx = x - game->traceHitX, hy = y - game->traceHitY;
			reach[axis] = Combat_WorldDistToTileDist(app->combat, mul32(hy, hy) + mul32(hx, hx));
		} else {
			reach[axis] = Combat_WorldDistToTileDist(app->combat, Entity_distSqFrom(hit, x, y)) - 1;
		}
		if (reach[axis] > dist)
			reach[axis] = dist;
		if (reach[axis] <= 0)
			dir[axis] = 0;
	}

	Sound_play(app->sndSys, 0xe, 0, 2, -1);
	if (dir[0] == 0) {
		if (dir[1] == 0)
			return;
	} else if (dir[1] != 0) {
		dir[Random_nextInt(&app->random) & 1] = 0; /* diagonal: keep one axis */
	}

	int axis = dir[0] != 0 ? 0 : 1;
	int tx = (x + reach[axis] * dir[0] * 64) >> 6;
	int ty = (y + reach[axis] * dir[1] * 64) >> 6;
	if (ent->def->eType == 1) {
		if (ent->def->eSubType != 1) {
			Canvas *cv = app->canvas;
			cv->moveDirX = dir[0];
			cv->moveDirY = dir[1];
			cv->moveCount = reach[axis];
			cv->f2c8 = reach[axis];
		}
	} else {
		ent->monster->destX = tx * 64 + 32;
		ent->monster->destY = ty * 64 + 32;
		ent->monster->flags |= 0x40;
		Game_unlinkEntity_43710(app->game, ent);
		Game_linkEntity(app->game, ent, tx, ty);
		app->game->monstersMoving = 1;
	}
}

/* ------------------------------------------------------------------ resurrection */

/* 0x000355cc: corpse back to a monster at (x,y,z), keeping its stat block */
void Entity_resurrect(Entity *entity, int x, int y, int z)
{
	Applet *app = g_app;
	Sprite *spr = entSprite(entity);

	entity->def = EntityDefs_find(app->entityDefs, 2, entity->def->eSubType, entity->def->parm);
	String_assign(&entity->name, &entity->def->name);
	EntityMonster_clearEffects(entity->monster);
	entity->monster->destX = x;
	entity->monster->ce.x = x;
	spr->x = x;
	entity->monster->destY = y;
	entity->monster->ce.y = y;
	spr->z = z;
	spr->y = y;
	spr->info &= 0xfefef0ff; /* visible, frame 0, unflipped */
	if (Random_nextInt(&app->random) & 1)
		spr->info |= 0x1000000;
	Render_relinkSprite(app->render, spr);
	entity->info = (entity->info & 0xdc7dffff) | 0x40000;

	CombatEntity saved = entity->monster->ce;
	int scale = spr->scale;
	Entity_initspawn(entity);
	entity->monster->ce = saved;
	spr->scale = scale;
	if (scale == 0x40)
		entity->monster->flags &= ~0x400;
	entity->monster->frameTime = (int32_t)GETUPTIMEMS();
	entity->monster->flags |= 0x8000;
	CombatEntity_setHealth(&entity->monster->ce, CombatEntity_getMaxHealth(&entity->monster->ce));
	Game_unlinkEntity_43710(app->game, entity);
	Game_linkEntity(app->game, entity, x >> 6, y >> 6);
	Canvas_updateFacing(app->canvas);
}

/* ------------------------------------------------------------------ save / load */

/* 0x0003536c: apply a saved on/off state (flags bit 0x400) */
void Entity_restoreState(Entity *entity, int flags)
{
	Applet *app = g_app;
	Line *line = (entity->info & 0x200000) ? entLine(entity) : NULL;
	int on = (flags >> 10) & 1;

	switch (entity->def->eType) {
	case 5: { /* door: open/closed, locked (0x8000) */
		int idx = Render_getLineIndex(line);
		uint32_t old = line->flags;
		line->flags = old & ~0x400u;
		Game_useDoor(app->game, on ? 0xf : 0x10, idx, 0);
		if ((flags & 0x8000) || (old & 0x400))
			Game_setLineLocked(app->game, idx, (flags & 0x8000) != 0, 0);
		break;
	}
	case 6:
	case 10:
	case 11:
		Game_unlinkEntity_43710(app->game, entity);
		if (on) {
			int li = entity->linkIndex;
			Game_linkEntity(app->game, entity, li % 32, li / 32);
		}
		break;
	case 7:
		if (entity->def->eSubType == 4)
			entity->param = on;
		break;
	case 14:
		Game_setSwitch(app->game, entity, on != 0);
		break;
	default:
		break;
	}
}

/* 0x00036f2c: flags is the handle from Entity_getSaveHandle */
void Entity_saveState(Entity *entity, OutputStream *os, int flags)
{
	if (flags & 0x800) {
		int strIdx = 0;
		Entity_isNamedEntity(entity, &strIdx);
		OutStream_writeShort(os, (int16_t)strIdx);
	}
	if (flags & 0x2000)
		return;

	Sprite *spr = entSprite(entity);
	OutStream_writeInt(os, (int)entity->info);
	if (!(entity->info & 0x20000)) {
		OutStream_writeShort(os, (int32_t)spr->info >> 16);
		OutStream_writeBool(os, (spr->info & 0xfff) >> 8);
		if (!(spr->info & 0x780000)) {
			OutStream_writeBool(os, (int8_t)(spr->x >> 3));
			OutStream_writeBool(os, (int8_t)(spr->y >> 3));
		}
		if (Entity_isDroppedEntity(entity) || (entity->def && entity->def->eType == 3))
			OutStream_writeInt(os, entity->param);
		OutStream_writeShort(os, entity->linkIndex);
	}

	if (entity->monster == NULL) {
		if (!Entity_isDroppedEntity(entity)) {
			OutStream_writeBool(os, (int8_t)spr->z);
			OutStream_writeBool(os, (int8_t)spr->info);
		} else {
			OutStream_writeBool(os, (int8_t)entity->def->eType);
			OutStream_writeBool(os, (int8_t)entity->def->eSubType);
			OutStream_writeInt(os, entity->def->parm);
		}
		return;
	}

	OutStream_writeShort(os, (int16_t)entity->monster->flags);
	if (!(entity->info & 0x20000)) {
		if (entity->monster->flags & 0x400)
			OutStream_writeBool(os, (int8_t)spr->scale);
		if (!(flags & 0x4000)) {
			OutStream_writeShort(os, (int16_t)entity->monster->monsterEffects);
			CombatEntity_saveState(&entity->monster->ce, os);
			MonsterGoal_saveState(&entity->monster->goal, os);
		}
	}
}

/* 0x00029060 */
void Entity_loadState(Entity *ent, InputStream *stream, int flags)
{
	Applet *app = g_app;
	String *s = &ent->name;
	Sprite *spr = NULL;

	if (flags & 0x800) {
		int id = Stream_readShort(stream);
		String_setLength(s, 0);
		Text_getString(app->canvas, app->canvas->mapStringGroup, id, s);
	}

	if (!(ent->info & 0x200000)) {
		spr = entSprite(ent);
		if (!(flags & 0x1000)) {
			spr->info |= 0x10000;
			if (ent->info & 0x4000000)
				Game_unlinkEntity_43710(app->game, ent);
		} else {
			spr->info &= ~0x10000u;
			if ((flags & 0x2000) && !(ent->info & 0x4000000))
				Game_linkEntity(app->game, ent, spr->x >> 6, spr->y >> 6);
		}
	}

	if (Entity_isBinaryEntity(ent, NULL)) {
		Entity_restoreState(ent, flags);
		if (flags & 0x2000)
			return;
	}

	if (flags & 0x4000) {
		if (!(flags & 0x1000)) {
			uint32_t old = ent->info;
			ent->info = old | 0x20000;
			if (old & 0x4000000)
				Game_unlinkEntity_43710(app->game, ent);
		} else if (flags & 0x2000) {
			App_fatalError(app, "Clean corpse found - should be dirty!");
		}
		if (spr)
			setSpriteFrame(spr, 8);
	}
	if (flags & 0x2000)
		return;

	if (ent->info & 0x4000000)
		Game_unlinkEntity_43710(app->game, ent);
	ent->info = (uint32_t)Stream_readInt(stream);
	if (ent->info & 0x1000000) {
		String_setLength(s, 0);
		Text_getString(app->canvas, 0,
		               ModData_s16(&g_mod, MOD_CORPSE_NAME_IDS + ent->def->parm * 2 + ent->def->eSubType * 6), s);
	}

	spr = entSprite(ent);
	if (!(ent->info & 0x20000)) {
		int hi = Stream_readShort(stream);
		int frame = Stream_readUByte(stream);
		spr->info = (spr->info & 0xf0ff) | ((uint32_t)frame << 8) | ((uint32_t)hi << 16);
		if (!(spr->info & 0x780000)) {
			spr->x = Stream_readUByte2(stream) << 3;
			spr->y = Stream_readUByte2(stream) << 3;
			Render_relinkSprite(app->render, spr);
		}
		if (Entity_isDroppedEntity(ent) || (ent->def && ent->def->eType == 3))
			ent->param = Stream_readInt(stream);
		int li = Stream_readShort(stream);
		ent->linkIndex = (int16_t)li;
		uint32_t wall = spr->info & 0x780000;
		if ((wall == 0x600000 || wall == 0x180000) && (ent->def->eType == 13 || ent->def->eType == 12)) {
			/* wall-mounted sprite: centre it on its tile */
			spr->x = (li % 32) * 64 + 32;
			spr->y = (li / 32) * 64 + 32;
			Render_relinkSprite(app->render, spr);
		}
	}

	if (ent->monster == NULL) {
		if (!Entity_isDroppedEntity(ent)) {
			spr->z = Stream_readUByte2(stream);
			spr->info &= ~0xffu;
			spr->info |= (uint32_t)Stream_readUByte2(stream);
		} else {
			int t = Stream_readUByte(stream);
			int st = Stream_readUByte(stream);
			int parm = Stream_readInt(stream);
			ent->def = EntityDefs_find(app->entityDefs, t, st, parm);
			String_assign(s, &ent->def->name);
			spr->info = (spr->info & ~0xffu) | (uint32_t)(int32_t)ent->def->tileIndex;
			spr->ent = ent;
		}
	} else {
		uint16_t mflags = (uint16_t)Stream_readShort(stream);
		ent->monster->flags = mflags;
		if (!(ent->info & 0x20000)) {
			if (mflags & 0x400)
				spr->scale = Stream_readUByte2(stream);
			else if (ent->def->eSubType == 1 && ent->def->parm == 2)
				spr->scale = 0x40;
			if (!(flags & 0x4000)) {
				ent->monster->monsterEffects = (uint16_t)Stream_readShort(stream);
				CombatEntity_loadState(&ent->monster->ce, stream);
				MonsterGoal_loadState(&ent->monster->goal, stream);
			}
		}
		if (ent->info & 0x80000) {
			ent->info &= ~0x80000u;
			Game_activateEx(app->game, ent, 0, 0);
		}
		if (ent->info & 0x20000000)
			ent->def = EntityDefs_find(app->entityDefs, 9, ent->def->eSubType, ent->def->parm);
	}

	if (!(ent->info & 0x4000000)) {
		if (ent->def->eType == 7 && ent->def->eSubType == 3)
			spr->info = (spr->info & ~0xffu) | 0x9b;
		return;
	}
	int tx, ty;
	if (!(spr->info & 0x780000)) {
		ty = spr->y >> 6;
		tx = spr->x >> 6;
	} else {
		tx = ent->linkIndex % 32;
		ty = ent->linkIndex / 32;
	}
	Game_linkEntity(app->game, ent, tx, ty);
}
