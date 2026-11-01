/*
 * The client of the headless QA host (oe-headless): no window, no keys, no
 * console drawing. The game module runs exactly as in oe.exe; its screen is
 * only kept for OE_SHOT / "shot PATH", and sounds report "not playing"
 * (built with OE_NO_AUDIO_DEVICE). Used to compare the QVM and DLL builds
 * with the native ports (golden traces, OE_PIPE bots).
 */
#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../oe/oe_local.h"

cvar_t *cl_shownet;

void CL_InitKeyCommands( void ) {}
void CL_Init( void ) { Cvar_Set( "cl_running", "1" ); }
void CL_Shutdown( void ) { Cvar_Set( "cl_running", "0" ); }
void CL_Frame( int msec ) { (void)msec; }
void CL_KeyEvent( int key, qboolean down, unsigned time ) { (void)key; (void)down; (void)time; }
void CL_CharEvent( int key ) { (void)key; }
void CL_MouseEvent( int dx, int dy, int time ) { (void)dx; (void)dy; (void)time; }
void CL_JoystickEvent( int axis, int value, int time ) { (void)axis; (void)value; (void)time; }
void CL_ConsolePrint( char *text ) { (void)text; }
void CL_GameStopped( const char *reason ) { (void)reason; Cbuf_AddText( "quit\n" ); }
void CL_WindowSize( int *width, int *height ) { *width = *height = 0; }

/* no GPU: the game draws everything itself */
qboolean CL_HwAvailable( void ) { return qfalse; }
void CL_HwReset( void ) {}
void CL_HwTexture( const oeHwTexture_t *desc, const void *pixels, const byte *mask, const unsigned short *palette ) {
	(void)desc; (void)pixels; (void)mask; (void)palette;
}
void CL_HwScene( const oeHwScene_t *scene, const oeHwQuad_t *quads, const short *flatTiles, const unsigned short *screen ) {
	(void)scene; (void)quads; (void)flatTiles; (void)screen;
}
void CL_HwScreen( const unsigned short *px, int w, int h, int pitch ) { (void)px; (void)w; (void)h; (void)pitch; }
qboolean CL_HwFreeView( int *view ) { (void)view; return qfalse; }
byte *CL_LoadImageRGBA( const char *name, int *w, int *h ) { (void)name; *w = *h = 0; return NULL; }
void CL_HwTexturesChanged( void ) {}
int CL_FreeMove( oeFreeMove_t *move ) { Com_Memset( move, 0, sizeof( *move ) ); return 0; }	/* QA runs turn-based */
void CL_HwLeaveQ3( void ) {}
qboolean CL_DsStart( void ) { Com_Printf( "Orcs & Elves DS needs the client (its maps are drawn on the GPU)\n" ); return qfalse; }
int CL_DsFile( const char *path, void *buf, int size ) { (void)path; (void)buf; (void)size; return -1; }
qboolean CL_DsModuleMap( const char *name ) { (void)name; return qfalse; }
void CL_DsSprites( const oeDsSprite_t *sprites, int count ) { (void)sprites; (void)count; }
qboolean CL_GamePointer( int *x, int *y, int *buttons ) { *x = *y = *buttons = 0; return qfalse; }
void CL_StartHunkUsers( qboolean rendererOnly ) { (void)rendererOnly; }
void Key_KeynameCompletion( void (*callback)( const char *s ) ) { (void)callback; }
void Key_WriteBindings( fileHandle_t f ) { (void)f; }
int Key_StringToKeynum( char *str ) { (void)str; return -1; }
void Key_SetBinding( int keynum, const char *binding ) { (void)keynum; (void)binding; }
char *Key_GetBinding( int keynum ) { (void)keynum; return ""; }
void Sys_SetWindowTitle( const char *title ) { (void)title; }

/* no menus: oe-headless needs +set fs_game */
qboolean CL_UIStart( void ) { return qfalse; }
void CL_UIStop( void ) {}
qboolean CL_UIRunning( void ) { return qfalse; }
void CL_UIError( void ) {}
qboolean CL_UICommand( void ) { return qfalse; }
