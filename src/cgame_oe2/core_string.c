/*
 * Java-like String class (16 bytes: buf, cap, len, flags) and the word-wrap
 * helper object TextUtil (Applet+0x148).
 *
 * A String either borrows a C literal (flags == 0, String_ctorCStr) or owns a
 * heap buffer (flags == 1). Any mutation first makes it owned
 * (String_makeOwned). A default-constructed String has buf == NULL.
 */
#include <stdio.h>

#include "core_local.h"

/* ------------------------------------------------------------ basic fields */

/* 0x000241a8 */
void String_initFields(String *s)
{
	s->buf = NULL;
	s->cap = 0;
	s->len = 0;
	s->flags = 0;
}

/* 0x00040204  New buffer of `size` bytes holding the first n bytes of src
 * (or an empty string when src is NULL). The second argument is a raw char
 * pointer in practice; the prototype keeps the original's String* typing. */
char *String_allocCopy(int size, String *src, int n)
{
	/* calloc instead of malloc: bytes past the copied part are defined (the
	 * original left them uninitialised; String_setLength can expose them). */
	char *p = calloc((size_t)(size > 0 ? size : 1), 1);
	if (!p)
		return NULL;
	if (src == NULL)
		p[0] = '\0';
	else if (n > 0)
		memmove(p, (const char *)src, (size_t)n);
	return p;
}

/* 0x00040384 */
void String_freeBuffer(String *s)
{
	if (s->buf) {
		free(s->buf);
		s->buf = NULL;
	}
}

/* 0x0001ba78 */
void String_release(String *s)
{
	if (s->flags != 0) {
		String_freeBuffer(s);
		s->cap = 0;
		s->len = 0;
		s->flags = 0;
	}
}

/* 0x0002b768  Copy a borrowed literal into an owned buffer of `cap` bytes
 * (cap 0 = just big enough). */
void String_makeOwned(String *s, int cap)
{
	int n;
	if (s->flags != 0)
		return;
	if (cap == 0)
		cap = s->len + 1;
	n = cap < s->cap ? cap : s->cap;
	s->buf = String_allocCopy(cap, (String *)s->buf, n);
	s->cap = cap;
	s->flags = 1;
}

/* 0x00035324  Grow the owned buffer to `cap` bytes. */
void String_realloc(String *s, int cap)
{
	char *old = s->buf;
	int keep = s->len + 1;
	if (keep > cap)
		keep = cap; /* the original copied len+1 regardless; never larger than the new buffer here */
	s->buf = String_allocCopy(cap, (String *)old, keep);
	free(old);
	if (cap < s->len)
		s->len = cap;
	s->cap = cap;
}

/* 0x0001cb10 */
void String_ensureCapacity(String *s, int cap)
{
	String_makeOwned(s, cap);
	if (cap > s->cap)
		String_realloc(s, cap);
}

/* ------------------------------------------------------------ constructors */

static String *string_alloc(String *self)
{
	/* operator_new for a String constructed with this == NULL */
	return self ? self : calloc(1, sizeof(String));
}

/* 0x00003fe0 */
String *String_ctor(String *self)
{
	self = string_alloc(self);
	if (self)
		String_initFields(self);
	return self;
}

/* 0x00003e58  Wrap a literal without copying it. */
String *String_ctorCStr(String *self, char *s)
{
	self = string_alloc(self);
	if (self) {
		String_initFields(self);
		self->flags = 0;
		self->buf = s;
		self->len = (int32_t)strlen(s);
		self->cap = self->len + 1;
	}
	return self;
}

/* 0x000402f4  Deep copy (dst is overwritten, not released). */
void String_copyCtor(String *dst, String *src)
{
	if (src->len < 1) {
		dst->buf = NULL;
		dst->cap = 0;
		dst->len = 0;
		dst->flags = 0;
		return;
	}
	dst->buf = calloc((size_t)src->len + 1, 1);
	if (dst->buf)
		memmove(dst->buf, src->buf, (size_t)src->len + 1);
	dst->cap = src->len + 1;
	dst->len = src->len;
	dst->flags = 1;
}

