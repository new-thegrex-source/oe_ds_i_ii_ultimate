/*
 * Entity core: spawning, death, damage reactions, knockback, line of sight,
 * save/restore of entity records and the small Entity/EntityMonster helpers.
 *
 * Original code lived in several .cpp files of the BREW build (Entity.cpp,
 * EntityMonster.cpp, MonsterGoal.cpp); addresses are given per function.
 */

#include "game.h"

/* ---- .mod data (read at runtime, never copied) ---- */
#define MOD_MONSTER_SOUNDS      0x2fd9cu /* u16 [monster class * 12 + slot], 0 = none; up to 3 variants per slot */
#define MOD_STR_FIREPIT_MISSING 0x3004cu
#define MOD_STR_FLEES           0x30134u
#define MOD_STR_DESTROYED       0x301c4u
#define MOD_STR_MONSTERS_CLEARED 0x301d4u
#define MOD_STR_FIRE_CLEARED    0x30234u

/* Map resource ids (logic constants) */
#define MAP_M08 0x1777
#define MAP_HUB 6011   /* DS Dragon's Lair (BREW 6008) */

static uint32_t bit32(int n) { return (n >= 0 && n < 32) ? (1u << n) : 0u; }

static Sprite *entitySprite(Entity *ent)
{
	return Render_spriteAt(ent->app->render, (ent->info & 0xffff));
}

/* Tile of a link index (x + y*32), with the original's signed division. */
static void linkIndexToTile(int linkIndex, int *tx, int *ty)
{
	*ty = linkIndex / 32;
	*tx = linkIndex - *ty * 32;
}

/* 0x000074e8: sum of the amounts (arg1 >> 8) of the "op 0x2f, kind 4" tile
 * commands on the tile the entity's sprite stands on. */
int Entity_sumTileEventOp2F(Entity *ent)
{
	Render *render = ent->app->render;
	Sprite *spr = entitySprite(ent);
	int ev = Render_findTileEvent(render, (spr->y >> 6) * 32 + (spr->x >> 6));
	int sum = 0;

	if (ev != 0) {
		render = ent->app->render;
		uint32_t word = render->tileEvents[ev];
		int first = (int)((word & 0xfffff) >> 9);	/* (DS: 11 bits from bit 9) */
		int count = (int)((word & 0x7ffffff) >> 20);
		for (int i = 0; i < count; i++) {
			Command *cmd = &render->commands[first + i];
			if (cmd->op == 0x2f && (cmd->arg1 & 0xff) == 4)
				sum += (int)(((uint32_t)cmd->arg1 & 0xffffff) >> 8);
		}
	}
	return sum;
}

