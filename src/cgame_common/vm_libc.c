/*
 * C library for game modules compiled to QVM bytecode. A DLL build uses the
 * platform's C library instead and does not compile this file.
 *
 * memset/memcpy/memmove/sqrt/sin are system calls (oe_syscalls.asm); stdout and
 * stderr go to the engine console; getenv reads engine cvars; malloc manages a
 * fixed pool in the module's memory.
 */
#ifndef Q3_VM
#error "vm_libc.c is only for QVM builds"
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

#include "oe_vm.h"

int errno;

/* ================================================================ heap
 * Boundary-tag allocator over one static pool. Every block starts with a
 * header {size, prevSize}; size includes the header, is a multiple of 8 and
 * has bit 0 set while the block is in use. Free blocks are kept in a doubly
 * linked list (first fit) and merged with free neighbours. */

#ifndef VM_HEAP_SIZE
#define VM_HEAP_SIZE (26 << 20)
#endif

typedef struct vmBlock {
	unsigned size;
	unsigned prevSize;
	struct vmBlock *next;	/* free blocks only */
	struct vmBlock *prev;
} vmBlock;

#define HDR 8
#define MIN_BLOCK 16
#define BLK_USED 1u
#define BLK_SIZE(b) ((b)->size & ~7u)

static unsigned char vmHeap[VM_HEAP_SIZE];
static vmBlock *freeList;
static vmBlock *heapEnd;
static int heapReady;

static void heap_unlink(vmBlock *b)
{
	if (b->prev)
		b->prev->next = b->next;
	else
		freeList = b->next;
	if (b->next)
		b->next->prev = b->prev;
}

static void heap_link(vmBlock *b)
{
	b->prev = NULL;
	b->next = freeList;
	if (freeList)
		freeList->prev = b;
	freeList = b;
}

static void heap_init(void)
{
	vmBlock *b = (vmBlock *)(((uintptr_t)vmHeap + 7) & ~(uintptr_t)7);
	unsigned size = (unsigned)((vmHeap + VM_HEAP_SIZE - (unsigned char *)b) & ~7);
	b->size = size;
	b->prevSize = 0;
	heapEnd = (vmBlock *)((unsigned char *)b + size);
	freeList = NULL;
	heap_link(b);
	heapReady = 1;
}

static vmBlock *heap_nextBlock(vmBlock *b)
{
	vmBlock *n = (vmBlock *)((unsigned char *)b + BLK_SIZE(b));
	return n < heapEnd ? n : NULL;
}

void *malloc(size_t n)
{
	vmBlock *b;
	unsigned need;
	if (!heapReady)
		heap_init();
	if (n > VM_HEAP_SIZE)
		return NULL;
	need = (unsigned)((n + HDR + 7) & ~7u);
	if (need < MIN_BLOCK)
		need = MIN_BLOCK;
	for (b = freeList; b; b = b->next) {
		unsigned size = BLK_SIZE(b);
		if (size < need)
			continue;
		heap_unlink(b);
		if (size - need >= MIN_BLOCK) {
			vmBlock *rest = (vmBlock *)((unsigned char *)b + need), *after;
			rest->size = size - need;
			rest->prevSize = need;
			after = heap_nextBlock(rest);
			if (after)
				after->prevSize = rest->size;
			heap_link(rest);
			size = need;
		}
		b->size = size | BLK_USED;
		return (unsigned char *)b + HDR;
	}
	return NULL;
}

void free(void *p)
{
	vmBlock *b, *n;
	if (!p)
		return;
	b = (vmBlock *)((unsigned char *)p - HDR);
	if (!(b->size & BLK_USED)) {
		trap_Error("free: block is not in use (double free or bad pointer)");
		return;
	}
	b->size &= ~BLK_USED;
	/* merge with the next block */
	n = heap_nextBlock(b);
	if (n && !(n->size & BLK_USED)) {
		heap_unlink(n);
		b->size += BLK_SIZE(n);
	}
	/* merge with the previous block */
	if (b->prevSize) {
		vmBlock *pb = (vmBlock *)((unsigned char *)b - b->prevSize);
		if (!(pb->size & BLK_USED)) {
			heap_unlink(pb);
			pb->size += BLK_SIZE(b);
			b = pb;
		}
	}
	n = heap_nextBlock(b);
	if (n)
		n->prevSize = BLK_SIZE(b);
	heap_link(b);
}

