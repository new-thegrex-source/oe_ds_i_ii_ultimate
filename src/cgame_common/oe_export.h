#ifndef OE_EXPORT_H
#define OE_EXPORT_H

/* oe_export, the game module's part (oe_export.c): where a game keeps its textures */
typedef struct {
	int mappings, palettes, bitshapes;	/* resource ids */
	int texelBase[4];					/* by OE_HWSRC_*: texels number n is resource base + n (O&E) */
	int texelBin[4];					/* or an entry of this resource (O&E II) */
} oeExportGame_t;

void Export_textures(const oeExportGame_t *g);

#endif
