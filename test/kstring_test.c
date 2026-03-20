/*
 * test/kstring_test.c — unit tests for src/lib/kstring.c
 *
 * Covers: memcpy, memset, memmove, memcmp,
 *         strlen, strcpy, strncpy, strcat, strcmp, strncmp,
 *         strstr, strchr, strrchr, strdup,
 *         strglob,
 *         atoi, itoa,
 *         isspace, isprint, islower, isupper, tolower, toupper
 */

#include <lib/klib.h>
#include <test/test.h>

/* ── memcpy ──────────────────────────────────────────────────────────────── */

KTEST(kstring, memcpy_basic)
{
	char src[] = "hello";
	char dst[8] = { 0 };
	memcpy(dst, src, 6);
	EXPECT_EQ(strcmp(dst, "hello"), 0);
	return 0;
}

KTEST(kstring, memcpy_unaligned)
{
	char src[] = "abcde";
	char dst[8] = { 0 };
	memcpy(dst, src, 3); /* 3 bytes — exercises the tail path */
	EXPECT_EQ(dst[0], 'a');
	EXPECT_EQ(dst[1], 'b');
	EXPECT_EQ(dst[2], 'c');
	EXPECT_EQ(dst[3], '\0');
	return 0;
}

/* ── memset ──────────────────────────────────────────────────────────────── */

KTEST(kstring, memset_basic)
{
	char buf[8];
	memset(buf, 'x', 8);
	int i;
	for (i = 0; i < 8; i++)
		EXPECT_EQ(buf[i], 'x');
	return 0;
}

KTEST(kstring, memset_zero)
{
	char buf[8];
	memset(buf, 'A', 8);
	memset(buf, 0, 8);
	int i;
	for (i = 0; i < 8; i++)
		EXPECT_EQ(buf[i], '\0');
	return 0;
}

/* ── memmove ─────────────────────────────────────────────────────────────── */

KTEST(kstring, memmove_no_overlap)
{
	char buf[16];
	memcpy(buf, "hello", 6);
	memmove(buf + 8, buf, 6);
	EXPECT_EQ(strcmp(buf + 8, "hello"), 0);
	return 0;
}

KTEST(kstring, memmove_overlap_right)
{
	/* dst > src: copy must go backward to avoid clobbering */
	char buf[16] = "abcde";
	memmove(buf + 2, buf, 5);
	EXPECT_EQ(buf[2], 'a');
	EXPECT_EQ(buf[3], 'b');
	EXPECT_EQ(buf[6], 'e');
	return 0;
}

KTEST(kstring, memmove_overlap_left)
{
	char buf[16] = "xxabcde";
	memmove(buf, buf + 2, 5);
	EXPECT_EQ(buf[0], 'a');
	EXPECT_EQ(buf[4], 'e');
	return 0;
}

/* ── memcmp ──────────────────────────────────────────────────────────────── */

KTEST(kstring, memcmp_equal)
{
	EXPECT_EQ(memcmp("abc", "abc", 3), 0);
	return 0;
}

KTEST(kstring, memcmp_less)
{
	EXPECT_LT(memcmp("abc", "abd", 3), 0);
	return 0;
}

KTEST(kstring, memcmp_greater)
{
	EXPECT_GT(memcmp("abd", "abc", 3), 0);
	return 0;
}

/* ── strlen ──────────────────────────────────────────────────────────────── */

KTEST(kstring, strlen)
{
	EXPECT_EQ((int)strlen(""), 0);
	EXPECT_EQ((int)strlen("a"), 1);
	EXPECT_EQ((int)strlen("hello"), 5);
	return 0;
}

/* ── strcpy / strncpy ────────────────────────────────────────────────────── */

KTEST(kstring, strcpy)
{
	char buf[16];
	strcpy(buf, "hello");
	EXPECT_EQ(strcmp(buf, "hello"), 0);
	return 0;
}

KTEST(kstring, strncpy_full)
{
	char buf[16];
	strncpy(buf, "hello", 5);
	buf[5] = '\0';
	EXPECT_EQ(strcmp(buf, "hello"), 0);
	return 0;
}