void *calloc(size_t n, size_t size)
{
	void *p;
	if (size && n > (size_t)VM_HEAP_SIZE / size)
		return NULL;
	p = malloc(n * size);
	if (p)
		memset(p, 0, n * size);
	return p;
}

void *realloc(void *p, size_t n)
{
	vmBlock *b;
	size_t have;
	void *q;
	if (!p)
		return malloc(n);
	if (n == 0) {
		free(p);
		return NULL;
	}
	b = (vmBlock *)((unsigned char *)p - HDR);
	have = BLK_SIZE(b) - HDR;
	if (n <= have)
		return p;
	q = malloc(n);
	if (!q)
		return NULL;
	memcpy(q, p, have);
	free(p);
	return q;
}

/* ================================================================ strings */

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;
	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;
	for (; n; n--, p++)
		if (*p == (unsigned char)c)
			return (void *)p;
	return NULL;
}

size_t strlen(const char *s)
{
	const char *p = s;
	while (*p)
		p++;
	return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
	size_t n = 0;
	while (n < max && s[n])
		n++;
	return n;
}

char *strcpy(char *dst, const char *src)
{
	char *d = dst;
	while ((*d++ = *src++) != 0)
		;
	return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
	size_t i;
	for (i = 0; i < n && src[i]; i++)
		dst[i] = src[i];
	for (; i < n; i++)
		dst[i] = 0;
	return dst;
}

char *strcat(char *dst, const char *src)
{
	strcpy(dst + strlen(dst), src);
	return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
	char *d = dst + strlen(dst);
	while (n-- && *src)
		*d++ = *src++;
	*d = 0;
	return dst;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (unsigned char)*a - (unsigned char)*b;
		if (!*a)
			return 0;
	}
	return 0;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}

char *strrchr(const char *s, int c)
{
	const char *last = NULL;
	for (;; s++) {
		if (*s == (char)c)
			last = s;
		if (!*s)
			return (char *)last;
	}
}

char *strstr(const char *s, const char *find)
{
	size_t n = strlen(find);
	if (!n)
		return (char *)s;
	for (; *s; s++)
		if (*s == *find && !strncmp(s, find, n))
			return (char *)s;
	return NULL;
}

size_t strspn(const char *s, const char *accept)
{
	size_t n = 0;
	while (s[n] && strchr(accept, s[n]))
		n++;
	return n;
}

size_t strcspn(const char *s, const char *reject)
{
	size_t n = 0;
	while (s[n] && !strchr(reject, s[n]))
		n++;
	return n;
}

/* ================================================================ ctype (ASCII) */

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isprint(int c) { return c >= 0x20 && c < 0x7f; }
int ispunct(int c) { return isprint(c) && !isalnum(c) && c != ' '; }
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }

/* ================================================================ numbers */

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

static unsigned long long parse_ull(const char *s, char **end, int base, int *neg)
{
	unsigned long long v = 0;
	const char *p = s;
	int any = 0;
	*neg = 0;
	while (isspace((unsigned char)*p))
		p++;
	if (*p == '+' || *p == '-')
		*neg = *p++ == '-';
	if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
		p += 2;
		base = 16;
	} else if (base == 0) {
		base = *p == '0' ? 8 : 10;
	}
	for (;; p++) {
		int d;
		if (isdigit((unsigned char)*p))
			d = *p - '0';
		else if (*p >= 'a' && *p <= 'z')
			d = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'Z')
			d = *p - 'A' + 10;
		else
			break;
		if (d >= base)
			break;
		v = v * (unsigned)base + (unsigned)d;
		any = 1;
	}
	if (end)
		*end = (char *)(any ? p : s);
	return v;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
	int neg;
	unsigned long long v = parse_ull(s, end, base, &neg);
	return neg ? 0 - v : v;
}

