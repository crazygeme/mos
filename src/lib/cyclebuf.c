#include <cyclebuf.h>
#include <lock.h>
#include <klib.h>
#include <config.h>
#include <ps.h>

typedef struct _cy_buf {
	unsigned length;
	unsigned write_idx;
	unsigned read_idx;
	cond_t event;
	spinlock_t lock;
	int writer_count;
	int reader_count;
	unsigned ref_count;
	char *buf;
} cy_buf;

cy_buf *cyb_create()
{
	cy_buf *b = calloc(1, sizeof(*b));
	b->buf = vm_alloc(PIPE_BUF_LEN / PAGE_SIZE);
	b->reader_count = b->writer_count = 1;
	b->ref_count = 2;
	cond_init(&b->event, 1);
	spinlock_init(&b->lock);
	return b;
}

void cyb_destroy(cy_buf *b)
{
	if (__sync_add_and_fetch(&b->ref_count, -1) == 0) {
		vm_free(b->buf, PIPE_BUF_LEN / PAGE_SIZE);
		kfree(b);
	}
}

/* -------------------------------------------------------------------------
 * Write path
 * ------------------------------------------------------------------------- */

void cyb_putc(cy_buf *b, unsigned char c)
{
	int notify;
	spinlock_lock(&b->lock);
	notify = (b->length == 0);
	if (b->length == PIPE_BUF_LEN) {
		/* Full: drop oldest byte to make room */
		b->read_idx = (b->read_idx + 1) % PIPE_BUF_LEN;
		b->length--;
	}
	b->buf[b->write_idx] = c;
	b->write_idx = (b->write_idx + 1) % PIPE_BUF_LEN;
	b->length++;
	spinlock_unlock(&b->lock);
	if (notify)
		cond_notify(&b->event);
}

void cyb_putbuf(cy_buf *b, unsigned char *buf, unsigned len)
{
	unsigned i;
	int notify;
	if (!len)
		return;
	spinlock_lock(&b->lock);
	notify = (b->length == 0);
	for (i = 0; i < len; i++) {
		if (b->length == PIPE_BUF_LEN) {
			b->read_idx = (b->read_idx + 1) % PIPE_BUF_LEN;
			b->length--;
		}
		b->buf[b->write_idx] = buf[i];
		b->write_idx = (b->write_idx + 1) % PIPE_BUF_LEN;
		b->length++;
	}
	spinlock_unlock(&b->lock);
	if (notify)
		cond_notify(&b->event);
}

/* -------------------------------------------------------------------------
 * Read path
 * ------------------------------------------------------------------------- */

unsigned char cyb_getc(cy_buf *b)
{
	unsigned char ret;
	for (;;) {
		spinlock_lock(&b->lock);
		if (b->length > 0)
			break;
		/* Buffer empty: return EOF if no writers will ever add data. */
		if (__sync_add_and_fetch(&b->writer_count, 0) == 0) {
			spinlock_unlock(&b->lock);
			return EOF;
		}
		spinlock_unlock(&b->lock);
		cond_wait(&b->event);
	}
	ret = b->buf[b->read_idx];
	b->read_idx = (b->read_idx + 1) % PIPE_BUF_LEN;
	b->length--;
	if (b->length == 0)
		cond_reset(&b->event);
	spinlock_unlock(&b->lock);
	return ret;
}

/* -------------------------------------------------------------------------
 * Queries
 * ------------------------------------------------------------------------- */

int cyb_isempty(cy_buf *b)
{
	spinlock_lock(&b->lock);
	int empty = (b->length == 0);
	spinlock_unlock(&b->lock);
	return empty;
}

int cyb_isfull(cy_buf *b)
{
	spinlock_lock(&b->lock);
	int full = (b->length == PIPE_BUF_LEN);
	spinlock_unlock(&b->lock);
	return full;
}

int cyb_get_buf_len(cy_buf *b)
{
	spinlock_lock(&b->lock);
	int len = (int)b->length;
	spinlock_unlock(&b->lock);
	return len;
}

int cyb_writer_count(cy_buf *b)
{
	return __sync_add_and_fetch(&b->writer_count, 0);
}

int cyb_reader_count(cy_buf *b)
{
	return __sync_add_and_fetch(&b->reader_count, 0);
}

/* -------------------------------------------------------------------------
 * Close
 * ------------------------------------------------------------------------- */

void cyb_writer_close(cy_buf *b)
{
	__sync_add_and_fetch(&b->writer_count, -1);
	cond_notify(&b->event);
	cyb_destroy(b);
}

void cyb_reader_close(cy_buf *b)
{
	__sync_add_and_fetch(&b->reader_count, -1);
	cyb_destroy(b);
}
