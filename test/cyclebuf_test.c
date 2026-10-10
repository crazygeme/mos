/*
 * test/cyclebuf_test.c — unit tests for lib/cyclebuf.c
 *
 * Covers: create/destroy, putbuf/getbuf (single and multi-byte),
 *         isempty/isfull, buf_len, writer/reader counts, flush,
 *         putbuf stops at full, EOF on closed writer.
 *
 * Note: cyb_getbuf blocks when empty.  All tests that call it pre-fill
 * the buffer so no blocking occurs.
 */

#include <lib/cyclebuf.h>
#include <lib/klib.h>
#include <config.h>
#include <fs/fs.h>
#include <int/int.h>
#include <int/dsr.h>
#include <ps/ps.h>
#include <ps/smp.h>
#include <test/test.h>

/* ── create / destroy ────────────────────────────────────────────── */

struct deferred_poll_test {
	cy_buf *buf;
	task_struct *consumer;
	int own_stack;
	int completed;
	int written;
};

static void deferred_poll_write(void *param)
{
	struct deferred_poll_test *ctx = param;
	unsigned char byte = 0x08;

	ctx->own_stack = current != ctx->consumer &&
			 current->sched->status == ps_running;
	ctx->written = cyb_putbuf(ctx->buf, &byte, 1, 0, 0);
	/* Exercise a consumer resuming before the callback finishes. */
	task_sched();
	__atomic_store_n(&ctx->completed, 1, __ATOMIC_RELEASE);
}

KTEST(cyclebuf, deferred_notify_poll)
{
	struct deferred_poll_test *ctx = kmalloc(sizeof(*ctx));
	poll_table wait;
	poll_table_entry entry;
	unsigned irq;
	int queued, needs_schedule, completed;
	unsigned long long deadline;

	ASSERT_NONNULL(ctx);
	memset(ctx, 0, sizeof(*ctx));
	ctx->buf = cyb_create(1);
	ctx->consumer = current;
	ASSERT_NONNULL(ctx->buf);
	poll_table_init(&wait, current, &entry, 1);
	cyb_poll_read(ctx->buf, &wait);

	/* Queue the producer with the consumer waiting. Data and readiness
	 * notifications must be published before the callback completes. */
	irq = int_intr_disable();
	deadline = time_deadline_ms(1000);
	ps_prepare_timed_wait(current, 1000);
	queued = dsr_add(deferred_poll_write, ctx);
	needs_schedule = dsr_needs_schedule();
	if (!queued)
		ps_put_to_ready_queue(current);
	task_sched();
	ps_finish_timed_wait(current);
	int_intr_setlevel(irq);
	poll_table_cleanup(&wait);

	/* Poll notification precedes callback completion. The producer may
	 * still be running on another CPU, or have yielded after the write. */
	while (queued && !__atomic_load_n(&ctx->completed, __ATOMIC_ACQUIRE) &&
	       time_now_ms() < deadline)
		time_wait(1);
	completed = __atomic_load_n(&ctx->completed, __ATOMIC_ACQUIRE);

	EXPECT_TRUE(queued);
	/* With local IRQs masked, only a single-CPU system guarantees the
	 * worker stays ready until the scheduling hint is sampled. */
	if (smp_cpu_count() == 1)
		EXPECT_TRUE(needs_schedule);
	EXPECT_TRUE(completed);
	if (completed) {
		EXPECT_TRUE(ctx->own_stack);
		EXPECT_EQ(ctx->written, 1);
	}
	EXPECT_EQ(cyb_get_buf_len(ctx->buf), 1);
	/* A timed-out callback may still reference the context. */
	if (!queued || completed) {
		cyb_writer_close(ctx->buf);
		cyb_reader_close(ctx->buf);
		kfree(ctx);
	}
	return 0;
}

