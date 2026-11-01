#ifndef VM_STDIO_H
#define VM_STDIO_H
#include <stddef.h>
#include <stdarg.h>
/* Only what the game code uses: formatting, and stdout/stderr going to the engine console */
typedef struct _vm_FILE FILE;
extern FILE *stdout;
extern FILE *stderr;
#define EOF (-1)
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int fprintf(FILE *f, const char *fmt, ...);
int printf(const char *fmt, ...);
int fputs(const char *s, FILE *f);
int fputc(int c, FILE *f);
int puts(const char *s);
int fflush(FILE *f);
#endif