/* 0x00003ec4 */
String *String_ctorCopy(String *self, String *other)
{
	self = string_alloc(self);
	if (self) {
		String_initFields(self);
		String_copyCtor(self, other);
	}
	return self;
}

/* 0x00003f04 */
String *String_ctorChar(String *self, int c)
{
	self = string_alloc(self);
	if (self) {
		String_initFields(self);
		self->buf = calloc(0x10, 1);
		self->buf[0] = (char)c;
		self->buf[1] = '\0';
		self->flags = 1;
		self->len = 1;
		self->cap = 0x10;
	}
	return self;
}

/* 0x00003f68 */
String *String_ctorInt(String *self, int v)
{
	char tmp[64];
	String lit;
	self = string_alloc(self);
	if (self) {
		String_initFields(self);
		App_snprintf(tmp, sizeof(tmp), "%i", v);
		String_ctorCStr(&lit, tmp);
		String_copyCtor(self, &lit);
		String_release(&lit);
	}
	return self;
}

/* 0x0000409c */
void String_dtor(String *self, int del)
{
	String_release(self);
	if (del)
		free(self);
}

/* 0x00003d64  operator= */
String *String_assign(String *dst, String *src)
{
	String_release(dst);
	String_copyCtor(dst, src);
	return dst;
}

/* ------------------------------------------------------------ accessors */

/* 0x00025424 */
int String_length(String *s)
{
	return s->len;
}

/* 0x000096ac  Unsigned byte at i (no range check, like the original). */
int String_charAt(String *s, int i)
{
	return (uint8_t)s->buf[i];
}

/* 0x00030d84 */
char *String_cstr(String *str)
{
	return str->buf ? str->buf : "";
}

/* 0x0002e7b8 */
int String_toInt(String *str)
{
	return atoi(String_cstr(str));
}

/* 0x0000a270  -1 / 0 / 1; a NULL buffer sorts after everything. */
int String_compareTo(String *a, String *b)
{
	int r;
	if (a->buf == b->buf)
		return 0;
	if (a->buf == NULL)
		return 1;
	if (b->buf == NULL)
		return -1;
	r = strcmp(a->buf, b->buf);
	if (r == 0)
		return 0;
	return r > 0 ? 1 : -1;
}

/* 0x000040fc */
int String_equals_40fc(String *a, String *b)
{
	return String_compareTo(a, b) == 0;
}

/* 0x000144a0  (a second copy of equals in the binary) */
int String_equals_144a0(String *a, String *b)
{
	return String_compareTo(a, b) == 0;
}

/* 0x0000412c */
int String_notEquals(String *a, String *b)
{
	return String_compareTo(a, b) != 0;
}

/* 0x0001f368 */
int String_indexOf(String *s, int ch, int from)
{
	const char *p;
	if (!s->buf)
		return -1;
	p = strchr(s->buf + from, ch);
	return p ? (int)(p - s->buf) : -1;
}

/* 0x0003da4c */
int String_startsWith(String *s, String *prefix)
{
	return strncmp(String_cstr(s), String_cstr(prefix), (size_t)prefix->len) == 0;
}

/* ------------------------------------------------------------ mutation */

/* 0x00040274 */
void String_appendCStr_40274(String *s, char *cstr)
{
	int n;
	char *copy = NULL;
	if (cstr == NULL)
		return;
	n = (int)strlen(cstr);
	/* appending a string to itself: keep the source alive across the realloc */
	if (s->buf && cstr >= s->buf && cstr < s->buf + s->cap) {
		copy = malloc((size_t)n + 1);
		if (!copy)
			return;
		memcpy(copy, cstr, (size_t)n + 1);
		cstr = copy;
	}
	String_ensureCapacity(s, s->len + n + 1);
	memmove(s->buf + s->len, cstr, (size_t)n + 1);
	s->len += n;
	free(copy);
}