/* 0x000080f4: "Entity::died" */
void Entity_died(Entity *ent, int byPlayer)
{
	Player_checkCanaries(ent->app->player);

	uint32_t info = ent->info;
	if (!(info & 0x40000))
		return;
	if (ent->monster && (ent->monster->flags & 0x800))
		return; /* cannot die */

	ent->info = info & ~0x40000u;
	Applet *app = ent->app;
	Game *game = app->game;

	switch (ent->def->eType) {
	case 2: { /* monster */
		ent->info = (info & ~0x40000u) | 0x80000000u;
		unsigned spriteIdx = info & 0xffff;
		Render *render = app->render;
		Sprite *spr = Render_spriteAt(render, spriteIdx);
		EntityMonster *mon;

		/* m08 reinforcements: one fewer out */
		if (app->canvas->mapId == MAP_M08 && !Entity_isBoss(ent)) {
			Game *g = ent->app->game;
			int16_t mflags = (int16_t)ent->monster->flags;
			if (g->vars[1] != 0 && !(mflags & 0x100) && (mflags & 0x80) && (ent->info & 0x11000000) == 0)
				g->vars[0]--;
		}

		mon = ent->monster;
		ent->app->statKills[ent->def->eSubType]++;
		MonsterGoal_reset(&mon->goal);
		Game_snapMonsterSprite(ent);

		if (mon->petrifyTurns == 0) {
			spr->info = (spr->info & 0xfffff0ff) + 0x700;
			mon->frameTime = ent->app->tickTime + 250;
		} else {
			spr->info = (spr->info & 0xfffff0ff) + 0x800;
			mon->frameTime = ent->app->tickTime;
		}

		if (ent->info & 0x20000) {
			/* gibbed */
			spr->info |= 0x10000;
			if (byPlayer) {
				int snd;
				if (mon->petrifyTurns != 0)
					snd = 0x1427;
				else if (ent->def->eSubType == 2)
					snd = 0x140f;
				else
					snd = 0x13e3;
				Sound_play(ent->app->sound, snd, 0, 3, -1);
			}
		} else {
			/* leaves a corpse */
			ent->info |= 0x40040000;
			if (byPlayer && mon->petrifyTurns == 0) {
				int snd = EntityMonster_getRandomSound(mon, 8);
				if (snd != 0)
					Sound_play(ent->app->sound, snd, 0, 3, -1);
			}
			Game_hideExcessCorpses(ent, spr->x, spr->y);
		}

		mon->petrifyTurns = 0;
		mon->flags &= 0xfdff;
		if (ent->def->eSubType == 0 && ((int16_t)mon->flags & 8)) {
			Game *g = ent->app->game;
			g->vars[0]++;
			g = ent->app->game;
			if ((g->vars[0] & 7) == 0)
				Game_executeTile(g, 0, 1, 0xf000, 1);
		}

		Game_deactivate(ent->app->game, ent);
		Game_removeFromCombat(ent);
		if (byPlayer && (ent->info & 0x11000000) == 0 && !(mon->flags & 0x80))
			Entity_dropLoot(ent);

		uint8_t sub = ent->def->eSubType;
		if (sub == 0xe || sub == 0xc || sub == 0xd || sub == 0xf) {
			/* boss */
			ent->app->player->attacked = 0;
			Game_executeTile(ent->app->game, 0, 2, 0xf000, 0);
		} else if (sub == 0xb) {
			Game_gspriteAllocAnim(ent->app->game, 0xc0, spr->x, spr->y, 0x20);
			ent->info = (ent->info & 0xbffbffffu) | 0x20000;
		} else if (sub == 5 || (ent->info & 0x1000000)) {
			Game_gspriteAllocAnim(ent->app->game, 0xc6, spr->x, spr->y, 0x20);
			ent->info = (ent->info & 0xbffbffffu) | 0x20000;
			spr->info |= 0x10000;
		} else if (ent->info & 0x10000000) {
			Game_gspriteAllocAnim(ent->app->game, 0xc0, spr->x, spr->y, 0x20);
			ent->info = (ent->info & 0xbffbffffu) | 0x20000;
			spr->info |= 0x10000;
		}

		/* experience */
		if (byPlayer && !(mon->flags & 0x80)) {
			if ((ent->info & 0x11000000) == 0) {
				int killed, total;
				Game_countMonsters(ent->app->player, &killed, &total);
				Player_addXP(ent->app->player, CombatEntity_calcXP(&mon->ce));
				Applet *a = ent->app;
				if (killed == total && !(a->player->killsDoneMaps & bit32((a->canvas->mapId - 6000) & 0xff))) {
					/* NB: for subtype 0xf the original never computes the bonus and
					 * awards the value left in the "total" slot (the monster count). */
					int bonus = total;
					if (ent->def->eSubType != 0xf) {
						Sound_play(a->sound, 0x141c, 2, 5, -1);
						Player *p = ent->app->player;
						int xpL = Player_xpForLevel(p, p->level);
						p = ent->app->player;
						int xpPrev = Player_xpForLevel(p, p->level - 1);
						bonus = (xpL - xpPrev) >> 4;
						Canvas *cv = ent->app->canvas;
						int slot = cv->helpBufIdx;
						cv->helpBufIdx = (slot + 1) % 2;
						char *buf = cv->helpBuf + slot * 600;
						App_snprintf(buf, 600, (char *)ModData_str(&g_mod, MOD_STR_MONSTERS_CLEARED),
						             ent->app->render->mapName, bonus);
						Canvas_addHelpMessage(ent->app->canvas, buf, 2);
					}
					Player_addXP(ent->app->player, bonus);
					ent->app->player->killsDoneMaps |= bit32((ent->app->canvas->mapId - 6000) & 0xff);
				}
			} else {
				Player_addXP(ent->app->player, CombatEntity_calcXP(&mon->ce));
			}
		}

		/* turn into a corpse, or disappear */
		sub = ent->def->eSubType;
		if ((ent->info & 0x11020000) == 0 && sub != 2 && sub != 5 && sub != 0xb) {
			ent->def = EntityDefs_find(ent->app->entityDefs, 9, sub, ent->def->parm);
		} else {
			ent->info = (ent->info & 0xbffbffffu) | 0x20000;
			Game_unlinkEntity_2ae8c(ent->app->game, ent);
		}
		ent->info &= 0xeeffffffu;
		ent->name = ent->def->name;
		break;
	}

	case 8: /* fire */
		Game_unlinkEntity_201d8(game, ent);
		Hud_addMessage(ent->app->hud, (char *)ModData_str(&g_mod, MOD_STR_FIRE_CLEARED), 0);
		Player_addXP(ent->app->player, 2);
		break;

	case 9: /* corpse: gibbed */
		Render_spriteAt(app->render, (info & 0xffff))->info |= 0x10000;
		ent->info = (ent->info & 0xbfffffffu) | 0x20000;
		ent->app->statGibs++;
		Sound_play(ent->app->sound, 0x13e3, 0, 3, -1);
		Game_unlinkEntity_2ae8c(ent->app->game, ent);
		Hud_randomBark(ent->app->hud, 4, 0);
		break;

	case 10: { /* destructible */
		char msg[0x40];
		App_snprintf(msg, sizeof msg, (char *)ModData_str(&g_mod, MOD_STR_DESTROYED), ent->name);
		Hud_addMessage(ent->app->hud, msg, 0);
		Sprite *spr = entitySprite(ent);
		uint8_t sub = ent->def->eSubType;
		if (sub == 1 || sub == 0) {
			int extra = 1;
			if (sub == 0) {
				Game_openContainer(ent);
				extra = 0;
			}
			Game_executeTile(game, spr->x >> 6, spr->y >> 6, 0xf000, extra);
		}
		Game_unlinkEntity_201d8(game, ent);
		break;
	}

	case 3:
		Game_unlinkEntity_201d8(game, ent);
		break;

	default:
		break;
	}
	Canvas_updateFacing(ent->app->canvas);
}

