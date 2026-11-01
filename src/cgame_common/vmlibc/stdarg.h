#ifndef VM_STDARG_H
#define VM_STDARG_H
/* QVM arguments are 4-byte slots in increasing addresses; long long takes two */
typedef char *va_list;
#define _VM_ARGSIZE(t) ((sizeof(t) + 3u) & ~3u)
#define va_start(ap, last) ((ap) = (va_list)&(last) + _VM_ARGSIZE(last))
#define va_arg(ap, t) (*(t *)(((ap) += _VM_ARGSIZE(t)) - _VM_ARGSIZE(t)))
#define va_end(ap) ((ap) = (va_list)0)
#define va_copy(d, s) ((d) = (s))
#endif
