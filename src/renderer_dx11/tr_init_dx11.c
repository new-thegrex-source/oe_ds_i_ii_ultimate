/*
 * renderer_dx11 — Direct3D 11 backend (stub)
 */

#include "../renderer/tr_public.h"

#ifdef _WIN32
__declspec(dllexport)
#endif
refexport_t *GetRefAPI( int apiVersion, refimport_t *rimp )
{
	(void)apiVersion;
	(void)rimp;
	/* TODO: implement D3D11 renderer */
	return NULL;
}