/* 0x00008814 */
int Entity_distSqFrom(Entity *ent, int x, int y)
{
	int ex, ey;
	Entity_calcPosition(ent, &ex, &ey);
	return (x - ex) * (x - ex) + (y - ey) * (y - ey);
}

/* 0x0000d4ac: monster drops (gold, potions, keys) when killed by the player */
void Entity_dropLoot(Entity *entity)
{
	if (entity->info & 0x100000)
		return; /* already dropped */

	Applet *app = entity->app;
	Sprite *spr = entitySprite(entity);
	int tile, sub = 0, parm = 0, count = 0;

	switch (entity->def->eSubType) {
	case 8:
		if ((unsigned)Random_nextByte(&app->random) < 0x80)
			return;
		parm = entity->def->parm;
		if (parm == 2 || (parm == 1 && (Random_nextByte(&entity->app->random) & 1))) {
			tile = 99;
			parm = 1;
		} else {
			tile = 0x62;
			parm = 0;
		}
		break;

	case 1: {
		/* gold; the thief gives back what was paid to it */
		int bonus = 0;
		if (app->render->hubSprite == spr) {
			Player_addMood(app->player, 0x10);
			bonus = 3;
		}
		unsigned gold;
		if ((entity->monster->flags & 8) && entity->app->game->goldPaid != 0) {
			Game *game = entity->app->game;
			gold = (uint16_t)game->goldPaid;
			game->goldPaid = 0;
		} else {
			if (bonus == 0 && (unsigned)Random_nextByte(&entity->app->random) < 0x33)
				return;
			int p = entity->def->parm;
			if (p == 2)
				gold = (Random_nextByte(&entity->app->random) & 3) + bonus + 8;
			else if (p == 1)
				gold = (Random_nextByte(&entity->app->random) & 3) + bonus + 4;
			else
				gold = (Random_nextByte(&entity->app->random) & 3) + bonus + 2;
		}
		Game_spawnDropItem(entity->app->game, spr->x, spr->y, 0x72, 6, 4, 0, (int)gold);
		Sound_play(entity->app->sound, 0x1411, 0, 3, -1);
		entity->info |= 0x100000;
		return;
	}

	case 2:
		if (entity->def->parm != 0 && (unsigned)Random_nextByte(&app->random) < 0x66)
			return;
		if ((unsigned)Random_nextByte(&entity->app->random) < 0x32) {
			int p = entity->def->parm;
			if (p == 2 || (p == 1 && (Random_nextByte(&entity->app->random) & 1)))
				tile = 99;
			else
				tile = 0x62;
		} else {
			int p = entity->def->parm;
			if (p == 2)
				count = (Random_nextByte(&entity->app->random) & 3) + 8;
			else if (p == 1)
				count = (Random_nextByte(&entity->app->random) & 3) + 4;
			else
				count = (Random_nextByte(&entity->app->random) & 3) + 2;
			sub = 4;
			tile = 0x72;
		}
		break;

	case 7:
		if ((unsigned)Random_nextByte(&app->random) < 0x99)
			return;
		parm = 7;
		tile = 0x69;
		break;

	case 0xc:
		sub = 8;
		parm = 2;
		tile = 0x7e;
		break;

	case 0xd:
		sub = 8;
		parm = 1;
		tile = 0x7f;
		break;

	case 0xe:
		sub = 8;
		parm = 0;
		tile = 0x7d;
		break;

	default:
		entity->info |= 0x100000;
		return;
	}
	Game_spawnDropItem(entity->app->game, spr->x, spr->y, tile, 6, sub, parm, count);
	entity->info |= 0x100000;
}

/* 0x00012a0c: 0 healthy .. 3 below a quarter of max health */
int Entity_getWoundLevel(Entity *entity)
{
	int hp = CombatEntity_getHealth(&entity->monster->ce);
	int max = CombatEntity_getMaxHealth(&entity->monster->ce);
	if (hp < (max >> 2))
		return 3;
	if (hp < (max >> 1))
		return 2;
	if (hp < max - (max >> 2))
		return 1;
	return 0;
}

/* 0x00012bbc */
void Entity_calcPosition(Entity *entity, int *x, int *y)
{
	uint8_t type = entity->def->eType;
	if (type == 1) {
		*x = entity->app->canvas->destX;
		*y = entity->app->canvas->destY;
	} else if (type == 2) {
		*x = entity->monster->destX;
		*y = entity->monster->destY;
	} else if (entity->info & 0x200000) {
		/* line entity (door): midpoint of the line */
		Line *ln = &entity->app->render->lines[(entity->info & 0xffff) - 1];
		*x = (ln->x2 - ln->x1) / 2 + ln->x1;
		*y = (ln->y2 - ln->y1) / 2 + ln->y1;
	} else {
		Sprite *spr = entitySprite(entity);
		*x = spr->x;
		*y = spr->y;
	}
}

/* Monster sounds of slots 0..3 are muted on maps with flag bit 0. */
static int monsterSlotMuted(int slot)
{
	return (g_app->render->mapFlagBits & 1) && (slot == 0 || slot == 1 || slot == 2 || slot == 3);
}

