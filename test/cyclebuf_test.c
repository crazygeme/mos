/*
 * test/cyclebuf_test.c — unit tests for lib/cyclebuf.c
 *
 * Covers: create/destroy, putc/getc, putbuf/getbuf, isempty/isfull,
 *         buf_len, writer/reader counts, flush, overflow (putc drops
 *         oldest), putbuf stops at full, EOF on closed writer.
 *
 * Note: cyb_getc / cyb_getbuf block when empty.  All tests that call
 * them pre-fill the buffer so no blocking occurs.
 */

#include <lib/cyclebuf.h>
#include <lib/klib.h>
#include <config.h>
#include <test/test.h>

/* ── create / destroy ────────────────────────────────────────────── */

KTEST(cyclebuf, create_destroy)
{
	cy_buf *b = cyb_create();
	ASSERT_NONNULL(b);
	EXPECT_TRUE(cyb_isempty(b));
	EXPECT_EQ(cyb_get_buf_len(b), 0);
	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── putc / getc ─────────────────────────────────────────────────── */

KTEST(cyclebuf, putc_getc)
{
	cy_buf *b = cyb_create();

	cyb_putc(b, 'A');
	EXPECT_FALSE(cyb_isempty(b));
	EXPECT_EQ(cyb_get_buf_len(b), 1);

	unsigned char c = (unsigned char)cyb_getc(b, 0);
	EXPECT_EQ(c, (unsigned char)'A');
	EXPECT_TRUE(cyb_isempty(b));

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

KTEST(cyclebuf, putc_getc_order)
{
	cy_buf *b = cyb_create();
	int i;

	for (i = 0; i < 5; i++)
		cyb_putc(b, (unsigned char)('a' + i));

	EXPECT_EQ(cyb_get_buf_len(b), 5);

	for (i = 0; i < 5; i++) {
		unsigned char c = cyb_getc(b, 0);
		EXPECT_EQ(c, (unsigned char)('a' + i));
	}

	EXPECT_TRUE(cyb_isempty(b));
	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── putbuf / getbuf ─────────────────────────────────────────────── */

KTEST(cyclebuf, putbuf_getbuf)
{
	cy_buf *b = cyb_create();
	unsigned char src[] = "hello";
	unsigned char dst[8] = { 0 };

	unsigned written = cyb_putbuf(b, src, 5);
	EXPECT_EQ((int)written, 5);
	EXPECT_EQ(cyb_get_buf_len(b), 5);

	int n = cyb_getbuf(b, dst, 5, 0);
	EXPECT_EQ(n, 5);
	EXPECT_EQ(memcmp(dst, src, 5), 0);
	EXPECT_TRUE(cyb_isempty(b));

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

KTEST(cyclebuf, putbuf_zero_len)
{
	cy_buf *b = cyb_create();
	unsigned char buf[4] = { 1, 2, 3, 4 };

	unsigned written = cyb_putbuf(b, buf, 0);
	EXPECT_EQ((int)written, 0);
	EXPECT_TRUE(cyb_isempty(b));

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── isempty / isfull ────────────────────────────────────────────── */

KTEST(cyclebuf, isempty_after_create)
{
	cy_buf *b = cyb_create();
	EXPECT_TRUE(cyb_isempty(b));
	EXPECT_FALSE(cyb_isfull(b));
	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

KTEST(cyclebuf, isfull)
{
	cy_buf *b = cyb_create();
	int i;

	for (i = 0; i < PIPE_BUF_LEN; i++)
		cyb_putc(b, (unsigned char)(i & 0xff));

	EXPECT_TRUE(cyb_isfull(b));
	EXPECT_EQ(cyb_get_buf_len(b), PIPE_BUF_LEN);

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── buf_len ─────────────────────────────────────────────────────── */

KTEST(cyclebuf, buf_len_tracking)
{
	cy_buf *b = cyb_create();

	EXPECT_EQ(cyb_get_buf_len(b), 0);
	cyb_putc(b, 'x');
	EXPECT_EQ(cyb_get_buf_len(b), 1);
	cyb_putc(b, 'y');
	EXPECT_EQ(cyb_get_buf_len(b), 2);
	cyb_getc(b, 0);
	EXPECT_EQ(cyb_get_buf_len(b), 1);
	cyb_getc(b, 0);
	EXPECT_EQ(cyb_get_buf_len(b), 0);

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── writer / reader counts ──────────────────────────────────────── */

KTEST(cyclebuf, initial_counts)
{
	cy_buf *b = cyb_create();
	EXPECT_EQ(cyb_writer_count(b), 1);
	EXPECT_EQ(cyb_reader_count(b), 1);
	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── flush ───────────────────────────────────────────────────────── */

KTEST(cyclebuf, flush)
{
	cy_buf *b = cyb_create();
	int i;

	for (i = 0; i < 8; i++)
		cyb_putc(b, (unsigned char)i);

	EXPECT_EQ(cyb_get_buf_len(b), 8);
	cyb_flush(b);
	EXPECT_TRUE(cyb_isempty(b));
	EXPECT_EQ(cyb_get_buf_len(b), 0);

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── overflow: putc drops oldest ─────────────────────────────────── */

KTEST(cyclebuf, putc_overflow_drops_oldest)
{
	cy_buf *b = cyb_create();
	int i;

	/* Fill to capacity with bytes 0x00..0xff repeating */
	for (i = 0; i < PIPE_BUF_LEN; i++)
		cyb_putc(b, (unsigned char)(i & 0xff));

	/* One more write — oldest byte (value 0) is dropped */
	cyb_putc(b, 0xAB);

	EXPECT_TRUE(cyb_isfull(b));

	/* First byte is now 1 (0 was evicted) */
	unsigned char first = cyb_getc(b, 0);
	EXPECT_EQ(first, 1);

	/* Drain remaining PIPE_BUF_LEN - 2 bytes */
	for (i = 0; i < PIPE_BUF_LEN - 2; i++)
		cyb_getc(b, 0);

	/* Last byte is 0xAB */
	unsigned char last = cyb_getc(b, 0);
	EXPECT_EQ(last, 0xAB);

	EXPECT_TRUE(cyb_isempty(b));
	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── putbuf stops when full ──────────────────────────────────────── */

KTEST(cyclebuf, putbuf_stops_at_full)
{
	cy_buf *b = cyb_create();
	unsigned char chunk[64];
	int i;
	unsigned total = 0, written;

	for (i = 0; i < (int)sizeof(chunk); i++)
		chunk[i] = (unsigned char)i;

	/* Fill to capacity in 64-byte chunks */
	while (!cyb_isfull(b)) {
		written = cyb_putbuf(b, chunk, sizeof(chunk));
		total += written;
	}
	EXPECT_EQ((int)total, PIPE_BUF_LEN);

	/* Another write on a full buffer must return 0 */
	written = cyb_putbuf(b, chunk, sizeof(chunk));
	EXPECT_EQ((int)written, 0);

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}

/* ── EOF when writer closed and buffer empty ─────────────────────── */

KTEST(cyclebuf, getc_eof_after_writer_close)
{
	cy_buf *b = cyb_create();

	/* Close writer side; buffer is empty → getc must return EOF */
	cyb_writer_close(b); /* decrements writer_count to 0 */
	unsigned char c = cyb_getc(b, 0);
	EXPECT_EQ(c, EOF);

	cyb_reader_close(b);
	return 0;
}

KTEST(cyclebuf, getbuf_eof_after_writer_close)
{
	cy_buf *b = cyb_create();
	unsigned char dst[8];

	cyb_writer_close(b);
	int n = cyb_getbuf(b, dst, sizeof(dst), 0);
	EXPECT_EQ(n, 0); /* 0 = EOF */

	cyb_reader_close(b);
	return 0;
}

/* ── getbuf reads partial data ───────────────────────────────────── */

KTEST(cyclebuf, getbuf_partial_read)
{
	cy_buf *b = cyb_create();
	unsigned char src[] = { 10, 20, 30, 40, 50 };
	unsigned char dst[8] = { 0 };

	cyb_putbuf(b, src, 5);

	/* Request more than available — should get only what is there */
	int n = cyb_getbuf(b, dst, 8, 0);
	EXPECT_EQ(n, 5);
	EXPECT_EQ(memcmp(dst, src, 5), 0);
	EXPECT_TRUE(cyb_isempty(b));

	cyb_writer_close(b);
	cyb_reader_close(b);
	return 0;
}
