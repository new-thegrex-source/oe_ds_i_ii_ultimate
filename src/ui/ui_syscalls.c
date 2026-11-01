/*
 * System calls of the ui module built as a DLL (uix86_64.dll). The QVM build
 * uses ui_syscalls.asm instead, which maps the same functions to the
 * negative trap numbers the interpreter hands to the engine.
 */
#ifdef Q3_VM
#error "Do not compile ui_syscalls.c into the QVM: use ui_syscalls.asm"
#endif

#include "ui_local.h"

#if defined(_WIN32)
#define UI_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define UI_EXPORT __attribute__((visibility("default")))
#else
#define UI_EXPORT
#endif

static intptr_t (*ui_syscall)(intptr_t arg, ...) = (intptr_t (*)(intptr_t, ...))-1;

UI_EXPORT void dllEntry(intptr_t (*syscallptr)(intptr_t arg, ...))
{
	ui_syscall = syscallptr;
}

/* floats travel as their bits, as in a QVM */
static intptr_t passFloat(float f)
{
	union { float f; int i; } u;
	u.f = f;
	return u.i;
}

void trap_Print(const char *text) { ui_syscall(UI_PRINT, text); }
void trap_Error(const char *text) { ui_syscall(UI_ERROR, text); }
int trap_Milliseconds(void) { return (int)ui_syscall(UI_MILLISECONDS); }
void trap_Cvar_Set(const char *name, const char *value) { ui_syscall(UI_CVAR_SET, name, value); }
void trap_Cvar_VariableStringBuffer(const char *name, char *buf, int size) { ui_syscall(UI_CVAR_VARIABLESTRINGBUFFER, name, buf, size); }
int trap_Argc(void) { return (int)ui_syscall(UI_ARGC); }
void trap_Argv(int n, char *buf, int size) { ui_syscall(UI_ARGV, n, buf, size); }
void trap_Cmd_ExecuteText(const char *text) { ui_syscall(UI_CMD_EXECUTETEXT, text); }
int trap_FS_ReadFile(const char *path, char *buf, int size) { return (int)ui_syscall(UI_FS_READFILE, path, buf, size); }
int trap_R_RegisterShaderNoMip(const char *name) { return (int)ui_syscall(UI_R_REGISTERSHADERNOMIP, name); }
void trap_R_SetColor(const float *rgba) { ui_syscall(UI_R_SETCOLOR, rgba); }
void trap_R_DrawStretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2, int shader)
{
	ui_syscall(UI_R_DRAWSTRETCHPIC, passFloat(x), passFloat(y), passFloat(w), passFloat(h), passFloat(s1),
		passFloat(t1), passFloat(s2), passFloat(t2), shader);
}
int trap_R_ConsoleFont(void) { return (int)ui_syscall(UI_R_CONSOLEFONT); }
int trap_Key_GetCatcher(void) { return (int)ui_syscall(UI_KEY_GETCATCHER); }
void trap_Key_SetCatcher(int catcher) { ui_syscall(UI_KEY_SETCATCHER, catcher); }
int trap_R_White(void) { return (int)ui_syscall(UI_R_WHITE); }
void trap_R_RegisterFont(const char *name, int pointSize, fontInfo_t *font) {
	ui_syscall(UI_R_REGISTERFONT, name, pointSize, font);
}