/* 0x00012c58: one of up to 3 consecutive sound variants of a slot */
int EntityMonster_getRandomSound(EntityMonster *mon, int slot)
{
	Applet *app = g_app;
	if (monsterSlotMuted(slot))
		return 0;

	int base = mon->ce.type * 12 + slot;
	int n = 0;
	while (n < 3 && ModData_s16(&g_mod, MOD_MONSTER_SOUNDS + (uint32_t)(base + n) * 2) != 0)
		n++;
	if (n == 0)
		return 0;
	int pick = Random_nextByte(&app->random) % n;
	return ModData_u16(&g_mod, MOD_MONSTER_SOUNDS + (uint32_t)(base + pick) * 2);
}

/* 0x00012d1c: compact save handle: entity index plus state bits
 * (0x400 binary state on, 0x800 renamed, 0x1000 visible, 0x2000 alive/clean,
 * 0x4000 corpse/removed). Returns 0 for entities without a sprite or def. */
int Entity_getSaveHandle(Entity *entity, short *out)
{
	if ((entity->info & 0xffff) == 0 || entity->def == NULL)
		return 0;

	short handle = (short)(entity - entity->app->game->entities);
	int state;
	short strIdx;

	if (Entity_isBinaryEntity(entity, &state, &handle) && state != 0)
		handle |= 0x400;
	if (Entity_isNamedEntity(entity, &strIdx))
		handle |= 0x800;
	if (!(entity->info & 0x200000) && !(entitySprite(entity)->info & 0x10000))
		handle |= 0x1000;
	if (!(entity->info & 0x80000000u))
		handle |= 0x2000;
	if (entity->info & 0x40000000)
		handle |= 0x4000;
	if (entity->info & 0x20000) {
		uint8_t sub = entity->def->eSubType;
		if (sub != 2 && sub != 4)
			handle &= (short)0xefff;
		handle |= 0x4000;
	}
	*out = handle;
	return 1;
}

/* 0x00012e10 */
int EntityMonster_getSound(EntityMonster *mon, int slot)
{
	if (monsterSlotMuted(slot))
		return 0;
	return ModData_u16(&g_mod, MOD_MONSTER_SOUNDS + (uint32_t)(mon->ce.type * 12 + slot) * 2);
}

/* 0x00014cb4: the fire pit's destination map comes from tile command 0x3a */
void Entity_initFirePit(Entity *entity)
{
	Render *render = entity->app->render;
	Sprite *spr = entitySprite(entity);
	int tx = spr->x >> 6, ty = spr->y >> 6;
	int tile = ty * 32 + tx;

	if (!(render->mapFlags[tile] & 8)) {
		App_setErrorStr(entity->app, (char *)ModData_str(&g_mod, MOD_STR_FIREPIT_MISSING), tx, ty);
		App_setErrorCode(g_app, -666);
		return;
	}

	int ev = Render_findTileEvent(render, tile);
	Applet *app = entity->app;
	render = app->render;
	uint32_t word = render->tileEvents[ev];
	int first = (int)((word & 0xfffff) >> 9);	/* (DS: 11 bits from bit 9) */
	int count = (int)((word & 0x7ffffff) >> 20);
	for (int i = 0; i < count; i++) {
		Command *cmd = &render->commands[first + i];
		if (cmd->op == 0x3a) {
			int dest = cmd->arg1 & 0xff;
			entity->param = dest;
			if (app->canvas->mapId == MAP_HUB && !(app->player->firePortals & bit32(dest)))
				return; /* portal not yet unlocked */
			entity->param = dest | 0x100;
			return;
		}
	}
}

/* 0x00017a00 */
void Entity_initspawn(Entity *ent)
{
	Render *render = ent->app->render;
	Sprite *spr = entitySprite(ent);
	EntityDef *def = ent->def;

	ent->name = def->name;
	uint8_t type = def->eType;

	if (type != 2) {
		if ((type == 7 && def->parm != 0) || type == 8 || type == 10) {
			ent->info |= 0x40000;
		} else if (type == 3) {
			if (def->eSubType == 1)
				spr->renderMode = 7;
			else if (def->tileIndex == 0x45)
				spr->scale = 0x63;
			ent->param = 1;
			if (ent->def->eSubType == 5) {
				ent->param = 0;
				spr->renderMode = 7;
			}
		} else if (type == 0xf) {
			Entity_initFirePit(ent);
		}
		return;
	}

	/* monster: stats from the class table, positions from the sprite */
	ent->monster->ce = ent->app->combat->monsterStats[def->eSubType * 3 + def->parm];
	ent->monster->destX = spr->x;
	ent->monster->ce.x = spr->x;
	ent->monster->destY = spr->y;
	ent->monster->ce.y = spr->y;
	spr->renderMode = 0;

	/* DS Entity_initspawn (0x020241d4): its own subtypes; the scale is the sprite's +0x15 (the
	 * renderer sizes the billboard by it) */
	def = ent->def;
	switch (def->eSubType) {
	case 5:
		spr->renderMode = 0xc;
		spr->info |= 0x20000000;
		break;
	case 4:
		spr->renderMode = 6;
		break;
	case 10:
		spr->renderMode = 4;
		break;
	case 0x12:
		spr->scale = 0x58;
		break;
	case 0x13:
		spr->scale = 0x48;
		break;
	case 1:
	case 3:
	case 6:
		if (def->parm == 0)
			spr->scale = 0x2c;
		break;
	default:
		break;
	}

	if (spr->scale != 0x40)
		ent->monster->flags |= 0x400;
	ent->info |= 0x40000;
	if (ent->app->render->hubSprite == spr) {
		ent->monster->goal.param = 2;
		ent->monster->flags |= 0x80;
	}
	(void)render;
}

