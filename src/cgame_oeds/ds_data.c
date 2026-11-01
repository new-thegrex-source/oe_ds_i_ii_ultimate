/*
 * Orcs & Elves DS: the cartridge's map table and texts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds_local.h"

/* DS Render_beginLoadMap (0x02042364): name = table[mapId - 6000] (0x0206d84c), and the map's
 * string group (0x0206b6bc), the index of strings/<language>/map_<name>.bin among the groups */
static const struct {
	const char	*name;
	int			group;
} dsMaps[DS_NUM_MAPS] = {
	{ "map_m01", 7 }, { "map_m02", 8 }, { "map_m03", 9 }, { "map_lava", 6 }, { "map_bug", 3 },
	{ "map_m04", 10 }, { "map_m05", 11 }, { "map_forge", 4 }, { "map_m06", 12 }, { "map_m07", 13 },
	{ "map_m08", 14 }, { "map_hub", 5 }, { "map_menu", 15 }, { "map_test", 16 },
};

const char *DsMap_name(int mapId)
{
	int i = mapId - DS_MAP_FIRST;
	return i >= 0 && i < DS_NUM_MAPS ? dsMaps[i].name : NULL;
}

int DsMap_stringGroup(int mapId)
{
	int i = mapId - DS_MAP_FIRST;
	return i >= 0 && i < DS_NUM_MAPS ? dsMaps[i].group : -1;
}

/* ------------------------------------------------------------ script commands */

/* The DS numbers its script commands (DS ScriptThread_executeEvent 0x0204d388, which logs their
 * names "EV_...") like the BREW game up to 0x38; it put a data command at 0x39, so from 0x3a on
 * each is the BREW one plus 1; 0x67 is a SHOW that wakes its monster and 0x77 a sprite move with
 * a flag of its own; 0x69..0x76 are new. The map's commands are turned into the BREW numbers when
 * loaded (the BREW code, which also looks for some by number, then runs them); the new ones keep
 * theirs (DS_OP_*). */
int DsScript_brewOp(int op)
{
	if (op <= 0x38)
		return op;					/* (0x35, 0x36, 0x38: the cinematic camera, both) */
	if (op == 0x39)
		return DS_OP_DATA;			/* a DS data command, not run */
	if (op <= 0x66)
		return op - 1;				/* (0x62 EV_SHOWHAMMERCUES is BREW's 0x61 "timestamp") */
	if (op == 0x67)
		return DS_OP_SHOWWAKE;
	if (op == 0x68)
		return DS_OP_DATA;
	if (op == 0x76)
		return 0x26;				/* EV_REQITEM: the DS runs it as 0x26 (EV_CHECKITEM) */
	if (op == 0x77)
		return DS_OP_LERPSPRITE77;
	return op;						/* 0x69..0x75: the DS's own */
}

/* ------------------------------------------------------------ texts */

/* a loaded group: its bytes (the strings, NUL-terminated one after the other) and where each starts */
typedef struct {
	char		name[32];
	char		*data;
	const char	**strings;
	int			count;
} dsStringGroup_t;

#define DS_MAX_GROUPS 8

static dsStringGroup_t	dsGroups[DS_MAX_GROUPS];

/* The DS marks where words may break with '-' inside them ("En-trance"); the BREW text code would
 * print it. A '-' between two letters goes; "--" stays as one real hyphen. */
static int ds_letter(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static void ds_unhyphenate(char *s)
{
	char *start = s, *o = s;
	for (; *s; s++) {
		if (s[0] == '-' && s[1] == '-') {
			*o++ = '-';
			s++;
			continue;
		}
		if (s[0] == '-' && o > start && ds_letter(o[-1]) && ds_letter(s[1]))
			continue;
		*o++ = *s;
	}
	*o = 0;
}

static dsStringGroup_t *ds_group(const char *group)
{
	char path[64];
	int i, size, n, k;
	dsStringGroup_t *g = NULL;

	for (i = 0; i < DS_MAX_GROUPS; i++) {
		if (dsGroups[i].data && !strcmp(dsGroups[i].name, group))
			return &dsGroups[i];
		if (!dsGroups[i].data && !g)
			g = &dsGroups[i];
	}
	if (!g) {
		/* full: the oldest goes */
		DsStrings_free();
		g = &dsGroups[0];
	}
	snprintf(path, sizeof path, "strings/English/%s.bin", group);
	size = trap_DsFile(path, NULL, 0);
	if (size < 0)
		return NULL;
	g->data = calloc(1, (size_t)size + 1);
	if (!g->data || trap_DsFile(path, g->data, size) != size) {
		free(g->data);
		g->data = NULL;
		return NULL;
	}
	for (n = 0, k = 0; k < size; k++)
		n += g->data[k] == 0;
	if (size > 0 && g->data[size - 1] != 0)
		n++;
	g->strings = calloc((size_t)n + 1, sizeof(*g->strings));
	if (!g->strings) {
		free(g->data);
		g->data = NULL;
		return NULL;
	}
	for (n = 0, k = 0; k < size; n++) {
		int start = k;
		while (k < size && g->data[k] != 0)
			k++;
		k++;
		g->strings[n] = g->data + start;
		ds_unhyphenate(g->data + start);	/* (shorter in place: after its end was found) */
	}
	g->count = n;
	snprintf(g->name, sizeof g->name, "%s", group);
	return g;
}

int DsStrings_count(const char *group)
{
	dsStringGroup_t *g = ds_group(group);
	return g ? g->count : 0;
}

const char *DsStrings_get(const char *group, int index)
{
	dsStringGroup_t *g = ds_group(group);
	return g && index >= 0 && index < g->count ? g->strings[index] : "";
}

void DsStrings_free(void)
{
	int i;
	for (i = 0; i < DS_MAX_GROUPS; i++) {
		free(dsGroups[i].data);
		free((void *)dsGroups[i].strings);
		memset(&dsGroups[i], 0, sizeof dsGroups[i]);
	}
}
