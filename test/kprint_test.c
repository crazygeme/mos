/*
 * test/kprint_test.c — unit tests for sprintf / vsprintf (src/lib/kprint.c)
 *
 * Covers: %d, %u, %x/%p, %s, %c, %b, %%, field width, zero-pad, left-align,
 *         negative numbers, NULL string, vsprintf return value,
 *         %h (human-readable size), %lld/%llu (64-bit integers).
 */

#include <lib/klib.h>
#include <test/test.h>

/* ── %d ──────────────────────────────────────────────────────────────────── */

KTEST(kprint, sprintf_d_zero)
{
	char buf[32];
	sprintf(buf, "%d", 0);
	EXPECT_EQ(strcmp(buf, "0"), 0);
	return 0;
}

KTEST(kprint, sprintf_d_positive)
{
	char buf[32];
	sprintf(buf, "%d", 42);
	EXPECT_EQ(strcmp(buf, "42"), 0);
	return 0;
}

KTEST(kprint, sprintf_d_negative)
{
	char buf[32];
	sprintf(buf, "%d", -7);
	EXPECT_EQ(strcmp(buf, "-7"), 0);
	return 0;
}

/* ── %u ──────────────────────────────────────────────────────────────────── */

KTEST(kprint, sprintf_u_basic)
{
	char buf[32];
	sprintf(buf, "%u", 255u);
	EXPECT_EQ(strcmp(buf, "255"), 0);
	return 0;
}

/* ── %x / %p ─────────────────────────────────────────────────────────────── */

KTEST(kprint, sprintf_x_basic)
{
	char buf[32];
	sprintf(buf, "%x", 0xdead);
	EXPECT_EQ(strcmp(buf, "dead"), 0);
	return 0;
}

KTEST(kprint, sprintf_x_zero)
{
	char buf[32];
	sprintf(buf, "%x", 0u);
	EXPECT_EQ(strcmp(buf, "0"), 0);
	return 0;
}

KTEST(kprint, sprintf_p_same_as_x)
{
	char xbuf[32], pbuf[32];
	sprintf(xbuf, "%x", 0xabcdu);
	sprintf(pbuf, "%p", 0xabcdu);
	EXPECT_EQ(strcmp(xbuf, pbuf), 0);
	return 0;
}

/* ── %s ──────────────────────────────────────────────────────────────────── */

KTEST(kprint, sprintf_s_basic)
{
	char buf[32];
	sprintf(buf, "%s", "hello");
	EXPECT_EQ(strcmp(buf, "hello"), 0);
	return 0;
}

KTEST(kprint, sprintf_s_null)
{
	char buf[32];
	sprintf(buf, "%s", (char *)0);
	EXPECT_EQ(strcmp(buf, "(null)"), 0);
	return 0;
}

/* ── %c ──────────────────────────────────────────────────────────────────── */

KTEST(kprint, sprintf_c_basic)
{
	char buf[8];
	sprintf(buf, "%c", 'A');
	EXPECT_EQ(buf[0], 'A');
	EXPECT_EQ(buf[1], '\0');
	return 0;
}

/* ── %b ──────────────────────────────────────────────────────────────────── */

KTEST(kprint, sprintf_b_single_nibble)
{
	char buf[8];
	sprintf(buf, "%b", 0x0fu);
	EXPECT_EQ(strcmp(buf, "0f"), 0);
	return 0;
}

KTEST(kprint, sprintf_b_two_nibbles)
{
	char buf[8];
	sprintf(buf, "%b", 0xabu);
	EXPECT_EQ(strcmp(buf, "ab"), 0);
	return 0;
}

/* ── %% ──────────────────────────────────────────────────────────────────── */

KTEST(kprint, sprintf_percent_literal)
{
	char buf[8];
	sprintf(buf, "100%%");
	EXPECT_EQ(strcmp(buf, "100%"), 0);
	return 0;
}

/* ── field width ─────────────────────────────────────────────────────────── */

KTEST(kprint, sprintf_width_right_align)
{
	char buf[16];
	sprintf(buf, "%5d", 42);
	EXPECT_EQ(strcmp(buf, "   42"), 0);
	return 0;
}

