#include <lib/cyclebuf.h>
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
	/* poll/select wakeup: woken when read or write becomes possible */
	task_struct *poll_read_task;
	task_struct *poll_write_task;
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
	return b;
}

void cyb_destroy(cy_buf *b)
{
	if (__sync_add_and_fetch(&b->ref_count, -1) == 0) {
		vm_free(b->buf, b->buf_size / PAGE_SIZE);
		kfree(b);
	}
}

/* Notify a poll/select waiter if one is registered.  Must be called without
 * poll_lock held, and after the data state has been updated. */
static void cyb_notify_poll(cy_buf *b, int read)
{
	task_struct *t;
	int irq;

	spinlock_lock(&b->poll_lock, &irq);
	t = read ? b->poll_read_task : b->poll_write_task;
	spinlock_unlock(&b->poll_lock, irq);
	if (t)
		ps_put_to_ready_queue(t);
}

/*
 * Write path
 */

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
		for (i = 0; i < len - written; i++) {
			if (b->length == b->buf_size)
				break;
			b->buf[b->write_idx] = buf[written + i];
			b->write_idx = (b->write_idx + 1) % b->buf_size;
			b->length++;
		}
		if (b->length == b->buf_size)
			cond_reset(&b->write_event);
		spinlock_unlock(&b->lock, irq);
		if (notify && i > 0) {
			cond_notify(&b->read_event);
			cyb_notify_poll(b, 1);
		}
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
	int irq;

	/* Block until at least one byte is available or EOF */
	for (;;) {
		spinlock_lock(&b->lock, &irq);
		if (b->length > 0)
			break;
		if (__sync_add_and_fetch(&b->writer_count, 0) == 0) {
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
			if (__sync_add_and_fetch(&b->writer_count, 0) == 0)
				continue;
			return -1; /* genuine EINTR */
		}
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
	spinlock_unlock(&b->lock, irq);
	if (was_full) {
		cond_notify(&b->write_event); /* wake any blocked writer */
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
	return __sync_add_and_fetch(&b->writer_count, 0);
}

int cyb_reader_count(cy_buf *b)
{
	return __sync_add_and_fetch(&b->reader_count, 0);
}

void cyb_flush(cy_buf *b)
{
	int irq;
	spinlock_lock(&b->lock, &irq);
	b->read_idx = b->write_idx;
	b->length = 0;
	cond_reset(&b->read_event);
	spinlock_unlock(&b->lock, irq);
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

void cyb_set_poll_read(cy_buf *b, task_struct *task)
{
	int irq;
	spinlock_lock(&b->poll_lock, &irq);
	b->poll_read_task = task;
	spinlock_unlock(&b->poll_lock, irq);
}

void cyb_clear_poll_read(cy_buf *b)
{
	int irq;
	spinlock_lock(&b->poll_lock, &irq);
	b->poll_read_task = NULL;
	spinlock_unlock(&b->poll_lock, irq);
}

void cyb_set_poll_write(cy_buf *b, task_struct *task)
{
	int irq;
	spinlock_lock(&b->poll_lock, &irq);
	b->poll_write_task = task;
	spinlock_unlock(&b->poll_lock, irq);
}

void cyb_clear_poll_write(cy_buf *b)
{
	int irq;
	spinlock_lock(&b->poll_lock, &irq);
	b->poll_write_task = NULL;
	spinlock_unlock(&b->poll_lock, irq);
}
