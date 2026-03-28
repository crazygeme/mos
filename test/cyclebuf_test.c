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
#include <test/test.h>

/* ── create / destroy ────────────────────────────────────────────── */

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
