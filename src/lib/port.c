#include <port.h>

unsigned char port_read_byte(unsigned short port)
{
	/* Reads and returns 8 bits from PORT. */
	unsigned char data;
	/* See [IA32-v2a] "IN". */
	asm volatile("inb %w1, %b0" : "=a"(data) : "Nd"(port));
	return data;
}

void port_read_bytes(unsigned short port, void *addr, unsigned cnt)
{
	/* See [IA32-v2a] "INS". */
	asm volatile("rep insb" : "+D"(addr), "+c"(cnt) : "d"(port) : "memory");
}

unsigned short port_read_word(unsigned short port)
{
	unsigned short data;
	/* See [IA32-v2a] "IN". */
	asm volatile("inw %w1, %w0" : "=a"(data) : "Nd"(port));
	return data;
}

void port_read_words(unsigned short port, void *addr, unsigned cnt)
{
	/* See [IA32-v2a] "INS". */
	asm volatile("rep insw" : "+D"(addr), "+c"(cnt) : "d"(port) : "memory");
}

unsigned int port_read_dword(unsigned short port)
{
	/* See [IA32-v2a] "IN". */
	unsigned int data;
	asm volatile("inl %w1, %0" : "=a"(data) : "Nd"(port));
	return data;
}

void port_read_dwords(unsigned short port, void *addr, unsigned cnt)
{
	/* See [IA32-v2a] "INS". */
	asm volatile("rep insl" : "+D"(addr), "+c"(cnt) : "d"(port) : "memory");
}

void port_write_byte(unsigned short port, unsigned char data)
{
	/* See [IA32-v2b] "OUT". */
	asm volatile("outb %b0, %w1" : : "a"(data), "Nd"(port));
}

void port_write_bytes(unsigned short port, const void *addr, unsigned cnt)
{
	/* See [IA32-v2b] "OUTS". */
	asm volatile("rep outsb" : "+S"(addr), "+c"(cnt) : "d"(port));
}

void port_write_word(unsigned short port, unsigned short data)
{
	/* See [IA32-v2b] "OUT". */
	asm volatile("outw %w0, %w1" : : "a"(data), "Nd"(port));
}

void port_write_words(unsigned short port, const void *addr, unsigned cnt)
{
	/* See [IA32-v2b] "OUTS". */
	asm volatile("rep outsw" : "+S"(addr), "+c"(cnt) : "d"(port));
}

void port_write_dword(unsigned short port, unsigned int data)
{
	/* See [IA32-v2b] "OUT". */
	asm volatile("outl %0, %w1" : : "a"(data), "Nd"(port));
}

void port_write_dwords(unsigned short port, const void *addr, unsigned cnt)
{
	/* See [IA32-v2b] "OUTS". */
	asm volatile("rep outsl" : "+S"(addr), "+c"(cnt) : "d"(port));
}
