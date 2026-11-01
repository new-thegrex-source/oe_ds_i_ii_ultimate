/*
 * renderer_dx9 — Direct3D 9 backend (stub)
 *
 * When implemented this file will export GetRefAPI() for the D3D9 path.
 * Until then the client falls back to the built-in OpenGL renderer.
 */

#include "../renderer/tr_public.h"

/*
=================
GetRefAPI

Stub. Returns NULL so the client knows this backend is not ready.
=================
*/
#ifdef _WIN32
__declspec(dllexport)
#endif
refexport_t *GetRefAPI( int apiVersion, refimport_t *rimp )
{
	(void)apiVersion;
	(void)rimp;
	/* TODO: implement D3D9 renderer */
	return NULL;
}