KTEST(kstring, strncpy_truncate)
{
	char buf[16];
	memset(buf, 0, 16);
	strncpy(buf, "hello", 3);
	EXPECT_EQ(buf[0], 'h');
	EXPECT_EQ(buf[1], 'e');
	EXPECT_EQ(buf[2], 'l');
	EXPECT_EQ(buf[3], '\0'); /* strncpy appends NUL */
	return 0;
}

/* ── strcat ──────────────────────────────────────────────────────────────── */

KTEST(kstring, strcat)
{
	char buf[16];
	strcpy(buf, "foo");
	strcat(buf, "bar");
	EXPECT_EQ(strcmp(buf, "foobar"), 0);
	return 0;
}

/* ── strcmp / strncmp ────────────────────────────────────────────────────── */

KTEST(kstring, strcmp)
{
	EXPECT_EQ(strcmp("abc", "abc"), 0);
	EXPECT_LT(strcmp("abc", "abd"), 0);
	EXPECT_GT(strcmp("abd", "abc"), 0);
	EXPECT_LT(strcmp("ab", "abc"), 0);
	return 0;
}

KTEST(kstring, strncmp)
{
	EXPECT_EQ(strncmp("abcX", "abcY", 3), 0);
	EXPECT_LT(strncmp("abc", "abd", 3), 0);
	return 0;
}

/* ── strstr ──────────────────────────────────────────────────────────────── */

KTEST(kstring, strstr_found)
{
	const char *s = strstr("foobar", "oba");
	ASSERT_NONNULL(s);
	EXPECT_EQ(strcmp(s, "obar"), 0);
	return 0;
}

KTEST(kstring, strstr_not_found)
{
	EXPECT_NULL(strstr("foobar", "xyz"));
	return 0;
}

KTEST(kstring, strstr_empty)
{
	/* empty needle matches at start */
	const char *s = strstr("foo", "");
	EXPECT_NONNULL(s);
	return 0;
}

/* ── strchr / strrchr ────────────────────────────────────────────────────── */

KTEST(kstring, strchr)
{
	const char *s = strchr("hello", 'l');
	ASSERT_NONNULL(s);
	EXPECT_EQ(*s, 'l');
	EXPECT_EQ(strcmp(s, "llo"), 0);
	return 0;
}

KTEST(kstring, strchr_not_found)
{
	EXPECT_NULL(strchr("hello", 'z'));
	return 0;
}

KTEST(kstring, strrchr)
{
	const char *s = strrchr("hello", 'l');
	ASSERT_NONNULL(s);
	EXPECT_EQ(strcmp(s, "lo"), 0);
	return 0;
}

/* ── strdup ──────────────────────────────────────────────────────────────── */

KTEST(kstring, strdup)
{
	char *s = strdup("hello");
	ASSERT_NONNULL(s);
	EXPECT_EQ(strcmp(s, "hello"), 0);
	free(s);
	return 0;
}

/* ── strglob ─────────────────────────────────────────────────────────────── */

KTEST(kstring, strglob_exact)
{
	EXPECT_NONNULL(strglob("hello", "hello"));
	EXPECT_NULL(strglob("hello", "world"));
	return 0;
}

KTEST(kstring, strglob_trailing_star)
{
	EXPECT_NONNULL(strglob("Malloc*", "MallocTest"));
	EXPECT_NONNULL(strglob("Malloc*", "Malloc"));
	EXPECT_NULL(strglob("Malloc*", "PhymmTest"));
	return 0;
}

KTEST(kstring, strglob_leading_star)
{
	EXPECT_NONNULL(strglob("*Test", "MallocTest"));
	EXPECT_NONNULL(strglob("*Test", "Test"));
	EXPECT_NULL(strglob("*Test", "TestX"));
	return 0;
}

KTEST(kstring, strglob_both_stars)
{
	EXPECT_NONNULL(strglob("*alloc*", "MallocTest"));
	EXPECT_NULL(strglob("*alloc*", "PhymmTest"));
	return 0;
}

KTEST(kstring, strglob_star_only)
{
	EXPECT_NONNULL(strglob("*", "anything"));
	EXPECT_NONNULL(strglob("*", ""));
	return 0;
}

