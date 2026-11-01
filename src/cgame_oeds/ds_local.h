#ifndef OE_DS_LOCAL_H
#define OE_DS_LOCAL_H

/* Orcs & Elves DS on the Orcs & Elves (BREW) game code: what this module does differently.
 *
 * The DS version is the same game code (Applet, Canvas, Game, Entity, Player, Render,
 * ScriptThread, ...) grown with new maps, monsters and story; its data comes from the user's
 * cartridge (OE_TRAP_DS_FILE) instead of orcsandelves.bar. The HUD, menus, fonts and sounds stay
 * those of the BREW game (its .bar). The software renderer still runs (the game logic depends on
 * what it sees) with the BREW textures the DS .bsp still names; the picture of the 3D view is the
 * cartridge's 3D, drawn by the engine (OE_TRAP_DS_MAP, OE_TRAP_DS_SPRITES).
 *
 * Addresses "DS 0x0204xxxx" are functions of the DS arm9 (USA, YOEE); see README-OE-DS-MAPS.md. */

#include "game.h"
#include "oe_vm.h"

/* The DS maps: map id = DS_MAP_FIRST + index (DS Render_beginLoadMap 0x02042364, table 0x0206d84c).
 * Lava, bug and forge are the DS's own; the hub moved from 6008 (BREW) to 6011. */
#define DS_MAP_FIRST	6000
#define DS_NUM_MAPS		14
#define DS_MAP_HUB		6011
#define DS_MAP_MENU		6012

const char *DsMap_name(int mapId);		/* "map_m01", or NULL */
int			DsMap_stringGroup(int mapId);	/* its strings/<language>/ group (byte table 0x0206b6bc) */

/* a sprite's info as the DS packs it (render_load.c keeps BREW's: its flags 4 bits higher) */
static inline uint32_t DsSprite_info(const Sprite *s)
{
	return (s->info & 0xfff) | ((s->info >> 4) & 0x0ffff000) | s->dsInfoHigh;
}

/* ds_data.c: the map's script commands in BREW numbers (DS 0x69..0x75 keep theirs) */
enum {
	DS_OP_MARKTILE = 0x69, DS_OP_LAUNCHFLAMES, DS_OP_TOUCHENTITIES, DS_OP_TURNPLAYERTO,
	DS_OP_RESPAWNMONSTER, DS_OP_UNMARKTILE, DS_OP_SET, DS_OP_TRIGGERIF, DS_OP_LERPSPRITETOOPENTILE,
	DS_OP_ABORTUNLESSTILEOPEN, DS_OP_PLAYER73, DS_OP_EMITTER, DS_OP_PLAYER75,
	DS_OP_SHOWWAKE = 0x76,		/* DS 0x67: EV_SHOW that also wakes the monster shown */
	DS_OP_LERPSPRITE77 = 0x77,	/* DS 0x77: EV_LERPSPRITE with the lerp's flag 4 */
	DS_OP_DATA = 0x7f			/* DS commands that only hold data (or are not done) */
};
int			DsScript_brewOp(int dsOp);

/* ds_spells.c: the wand spells' circle (the DS touch screen's), on Q and the mouse */
int			DsSpells_open(void);					/* the circle is up */
void		DsSpells_toggle(Canvas *cv);			/* Q in play */
int			DsSpells_key(Canvas *cv, int avk);		/* a key while it is up */
void		DsSpells_frame(Canvas *cv);				/* every tick in play */
void		DsSpells_attackDone(Combat *combat);	/* the player's attack is over */
void		DsSpells_debugCast(Canvas *cv, int s);	/* developer: castspell N */
void		Canvas_attackAhead(Canvas *canvas);		/* canvas_input.c: the use/attack key's attack */

/* core_stream.c: a cartridge file into the stream (like Resource_open with inflate) */
int			DsFile_open(InputStream *is, const char *path);

/* ds_data.c: the cartridge's texts (strings/<language>/<group>.bin: NUL-terminated strings) */
const char	*DsStrings_get(const char *group, int index);	/* "" if missing */
int			DsStrings_count(const char *group);
void		DsStrings_free(void);

#endif