/* 0x00017b7c: entities with an on/off state (chests, doors, switches...) */
int Entity_isBinaryEntity(Entity *ent, int *state, short *flags)
{
	if (ent->def == NULL || Entity_isDroppedEntity(ent))
		return 0;

	int on;
	switch (ent->def->eType) {
	case 6:
	case 10:
	case 11:
		on = (ent->info & 0x4000000) != 0;
		break;
	case 5: {
		uint32_t lflags = ent->app->render->lines[(ent->info & 0xffff) - 1].flags;
		on = (lflags & 0x40) != 0;
		if ((lflags & 0x400) && flags != NULL)
			*flags |= (short)0x8000;
		break;
	}
	case 0xe:
		on = ent->def->eSubType == 1;
		break;
	default:
		return 0;
	}
	if (state != NULL)
		*state = on;
	return 1;
}

/* 0x00017c10 */
int Entity_isBoss(Entity *ent)
{
	uint8_t sub = ent->def->eSubType;
	if (sub == 0xd)
		return (ent->monster->flags & 8) != 0;
	return sub >= 0xc;
}

/* 0x00017c4c: one of the 8 drop-item entities */
int Entity_isDroppedEntity(Entity *ent)
{
	Game *game = ent->app->game;
	int idx = (int)(ent - game->entities);
	return idx >= game->firstDrop && idx < game->firstDrop + 8;
}

/* 0x00017c88 */
int Entity_isMonsterType13or14(Entity *ent)
{
	uint8_t sub = ent->def->eSubType;
	if (sub == 0xd)
		return (ent->monster->flags & 8) != 0;
	return sub == 0xf;
}

/* 0x00017ca8: entity renamed by a map string; *strIdx = that string's index */
int Entity_isNamedEntity(Entity *ent, short *strIdx)
{
	EntityDef *def = ent->def;
	if (def == NULL || ent->name == def->name || def->eType == 9 || (ent->info & 0x1000000))
		return 0;
	for (int16_t i = 0; i < ent->app->render->numMapStrings; i++) {
		Render *render = ent->app->render;
		if (strcmp(render->mapStrings[i], ent->name) == 0) {
			*strIdx = i;
			return 1;
		}
	}
	return 0;
}

/* 0x00017d14: 1 if nothing blocks the tile-centre line (a door on the source
 * tile is ignored when it is not the only hit) */
int Entity_checkLineOfSight(Entity *ent, int sx, int sy, int dx, int dy, int mask)
{
	Game *game = ent->app->game;
	Game_trace(game, sx * 64 + 32, sy * 64 + 32, dx * 64 + 32, dy * 64 + 32, ent, mask);

	Entity *hit = game->traceEntity;
	if (hit != NULL && hit->def->eType == 5) {
		Line *ln = &ent->app->render->lines[(hit->info & 0xffff) - 1];
		if (((ln->x2 - ln->x1) / 2 + ln->x1) >> 6 == sx && ((ln->y2 - ln->y1) / 2 + ln->y1) >> 6 == sy) {
			if (game->numTraceHits == 1)
				return 1;
			game->traceEntity = game->traceHits[1];
		}
	}
	return game->traceEntity == NULL;
}

/* 0x00018094: push the player or a monster up to dist tiles away from
 * (srcX, srcY), one axis (random if diagonal) */
void Entity_knockback(Entity *ent, int srcX, int srcY, int dist)
{
	int x, y, mask;
	int reach[2];
	int dir[2];

	if (ent->def->eType == 1) {
		mask = 0x74b5;
		x = ent->app->canvas->viewX;
		y = ent->app->canvas->viewY;
	} else {
		if (ent->info & 0x200000)
			return;
		Sprite *spr = entitySprite(ent);
		x = spr->x;
		y = spr->y;
		mask = (ent->def->eSubType == 0xf) ? 0xfdbf : 0xfdaf;
	}

	dir[0] = x - srcX;
	dir[1] = y - srcY;
	for (int axis = 0; axis < 2; axis++) {
		if (dir[axis] < 0)
			dir[axis] = -1;
		else if (dir[axis] > 0)
			dir[axis] = 1;
		if (dir[axis] == 0)
			continue;

		int tx = x, ty = y;
		if (axis == 0)
			tx = dist * dir[0] * 64 + x;
		else
			ty = dist * dir[1] * 64 + y;
		Game_trace(ent->app->game, srcX, srcY, tx, ty, ent, mask);

		Game *game = ent->app->game;
		Entity *hit = game->traceEntity;
		if (hit == NULL) {
			reach[axis] = dist;
			continue;
		}
		int r;
		if (hit->def->eType == 0) {
			int ddx = x - game->traceX, ddy = y - game->traceY;
			r = Combat_worldDistToTileDist(ddx * ddx + ddy * ddy);
		} else {
			r = Combat_worldDistToTileDist(Entity_distSqFrom(hit, x, y)) - 1;
		}
		reach[axis] = r;
		if (reach[axis] > dist)
			reach[axis] = dist;
		if (reach[axis] < 1)
			dir[axis] = 0;
	}

	Sound_play(ent->app->sound, 0x1449, 0, 3, -1);
	if (dir[0] == 0) {
		if (dir[1] == 0)
			return;
	} else if (dir[1] != 0) {
		dir[Random_nextByte(&ent->app->random) & 1] = 0;
	}

	int steps = reach[dir[0] == 0 ? 1 : 0];
	int tileX = (steps * dir[0] * 64 + x) >> 6;
	int tileY = (steps * dir[1] * 64 + y) >> 6;

	if (ent->def->eType == 1) {
		Canvas *cv = ent->app->canvas;
		cv->pushDx = dir[0];
		cv->pushDy = dir[1];
		cv->pushSteps = steps;
		cv->f230 = steps;
	} else {
		ent->monster->destX = tileX * 64 + 32;
		ent->monster->destY = tileY * 64 + 32;
		ent->monster->flags |= 0x40;
		if (ent->def->eSubType != 0xf)
			MonsterGoal_reset(&ent->monster->goal);
		Game_unlinkEntity_2ae8c(ent->app->game, ent);
		Game_linkEntity(ent->app->game, ent, tileX, tileY);
		ent->app->game->monstersMoving = 1;
	}
}

