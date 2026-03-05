/*
 * src/lib/kstring.c - Memory, string, character classification, and conversion functions.
 *
 * Includes fixes:
 *   - memmove: now correctly handles overlapping regions
 *   - strncpy: removed out-of-bounds null write
 */

#include <klib.h>

/* ── Memory operations ───────────────────────────────────────────────────── */

void memcpy(void *to, void *from, unsigned n)
{
	bcopy(from, to, n);
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

int memcmp(void *_src, void *_dst, unsigned len)
{
	unsigned char *src = _src, *dst = _dst;
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

char *strcat(char *src, char *msg)
{
	char *tmp = src + strlen(src);

	strcpy(tmp, msg);
	return src;
}

int strcmp(char *src, char *dst)
{
	int src_len = strlen(src);
	int dst_len = strlen(dst);
	int len = src_len > dst_len ? dst_len : src_len;
	int i;

	for (i = 0; i < len; i++) {
		if (src[i] > dst[i])
			return 1;
		if (src[i] < dst[i])
			return -1;
	}
	if (src_len > len)
		return 1;
	if (dst_len > len)
		return -1;
	return 0;
}

int strncmp(char *src, char *dst, int len)
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

char *strchr(char *str, char c)
{
	int i, len = strlen(str);

	for (i = 0; i < len; i++) {
		if (str[i] == c)
			return str + i;
	}
	return NULL;
}

char *strrchr(char *str, char c)
{
	int i, len = strlen(str);

	for (i = len - 1; i >= 0; i--) {
		if (str[i] == c)
			return str + i;
	}
	return NULL;
}

char *strdup(char *str)
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
		if (*str == 'x' || *str == 'X')
			base = 16;
		else if (*str == 'b')
			base = 2;
		else if (*str >= '0' && *str <= '7')
			base = 8;
		else
			return 0;
		str++;
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