long long strtoll(const char *s, char **end, int base)
{
	int neg;
	unsigned long long v = parse_ull(s, end, base, &neg);
	return neg ? (long long)(0 - v) : (long long)v;
}

unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
long strtol(const char *s, char **end, int base) { return (long)strtoll(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }

/* insertion sort: the game sorts only short lists */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
	unsigned char *b = base, tmp[256];
	size_t i, j;
	if (size > sizeof(tmp)) {
		trap_Error("qsort: element too large");
		return;
	}
	for (i = 1; i < n; i++) {
		memcpy(tmp, b + i * size, size);
		for (j = i; j > 0 && cmp(b + (j - 1) * size, tmp) > 0; j--)
			memcpy(b + j * size, b + (j - 1) * size, size);
		memcpy(b + j * size, tmp, size);
	}
}

void abort(void) { trap_Error("abort() called"); }
void exit(int status) { (void)status; trap_Error("exit() called"); }

void _vm_assert_fail(const char *expr, const char *file, int line)
{
	char buf[512];
	snprintf(buf, sizeof(buf), "assertion failed: %s (%s:%d)", expr, file, line);
	trap_Error(buf);
}

/* getenv reads the engine cvar of the same name (the engine copies OE_* environment variables) */
char *getenv(const char *name)
{
	static char buf[4][256];
	static int slot;
	char *b = buf[slot++ & 3];
	trap_Cvar_VariableStringBuffer(name, b, sizeof(buf[0]));
	return b[0] ? b : NULL;
}

/* ================================================================ printf */

typedef struct {
	char *buf;
	size_t size, len;
} vmOut;

static void out_char(vmOut *o, char c)
{
	if (o->len + 1 < o->size)
		o->buf[o->len] = c;
	o->len++;
}