/* 0x000076f0 */
String *String_appendCStr_76f0(String *s, char *cstr)
{
	String_appendCStr_40274(s, cstr);
	return s;
}

/* 0x00007708 */
String *String_appendString(String *s, String *other)
{
	String_appendCStr_40274(s, other->buf);
	return s;
}

/* 0x00007724  (a NUL char appends nothing, as in the original) */
String *String_appendChar(String *s, int c)
{
	char tmp[2];
	tmp[0] = (char)c;
	tmp[1] = '\0';
	String_appendCStr_40274(s, tmp);
	return s;
}

/* 0x00007750 */
String *String_appendInt(String *s, int v)
{
	char tmp[64];
	App_snprintf(tmp, sizeof(tmp), "%d", v);
	String_appendCStr_40274(s, tmp);
	return s;
}

/* 0x00039cc0  Writing at index len extends the string by one. */
void String_setCharAt(String *s, int i, int c)
{
	int len = s->len;
	String_ensureCapacity(s, len == i ? len + 2 : len + 1);
	s->buf[i] = (char)c;
	if (len == i) {
		s->buf[i + 1] = '\0';
		s->len = i + 1;
	}
}

/* 0x0003a1dc */
void String_setLength(String *s, int n)
{
	String_ensureCapacity(s, n + 1);
	s->len = n;
	s->buf[n] = '\0';
}

/* 0x0000a9e0 */
void String_deleteCharAt(String *s, int i)
{
	String_makeOwned(s, 0);
	if (s->len - 1 == i) {
		s->len--;
		s->buf[s->len] = '\0';
	} else {
		memmove(s->buf + i, s->buf + i + 1, (size_t)(s->len - i));
		s->len--;
	}
}

/* 0x00024474 */
void String_insert(String *s, int pos, String *other)
{
	int n = other->len;
	String_ensureCapacity(s, s->len + n + 1);
	if (s->len != pos)
		memmove(s->buf + pos + n, s->buf + pos, (size_t)(s->len - pos));
	memmove(s->buf + pos, String_cstr(other), (size_t)n);
	s->buf[s->len + n] = '\0';
	s->len += n;
}

/* ------------------------------------------------------------ derived strings */

/* 0x00040894  [start, end); end == 0 means "to the end". `out` is constructed. */
void String_substring(String *out, String *s, int start, int end)
{
	String tmp;
	int n = end == 0 ? s->len - start : end - start;
	String_initFields(&tmp);
	if (n > 0) {
		String_ensureCapacity(&tmp, n + 1);
		memmove(tmp.buf, s->buf + start, (size_t)n);
		tmp.buf[n] = '\0';
		tmp.len = n;
	}
	String_initFields(out);
	String_copyCtor(out, &tmp);
	String_release(&tmp);
}

/* 0x00042098  ASCII upper case copy. */
void String_toUpper(String *out, String *s)
{
	String tmp;
	String_initFields(&tmp);
	String_copyCtor(&tmp, s);
	if (tmp.buf) {
		for (int i = 0; i < s->len + 1; i++) {
			uint8_t c = (uint8_t)tmp.buf[i];
			if (c > 0x60 && c < 0x7b)
				c -= 0x20;
			tmp.buf[i] = (char)c;
		}
	}
	String_initFields(out);
	String_copyCtor(out, &tmp);
	String_release(&tmp);
}

static int is_trim_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* 0x00043400  Strip ' ', '\t', '\r', '\n' at both ends. `out` is constructed. */
void String_trim(String *out, String *s)
{
	int start = 0, last;
	while (start < s->len && is_trim_space(s->buf[start]))
		start++;
	if (start >= s->len) {
		String_initFields(out);
		return;
	}
	last = s->len - 1;
	while (last >= 0 && is_trim_space(s->buf[last]))
		last--;
	String_substring(out, s, start, last + 1);
}