/* 0x0001a8c0: flags = save-handle bits (0x800 renamed, 0x1000 visible,
 * 0x2000 short record, 0x4000 corpse, 0x400 binary state, 0x8000 locked) */
void Entity_loadState(Entity *ent, void *file, int flags)
{
	File *in = file;
	Render *render;
	Sprite *spr = NULL;

	if (flags & 0x800) {
		int strIdx = Stream_readShort(in);
		ent->name = ent->app->render->mapStrings[strIdx];
	}

	if (!(ent->info & 0x200000)) {
		spr = entitySprite(ent);
		if (flags & 0x1000) {
			spr->info &= ~0x10000u;
			if ((flags & 0x2000) && !(ent->info & 0x4000000))
				Game_linkEntity(ent->app->game, ent, spr->x >> 6, spr->y >> 6);
		} else {
			spr->info |= 0x10000;
			if (ent->info & 0x4000000)
				Game_unlinkEntity_2ae8c(ent->app->game, ent);
		}
	}

	if (Entity_isBinaryEntity(ent, NULL, NULL)) {
		Entity_restoreState(ent, flags);
		if (flags & 0x2000)
			return;
	}

	if (flags & 0x4000) {
		if (!(flags & 0x1000)) {
			ent->info |= 0x20000;
			if (ent->info & 0x4000000)
				Game_unlinkEntity_2ae8c(ent->app->game, ent);
		}
		if (spr != NULL) /* the original never reaches this for line entities */
			spr->info = (spr->info & 0xfffff0ff) + 0x800;
	}
	if (flags & 0x2000)
		return;

	/* full record */
	if (ent->info & 0x4000000)
		Game_unlinkEntity_2ae8c(ent->app->game, ent);
	uint32_t info = (uint32_t)Stream_readInt(in);
	ent->info = info;
	if (info & 0x1000000) { /* a shadow keeps its name when its class has none (see Combat_raiseDead) */
		char *shadowName = ent->app->combat->monsterNames[ent->def->eSubType * 3 + ent->def->parm];
		if (shadowName)
			ent->name = shadowName;
	}

	render = ent->app->render;
	spr = Render_spriteAt(render, (info & 0xffff));
	if (!(info & 0x20000)) {
		int hi = Stream_readShort(in);
		int frame = Stream_readByte(in);
		spr->info = ((uint32_t)(frame & 0xff) << 8) | (spr->info & 0xf0ff) | ((uint32_t)hi << 16);
		if ((((uint32_t)hi << 16) & 0x7fffff) >> 19 == 0) {
			spr->x = (Stream_readByte(in) & 0xff) << 3;
			spr->y = (Stream_readByte(in) & 0xff) << 3;
			Render_relinkSpriteAt(ent->app->render, spr, -1, -1);
		}
		if (Entity_isDroppedEntity(ent) || (ent->def != NULL && ent->def->eType == 3))
			ent->param = Stream_readInt(in);
		ent->linkIndex = (int16_t)Stream_readShort(in);
	}

	if (ent->monster == NULL) {
		if (!Entity_isDroppedEntity(ent)) {
			spr->z = Stream_readByte(in);
			spr->info &= 0xffffff00u;
			spr->info |= (uint32_t)(Stream_readByte(in) & 0xff);
		} else {
			int eType = Stream_readByte(in) & 0xff;
			int eSub = Stream_readByte(in) & 0xff;
			int parm = Stream_readInt(in);
			EntityDef *def = EntityDefs_find(ent->app->entityDefs, eType, eSub, parm);
			ent->def = def;
			ent->name = def->name;
			spr->info = (uint32_t)(int)def->tileIndex | (spr->info & 0xffffff00u);
			spr->ent = ent;
		}
	} else {
		int mflags = Stream_readShort(in);
		ent->monster->flags = (uint16_t)mflags;
		if (!(ent->info & 0x20000)) {
			if (mflags & 0x400)
				spr->scale = (uint8_t)Stream_readByte(in);
			if (!(flags & 0x4000)) {
				if (ent->monster->flags & 0x200)
					ent->monster->petrifyTurns = (uint8_t)Stream_readByte(in);
				CombatEntity_loadState(&ent->monster->ce, file);
				MonsterGoal_loadState(&ent->monster->goal, file);
			}
		}
		if (ent->info & 0x80000) {
			ent->info &= ~0x80000u;
			Game_activateEx(ent->app->game, ent, 0, 0);
		}
		if (ent->info & 0x40000000)
			ent->def = EntityDefs_find(ent->app->entityDefs, 9, ent->def->eSubType, ent->def->parm);
	}

	if (ent->info & 0x4000000) {
		int tx, ty;
		if ((spr->info & 0x7fffff) >> 19 == 0) {
			tx = spr->x >> 6;
			ty = spr->y >> 6;
		} else {
			linkIndexToTile(ent->linkIndex, &tx, &ty);
		}
		Game_linkEntity(ent->app->game, ent, tx, ty);
	} else if (ent->def->eType == 7 && ent->def->eSubType == 3) {
		spr->info = (spr->info & 0xffffff00u) + 0x9b;
	}
}

