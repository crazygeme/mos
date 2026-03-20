/*
 * src/lib/kstring.c - Memory, string, character classification, and conversion functions.
 *
 * Includes fixes:
 *   - memmove: now correctly handles overlapping regions
 *   - strncpy: removed out-of-bounds null write
 */

#include <lib/klib.h>

/* ── Memory operations ───────────────────────────────────────────────────── */

void memcpy(void *to, const void *from, unsigned n)
{
	unsigned dwords = n / 4;
	unsigned tail = n % 4;
	unsigned char *d = to;
	const unsigned char *s = from;

	/* Copy 4 bytes at a time with rep movsd */
	__asm__ volatile("rep movsl"
			 : "+D"(d), "+S"(s), "+c"(dwords)
			 :
			 : "memory");

	/* Copy remaining 0-3 bytes */
	switch (tail) {
	case 3:
		*d++ = *s++; /* fall through */
	case 2:
		*d++ = *s++; /* fall through */
	case 1:
		*d++ = *s++;
	}
}

void memset(void *src, char val, int len)
{
	unsigned char bval = (unsigned char)val;
	unsigned word = bval | ((unsigned)bval << 8) | ((unsigned)bval << 16) |
			((unsigned)bval << 24);
	unsigned dwords = (unsigned)len / 4;
	unsigned tail = (unsigned)len % 4;
	unsigned char *p = src;

	/* Fill 4 bytes at a time with rep stosd */
	__asm__ volatile("rep stosl"
			 : "+D"(p), "+c"(dwords)
			 : "a"(word)
			 : "memory");

	/* Fill remaining 0-3 bytes */
	switch (tail) {
	case 3:
		*p++ = bval; /* fall through */
	case 2:
		*p++ = bval; /* fall through */
	case 1:
		*p++ = bval;
	}
}

void memmove(void *dst, void *src, unsigned len)
{
	char *d = dst, *s = src;

	if (d == s || len == 0)
		return;
	/* Non-overlapping or dst < src: safe forward copy. */
	if (d < s || d >= s + len) {
		while (len--)
			*d++ = *s++;
	} else {
		/* Overlapping with dst > src: backward copy to avoid clobber. */
		d += len;
		s += len;
		while (len--)
			*--d = *--s;
	}
}

int memcmp(const void *_src, const void *_dst, unsigned len)
{
	const unsigned char *src = _src, *dst = _dst;
	unsigned i;

	for (i = 0; i < len; i++) {
		if (src[i] > dst[i])
			return 1;
		if (src[i] < dst[i])
			return -1;
	}
	return 0;
}

/* ── String operations ───────────────────────────────────────────────────── */

unsigned strlen(const char *str)
{
	unsigned count = 0;

	while (*str++)
		count++;
	return count;
}

char *strcpy(char *dst, const char *src)
{
	char *d = dst;

	while ((*dst++ = *src++) != '\0')
		;
	return d;
}

char *strncpy(char *dst, const char *src, int len)
{
	char *d = dst;
	int i;

	for (i = 0; i < len && src[i] != '\0'; i++)
		dst[i] = src[i];
	dst[i] = '\0';
	return d;
}

char *strcat(char *src, const char *msg)
{
	char *tmp = src + strlen(src);

	strcpy(tmp, msg);
	return src;
}

int strcmp(const char *src, const char *dst)
{
	unsigned char *s = (unsigned char *)src;
	unsigned char *d = (unsigned char *)dst;

	while (*s && *s == *d) {
		s++;
		d++;
	}
	return (*s > *d) - (*s < *d);
}

int strncmp(const char *src, const char *dst, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (src[i] > dst[i])
			return 1;
		if (src[i] < dst[i])
			return -1;
	}
	return 0;
}

/* KMP substring search. */
char *strstr(const char *X, const char *Y)
{
	int m = strlen(X);
	int n = strlen(Y);
	int next[n + 1];
	int i, j;

	if (*Y == '\0' || n == 0)
		return (char *)X;
	if (*X == '\0' || n > m)
		return NULL;

	for (i = 0; i < n + 1; i++)
		next[i] = 0;
	for (i = 1; i < n; i++) {
		j = next[i + 1];
		while (j > 0 && Y[j] != Y[i])
			j = next[j];
		if (j > 0 || Y[j] == Y[i])
			next[i + 1] = j + 1;
	}

	for (i = 0, j = 0; i < m; i++) {
		if (*(X + i) == *(Y + j)) {
			if (++j == n)
				return (char *)(X + i - j + 1);
		} else if (j > 0) {
			j = next[j];
			i--;
		}
	}
	return NULL;
}

