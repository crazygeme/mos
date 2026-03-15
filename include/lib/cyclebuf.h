#ifndef _LIB_CYCLEBUF_H
#define _LIB_CYCLEBUF_H

#define EOF ((unsigned char)(-1))

typedef struct _cy_buf cy_buf;

cy_buf *cyb_create();

void cyb_destroy(cy_buf *cyb);

void cyb_putc(cy_buf *b, unsigned char c);

unsigned cyb_putbuf(cy_buf *b, unsigned char *buf, unsigned len);

unsigned char cyb_getc(cy_buf *b);

int cyb_isempty(cy_buf *b);

int cyb_isfull(cy_buf *b);

int cyb_get_buf_len(cy_buf *b);

int cyb_writer_count(cy_buf *b);

int cyb_reader_count(cy_buf *b);

/* Read 1..len bytes into buf, blocking until at least one byte is available.
 * Returns the number of bytes read, or 0 on EOF (no writers remain). */
int cyb_getbuf(cy_buf *b, void *buf, int len);

void cyb_writer_close(cy_buf *b);

void cyb_reader_close(cy_buf *b);

#endif