KTEST(kprint, sprintf_width_exact_fit)
{
	char buf[16];
	sprintf(buf, "%2d", 42);
	EXPECT_EQ(strcmp(buf, "42"), 0);
	return 0;
}

KTEST(kprint, sprintf_width_overflow)
{
	char buf[16];
	sprintf(buf, "%2d", 1234);
	EXPECT_EQ(strcmp(buf, "1234"), 0);
	return 0;
}

/* ── zero-pad ────────────────────────────────────────────────────────────── */

KTEST(kprint, sprintf_zero_pad_positive)
{
	char buf[16];
	sprintf(buf, "%05d", 42);
	EXPECT_EQ(strcmp(buf, "00042"), 0);
	return 0;
}

KTEST(kprint, sprintf_zero_pad_negative)
{
	char buf[16];
	sprintf(buf, "%05d", -7);
	EXPECT_EQ(strcmp(buf, "-0007"), 0);
	return 0;
}

KTEST(kprint, sprintf_zero_pad_unsigned)
{
	char buf[16];
	sprintf(buf, "%04u", 9u);
	EXPECT_EQ(strcmp(buf, "0009"), 0);
	return 0;
}

/* ── left-align ──────────────────────────────────────────────────────────── */

KTEST(kprint, sprintf_left_align_d)
{
	char buf[16];
	sprintf(buf, "%-5d|", 42);
	EXPECT_EQ(strcmp(buf, "42   |"), 0);
	return 0;
}

KTEST(kprint, sprintf_left_align_s)
{
	char buf[16];
	sprintf(buf, "%-6s|", "hi");
	EXPECT_EQ(strcmp(buf, "hi    |"), 0);
	return 0;
}

KTEST(kprint, sprintf_left_overrides_zero)
{
	/* -0 combination: left-align wins, no zero-padding */
	char buf[16];
	sprintf(buf, "%-05d|", 3);
	EXPECT_EQ(strcmp(buf, "3    |"), 0);
	return 0;
}

/* ── width on %s ─────────────────────────────────────────────────────────── */

KTEST(kprint, sprintf_width_s_right)
{
	char buf[16];
	sprintf(buf, "%8s", "hi");
	EXPECT_EQ(strcmp(buf, "      hi"), 0);
	return 0;
}

/* ── width on %c ─────────────────────────────────────────────────────────── */

KTEST(kprint, sprintf_width_c_right)
{
	char buf[16];
	sprintf(buf, "%3c", 'X');
	EXPECT_EQ(strcmp(buf, "  X"), 0);
	return 0;
}

KTEST(kprint, sprintf_width_c_left)
{
	char buf[16];
	sprintf(buf, "%-3c|", 'X');
	EXPECT_EQ(strcmp(buf, "X  |"), 0);
	return 0;
}

/* ── mixed format string ─────────────────────────────────────────────────── */

KTEST(kprint, sprintf_mixed)
{
	char buf[64];
	sprintf(buf, "val=%d str=%s hex=%x", 10, "ok", 0xff);
	EXPECT_EQ(strcmp(buf, "val=10 str=ok hex=ff"), 0);
	return 0;
}

/* ── vsprintf return value ───────────────────────────────────────────────── */

KTEST(kprint, vsprintf_return_value)
{
	char buf[32];
	int n = sprintf(buf, "hello %d", 42);
	EXPECT_EQ(n, (int)strlen(buf));
	EXPECT_EQ(strcmp(buf, "hello 42"), 0);
	return 0;
}

KTEST(kprint, vsprintf_empty_format)
{
	char buf[8];
	int n = sprintf(buf, "");
	EXPECT_EQ(n, 0);
	EXPECT_EQ(buf[0], '\0');
	return 0;
}

/* ── %h (human-readable size) ────────────────────────────────────────────── */

KTEST(kprint, sprintf_h_one)
{
	/* Special case: exactly 1 byte → "1" */
	char buf[16];
	sprintf(buf, "%h", 1u);
	EXPECT_EQ(strcmp(buf, "1.0"), 0);
	return 0;
}

