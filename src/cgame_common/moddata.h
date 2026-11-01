#ifndef OE_MODDATA_H
#define OE_MODDATA_H

/*
 * Read-only access to data tables that the original game keeps inside its
 * executable (orcsandelves.mod / orcsandelves2.mod). We never copy those tables
 * into our source: the user supplies their own .mod, the engine verifies it is
 * the release we know the offsets for (size and CRC32) and the game module
 * loads a copy of it at startup (OE_TRAP_GAMEDATA_*).
 */

#include <stddef.h>
#include <stdint.h>

typedef struct ModData {
	uint8_t *data;
	size_t size;
} ModData;

/* 0 ok, -1 if the engine has no .mod for this game. */
int ModData_load(ModData *m);
void ModData_close(ModData *m);

/* Little-endian readers at a file offset (the module loads at address 0, so a
 * code address and its file offset are the same). Out-of-range reads return 0. */
uint8_t ModData_u8(const ModData *m, uint32_t off);
int8_t ModData_s8(const ModData *m, uint32_t off);
uint16_t ModData_u16(const ModData *m, uint32_t off);
int16_t ModData_s16(const ModData *m, uint32_t off);
uint32_t ModData_u32(const ModData *m, uint32_t off);
int32_t ModData_s32(const ModData *m, uint32_t off);
/* NUL-terminated string at off, or "" if out of range. */
const char *ModData_str(const ModData *m, uint32_t off);
const uint8_t *ModData_ptr(const ModData *m, uint32_t off, size_t len);

/* The one loaded .mod used by the game. */
extern ModData g_mod;

#endif
