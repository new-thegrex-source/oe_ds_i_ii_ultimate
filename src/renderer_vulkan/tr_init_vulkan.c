/*
 * renderer_vulkan — Vulkan + PBR / RTX backend (stub)
 *
 * Future home for code adapted from https://github.com/timfox/idTech3
 */

#include "../renderer/tr_public.h"

#ifdef _WIN32
__declspec(dllexport)
#endif
refexport_t *GetRefAPI( int apiVersion, refimport_t *rimp )
{
	(void)apiVersion;
	(void)rimp;
	/* TODO: implement Vulkan renderer with PBR */
	return NULL;
}
