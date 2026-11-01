/*
 * renderer_software — pure software backend (stub)
 */

#include "../renderer/tr_public.h"

#ifdef _WIN32
__declspec(dllexport)
#endif
refexport_t *GetRefAPI( int apiVersion, refimport_t *rimp )
{
	(void)apiVersion;
	(void)rimp;
	/* TODO: implement software renderer */
	return NULL;
}