/* 0x0001c320  Find `key` in a text file and return the rest of that line
 * (leading spaces skipped, CR/LF excluded); "" when the key is missing.
 * `out` is constructed. Returns out. */
void *String_getKeyValue(String *out, int unused, char *text, char *key)
{
	char *p = strstr(text, key);
	char *end;
	String lit;
	(void)unused;
	if (p == NULL) {
		String_ctorCStr(out, "");
		return out;
	}
	p += strlen(key);
	while (*p == ' ')
		p++;
	end = strchr(p, '\n');
	if (end) {
		char saved;
		if (end > p && end[-1] == '\r')
			end--;
		saved = *end;
		*end = '\0';
		String_ctorCStr(&lit, p);
		String_ctorCopy(out, &lit);
		*end = saved;
	} else {
		String_ctorCStr(&lit, p);
		String_ctorCopy(out, &lit);
	}
	String_dtor(&lit, 0);
	return out;
}

/* 0x00040878  Rough pixel width of a string in the small font. */
int Text_approxWidth(int unused, String *str)
{
	(void)unused;
	return String_length(str) * 7 + 9;
}

/* ------------------------------------------------------------ self test */

static int st_eq(String *a, const char *lit)
{
	String b;
	int r;
	String_ctorCStr(&b, (char *)lit);
	r = String_equals_144a0(a, &b);
	String_release(&b);
	return r == 1;
}

static int st_sub(String *s, int start, int end, const char *expect)
{
	String sub;
	int r;
	String_substring(&sub, s, start, end);
	r = st_eq(&sub, expect);
	String_release(&sub);
	return r;
}

static int st_starts(String *s, const char *prefix)
{
	String p;
	int r;
	String_ctorCStr(&p, (char *)prefix);
	r = String_startsWith(s, &p);
	String_release(&p);
	return r;
}

static int st_insert(String *s, int pos, const char *what, const char *expect)
{
	String w;
	String_ctorCStr(&w, (char *)what);
	String_insert(s, pos, &w);
	String_release(&w);
	return st_eq(s, expect);
}

/* 0x00040fc0  The original's built-in unit test of the String class; App_startup
 * refuses to start when it fails. Same checks, in the same order. */