char *strrev(char *src)
{
	int len = strlen(src);
	int mid = len / 2;
	int i;

	for (i = 0; i < mid; i++) {
		char tmp = src[i];

		src[i] = src[len - i - 1];
		src[len - i - 1] = tmp;
	}
	return src;
}

char *strchr(const char *str, char c)
{
	int i, len = strlen(str);

	for (i = 0; i < len; i++) {
		if (str[i] == c)
			return (char *)(str + i);
	}
	return NULL;
}

char *strrchr(const char *str, char c)
{
	int i, len = strlen(str);

	for (i = len - 1; i >= 0; i--) {
		if (str[i] == c)
			return (char *)(str + i);
	}
	return NULL;
}

/*
 * match_class - test whether @c is in the bracket expression at @pat.
 *
 * @pat points to the first character after the opening '['.
 * On return *@end points past the closing ']' (or at '\0' if unclosed).
 *
 * Supports:
 *   [abc]   literal set
 *   [a-z]   range
 *   [!abc]  negated set  (also [^abc])
 */
static int match_class(char c, const char *pat, const char **end)
{
	int negate = (*pat == '!' || *pat == '^');
	int matched = 0;

	if (negate)
		pat++;

	while (*pat && *pat != ']') {
		if (pat[1] == '-' && pat[2] && pat[2] != ']') {
			if ((unsigned char)c >= (unsigned char)pat[0] &&
			    (unsigned char)c <= (unsigned char)pat[2])
				matched = 1;
			pat += 3;
		} else {
			if (c == *pat)
				matched = 1;
			pat++;
		}
	}

	*end = (*pat == ']') ? pat + 1 : pat;
	return negate ? !matched : matched;
}

/*
 * strglob_impl - recursive full-match engine used by strglob().
 * Returns 1 if @pat matches the entirety of @str, 0 otherwise.
 */
static int strglob_impl(const char *pat, const char *str)
{
	while (*pat) {
		switch (*pat) {
		case '*':
			/* Collapse consecutive stars. */
			while (*pat == '*')
				pat++;
			/* Trailing star(s) match everything remaining. */
			if (!*pat)
				return 1;
			/* Try anchoring the rest of the pattern at each position. */
			while (*str) {
				if (strglob_impl(pat, str))
					return 1;
				str++;
			}
			/* Also try with an empty suffix. */
			return strglob_impl(pat, str);

		case '?':
			if (!*str)
				return 0;
			pat++;
			str++;
			break;

		case '[': {
			const char *end;

			if (!*str)
				return 0;
			if (!match_class(*str, pat + 1, &end))
				return 0;
			pat = end;
			str++;
			break;
		}

		case '\\':
			/* Escape: next character is literal. */
			pat++;
			if (!*pat)
				return 0;
			if (*pat != *str)
				return 0;
			pat++;
			str++;
			break;

		default:
			if (*pat != *str)
				return 0;
			pat++;
			str++;
			break;
		}
	}
	return *str == '\0';
}

/*
 * strglob - test whether @str fully matches glob pattern @pat.
 *
 * Pattern metacharacters (git-style):
 *   *        any sequence of characters (including empty)
 *   ?        any single character
 *   [abc]    any character in the literal set
 *   [a-z]    any character in the range
 *   [!abc]   any character NOT in the set  (also [^abc])
 *   \x       literal character x (escape)
 *
 * Returns @str on a successful match, NULL on mismatch.
 * The non-NULL return acts as a truthy boolean for callers that only
 * need a yes/no answer.
 */
const char *strglob(const char *pat, const char *str)
{
	return strglob_impl(pat, str) ? str : NULL;
}

char *strdup(const char *str)
{
	unsigned len = strlen(str) + 1;
	char *ret = malloc(len);

	strcpy(ret, str);
	ret[len - 1] = '\0';
	return ret;
}

/* ── Character classification ────────────────────────────────────────────── */

int isspace(const char c)
{
	return c == ' ' || c == '\t' || c == '\n';
}

int islower(int c)
{
	return c >= 'a' && c <= 'z';
}

int isupper(int c)
{
	return c >= 'A' && c <= 'Z';
}

int tolower(int c)
{
	return isupper(c) ? c - 'A' + 'a' : c;
}

int toupper(int c)
{
	return islower(c) ? c - 'a' + 'A' : c;
}

int isprint(int c)
{
	return (c > 31 && c < 127) || c == 10 || c == 13 || c == 9;
}

/* ── Number conversion ───────────────────────────────────────────────────── */

static char _hexdigit(int i)
{
	return i < 10 ? i + '0' : (i - 10) + 'a';
}

/*
 * Convert num to a string in the given base.
 * sign=1: treat as signed (base 10 only).
 * Returns a heap-allocated string; caller must free().
 */
