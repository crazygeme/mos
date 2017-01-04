#ifndef _CYCLEBUF_H_
#define _CYCLEBUF_H_

#define EOF ((unsigned char)(-1))

typedef struct _cy_buf cy_buf;

cy_buf* cyb_create(char* name);

void cyb_destroy(cy_buf* cyb);

void cyb_putc(cy_buf* b, unsigned char c);

void cyb_putbuf(cy_buf* b, unsigned char* buf, unsigned len);

unsigned char cyb_getc(cy_buf* b);

int cyb_isempty(cy_buf* b);

int cyb_isfull(cy_buf* b);

int cyb_get_buf_len(cy_buf* b);

int cyb_writer_count(cy_buf* b);

int cyb_reader_count(cy_buf* b);

int cyb_writer_close(cy_buf* b);

int cyb_reader_close(cy_buf* b);

#endif
