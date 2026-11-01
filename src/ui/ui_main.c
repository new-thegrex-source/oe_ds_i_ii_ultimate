/*
 * Entry point of the ui module (vmMain) and small helpers.
 *
 * In a QVM vmMain must be the first function of the first file given to
 * q3asm: the interpreter starts at code address 0. Keep it first here and
 * keep this file first in the QVM file list (vs2022/build_qvm.cmd).
 */
#include "ui_local.h"

#if defined(_WIN32) && !defined(Q3_VM)
#define UI_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) && !defined(Q3_VM)
#define UI_EXPORT __attribute__((visibility("default")))
#else
#define UI_EXPORT
#endif

#define UI_MAIN_MENU	"ui/main.menu"

UI_EXPORT intptr_t vmMain(int command, int arg0, int arg1, int arg2, int arg3, int arg4, int arg5, int arg6,
                          int arg7, int arg8, int arg9, int arg10, int arg11)
{
	char cmd[64];
	(void)arg3; (void)arg4; (void)arg5; (void)arg6; (void)arg7; (void)arg8; (void)arg9; (void)arg10; (void)arg11;
	switch (command) {
	case UI_INIT:
		if (arg0 != UI_API_VERSION) {
			UI_Printf("ui: engine API version %d, this module needs %d\n", arg0, UI_API_VERSION);
			return -1;
		}
		if (!Menu_Load(UI_MAIN_MENU))
			return -1;
		Menu_OpenMain();
		trap_Key_SetCatcher(KEYCATCH_UI);
		return 0;
	case UI_SHUTDOWN:
		return 0;
	case UI_KEY_EVENT:
		Menu_Key(arg0, arg1);
		return 0;
	case UI_CHAR_EVENT:
		Menu_Char(arg0);
		return 0;
	case UI_MOUSE_POSITION:
		Menu_MouseMove(arg0, arg1);
		return 0;
	case UI_REFRESH:
		Menu_Refresh(arg0, arg1, arg2);
		return 0;
	case UI_CONSOLE_COMMAND:
		trap_Argv(0, cmd, sizeof(cmd));
		return Menu_Command(cmd);
	}
	return -1;
}

void UI_Printf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	trap_Print(buf);
}

void UI_CvarString(const char *name, char *buf, int size)
{
	buf[0] = 0;
	if (name[0])
		trap_Cvar_VariableStringBuffer(name, buf, size);
}

float UI_CvarValue(const char *name)
{
	char buf[64];
	const char *s = buf;
	float value = 0, frac = 0.1f;
	int neg = 0;

	UI_CvarString(name, buf, sizeof(buf));
	if (*s == '-') {
		neg = 1;
		s++;
	}
	while (*s >= '0' && *s <= '9')
		value = value * 10 + (*s++ - '0');
	if (*s == '.') {
		for (s++; *s >= '0' && *s <= '9'; s++, frac *= 0.1f)
			value += (*s - '0') * frac;
	}
	return neg ? -value : value;
}

int Q_stricmpUI(const char *a, const char *b)
{
	for (;;) {
		int ca = (unsigned char)*a++, cb = (unsigned char)*b++;
		if (ca >= 'A' && ca <= 'Z')
			ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z')
			cb += 'a' - 'A';
		if (ca != cb)
			return ca - cb;
		if (!ca)
			return 0;
	}
}
