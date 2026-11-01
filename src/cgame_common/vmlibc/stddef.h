/* Minimal C library for game modules compiled to QVM bytecode (q3lcc). Not used by DLL builds. */
#ifndef VM_STDDEF_H
#define VM_STDDEF_H
typedef unsigned int size_t;
typedef int ptrdiff_t;
typedef unsigned short wchar_t;
#ifndef NULL
#define NULL ((void *)0)
#endif
#define offsetof(t, m) ((size_t)&(((t *)0)->m))
#endif