int String_selfTest(void)
{
	String s, t, u;
	int ok;

	String_ctorCStr(&s, "Hello World!");
	if (s.len != 12)
		goto fail;
	String_release(&s);
	String_ctorCStr(&s, "Hello World!");
	if (memcmp(s.buf, "Hello World!", 12) != 0)
		goto fail;
	if (!st_sub(&s, 1, 0, "ello World!") || !st_sub(&s, 0, 5, "Hello") || !st_sub(&s, 6, 12, "World!"))
		goto fail;
	String_substring(&t, &s, 0, 0);
	ok = String_equals_144a0(&t, &s) == 1;
	String_release(&t);
	if (!ok || !st_sub(&s, 1, 2, "e"))
		goto fail;

	String_ctorCStr(&t, "____________");
	String_release(&s);
	String_copyCtor(&s, &t);
	String_release(&t);
	if (!st_sub(&s, 0, 5, "_____") || !st_sub(&s, 0, 10, "__________") || !st_sub(&s, 0, 12, "____________"))
		goto fail;

	String_release(&s);
	String_ctorCStr(&s, "AAAAAA");
	String_ctorCStr(&t, "BBBBBB");
	String_ctorCStr(&u, "AAAAAA");
	ok = String_compareTo(&s, &u) == 0;
	String_release(&u);
	ok = ok && String_compareTo(&s, &t) == -1 && String_compareTo(&t, &s) == 1;
	String_release(&t);
	if (!ok)
		goto fail;

	String_release(&s);
	String_ctorCStr(&s, "Hello World!");
	if (String_indexOf(&s, 'H', 0) != 0 || String_indexOf(&s, 'H', 1) != -1 || String_indexOf(&s, '@', 0) != -1 ||
	    String_indexOf(&s, '!', 0) != 11)
		goto fail;
	if (st_starts(&s, "Hello") != 1 || st_starts(&s, "Hello World!") != 1 || st_starts(&s, "O RLY") != 0 ||
	    st_starts(&s, "Hello World! What's up?") != 0)
		goto fail;

	String_toUpper(&t, &s);
	ok = st_eq(&t, "HELLO WORLD!");
	String_release(&t);
	if (!ok)
		goto fail;
	String_release(&s);
	String_ctorCStr(&s, "   Hello World!    ");
	String_trim(&t, &s);
	ok = st_eq(&t, "Hello World!");
	String_release(&t);
	if (!ok)
		goto fail;

	String_release(&s);
	String_ctorCStr(&s, "Fellow Mold!");
	String_setCharAt(&s, 0, 'H');
	if (!st_eq(&s, "Hellow Mold!"))
		goto fail;
	String_setCharAt(&s, 5, ' ');
	if (!st_eq(&s, "Hello  Mold!"))
		goto fail;
	String_setCharAt(&s, 6, 'W');
	if (!st_eq(&s, "Hello WMold!"))
		goto fail;
	String_setCharAt(&s, 7, 'o');
	if (!st_eq(&s, "Hello Woold!"))
		goto fail;
	String_setCharAt(&s, 8, 'r');
	if (!st_eq(&s, "Hello World!"))
		goto fail;

	String_release(&s);
	String_initFields(&s);
	String_appendInt(&s, 10);
	if (!st_eq(&s, "10"))
		goto fail;
	String_appendInt(&s, 10);
	if (!st_eq(&s, "1010"))
		goto fail;
	String_appendInt(&s, -1010);
	if (!st_eq(&s, "1010-1010"))
		goto fail;

	String_release(&s);
	String_initFields(&s);
	if (!st_insert(&s, 0, "world", "world") || !st_insert(&s, 0, "hello ", "hello world"))
		goto fail;
	String_setLength(&s, 0);
	if (!st_insert(&s, 0, "hello", "hello") || !st_insert(&s, 5, " world", "hello world"))
		goto fail;
	String_setLength(&s, 0);
	if (!st_insert(&s, 0, "helloworld", "helloworld") || !st_insert(&s, 5, " ", "hello world"))
		goto fail;

	String_release(&s);
	String_initFields(&s);
	String_appendChar(&s, 'H');
	String_ctorChar(&t, 'H');
	ok = String_equals_40fc(&s, &t) == 1;
	String_release(&t);
	if (!ok)
		goto fail;
	String_appendChar(&s, 'e');
	String_appendChar(&s, 'l');
	String_appendChar(&s, 'l');
	String_appendChar(&s, 'o');
	if (!st_eq(&s, "Hello"))
		goto fail;

	String_release(&s);
	String_initFields(&s);
	String_appendCStr_40274(&s, "Hello");
	String_appendCStr_40274(&s, " ");
	String_appendCStr_40274(&s, "World!");
	if (!st_eq(&s, "Hello World!"))
		goto fail;

	String_release(&s);
	String_ctorCStr(&s, "Hello World!");
	String_setLength(&s, 5);
	if (s.len != 5 || !st_eq(&s, "Hello"))
		goto fail;
	String_setLength(&s, 0);
	if (s.len != 0 || !st_eq(&s, ""))
		goto fail;

	String_release(&s);
	String_ctorCStr(&s, "Hello World!");
	String_deleteCharAt(&s, 11);
	if (!st_eq(&s, "Hello World"))
		goto fail;
	String_deleteCharAt(&s, 5);
	if (!st_eq(&s, "HelloWorld"))
		goto fail;
	String_deleteCharAt(&s, 0);
	if (!st_eq(&s, "elloWorld"))
		goto fail;

	String_release(&s);
	String_ctorCStr(&s, "Hello");
	if (!st_eq(&s, "Hello"))
		goto fail;
	String_initFields(&t);
	if (String_equals_144a0(&t, &s) != 0)
		goto fail;
	String_release(&t);

	String_release(&s);
	String_initFields(&s);
	String_appendCStr_40274(&s, "Hello");
	if (!st_eq(&s, "Hello"))
		goto fail;
	String_appendCStr_40274(&s, " ");
	if (!st_eq(&s, "Hello "))
		goto fail;
	String_appendCStr_40274(&s, "World!");
	if (!st_eq(&s, "Hello World!"))
		goto fail;
	String_release(&s);
	String_initFields(&s);
	if (s.len != 0)
		goto fail;

	String_release(&s);
	String_ctorCStr(&s, "____________");
	String_substring(&t, &s, 0, 10);
	if (!st_eq(&t, "__________")) {
		String_release(&t);
		goto fail;
	}
	String_substring(&u, &s, 0, 1);
	String_release(&t);
	String_copyCtor(&t, &u);
	String_release(&u);
	ok = st_eq(&t, "_");
	String_release(&t);
	String_release(&s);
	return ok;

fail:
	String_release(&s);
	return 0;
}

