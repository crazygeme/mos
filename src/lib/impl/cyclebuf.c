#include <lib/cyclebuf.h>
#include <fs/fs.h>
#include <lib/lock.h>
#include <lib/klib.h>
#include <ps/ps.h>
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
	/* Readiness subscriptions for task waits and persistent event queues. */
	list_entry poll_readers;
	list_entry poll_writers;
	spinlock_t poll_lock;
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
	spinlock_init(&b->poll_lock);
	list_init(&b->poll_readers);
	list_init(&b->poll_writers);
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
	spinlock_init(&b->poll_lock);
	list_init(&b->poll_readers);
	list_init(&b->poll_writers);
	return b;
}

void cyb_destroy(cy_buf *b)
{
	if (__sync_add_and_fetch(&b->ref_count, -1) == 0) {
		vm_free(b->buf, b->buf_size / PAGE_SIZE);
		kfree(b);
	}
}

/* Notify readiness subscribers after publishing the buffer state. */
static void cyb_notify_poll(cy_buf *b, int read)
{
	int irq;
	spinlock_lock(&b->poll_lock, &irq);
	poll_notify(read ? &b->poll_readers : &b->poll_writers);
	spinlock_unlock(&b->poll_lock, irq);
}

/*
 * Write path
 */

/* Copy a bounded span in at most two contiguous transfers. */
static void cyb_copy_in(cy_buf *b, const unsigned char *src, unsigned len)
{
	unsigned first = b->buf_size - b->write_idx;

	if (first > len)
		first = len;
	memcpy(b->buf + b->write_idx, src, first);
	if (first < len)
		memcpy(b->buf, src + first, len - first);
	b->write_idx += len;
	if (b->write_idx >= b->buf_size)
		b->write_idx -= b->buf_size;
	b->length += len;
}

static void cyb_copy_out(cy_buf *b, unsigned char *dst, unsigned len)
{
	unsigned first = b->buf_size - b->read_idx;

	if (first > len)
		first = len;
	memcpy(dst, b->buf + b->read_idx, first);
	if (first < len)
		memcpy(dst + first, b->buf, len - first);
	b->read_idx += len;
	if (b->read_idx >= b->buf_size)
		b->read_idx -= b->buf_size;
	b->length -= len;
}

int cyb_putbuf(cy_buf *b, unsigned char *buf, unsigned len, int blocking,
	       int interruptible)
{
	unsigned written = 0;
	int irq;

	do {
		unsigned i;
		int notify;
		if (!len)
			return 0;
		spinlock_lock(&b->lock, &irq);
		notify = (b->length == 0);
		i = b->buf_size - b->length;
		if (i > len - written)
			i = len - written;
		if (i)
			cyb_copy_in(b, buf + written, i);
		if (b->length == b->buf_size)
			cond_reset(&b->write_event);
		spinlock_unlock(&b->lock, irq);
		if (notify && i > 0)
			cond_notify(&b->read_event);
		/* New input notifies edge subscribers even while input remains. */
		if (i > 0)
			cyb_notify_poll(b, 1);
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

/* Publish complete records before waking readers. */
int cyb_put_record(cy_buf *b, const unsigned char *buf, unsigned len)
{
	int irq, notify;

	if (!len)
		return 0;
	spinlock_lock(&b->lock, &irq);
	if (len > b->buf_size - b->length) {
		spinlock_unlock(&b->lock, irq);
		return 0;
	}
	notify = b->length == 0;
	cyb_copy_in(b, buf, len);
	if (b->length == b->buf_size)
		cond_reset(&b->write_event);
	spinlock_unlock(&b->lock, irq);
	if (notify)
		cond_notify(&b->read_event);
	cyb_notify_poll(b, 1);
	return (int)len;
}

/*
 * Read path
 */

int cyb_getbuf(cy_buf *b, void *buf, int len, int blocking, int interruptible)
{
	unsigned char *dst = (unsigned char *)buf;
	int n = 0;
	int irq;

	if (len <= 0)
		return 0;

	/* Block until at least one byte is available or EOF */
	for (;;) {
		spinlock_lock(&b->lock, &irq);
		if (b->length > 0)
			break;
		if (__atomic_load_n(&b->writer_count, __ATOMIC_ACQUIRE) == 0) {
			spinlock_unlock(&b->lock, irq);
			return 0;
		}
		if (!blocking) {
			spinlock_unlock(&b->lock, irq);
			return 0;
		}
		spinlock_unlock(&b->lock, irq);
		if (cond_wait(&b->read_event, interruptible) < 0) {
			/* Interrupted by a signal. Re-check whether the writer
			 * closed while we slept (common race: SIGCHLD arrives
			 * just before writer_count is decremented to 0).
			 * If so, loop back so the EOF check at the top fires
			 * cleanly instead of returning -EINTR. */
			if (__atomic_load_n(&b->writer_count, __ATOMIC_ACQUIRE) == 0)
				continue;
			return -1; /* genuine EINTR */
		}
	}

	/* Drain up to len bytes while they are immediately available */
	int was_full = (b->length == b->buf_size);
	n = b->length < (unsigned)len ? (int)b->length : len;
	cyb_copy_out(b, dst, (unsigned)n);
	if (b->length == 0)
		cond_reset(&b->read_event);
	spinlock_unlock(&b->lock, irq);
	if (was_full) {
		cond_notify(&b->write_event);
		cyb_notify_poll(b, 0);
	}
	return n;
}

/*
 * Queries
 */

int cyb_isempty(cy_buf *b)
{
	int irq;
	spinlock_lock(&b->lock, &irq);
	int empty = (b->length == 0);
	spinlock_unlock(&b->lock, irq);
	return empty;
}

int cyb_isfull(cy_buf *b)
{
	int irq;
	spinlock_lock(&b->lock, &irq);
	int full = (b->length == b->buf_size);
	spinlock_unlock(&b->lock, irq);
	return full;
}

int cyb_get_buf_len(cy_buf *b)
{
	int irq;
	spinlock_lock(&b->lock, &irq);
	int len = (int)b->length;
	spinlock_unlock(&b->lock, irq);
	return len;
}

int cyb_writer_count(cy_buf *b)
{
	return __atomic_load_n(&b->writer_count, __ATOMIC_ACQUIRE);
}

int cyb_reader_count(cy_buf *b)
{
	return __atomic_load_n(&b->reader_count, __ATOMIC_ACQUIRE);
}

void cyb_flush(cy_buf *b)
{
	int irq, was_full;
	spinlock_lock(&b->lock, &irq);
	was_full = b->length == b->buf_size;
	b->read_idx = b->write_idx;
	b->length = 0;
	cond_reset(&b->read_event);
	spinlock_unlock(&b->lock, irq);
	if (was_full) {
		cond_notify(&b->write_event);
		cyb_notify_poll(b, 0);
	}
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
	cyb_notify_poll(b, 1); /* wake poll waiters: EOF / POLLHUP */
	cyb_destroy(b);
}

void cyb_reader_close(cy_buf *b)
{
	__sync_add_and_fetch(&b->reader_count, -1);
	cond_notify(
		&b->write_event); /* wake any writer blocked on full buffer */
	cyb_notify_poll(b, 0); /* wake poll waiters on write side */
	cyb_destroy(b);
}

/*
 * Poll registration helpers.
 */

void cyb_poll_read(cy_buf *b, poll_table *pt)
{
	poll_subscribe(pt, &b->poll_readers, &b->poll_lock);
}

void cyb_poll_write(cy_buf *b, poll_table *pt)
{
	poll_subscribe(pt, &b->poll_writers, &b->poll_lock);
}