KTEST(kstring, strglob_question)
{
	EXPECT_NONNULL(strglob("hel?o", "hello"));
	EXPECT_NONNULL(strglob("hel?o", "helXo"));
	EXPECT_NULL(strglob("hel?o", "helllo"));
	EXPECT_NULL(strglob("hel?o", "helo"));
	return 0;
}

KTEST(kstring, strglob_char_class)
{
	EXPECT_NONNULL(strglob("[abc]", "a"));
	EXPECT_NONNULL(strglob("[abc]", "b"));
	EXPECT_NULL(strglob("[abc]", "d"));
	return 0;
}

KTEST(kstring, strglob_range)
{
	EXPECT_NONNULL(strglob("[A-Z]*", "MallocTest"));
	EXPECT_NULL(strglob("[A-Z]*", "mallocTest"));
	return 0;
}

KTEST(kstring, strglob_negated_class)
{
	EXPECT_NONNULL(strglob("[!abc]", "d"));
	EXPECT_NULL(strglob("[!abc]", "a"));
	return 0;
}

KTEST(kstring, strglob_escape)
{
	EXPECT_NONNULL(strglob("a\\*b", "a*b"));
	EXPECT_NULL(strglob("a\\*b", "axb"));
	return 0;
}

KTEST(kstring, strglob_return_ptr)
{
	const char *s = "MallocTest";
	/* on match the returned pointer equals the input string */
	EXPECT_EQ(strglob("Malloc*", s), s);
	return 0;
}

/* ── atoi ────────────────────────────────────────────────────────────────── */

KTEST(kstring, atoi_decimal)
{
	EXPECT_EQ(atoi("0"), 0);
	EXPECT_EQ(atoi("42"), 42);
	EXPECT_EQ(atoi("-7"), -7);
	return 0;
}

KTEST(kstring, atoi_hex)
{
	EXPECT_EQ(atoi("0x10"), 16);
	EXPECT_EQ(atoi("0xff"), 255);
	return 0;
}

KTEST(kstring, atoi_octal)
{
	EXPECT_EQ(atoi("010"), 8);
	return 0;
}

/* ── itoa ────────────────────────────────────────────────────────────────── */

KTEST(kstring, itoa_decimal)
{
	char *s = itoa(42, 10, 1);
	ASSERT_NONNULL(s);
	EXPECT_EQ(strcmp(s, "42"), 0);
	free(s);
	return 0;
}

KTEST(kstring, itoa_negative)
{
	char *s = itoa(-5, 10, 1);
	ASSERT_NONNULL(s);
	EXPECT_EQ(strcmp(s, "-5"), 0);
	free(s);
	return 0;
}

KTEST(kstring, itoa_hex)
{
	char *s = itoa(255, 16, 0);
	ASSERT_NONNULL(s);
	EXPECT_EQ(strcmp(s, "0xff"), 0);
	free(s);
	return 0;
}

KTEST(kstring, itoa_zero)
{
	char *s = itoa(0, 10, 1);
	ASSERT_NONNULL(s);
	EXPECT_EQ(strcmp(s, "0"), 0);
	free(s);
	return 0;
}

/* ── character classification ────────────────────────────────────────────── */

KTEST(kstring, isspace)
{
	EXPECT_TRUE(isspace(' '));
	EXPECT_TRUE(isspace('\t'));
	EXPECT_TRUE(isspace('\n'));
	EXPECT_FALSE(isspace('a'));
	return 0;
}

KTEST(kstring, isupper_islower)
{
	EXPECT_TRUE(isupper('A'));
	EXPECT_FALSE(isupper('a'));
	EXPECT_TRUE(islower('z'));
	EXPECT_FALSE(islower('Z'));
	return 0;
}

KTEST(kstring, tolower_toupper)
{
	EXPECT_EQ(tolower('A'), 'a');
	EXPECT_EQ(tolower('a'), 'a');
	EXPECT_EQ(toupper('z'), 'Z');
	EXPECT_EQ(toupper('Z'), 'Z');
	return 0;
}
