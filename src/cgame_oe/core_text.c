/*
 * Text helpers: line-break counting and word wrapping with soft hyphens.
 *
 * Wrapped text uses '|' (or '\n') as the line separator. In source texts a
 * single '-' marks a place where a long word may be hyphenated (it is not
 * printed otherwise) and "--" is a literal hyphen.
 */
#include "core_local.h"

/* the original kept up to 8 hyphenation points per word in a stack array and
 * did not check the bound; a larger array keeps the same results */
#define MAX_HYPHEN_POINTS 64

/* 0x000013e4  Number of '\n' and '|' characters. */
int Text_countLineBreaks(char *s)
{
	int n = 0;
	for (size_t i = 0; i < strlen(s); i++)
		if (s[i] == '\n' || s[i] == '|')
			n++;
	return n;
}

/* 0x00001ce8 */
int Text_isBreakChar(int c)
{
	return c == ' ' || c == '\n' || c == '|' || c == '\r' || c == '\t';
}

/* 0x0000141c  Copy the word at *src to *dst up to a break character or the
 * end, dropping soft hyphens and turning "--" into '-'. Stops after n output
 * characters (setting *trunc); always copies at least one. Both pointers are
 * advanced. */
void Text_copyChars(char **dst, char **src, int n, int *trunc)
{
	const uint8_t *s = (const uint8_t *)*src;
	char *d = *dst;
	int count = 0;

	while (*s != 0 && !Text_isBreakChar(*s)) {
		if (*s == '-') {
			if (s[1] != '-') {
				s++; /* soft hyphen */
				continue;
			}
			*d = '-';
			s += 2;
		} else {
			*d = (char)*s++;
		}
		d++;
		if (++count >= n) {
			if (trunc != NULL)
				*trunc = 1;
			break;
		}
	}
	*dst = d;
	*src = (char *)s;
}

/* 0x00002a84  Word-wrap src into dst (dstSize bytes) for lines of `width`
 * characters, separating lines with lineChar. A word that does not fit is
 * hyphenated at its last soft hyphen that fits, or moved to the next line.
 * Stops after maxLines line breaks (0 = no limit) or when dst is full
 * (*trunc = 1). Returns the number of source characters consumed. */
int Text_wrap(char *src, int width, char *dst, int dstSize, int maxLines, int lineChar, int *trunc)
{
	char *out = dst, *last = dst + dstSize - 1;
	char *in = src;
	int col = 0, lines = 0;
	int hyphenAt[MAX_HYPHEN_POINTS]; /* visible chars before each soft hyphen */

	if (trunc != NULL)
		*trunc = 0;

	while (*in != '\0') {
		const uint8_t *p;
		int visible, numHyphens, best;

		if (out >= last)
			goto full;
		if (*in == ' ') {
			in++;
			*out++ = ' ';
			col++;
			continue;
		}
		if (*in == '\n' || *in == '|') {
			*out++ = (char)lineChar;
			in++;
			col = 0;
			continue;
		}

		/* measure the word */
		visible = 0;
		numHyphens = 0;
		for (p = (const uint8_t *)in; !Text_isBreakChar(*p) && *p != 0;) {
			if (*p == '-') {
				if (p[1] == '-') {
					p += 2;
					visible += 2; /* (counts both characters of "--") */
					continue;
				}
				if (numHyphens < MAX_HYPHEN_POINTS)
					hyphenAt[numHyphens++] = visible;
			} else {
				visible++;
			}
			p++;
		}

		if (col + visible >= width) {
			/* last soft hyphen whose first part (plus the '-') still fits */
			best = -1;
			for (int i = 0; i < numHyphens && hyphenAt[i] + col + 1 <= width; i++)
				best = i;
			if (best == -1) {
				/* break before the word: drop trailing blanks first */
				while (out > dst && Text_isBreakChar((uint8_t)out[-1]))
					out--;
				if (last - out < 1)
					goto full;
				*out++ = (char)lineChar;
				lines++;
			} else {
				if (last - out < hyphenAt[best] + 2)
					goto full;
				Text_copyChars(&out, &in, hyphenAt[best], trunc);
				out[0] = '-';
				out[1] = (char)lineChar;
				out += 2;
				lines++;
				visible -= hyphenAt[best];
			}
			col = 0;
		}
		if (maxLines != 0 && lines >= maxLines)
			break;
		p = (const uint8_t *)in;
		Text_copyChars(&out, &in, (int)(last - out), trunc);
		col += visible;
		if ((const uint8_t *)in == p && (*in == '\r' || *in == '\t'))
			in++; /* the original looped forever on these */
	}
	*out = '\0';
	return (int)(in - src);

full:
	if (trunc != NULL)
		*trunc = 1;
	*out = '\0';
	return (int)(in - src);
}

/* 0x00029f20  Rough pixel width of a string in the 7-pixel font. */
int Text_approxWidth(int unused, char *s)
{
	(void)unused;
	return (int)strlen(s) * 7 + 2;
}
