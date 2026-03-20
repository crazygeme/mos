/*
 * src/test/malloc_test.c — unit tests for the kernel heap allocator.
 *
 *   echo 1 > /proc/test && cat /proc/test
 */

#include <lib/klib.h>
#include <test/test.h>

/* malloc(0) must return NULL */
KTEST(malloc, zero)
{
	ASSERT_NULL(malloc(0));
	return 0;
}

/* Basic alloc/free must not crash and must return a non-NULL pointer */
KTEST(malloc, basic)
{
	void *p = malloc(64);
	ASSERT_NONNULL(p);
	free(p);
	return 0;
}

/* Data written into the block must survive until free */
KTEST(malloc, pattern)
{
	unsigned char *p = (unsigned char *)malloc(256);
	unsigned i;

	ASSERT_NONNULL(p);
	for (i = 0; i < 256; i++)
		p[i] = (unsigned char)(i & 0xff);
	for (i = 0; i < 256; i++)
		ASSERT_EQ((int)p[i], (int)(i & 0xff));
	free(p);
	return 0;
}

/* zalloc must return a zeroed block */
KTEST(malloc, zeroed)
{
	unsigned char *p = (unsigned char *)zalloc(128);
	unsigned i;

	ASSERT_NONNULL(p);
	for (i = 0; i < 128; i++)
		EXPECT_EQ((int)p[i], 0);
	free(p);
	return 0;
}

/* Multiple live allocations must be at distinct addresses */
KTEST(malloc, multiple)
{
#define N 8
	void *ptrs[N];
	int i, j;

	for (i = 0; i < N; i++) {
		ptrs[i] = malloc((unsigned)(16 * (i + 1)));
		ASSERT_NONNULL(ptrs[i]);
	}
	for (i = 0; i < N; i++)
		for (j = i + 1; j < N; j++)
			EXPECT_NE(ptrs[i], ptrs[j]);
	for (i = 0; i < N; i++)
		free(ptrs[i]);
	return 0;
#undef N
}

/* free(NULL) must be a silent no-op */
KTEST(malloc, free_null)
{
	free(NULL);
	return 0;
}

/* Page-sized allocation: writable end-to-end */
KTEST(malloc, large)
{
	unsigned char *p = (unsigned char *)malloc(4096);

	ASSERT_NONNULL(p);
	memset(p, 0xab, 4096);
	EXPECT_EQ((int)p[0], 0xab);
	EXPECT_EQ((int)p[4095], 0xab);
	free(p);
	return 0;
}

/* Block must be reusable after free */
KTEST(malloc, reuse)
{
	void *a = malloc(128);
	ASSERT_NONNULL(a);
	free(a);

	void *b = malloc(128);
	ASSERT_NONNULL(b);
	free(b);
	return 0;
}

/* heap_quota must rise on alloc and return to baseline on free */
KTEST(malloc, quota)
{
	unsigned before = heap_quota;

	void *p = malloc(256);
	ASSERT_NONNULL(p);
	EXPECT_GT(heap_quota, before);

	free(p);
	EXPECT_EQ(heap_quota, before);
	return 0;
}
