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

/* Exercise splits, reuse/coalescing, and fresh chunks with odd sizes too.
 * Heap-backed 64-bit atomics require this even on the 32-bit kernel.
 */
KTEST(malloc, alignment)
{
	void *ptrs[64];
	unsigned before = heap_quota;
	for (unsigned round = 0; round < 3; round++) {
		for (unsigned i = 0; i < 64; i++) {
			unsigned size = i == 63 ? 4096 : i + 1;
			ptrs[i] = malloc(size);
			ASSERT_NONNULL(ptrs[i]);
			EXPECT_EQ((uintptr_t)ptrs[i] & 7u, 0u);
			memset(ptrs[i], (char)i, size);
		}
		for (unsigned i = 0; i < 64; i++) {
			unsigned size = i == 63 ? 4096 : i + 1;
			unsigned char *p = ptrs[i];
			for (unsigned j = 0; j < size; j++)
				EXPECT_EQ(p[j], (unsigned char)i);
		}
		/* Leave holes, then free right-to-left to exercise coalescing. */
		for (unsigned i = 0; i < 64; i += 2)
			free(ptrs[i]);
		for (int i = 63; i >= 1; i -= 2)
			free(ptrs[i]);
		EXPECT_EQ(heap_quota, before);
	}
	return 0;
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

/* Exercise bitmap word boundaries and the transition to large size ranges. */
KTEST(malloc, size_bin_boundaries)
{
	static const unsigned sizes[] = {
		1,    16,   17,	  23,	24,   247,  248,  249,	255,
		256,  263,  264,  503,	504,  511,  512,  519,	520,
		759,  760,  767,  768,	775,  776,  1007, 1008, 1015,
		1016, 1017, 1024, 2039, 2040, 2041, 4096,
	};
	unsigned char *blocks[sizeof(sizes) / sizeof(sizes[0])];
	unsigned count = sizeof(sizes) / sizeof(sizes[0]);
	unsigned before = heap_quota;

	for (unsigned round = 0; round < 3; round++) {
		for (unsigned i = 0; i < count; i++) {
			blocks[i] = malloc(sizes[i]);
			ASSERT_NONNULL(blocks[i]);
			memset(blocks[i], (char)i, sizes[i]);
		}
		for (unsigned i = 0; i < count; i += 2) {
			free(blocks[i]);
			blocks[i] = malloc(sizes[i]);
			ASSERT_NONNULL(blocks[i]);
			memset(blocks[i], (char)i, sizes[i]);
		}
		for (unsigned i = count; i-- > 0;) {
			for (unsigned j = 0; j < sizes[i]; j++)
				EXPECT_EQ(blocks[i][j], (unsigned char)i);
			free(blocks[i]);
		}
		EXPECT_EQ(heap_quota, before);
	}
	return 0;
}

/* Retain guards so fragmented 56-byte blocks cannot merge during reuse. */
KTEST(malloc, fragmented_small_bins)
{
	void *holes[128];
	unsigned char *guards[128];
	unsigned char *blocks[16];
	unsigned before = heap_quota;

	for (unsigned i = 0; i < 128; i++) {
		holes[i] = malloc(48);
		guards[i] = malloc(16);
		ASSERT_NONNULL(holes[i]);
		ASSERT_NONNULL(guards[i]);
		memset(guards[i], (char)i, 16);
	}
	for (unsigned i = 0; i < 128; i++)
		free(holes[i]);
	for (unsigned round = 0; round < 3; round++) {
		for (unsigned i = 0; i < 16; i++) {
			blocks[i] = malloc(88);
			ASSERT_NONNULL(blocks[i]);
			memset(blocks[i], (char)i, 88);
		}
		for (unsigned i = 0; i < 16; i++) {
			for (unsigned j = 0; j < 88; j++)
				EXPECT_EQ(blocks[i][j], (unsigned char)i);
			free(blocks[i]);
		}
	}
	for (unsigned i = 128; i-- > 0;) {
		for (unsigned j = 0; j < 16; j++)
			EXPECT_EQ(guards[i][j], (unsigned char)i);
		free(guards[i]);
	}
	EXPECT_EQ(heap_quota, before);
	return 0;
}