/* ------------------------------------------------------------ TextUtil */

/* 0x00001900  Index of the first NUL inside the string, else its length. */
int TextUtil_strlenZ(TextUtil *self, String *str)
{
	int i;
	(void)self;
	for (i = 0; i < String_length(str); i++)
		if (String_charAt(str, i) == 0)
			return i;
	return String_length(str);
}

/* 0x00001950 */
int TextUtil_clamp(TextUtil *self, int v, int lo, int hi)
{
	(void)self;
	if (v > hi)
		return hi;
	return v < lo ? lo : v;
}

/* 0x0000196c  Lines are separated by '\n' or '|'. */
int TextUtil_countLines(TextUtil *self, String *str)
{
	int n = 0;
	(void)self;
	for (int i = 0; i < String_length(str); i++) {
		int c = String_charAt(str, i);
		if (c == '\n' || c == '|')
			n++;
	}
	return n + 1;
}

/* 0x00002730 */
int TextUtil_isBreakChar(TextUtil *self, int c)
{
	(void)self;
	return c == ' ' || c == '\n' || c == '|' || c == '\r' || c == '\t';
}

/* 0x000019cc  Append up to n visible characters of src (from `start`) to dst.
 * A lone '-' is a soft-hyphen marker and is dropped; "--" is a real '-'.
 * Sets *trunc once n visible characters were copied. Returns the number of
 * source characters consumed. */
int TextUtil_copyChars(TextUtil *self, String *dst, String *src, int start, int n, int *trunc)
{
	int copied = 0, hyphens = 0;
	int end = start + n;
	(void)self;
	for (; start < end; start++) {
		if (String_charAt(src, start) == '-') {
			hyphens++;
			if (String_charAt(src, start + 1) != '-')
				continue;
			String_appendChar(dst, '-');
			start++;
		} else {
			String_appendChar(dst, String_charAt(src, start));
		}
		copied++;
		if (copied >= n) {
			if (trunc)
				*trunc = 1;
			break;
		}
	}
	return copied + hyphens;
}

/* 0x00001a88  Remove soft hyphens in place ("--" keeps one '-'). Like the
 * original, the kept '-' of a "--" pair is not rewritten when earlier
 * characters were removed (the slot keeps whatever byte was there). */
void TextUtil_dehyphenate(TextUtil *self, String *str)
{
	int out = 0;
	(void)self;
	for (int i = 0; i < String_length(str); i++) {
		if (String_charAt(str, i) == '-') {
			if (i < String_length(str) - 1 && String_charAt(str, i + 1) == '-') {
				i++;
				out++;
			}
		} else {
			if (out != i)
				String_setCharAt(str, out, String_charAt(str, i));
			out++;
		}
	}
	String_setLength(str, out);
}

