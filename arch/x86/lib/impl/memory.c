#include <lib/klib.h>

void memcpy(void *to, const void *from, unsigned n)
{
	unsigned dwords = n / 4;
	unsigned tail = n % 4;
	unsigned char *d = to;
	const unsigned char *s = from;

	asm volatile("rep movsl"
		     : "+D"(d), "+S"(s), "+c"(dwords)
		     :
		     : "memory");

	while (tail--)
		*d++ = *s++;
}

void memset(void *src, char val, int len)
{
	unsigned char bval = (unsigned char)val;
	unsigned word = bval | ((unsigned)bval << 8) |
			((unsigned)bval << 16) | ((unsigned)bval << 24);
	unsigned dwords = (unsigned)len / 4;
	unsigned tail = (unsigned)len % 4;
	unsigned char *p = src;

	asm volatile("rep stosl"
		     : "+D"(p), "+c"(dwords)
		     : "a"(word)
		     : "memory");

	while (tail--)
		*p++ = bval;
}
