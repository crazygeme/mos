#ifndef _LIB_CYCLEBUF_H
#define _LIB_CYCLEBUF_H

#define EOF ((unsigned char)(-1))

typedef struct _cy_buf cy_buf;

cy_buf *cyb_create();

void cyb_destroy(cy_buf *cyb);

void cyb_putc(cy_buf *b, unsigned char c);

unsigned cyb_putbuf(cy_buf *b, unsigned char *buf, unsigned len);

/* Returns the byte value as int (0-255) on success.
 * If interruptible is non-zero, returns -1 when a signal wakes the task.
 * 255 == (int)(unsigned char)EOF means writer-closed / no more data. */
int cyb_getc(cy_buf *b, int interruptible);

int cyb_isempty(cy_buf *b);

int cyb_isfull(cy_buf *b);

int cyb_get_buf_len(cy_buf *b);

int cyb_writer_count(cy_buf *b);

int cyb_reader_count(cy_buf *b);

/* Read 1..len bytes into buf, blocking until at least one byte is available.
 * Returns the number of bytes read, or 0 on EOF (no writers remain).
 * If interruptible is non-zero, returns -1 (EINTR) when a signal wakes the
 * task before any bytes are read; returns bytes read so far if some were. */
int cyb_getbuf(cy_buf *b, void *buf, int len, int interruptible);

void cyb_flush(cy_buf *b);
void cyb_writer_close(cy_buf *b);

void cyb_reader_close(cy_buf *b);

#endif