/* 0x0001aeec */
void MonsterGoal_loadState(MonsterGoal *goal, void *file)
{
	File *in = file;
	goal->type = (uint8_t)Stream_readByte(in);
	goal->flags = (uint8_t)Stream_readByte(in);
	goal->turns = (uint8_t)Stream_readByte(in);
	goal->x = (uint8_t)Stream_readByte(in);
	goal->y = (uint8_t)Stream_readByte(in);
	goal->param = (int16_t)Stream_readShort(in);
}

/* 0x0001ccc0: monster hit for dmg; pain frame and low-health reactions */
void Entity_takeDamage(Entity *ent, int dmg)
{
	EntityMonster *mon = ent->monster;
	int hp = CombatEntity_getHealth(&mon->ce) - dmg;
	if ((mon->flags & 0x800) && hp < 1)
		hp = 1;
	CombatEntity_setHealth(&ent->monster->ce, hp);

	Render *render = ent->app->render;
	Sprite *spr = Render_spriteAt(render, (ent->info & 0xffff));
	if (hp < 1)
		return;

	uint8_t goal = ent->monster->goal.type;
	if (goal != 6 && goal != 10) {
		MonsterGoal_reset(&ent->monster->goal);
		if (ent->monster->petrifyTurns == 0) {
			spr->info = (spr->info & 0xfffff0ff) + 0x600;
			ent->monster->frameTime = ent->app->tickTime + 250;
		}
	}

	if (hp >= CombatEntity_getMaxHealth(&ent->monster->ce) / 2)
		return;

	if (ent->def->eSubType == 1) {
		if (ent->monster->petrifyTurns != 0)
			return;
		if ((int)ent->app->player->ce.level <= ent->def->parm * 3 + 3)
			return;
		if ((Random_nextByte(&ent->app->random) & 3) == 0) {
			char msg[0x40];
			App_snprintf(msg, sizeof msg, (char *)ModData_str(&g_mod, MOD_STR_FLEES), ent->name);
			Hud_addMessage(ent->app->hud, msg, 0);
			MonsterGoal_reset(&ent->monster->goal);
			ent->monster->goal.type = 4;
			ent->monster->goal.param = (int16_t)((unsigned)Random_nextByte(&ent->app->random) % 3 + 2);
			return;
		}
	}

	switch (ent->def->eSubType) {
	case 6: {
		/* summons once: effect sprite 0x95 at the monster */
		if (ent->monster->petrifyTurns != 0 || (ent->info & 0x800000))
			return;
		ent->info |= 0x800000;
		GameSprite *gs = Game_gspriteAlloc(ent->app->game, 0, 0x95, 0, 2);
		if (ent->info & 0x1000000)
			gs->sprite->renderMode = 0xc;
		gs->sprite->x = spr->x;
		gs->sprite->y = spr->y;
		gs->sprite->z = spr->z;
		Render_relinkSpriteAt(ent->app->render, gs->sprite, -1, -1);
		Hud_randomBark(ent->app->hud, 5, 0);
		return;
	}
	case 0xe: {
		/* "%s panics!" (0x30140) is formatted here by the original but never shown */
		int ev = Render_findTileEvent(ent->app->render, 0x20);
		uint32_t word = ent->app->render->tileEvents[ev];
		if ((word & 0x7fffffff) >> 27 != 0)
			return;
		ent->app->render->tileEvents[ev] = (word & 0x87ffffffu) | 0x8000000;
		Game_executeTile(ent->app->game, 0, 1, 0xf000, 1);
		return;
	}
	case 0xd: {
		int ev = Render_findTileEvent(ent->app->render, 0x3ff);
		if ((ent->app->render->tileEvents[ev] & 0x7fffffff) >> 27 != 0)
			return;
		Game_executeTile(ent->app->game, 0x1f, 0x1f, 0xf000, 1);
		return;
	}
	case 0xf: {
		int ev = Render_findTileEvent(ent->app->render, 0x20);
		if ((ent->app->render->tileEvents[ev] & 0x7fffffff) >> 27 != 0)
			return;
		Game_executeTile(ent->app->game, 0, 1, 0xf000, 1);
		return;
	}
	default:
		return;
	}
}

/* 0x00022308 */
void EntityMonster_reset(EntityMonster *monster)
{
	monster->prevOnList = NULL;
	monster->nextOnList = NULL;
	monster->target = NULL;
	monster->nextAttacker = NULL;
	monster->destY = 0;
	monster->destX = 0;
	monster->frameTime = 0;
	monster->flashUntil = 0;
	monster->flags = 0;
	monster->petrifyTurns = 0;
	memset(&monster->goal, 0, sizeof monster->goal);
}