KTEST(kprint, sprintf_h_zero)
{
	/* 0 bytes: below 1024, frac=0, unit="" → "0.0" */
	char buf[16];
	sprintf(buf, "%h", 0u);
	EXPECT_EQ(strcmp(buf, "0.0"), 0);
	return 0;
}

KTEST(kprint, sprintf_h_kilobytes)
{
	/* 1024 bytes → "1.0k" */
	char buf[16];
	sprintf(buf, "%h", 1024u);
	EXPECT_EQ(strcmp(buf, "1.0k"), 0);
	return 0;
}

KTEST(kprint, sprintf_h_kilobytes_frac)
{
	/* 1536 = 1024 + 512; frac = 512/100 = 5 → "1.5k" */
	char buf[16];
	sprintf(buf, "%h", 1536u);
	EXPECT_EQ(strcmp(buf, "1.5k"), 0);
	return 0;
}

KTEST(kprint, sprintf_h_megabytes)
{
	/* 1 MiB = 1024*1024 → "1.0M" */
	char buf[16];
	sprintf(buf, "%h", 1024u * 1024u);
	EXPECT_EQ(strcmp(buf, "1.0M"), 0);
	return 0;
}

KTEST(kprint, sprintf_h_gigabytes)
{
	/* 1 GiB = 1024^3 → "1.0G" */
	char buf[16];
	sprintf(buf, "%h", 1024u * 1024u * 1024u);
	EXPECT_EQ(strcmp(buf, "1.0G"), 0);
	return 0;
}

/* ── %lld (signed 64-bit) ────────────────────────────────────────────────── */

KTEST(kprint, sprintf_lld_zero)
{
	char buf[32];
	sprintf(buf, "%lld", 0LL);
	EXPECT_EQ(strcmp(buf, "0"), 0);
	return 0;
}

KTEST(kprint, sprintf_lld_positive)
{
	char buf[32];
	sprintf(buf, "%lld", 1234567890123LL);
	EXPECT_EQ(strcmp(buf, "1234567890123"), 0);
	return 0;
}

KTEST(kprint, sprintf_lld_negative)
{
	char buf[32];
	sprintf(buf, "%lld", -42LL);
	EXPECT_EQ(strcmp(buf, "-42"), 0);
	return 0;
}

KTEST(kprint, sprintf_lld_negative_large)
{
	char buf[32];
	sprintf(buf, "%lld", -1234567890123LL);
	EXPECT_EQ(strcmp(buf, "-1234567890123"), 0);
	return 0;
}

KTEST(kprint, sprintf_lld_zero_pad)
{
	char buf[16];
	sprintf(buf, "%010lld", 42LL);
	EXPECT_EQ(strcmp(buf, "0000000042"), 0);
	return 0;
}

KTEST(kprint, sprintf_lld_space_pad)
{
	char buf[16];
	sprintf(buf, "%10lld", 42LL);
	EXPECT_EQ(strcmp(buf, "        42"), 0);
	return 0;
}

KTEST(kprint, sprintf_lld_zero_pad_negative)
{
	char buf[16];
	sprintf(buf, "%06lld", -7LL);
	EXPECT_EQ(strcmp(buf, "-00007"), 0);
	return 0;
}

/* ── %llu (unsigned 64-bit) ──────────────────────────────────────────────── */

KTEST(kprint, sprintf_llu_zero)
{
	char buf[32];
	sprintf(buf, "%llu", 0ULL);
	EXPECT_EQ(strcmp(buf, "0"), 0);
	return 0;
}

KTEST(kprint, sprintf_llu_basic)
{
	char buf[32];
	sprintf(buf, "%llu", 9876543210ULL);
	EXPECT_EQ(strcmp(buf, "9876543210"), 0);
	return 0;
}

KTEST(kprint, sprintf_llu_large)
{
	/* 2^32 + 1 — exceeds 32-bit range */
	char buf[32];
	sprintf(buf, "%llu", 4294967297ULL);
	EXPECT_EQ(strcmp(buf, "4294967297"), 0);
	return 0;
}

KTEST(kprint, sprintf_llu_zero_pad)
{
	char buf[16];
	sprintf(buf, "%08llu", 5ULL);
	EXPECT_EQ(strcmp(buf, "00000005"), 0);
	return 0;
}
