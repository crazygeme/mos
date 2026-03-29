#ifndef _LIB_CYCLEBUF_H
#define _LIB_CYCLEBUF_H

#include <lib/lock.h>

typedef struct _task_struct task_struct;

#define EOF ((unsigned char)(-1))

typedef struct _cy_buf cy_buf;

cy_buf *cyb_create(int pages);

/* Create a cyclebuf for a named FIFO.  Starts with reader_count = 0 and
 * writer_count = 0.  The caller (device) holds one reference; call
 * cyb_destroy() when the device is unregistered to release it. */
cy_buf *cyb_create_named(int pages);

void cyb_destroy(cy_buf *cyb);

/* Write up to @len bytes into the buffer.
 * blocking=0: non-blocking, returns bytes actually written (may be < len).
 * blocking=1: blocks when full until all bytes are written or a signal
 *             interrupts; returns bytes written, or -EPIPE/-EINTR on error. */
int cyb_putbuf(cy_buf *b, unsigned char *buf, unsigned len, int blocking,
	       int interruptible);

int cyb_isempty(cy_buf *b);

int cyb_isfull(cy_buf *b);

int cyb_get_buf_len(cy_buf *b);

int cyb_writer_count(cy_buf *b);

int cyb_reader_count(cy_buf *b);

/* Read 1..len bytes into buf, blocking until at least one byte is available.
 * Returns the number of bytes read, or 0 on EOF (no writers remain).
 * If interruptible is non-zero, returns -1 (EINTR) when a signal wakes the
 * task before any bytes are read; returns bytes read so far if some were. */
int cyb_getbuf(cy_buf *b, void *buf, int len, int blocking, int interruptible);

void cyb_flush(cy_buf *b);

/* Increment reference counts on open; paired with cyb_writer/reader_close(). */
void cyb_writer_open(cy_buf *b);
void cyb_reader_open(cy_buf *b);

void cyb_writer_close(cy_buf *b);
void cyb_reader_close(cy_buf *b);

/*
 * Poll wakeup registration.  Call cyb_set_poll_read/write to register a
 * task_struct that will be woken (via ps_put_to_ready_queue) when data
 * arrives or space becomes available.  Call cyb_clear_poll_read/write to
 * deregister before the task exits.  Only one task per direction is supported.
 */
void cyb_set_poll_read(cy_buf *b, task_struct *task);
void cyb_clear_poll_read(cy_buf *b);
void cyb_set_poll_write(cy_buf *b, task_struct *task);
void cyb_clear_poll_write(cy_buf *b);

#endif