/* 0x00022340 (the Applet pointer at +0 is kept) */
void Entity_reset(Entity *entity)
{
	entity->def = NULL;
	entity->prevOnTile = NULL;
	entity->nextOnTile = NULL;
	entity->linkIndex = 0;
	entity->info = 0;
	entity->param = 0;
	entity->monster = NULL;
	entity->name = NULL;
}

/* 0x000224c4: re-apply a binary entity's saved state (flags bit 10 = on,
 * bit 15 = locked door) */
void Entity_restoreState(Entity *entity, int flags)
{
	int on = (flags >> 10) & 1;
	Applet *app = entity->app;
	Line *line = (entity->info & 0x200000) ? &app->render->lines[(entity->info & 0xffff) - 1] : NULL;

	switch (entity->def->eType) {
	case 5: { /* door */
		int lineIdx = (int)(line - app->render->lines); /* 0-based */
		uint32_t oldFlags = line->flags;
		line->flags = oldFlags & ~0x400u;
		Game_useDoor(entity->app->game, on ? 0xf : 0x10, lineIdx, 0);
		if (!(flags & 0x8000) && !(oldFlags & 0x400))
			return;
		Game_setLineLocked(entity->app->game, lineIdx, (flags & 0x8000) ? 1 : 0, 0);
		return;
	}
	case 6:
	case 10:
	case 11:
		Game_unlinkEntity_2ae8c(app->game, entity);
		if (on) {
			int tx, ty;
			linkIndexToTile(entity->linkIndex, &tx, &ty);
			Game_linkEntity(entity->app->game, entity, tx, ty);
		}
		return;
	case 0xe:
		Game_setSwitch(app->game, entity, on != 0, 0xe);
		return;
	default:
		return;
	}
}

/* 0x0002266c: corpse back to a living monster at (x, y, z), full health */
void Entity_resurrect(Entity *entity, int x, int y, int z)
{
	Render *render = entity->app->render;
	Sprite *spr = Render_spriteAt(render, (entity->info & 0xffff));

	EntityDef *def = EntityDefs_find(entity->app->entityDefs, 2, entity->def->eSubType, entity->def->parm);
	entity->def = def;
	entity->name = def->name;
	entity->monster->petrifyTurns = 0;
	entity->monster->flags &= 0xfdff;
	entity->monster->destX = x;
	entity->monster->ce.x = x;
	spr->x = x;
	entity->monster->destY = y;
	entity->monster->ce.y = y;
	spr->y = y;
	spr->z = z;
	spr->info &= 0xfefef0ffu;
	if (Random_nextByte(&entity->app->random) & 1)
		spr->info |= 0x1000000; /* mirrored */
	Render_relinkSpriteAt(entity->app->render, spr, -1, -1);
	entity->info = (entity->info & 0xac7dffffu) | 0x40000;

	/* initspawn resets the stats from the class table; keep the current ones */
	CombatEntity saved = entity->monster->ce;
	Entity_initspawn(entity);
	entity->monster->ce = saved;

	CombatEntity_setHealth(&entity->monster->ce, CombatEntity_getMaxHealth(&entity->monster->ce));
	Game_unlinkEntity_2ae8c(entity->app->game, entity);
	Game_linkEntity(entity->app->game, entity, x >> 6, y >> 6);
	Canvas_updateFacing(entity->app->canvas);
}

/* 0x000239d0: mirror of Entity_loadState */
void Entity_saveState(Entity *entity, void *os, int flags)
{
	Render *render = entity->app->render;
	Sprite *spr = Render_spriteAt(render, (entity->info & 0xffff));

	if (flags & 0x800) {
		short strIdx = 0;
		Entity_isNamedEntity(entity, &strIdx);
		OutStream_writeShort(os, strIdx);
	}
	if (flags & 0x2000)
		return;

	OutStream_writeInt(os, (int)entity->info);
	if (!(entity->info & 0x20000)) {
		OutStream_writeShort(os, (int32_t)spr->info >> 16);
		OutStream_writeBool(os, (int)((spr->info & 0xfff) >> 8));
		if ((spr->info & 0x7fffff) >> 19 == 0) {
			OutStream_writeBool(os, (int8_t)(spr->x >> 3));
			OutStream_writeBool(os, (int8_t)(spr->y >> 3));
		}
		if (Entity_isDroppedEntity(entity) || (entity->def != NULL && entity->def->eType == 3))
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
	} else {
		OutStream_writeShort(os, (int16_t)entity->monster->flags);
		if (!(entity->info & 0x20000)) {
			if (entity->monster->flags & 0x400)
				OutStream_writeBool(os, (int8_t)spr->scale);
			if (!(flags & 0x4000)) {
				if (entity->monster->flags & 0x200)
					OutStream_writeBool(os, (int8_t)entity->monster->petrifyTurns);
				CombatEntity_saveState(&entity->monster->ce, os);
				MonsterGoal_saveState(&entity->monster->goal, os);
			}
		}
	}
}

/* 0x00023e14 */
void MonsterGoal_saveState(MonsterGoal *goal, void *os)
{
	OutStream_writeBool(os, (int8_t)goal->type);
	OutStream_writeBool(os, (int8_t)goal->flags);
	OutStream_writeBool(os, (int8_t)goal->turns);
	OutStream_writeBool(os, (int8_t)goal->x);
	OutStream_writeBool(os, (int8_t)goal->y);
	OutStream_writeShort(os, goal->param);
}
