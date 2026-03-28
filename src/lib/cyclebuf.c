#include <lib/cyclebuf.h>
#include <lib/lock.h>
#include <lib/klib.h>
#include <config.h>
#include <macro.h>
#include <errno.h>

#define DEFAULT_BUF_PAGES (1)

typedef struct _cy_buf {
	unsigned length;
	unsigned write_idx;
	unsigned read_idx;
	cond_t read_event; /* readers wait here: fires when data is available */
	cond_t write_event; /* writers wait here: fires when space is available */
	spinlock_t lock;
	int writer_count;
	int reader_count;
	unsigned ref_count;
	char *buf;
	unsigned buf_size;
} cy_buf;

cy_buf *cyb_create(int pages)
{
	cy_buf *b = zalloc(sizeof(*b));
	if (pages == 0)
		pages = DEFAULT_BUF_PAGES;
	b->buf = vm_alloc(pages);
	b->buf_size = pages * PAGE_SIZE;
	b->reader_count = b->writer_count = 1;
	b->ref_count = 2;
	cond_init(&b->read_event, 1);
	cond_init(&b->write_event, 0); /* space available initially */
	spinlock_init(&b->lock);
	return b;
}

cy_buf *cyb_create_named(int pages)
{
	cy_buf *b = zalloc(sizeof(*b));
	if (pages == 0)
		pages = DEFAULT_BUF_PAGES;
	b->buf = vm_alloc(pages);
	b->buf_size = pages * PAGE_SIZE;
	b->reader_count = b->writer_count = 0;
	b->ref_count = 1; /* device holds one reference */
	cond_init(&b->read_event, 1);
	cond_init(&b->write_event, 0); /* space available initially */
	spinlock_init(&b->lock);
	return b;
}

void cyb_destroy(cy_buf *b)
{
	if (__sync_add_and_fetch(&b->ref_count, -1) == 0) {
		vm_free(b->buf, b->buf_size / PAGE_SIZE);
		kfree(b);
	}
}

/*
 * Write path
 */

int cyb_putbuf(cy_buf *b, unsigned char *buf, unsigned len, int blocking,
	       int interruptible)
{
	unsigned written = 0;
	do {
		unsigned i;
		int notify;
		if (!len)
			return 0;
		spinlock_lock(&b->lock);
		notify = (b->length == 0);
		for (i = 0; i < len - written; i++) {
			if (b->length == b->buf_size)
				break;
			b->buf[b->write_idx] = buf[written + i];
			b->write_idx = (b->write_idx + 1) % b->buf_size;
			b->length++;
		}
		if (b->length == b->buf_size)
			cond_reset(&b->write_event);
		spinlock_unlock(&b->lock);
		if (notify && i > 0)
			cond_notify(&b->read_event);
		written += i;
		if (!blocking || written == len)
			break;
		if (cyb_reader_count(b) == 0)
			return written > 0 ? (int)written : -EPIPE;
		if (cond_wait(&b->write_event, interruptible) < 0)
			return written > 0 ? (int)written : -EINTR;
	} while (written < len);
	return (int)written;
}

/*
 * Read path
 */

int cyb_getbuf(cy_buf *b, void *buf, int len, int blocking, int interruptible)
{
	unsigned char *dst = (unsigned char *)buf;
	int n = 0;

	/* Block until at least one byte is available or EOF */
	for (;;) {
		spinlock_lock(&b->lock);
		if (b->length > 0)
			break;
		if (__sync_add_and_fetch(&b->writer_count, 0) == 0) {
			spinlock_unlock(&b->lock);
			return 0;
		}
		if (!blocking) {
			spinlock_unlock(&b->lock);
			return 0;
		}
		spinlock_unlock(&b->lock);
		if (cond_wait(&b->read_event, interruptible) < 0)
			return -1; /* EINTR */
	}

	/* Drain up to len bytes while they are immediately available */
	int was_full = (b->length == b->buf_size);
	while (n < len && b->length > 0) {
		dst[n++] = b->buf[b->read_idx];
		b->read_idx = (b->read_idx + 1) % b->buf_size;
		b->length--;
	}
	if (b->length == 0)
		cond_reset(&b->read_event);
	spinlock_unlock(&b->lock);
	if (was_full)
		cond_notify(&b->write_event); /* wake any blocked writer */
	return n;
}

/*
 * Queries
 */

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
	int full = (b->length == b->buf_size);
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

void cyb_flush(cy_buf *b)
{
	spinlock_lock(&b->lock);
	b->read_idx = b->write_idx;
	b->length = 0;
	cond_reset(&b->read_event);
	spinlock_unlock(&b->lock);
}

/*
 * Close
 */

void cyb_writer_open(cy_buf *b)
{
	__sync_add_and_fetch(&b->writer_count, 1);
	__sync_add_and_fetch(&b->ref_count, 1);
}

void cyb_reader_open(cy_buf *b)
{
	__sync_add_and_fetch(&b->reader_count, 1);
	__sync_add_and_fetch(&b->ref_count, 1);
}

void cyb_writer_close(cy_buf *b)
{
	__sync_add_and_fetch(&b->writer_count, -1);
	cond_notify(&b->read_event);
	cyb_destroy(b);
}

void cyb_reader_close(cy_buf *b)
{
	__sync_add_and_fetch(&b->reader_count, -1);
	cond_notify(
		&b->write_event); /* wake any writer blocked on full buffer */
	cyb_destroy(b);
}