static void out_pad(vmOut *o, char c, int n)
{
	while (n-- > 0)
		out_char(o, c);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	vmOut o;
	o.buf = buf;
	o.size = size;
	o.len = 0;
	for (; *fmt; fmt++) {
		int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, prec = -1, lng = 0;
		char digits[72], *d;
		const char *s;
		int n, neg;
		unsigned long long u;

		if (*fmt != '%') {
			out_char(&o, *fmt);
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-') left = 1;
			else if (*fmt == '0') zero = 1;
			else if (*fmt == '+') plus = 1;
			else if (*fmt == ' ') space = 1;
			else if (*fmt == '#') alt = 1;
			else break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				left = 1;
				width = -width;
			}
			fmt++;
		} else {
			while (isdigit((unsigned char)*fmt))
				width = width * 10 + *fmt++ - '0';
		}
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else {
				while (isdigit((unsigned char)*fmt))
					prec = prec * 10 + *fmt++ - '0';
			}
		}
		for (;; fmt++) {
			if (*fmt == 'l') lng++;
			else if (*fmt == 'h' || *fmt == 'z' || *fmt == 't' || *fmt == 'j') { if (*fmt == 'j') lng = 2; }
			else break;
		}
		switch (*fmt) {
		case 'd': case 'i':
			if (lng >= 2) {
				long long v = va_arg(ap, long long);
				neg = v < 0;
				u = neg ? 0 - (unsigned long long)v : (unsigned long long)v;
			} else {
				int v = va_arg(ap, int);
				neg = v < 0;
				u = neg ? 0u - (unsigned)v : (unsigned)v;
			}
			goto number10;
		case 'u':
			u = lng >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
			neg = 0;
		number10:
			d = digits + sizeof(digits);
			*--d = 0;
			do {
				*--d = (char)('0' + u % 10);
				u /= 10;
			} while (u);
			s = d;
			goto emitnum;
		case 'x': case 'X': case 'o': case 'p':
			if (*fmt == 'p') {
				u = (unsigned)(uintptr_t)va_arg(ap, void *);
				alt = 1;
			} else {
				u = lng >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
			}
			neg = 0;
			d = digits + sizeof(digits);
			*--d = 0;
			if (*fmt == 'o') {
				do {
					*--d = (char)('0' + (u & 7));
					u >>= 3;
				} while (u);
				if (alt && *d != '0')
					*--d = '0';
				alt = 0;
			} else {
				const char *hex = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
				int any = u != 0;
				do {
					*--d = hex[u & 15];
					u >>= 4;
				} while (u);
				if (!any && *fmt != 'p')
					alt = 0;
			}
			s = d;
		emitnum:
			{
				int len = (int)strlen(s), zeros = 0, prefix = 0;
				char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
				if (prec >= 0) {
					zero = 0;
					if (prec == 0 && len == 1 && s[0] == '0')
						len = 0;
					if (prec > len)
						zeros = prec - len;
				}
				if (sign)
					prefix = 1;
				if (alt && (*fmt == 'x' || *fmt == 'X' || *fmt == 'p'))
					prefix += 2;
				n = width - len - zeros - prefix;
				if (zero && !left) {
					zeros += n > 0 ? n : 0;
					n = 0;
				}
				if (!left)
					out_pad(&o, ' ', n);
				if (sign)
					out_char(&o, sign);
				if (alt && (*fmt == 'x' || *fmt == 'X' || *fmt == 'p')) {
					out_char(&o, '0');
					out_char(&o, *fmt == 'X' ? 'X' : 'x');
				}
				out_pad(&o, '0', zeros);
				while (len-- > 0)
					out_char(&o, *s++);
				if (left)
					out_pad(&o, ' ', n);
			}
			break;
		case 'c':
			if (!left)
				out_pad(&o, ' ', width - 1);
			out_char(&o, (char)va_arg(ap, int));
			if (left)
				out_pad(&o, ' ', width - 1);
			break;
		case 's':
			s = va_arg(ap, const char *);
			if (!s)
				s = "(null)";
			n = prec >= 0 ? (int)strnlen(s, (size_t)prec) : (int)strlen(s);
			if (!left)
				out_pad(&o, ' ', width - n);
			{
				int i;
				for (i = 0; i < n; i++)
					out_char(&o, s[i]);
			}
			if (left)
				out_pad(&o, ' ', width - n);
			break;
		case '%':
			out_char(&o, '%');
			break;
		case 0:
			fmt--;
			break;
		default:
			/* unsupported conversion (floats are not used by the game code): show it as written */
			out_char(&o, '%');
			out_char(&o, *fmt);
			break;
		}
	}
	if (o.size)
		o.buf[o.len < o.size ? o.len : o.size - 1] = 0;
	return (int)o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
	return vsnprintf(buf, 0x7fffffff, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(buf, 0x7fffffff, fmt, ap);
	va_end(ap);
	return n;
}

/* stdout/stderr: lines collected and sent to the engine console */
struct _vm_FILE {
	char line[1024];
	int len;
};
static FILE vmStdout, vmStderr;
FILE *stdout = &vmStdout;
FILE *stderr = &vmStderr;

int fflush(FILE *f)
{
	if (f && f->len) {
		f->line[f->len] = 0;
		trap_Print(f->line);
		f->len = 0;
	}
	return 0;
}

int fputc(int c, FILE *f)
{
	if (!f)
		return EOF;
	f->line[f->len++] = (char)c;
	if (c == '\n' || f->len >= (int)sizeof(f->line) - 1)
		fflush(f);
	return (unsigned char)c;
}

int fputs(const char *s, FILE *f)
{
	while (*s)
		fputc(*s++, f);
	return 0;
}

int puts(const char *s)
{
	fputs(s, stdout);
	fputc('\n', stdout);
	return 0;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
	char buf[1024];
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	fputs(buf, f);
	return n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vfprintf(f, fmt, ap);
	va_end(ap);
	return n;
}

int printf(const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vfprintf(stdout, fmt, ap);
	va_end(ap);
	return n;
}
