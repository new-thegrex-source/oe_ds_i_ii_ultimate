/* The game's .mod (the original BREW executable), copied from the engine. Same readers as oe-re/oe2-re. */
#include "moddata.h"
#include "oe_vm.h"

#include <stdlib.h>
#include <string.h>

ModData g_mod;

int ModData_load(ModData *m)
{
	int n;

	memset(m, 0, sizeof(*m));
	n = trap_GameDataSize(OE_DATA_MOD);
	if (n <= 0)
		return -1;
	m->data = malloc((size_t)n);
	if (!m->data)
		return -1;
	if (trap_GameDataRead(OE_DATA_MOD, 0, m->data, n) != n) {
		ModData_close(m);
		return -1;
	}
	m->size = (size_t)n;
	return 0;
}

void ModData_close(ModData *m)
{
	free(m->data);
	memset(m, 0, sizeof(*m));
}

static int in_range(const ModData *m, uint32_t off, size_t len)
{
	return m->data && (size_t)off + len <= m->size;
}

uint8_t ModData_u8(const ModData *m, uint32_t off) { return in_range(m, off, 1) ? m->data[off] : 0; }
int8_t ModData_s8(const ModData *m, uint32_t off) { return (int8_t)ModData_u8(m, off); }

uint16_t ModData_u16(const ModData *m, uint32_t off)
{
	return in_range(m, off, 2) ? (uint16_t)(m->data[off] | (m->data[off + 1] << 8)) : 0;
}

int16_t ModData_s16(const ModData *m, uint32_t off) { return (int16_t)ModData_u16(m, off); }

uint32_t ModData_u32(const ModData *m, uint32_t off)
{
	if (!in_range(m, off, 4))
		return 0;
	return (uint32_t)m->data[off] | ((uint32_t)m->data[off + 1] << 8) |
	       ((uint32_t)m->data[off + 2] << 16) | ((uint32_t)m->data[off + 3] << 24);
}

int32_t ModData_s32(const ModData *m, uint32_t off) { return (int32_t)ModData_u32(m, off); }

const char *ModData_str(const ModData *m, uint32_t off)
{
	size_t i;
	if (!in_range(m, off, 1))
		return "";
	for (i = off; i < m->size; i++)
		if (m->data[i] == 0)
			return (const char *)m->data + off;
	return "";
}

const uint8_t *ModData_ptr(const ModData *m, uint32_t off, size_t len)
{
	return in_range(m, off, len) ? m->data + off : NULL;
}