/* 0x00003a20  Word-wrap src (from `start`) into dst for lines of `width`
 * characters, ending lines with lineChar. Words may break at soft hyphens.
 * Stops after maxLines lines (0 = unlimited) and sets *trunc. Returns the
 * source index where wrapping stopped. */
int TextUtil_wrapTextEx(TextUtil *self, String *src, int start, int width, String *dst, int maxLines, int lineChar,
			int *trunc)
{
	int lineLen = 0, lines = 0;
	int i;

	if (src == dst)
		return 0;
	if (trunc)
		*trunc = 0;

	for (i = start; i < String_length(src); i++) {
		int c = String_charAt(src, i);
		int wordLen, srcCount, numBreaks, j;

		if (c == ' ') {
			String_appendChar(dst, ' ');
			lineLen++;
			continue;
		}
		if (c == '\n' || c == '|') {
			String_appendChar(dst, lineChar);
			lines++;
			if (maxLines != 0 && lines >= maxLines) {
				if (trunc)
					*trunc = 1;
				return i + 1;
			}
			lineLen = 0;
			continue;
		}

		/* measure the word; remember where its soft hyphens are */
		wordLen = 0;
		srcCount = 0;
		numBreaks = 0;
		for (j = i; j < String_length(src); j++) {
			int cj = String_charAt(src, j);
			if (TextUtil_isBreakChar(self, cj) || width < srcCount)
				break;
			if (cj == '-') {
				if (String_charAt(src, j + 1) == '-') {
					wordLen++;
					srcCount++;
					j++;
					continue;
				}
				if (numBreaks < 8) { /* 8 slots; the original did not check */
					self->hyphenBreaks[numBreaks * 2] = wordLen;
					self->hyphenBreaks[numBreaks * 2 + 1] = srcCount;
					numBreaks++;
				}
			} else {
				wordLen++;
			}
			srcCount++;
		}

		if (width < lineLen + wordLen) {
			int best = -1;
			for (int k = 0; k < numBreaks && self->hyphenBreaks[k * 2] + lineLen + 1 <= width; k++)
				best = k;
			if (best == -1) {
				/* no hyphen fits: trim trailing blanks and start a new line */
				if (String_length(dst) > 0) {
					while (String_length(dst) > 0 &&
					       TextUtil_isBreakChar(self, String_charAt(dst, String_length(dst) - 1)))
						String_deleteCharAt(dst, String_length(dst) - 1);
					String_appendChar(dst, lineChar);
					lines++;
				}
			} else {
				i += TextUtil_copyChars(self, dst, src, i, self->hyphenBreaks[best * 2 + 1], trunc);
				String_appendChar(dst, '-');
				String_appendChar(dst, lineChar);
				lines++;
				wordLen -= self->hyphenBreaks[best * 2];
			}
			lineLen = 0;
		}
		if (maxLines != 0 && lines >= maxLines) {
			if (trunc)
				*trunc = 1;
			return i;
		}
		i += TextUtil_copyChars(self, dst, src, i, j - i, trunc) - 1;
		lineLen += wordLen;
	}
	return String_length(src);
}

/* 0x0000394c */
void TextUtil_wrapText(TextUtil *self, String *src, int width, String *dst)
{
	TextUtil_wrapTextEx(self, src, 0, width, dst, 0, '|', NULL);
}

/* 0x00003990  (the original ignores `trunc` here and passes NULL) */
void TextUtil_wrapTextLines(TextUtil *self, String *src, int width, String *dst, int maxLines, int lineChar, int *trunc)
{
	(void)trunc;
	TextUtil_wrapTextEx(self, src, 0, width, dst, maxLines, lineChar, NULL);
}

/* 0x000039d4 */
void TextUtil_wrapTextFrom(TextUtil *self, String *src, int start, int width, String *dst)
{
	TextUtil_wrapTextEx(self, src, start, width, dst, 0, '|', NULL);
}
