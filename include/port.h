#ifndef _PORT_H_
#define _PORT_H_
/* Reads and returns 8 bit from PORT. */
unsigned char port_read_byte(unsigned short port);

/* Reads CNT bytes from PORT, one after another, and stores them
   into the buffer starting at ADDR. */
void port_read_bytes(unsigned short port, void *addr, unsigned cnt);

/* Reads and returns 16 bits from PORT. */
unsigned short port_read_word(unsigned short port);

/* Reads CNT 16-bit (halfword) units from PORT, one after
   another, and stores them into the buffer starting at ADDR. */
void port_read_words(unsigned short port, void *addr, unsigned cnt);

/* Reads and returns 32 bits from PORT. */
unsigned int port_read_dword(unsigned short port);

/* Reads CNT 32-bit (word) units from PORT, one after another,
   and stores them into the buffer starting at ADDR. */
void port_read_dwords(unsigned short port, void *addr, unsigned cnt);

/* Writes the 8-bit DATA to PORT. */
void port_write_byte(unsigned short port, unsigned char data);

/* Writes to PORT each byte of data in the CNT-byte buffer
   starting at ADDR. */
void port_write_bytes(unsigned short port, const void *addr, unsigned cnt);

/* Writes the 16-bit DATA to PORT. */
void port_write_word(unsigned short port, unsigned short data);

/* Writes to PORT each 16-bit unit (halfword) of data in the
   CNT-halfword buffer starting at ADDR. */
void port_write_words(unsigned short port, const void *addr, unsigned cnt);

/* Writes the 32-bit DATA to PORT. */
void port_write_dword(unsigned short port, unsigned int data);

/* Writes to PORT each 32-bit unit (word) of data in the CNT-word
   buffer starting at ADDR. */
void port_write_dwords(unsigned short port, const void *addr, unsigned cnt);

#endif