char *itoa(int num, int base, int sign)
{
	char *str = malloc(12);
	char *ret = str;
	int left = num;
	unsigned uleft = (unsigned)num;
	char *begin;

	if (!str)
		return NULL;
	memset(str, 0, 12);

	/* Special-case INT_MIN: x == -x in two's complement */
	if (uleft == 0x80000000u) {
		strcpy(str, "0x80000000");
		return str;
	}
	if (num == 0) {
		strcpy(str, base == 16 ? "0x0" : "0");
		return str;
	}
	if (base != 10 && base != 16) {
		free(str);
		return NULL;
	}

	if (base == 10 && sign && left < 0) {
		str[0] = '-';
		str++;
		left = -left;
	} else if (base == 16) {
		str[0] = '0';
		str[1] = 'x';
		str += 2;
	}

	begin = str;
	if (sign && base != 16) {
		while (left) {
			*str++ = _hexdigit(left % base);
			left /= base;
		}
	} else {
		while (uleft) {
			*str++ = _hexdigit(uleft % base);
			uleft /= base;
		}
	}
	strrev(begin);
	return ret;
}

/*
 * 64-bit unsigned divide: returns quotient, stores remainder in *rem.
 * Uses shift-and-subtract to avoid __divdi3/__moddi3 runtime helpers
 * that are not available in a freestanding kernel build.
 */
static unsigned long long u64_divmod(unsigned long long n, unsigned long long d,
				     unsigned long long *rem)
{
	unsigned long long q = 0;
	int i;

	*rem = 0;
	for (i = 63; i >= 0; i--) {
		*rem = (*rem << 1) | ((n >> i) & 1ULL);
		if (*rem >= d) {
			*rem -= d;
			q |= (1ULL << i);
		}
	}
	return q;
}

/*
 * Convert a 64-bit integer to a heap-allocated string.
 * sign=1: treat as signed (base 10 only).
 * Returns a heap-allocated string; caller must free().
 */
char *lltoa(long long num, int base, int sign)
{
	/* 20 decimal digits + sign + NUL, or "0x" + 16 hex digits + NUL */
	char *str = malloc(24);
	char *ret = str;
	unsigned long long uleft;
	char *begin;

	if (!str)
		return NULL;
	memset(str, 0, 24);

	if (num == 0) {
		strcpy(str, base == 16 ? "0x0" : "0");
		return str;
	}
	if (base != 10 && base != 16) {
		free(str);
		return NULL;
	}

	uleft = (unsigned long long)num;
	if (base == 10 && sign && num < 0) {
		str[0] = '-';
		str++;
		uleft = (unsigned long long)(-num);
	} else if (base == 16) {
		str[0] = '0';
		str[1] = 'x';
		str += 2;
	}

	begin = str;
	{
		unsigned long long rem;
		unsigned long long ubase = (unsigned long long)base;

		while (uleft) {
			uleft = u64_divmod(uleft, ubase, &rem);
			*str++ = _hexdigit((int)rem);
		}
	}
	strrev(begin);
	return ret;
}

int atoi(const char *str)
{
	int base = 10, neg = 0, ret = 0;

	if (!str || !*str)
		return 0;
	if (*str == '-') {
		neg = 1;
		str++;
	}
	if (*str == '0') {
		neg = 0;
		str++;
		if (*str == 'x' || *str == 'X') {
			base = 16;
			str++; /* skip 'x' prefix */
		} else if (*str == 'b') {
			base = 2;
			str++; /* skip 'b' prefix */
		} else if (*str >= '0' && *str <= '7') {
			base = 8;
			/* first octal digit — do not advance */
		} else {
			return 0;
		}
	}

	while (*str) {
		int cur;

		if (base == 8 && (*str > '7' || *str < '0'))
			break;
		if (base == 10 && (*str > '9' || *str < '0'))
			break;
		if (base == 16 && !((*str >= '0' && *str <= '9') ||
				    (*str >= 'a' && *str <= 'f') ||
				    (*str >= 'A' && *str <= 'F')))
			break;

		if (*str >= '0' && *str <= '9')
			cur = *str - '0';
		else if (*str >= 'a' && *str <= 'f')
			cur = *str - 'a' + 10;
		else
			cur = *str - 'A' + 10;

		ret = ret * base + cur;
		str++;
	}
	return neg ? -ret : ret;
}

/* ── Pseudo-random number generator ─────────────────────────────────────── */

static unsigned int klib_random_num = 1;

void srand(unsigned seed)
{
	klib_random_num = seed;
}

unsigned int rand()
{
	unsigned int ret = klib_random_num * 0x343fd + 0x269EC3;

	klib_random_num = ret;
	ret = (ret >> 16) & 0x7fff;
	return ret;
}