KTEST(cyclebuf, create_destroy)
{
	cy_buf *b = cyb_create(1);
	ASSERT_NONNULL(b);
	EXPECT_TRUE(cyb_isempty(b));
	EXPECT_EQ(cyb_get_buf_len(b), 0);
	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── putbuf / getbuf — single byte ──────────────────────────────── */

KTEST(cyclebuf, putbuf_getbuf_one_byte)
{
	cy_buf *b = cyb_create(1);

	unsigned char put = 'A';
	cyb_putbuf(b, &put, 1, 0, 0);
	EXPECT_FALSE(cyb_isempty(b));
	EXPECT_EQ(cyb_get_buf_len(b), 1);

	unsigned char got = 0;
	int n = cyb_getbuf(b, &got, 1, 0, 0);
	EXPECT_EQ(n, 1);
	EXPECT_EQ(got, (unsigned char)'A');
	EXPECT_TRUE(cyb_isempty(b));

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

KTEST(cyclebuf, putbuf_getbuf_fifo_order)
{
	cy_buf *b = cyb_create(1);
	int i;

	for (i = 0; i < 5; i++) {
		unsigned char c = (unsigned char)('a' + i);
		cyb_putbuf(b, &c, 1, 0, 0);
	}

	EXPECT_EQ(cyb_get_buf_len(b), 5);

	for (i = 0; i < 5; i++) {
		unsigned char c = 0;
		int n = cyb_getbuf(b, &c, 1, 0, 0);
		EXPECT_EQ(n, 1);
		EXPECT_EQ(c, (unsigned char)('a' + i));
	}

	EXPECT_TRUE(cyb_isempty(b));
	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── putbuf / getbuf — multi-byte ────────────────────────────────── */

KTEST(cyclebuf, putbuf_getbuf)
{
	cy_buf *b = cyb_create(1);
	unsigned char src[] = "hello";
	unsigned char dst[8] = { 0 };

	int written = cyb_putbuf(b, src, 5, 0, 0);
	EXPECT_EQ(written, 5);
	EXPECT_EQ(cyb_get_buf_len(b), 5);

	int n = cyb_getbuf(b, dst, 5, 0, 0);
	EXPECT_EQ(n, 5);
	EXPECT_EQ(memcmp(dst, src, 5), 0);
	EXPECT_TRUE(cyb_isempty(b));

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

KTEST(cyclebuf, putbuf_zero_len)
{
	cy_buf *b = cyb_create(1);
	unsigned char buf[4] = { 1, 2, 3, 4 };

	int written = cyb_putbuf(b, buf, 0, 0, 0);
	EXPECT_EQ(written, 0);
	EXPECT_TRUE(cyb_isempty(b));

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── isempty / isfull ────────────────────────────────────────────── */

KTEST(cyclebuf, isempty_after_create)
{
	cy_buf *b = cyb_create(1);
	EXPECT_TRUE(cyb_isempty(b));
	EXPECT_FALSE(cyb_isfull(b));
	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

KTEST(cyclebuf, isfull)
{
	cy_buf *b = cyb_create(1);
	int i;

	for (i = 0; i < PAGE_SIZE; i++) {
		unsigned char byte = (unsigned char)(i & 0xff);
		cyb_putbuf(b, &byte, 1, 0, 0);
	}

	EXPECT_TRUE(cyb_isfull(b));
	EXPECT_EQ(cyb_get_buf_len(b), PAGE_SIZE);

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── buf_len ─────────────────────────────────────────────────────── */

KTEST(cyclebuf, buf_len_tracking)
{
	cy_buf *b = cyb_create(1);
	unsigned char ch;

	EXPECT_EQ(cyb_get_buf_len(b), 0);
	ch = 'x';
	cyb_putbuf(b, &ch, 1, 0, 0);
	EXPECT_EQ(cyb_get_buf_len(b), 1);
	ch = 'y';
	cyb_putbuf(b, &ch, 1, 0, 0);
	EXPECT_EQ(cyb_get_buf_len(b), 2);
	cyb_getbuf(b, &ch, 1, 0, 0);
	EXPECT_EQ(cyb_get_buf_len(b), 1);
	cyb_getbuf(b, &ch, 1, 0, 0);
	EXPECT_EQ(cyb_get_buf_len(b), 0);

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── writer / reader counts ──────────────────────────────────────── */

KTEST(cyclebuf, initial_counts)
{
	cy_buf *b = cyb_create(1);
	EXPECT_EQ(cyb_writer_count(b), 1);
	EXPECT_EQ(cyb_reader_count(b), 1);
	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── flush ───────────────────────────────────────────────────────── */

KTEST(cyclebuf, flush)
{
	cy_buf *b = cyb_create(1);
	int i;

	for (i = 0; i < 8; i++) {
		unsigned char byte = (unsigned char)i;
		cyb_putbuf(b, &byte, 1, 0, 0);
	}

	EXPECT_EQ(cyb_get_buf_len(b), 8);
	cyb_flush(b);
	EXPECT_TRUE(cyb_isempty(b));
	EXPECT_EQ(cyb_get_buf_len(b), 0);

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── putbuf stops when full ──────────────────────────────────────── */

KTEST(cyclebuf, putbuf_stops_at_full)
{
	cy_buf *b = cyb_create(1);
	unsigned char chunk[64];
	int i;
	int total = 0, written;

	for (i = 0; i < (int)sizeof(chunk); i++)
		chunk[i] = (unsigned char)i;

	/* Fill to capacity in 64-byte chunks */
	while (!cyb_isfull(b)) {
		written = cyb_putbuf(b, chunk, sizeof(chunk), 0, 0);
		total += written;
	}
	EXPECT_EQ(total, PAGE_SIZE);

	/* Another write on a full buffer must return 0 */
	written = cyb_putbuf(b, chunk, sizeof(chunk), 0, 0);
	EXPECT_EQ(written, 0);

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── EOF when writer closed and buffer empty ─────────────────────── */

KTEST(cyclebuf, getbuf_eof_after_writer_close)
{
	cy_buf *b = cyb_create(1);
	unsigned char dst[8];

	cyb_writer_close(b);
	int n = cyb_getbuf(b, dst, sizeof(dst), 0, 0);
	EXPECT_EQ(n, 0); /* 0 = EOF */

	cyb_reader_close(b);
	return 0;
}

/* ── getbuf reads partial data ───────────────────────────────────── */

KTEST(cyclebuf, getbuf_partial_read)
{
	cy_buf *b = cyb_create(1);
	unsigned char src[] = { 10, 20, 30, 40, 50 };
	unsigned char dst[8] = { 0 };

	cyb_putbuf(b, src, 5, 0, 0);

	/* Request more than available — should get only what is there */
	int n = cyb_getbuf(b, dst, 8, 0, 0);
	EXPECT_EQ(n, 5);
	EXPECT_EQ(memcmp(dst, src, 5), 0);
	EXPECT_TRUE(cyb_isempty(b));

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* A rejected packet must not publish a prefix; accepted packets may wrap. */
KTEST(cyclebuf, record_overflow_and_wrap)
{
	cy_buf *b = cyb_create(1);
	unsigned char *fill = kmalloc(PAGE_SIZE);
	unsigned char packet[4] = { 0x08, 0x11, 0x22, 0x00 };
	unsigned char got[4];
	unsigned i;

	ASSERT_NONNULL(b);
	ASSERT_NONNULL(fill);
	memset(fill, 0x5a, PAGE_SIZE);
	EXPECT_EQ(cyb_put_record(b, fill, PAGE_SIZE - 2), PAGE_SIZE - 2);
	EXPECT_EQ(cyb_put_record(b, packet, sizeof(packet)), 0);
	EXPECT_EQ(cyb_get_buf_len(b), PAGE_SIZE - 2);
	EXPECT_EQ(cyb_getbuf(b, fill, PAGE_SIZE, 0, 0), PAGE_SIZE - 2);
	for (i = 0; i < PAGE_SIZE - 2; i++) {
		if (fill[i] != 0x5a)
			break;
	}
	EXPECT_EQ(i, PAGE_SIZE - 2);
	EXPECT_EQ(cyb_put_record(b, packet, sizeof(packet)), sizeof(packet));
	EXPECT_EQ(cyb_getbuf(b, got, sizeof(got), 0, 0), sizeof(got));
	EXPECT_EQ(memcmp(got, packet, sizeof(packet)), 0);
	EXPECT_TRUE(cyb_isempty(b));
	kfree(fill);
	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}
