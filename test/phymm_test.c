/*
 * test/phymm_test.c — unit tests for the physical memory buddy allocator.
 *
 *   ./run.sh test
 *   echo 1 > /proc/test && cat /proc/test
 */

#include <mm/phymm.h>
#include <mm/mm.h>
#include <test/test.h>

extern unsigned phymm_used;

/* ── AllocUser ───────────────────────────────────────────────────────────── */

/* Single-page alloc returns a valid in-range index */
KTEST(phymm, alloc_user)
{
	unsigned idx = phymm_alloc_user();

	ASSERT_NE(idx, PHYMM_INVALID);
	ASSERT_GE(idx, phymm_begin);
	ASSERT_LT(idx, phymm_end);

	phymm_free_user(idx);
	return 0;
}

/* Two consecutive allocs return distinct pages */
KTEST(phymm, alloc_user_distinct)
{
	unsigned a = phymm_alloc_user();
	unsigned b = phymm_alloc_user();

	ASSERT_NE(a, PHYMM_INVALID);
	ASSERT_NE(b, PHYMM_INVALID);
	EXPECT_NE(a, b);

	phymm_free_user(b);
	phymm_free_user(a);
	return 0;
}

/* After free, the same order-0 slot is immediately reusable (LIFO free list) */
KTEST(phymm, free_user_reuse)
{
	unsigned a = phymm_alloc_user();
	ASSERT_NE(a, PHYMM_INVALID);
	phymm_free_user(a);

	unsigned b = phymm_alloc_user();
	ASSERT_NE(b, PHYMM_INVALID);
	/* Buddy free-list is LIFO: the just-freed page should come back */
	EXPECT_EQ(b, a);
	phymm_free_user(b);
	return 0;
}

/* free(PHYMM_INVALID) must be a silent no-op */
KTEST(phymm, free_user_invalid)
{
	phymm_free_user(PHYMM_INVALID);
	return 0;
}

/* ── AllocKernel ─────────────────────────────────────────────────────────── */

/* Allocating a single kernel page returns a valid index */
KTEST(phymm, alloc_kernel_single)
{
	unsigned idx = phymm_alloc_kernel(1);

	ASSERT_NE(idx, PHYMM_INVALID);
	ASSERT_GE(idx, phymm_begin);
	ASSERT_LT(idx, phymm_end);

	phymm_free_kernel(idx, 1);
	return 0;
}

/*
 * phymm_alloc_kernel(3) rounds up to 4 pages (order 2).
 * The returned block must be entirely within [phymm_begin, phymm_end).
 */
KTEST(phymm, alloc_kernel_block)
{
	unsigned idx = phymm_alloc_kernel(3);

	ASSERT_NE(idx, PHYMM_INVALID);
	ASSERT_GE(idx, phymm_begin);
	/* block of 4 pages must fit */
	ASSERT_LE(idx + 4, phymm_end);

	phymm_free_kernel(idx, 3);
	return 0;
}

/*
 * Buddy blocks of 2^k pages must be naturally aligned to 2^k pages.
 * Allocate a 4-page block (order 2) and verify idx % 4 == 0.
 */
KTEST(phymm, alloc_kernel_aligned)
{
	unsigned idx = phymm_alloc_kernel(4);

	ASSERT_NE(idx, PHYMM_INVALID);
	EXPECT_EQ(idx & 3u, 0u); /* 4-page aligned */

	phymm_free_kernel(idx, 4);
	return 0;
}

/* An 8-page block must be 8-page aligned */
KTEST(phymm, alloc_kernel_aligned8)
{
	unsigned idx = phymm_alloc_kernel(8);

	ASSERT_NE(idx, PHYMM_INVALID);
	EXPECT_EQ(idx & 7u, 0u);

	phymm_free_kernel(idx, 8);
	return 0;
}

/* After freeing a kernel block the pages are available for re-allocation */
KTEST(phymm, free_kernel_reuse)
{
	unsigned a = phymm_alloc_kernel(4);
	ASSERT_NE(a, PHYMM_INVALID);
	phymm_free_kernel(a, 4);

	unsigned b = phymm_alloc_kernel(4);
	ASSERT_NE(b, PHYMM_INVALID);
	EXPECT_EQ(b, a); /* LIFO: same block returned */
	phymm_free_kernel(b, 4);
	return 0;
}

/* ── Reference counting ──────────────────────────────────────────────────── */

/* reference_page raises ref_count; dereference_page lowers it */
KTEST(phymm, ref_count_basic)
{
	unsigned idx = phymm_alloc_user();
	ASSERT_NE(idx, PHYMM_INVALID);

	/* Fresh allocation: not yet "in use" by the ref layer */
	EXPECT_EQ(phymm_is_used(idx), 0);

	unsigned r1 = phymm_reference_page(idx);
	EXPECT_EQ((int)r1, 1);
	EXPECT_NE(phymm_is_used(idx), 0);

	unsigned r2 = phymm_dereference_page(idx);
	EXPECT_EQ((int)r2, 0);
	EXPECT_EQ(phymm_is_used(idx), 0);

	phymm_free_user(idx);
	return 0;
}

/* is_cow is true when ref_count > 1, false when ref_count <= 1 */
KTEST(phymm, ref_count_is_cow)
{
	unsigned idx = phymm_alloc_user();
	ASSERT_NE(idx, PHYMM_INVALID);

	phymm_reference_page(idx);
	EXPECT_EQ(phymm_is_cow(idx), 0); /* ref_count == 1: not COW */

	phymm_reference_page(idx);
	EXPECT_NE(phymm_is_cow(idx), 0); /* ref_count == 2: COW */

	phymm_dereference_page(idx);
	EXPECT_EQ(phymm_is_cow(idx), 0); /* ref_count == 1 again */

	phymm_dereference_page(idx);
	phymm_free_user(idx);
	return 0;
}

/* phymm_used tracks the number of pages with ref_count > 0 */
KTEST(phymm, used_counter)
{
	unsigned before = phymm_used;

	unsigned idx = phymm_alloc_user();
	ASSERT_NE(idx, PHYMM_INVALID);

	/* Alloc alone does not change phymm_used */
	EXPECT_EQ(phymm_used, before);

	phymm_reference_page(idx);
	EXPECT_EQ(phymm_used, before + 1);

	phymm_dereference_page(idx);
	EXPECT_EQ(phymm_used, before);

	phymm_free_user(idx);
	return 0;
}

/* Cache policy must preserve free headroom and account for reusable lines. */
KTEST(phymm, cache_budget_large_ram)
{
	phymm_cache_policy policy;
	unsigned total = 8ULL * 1024 * 1024 * 1024 / PAGE_SIZE;

	phymm_cache_budget(total, total, 0, &policy);
	EXPECT_EQ(policy.file_pages + policy.block_pages, total / 4);
	EXPECT_EQ(policy.reserve_pages, 256 * 1024 * 1024 / PAGE_SIZE);
#if MOS_HAS_NATIVE_USER
	EXPECT_EQ(policy.block_pages, total / 16);
#else
	EXPECT_EQ(policy.block_pages, HDD_CACHE_MAX_PAGES);
#endif
	return 0;
}

KTEST(phymm, cache_budget_pressure)
{
	phymm_cache_policy policy;
	unsigned total = 8ULL * 1024 * 1024 * 1024 / PAGE_SIZE;

	phymm_cache_budget(total, 1024, 1024, &policy);
	EXPECT_EQ(policy.file_pages, 1);
	EXPECT_EQ(policy.block_pages, 1);
	phymm_cache_budget(total, 65536, 4096, &policy);
	EXPECT_EQ(policy.file_pages + policy.block_pages, 4096);
	return 0;
}

KTEST(phymm, cache_budget_capacity_ceiling)
{
	phymm_cache_policy policy;
	unsigned total = 64ULL * 1024 * 1024 * 1024 / PAGE_SIZE;

	phymm_cache_budget(total, total, 0, &policy);
	EXPECT_EQ(policy.file_pages + policy.block_pages,
		  0x100000000ULL / PAGE_SIZE);
	return 0;
}

KTEST(phymm, cache_budget_matches_allocator)
{
	phymm_usage usage;
	phymm_cache_policy actual, expected;
	extern unsigned fs_page_cache_pages;
	extern unsigned hdd_cache_size;
	unsigned cached =
		fs_page_cache_pages + hdd_cache_size / (PAGE_SIZE / 512);

	phymm_get_usage(&usage);
	phymm_cache_budget(usage.low_total_pages + usage.high_total_pages,
			   usage.low_free_pages + usage.high_free_pages, cached,
			   &expected);
	phymm_get_cache_policy(&actual);
	EXPECT_EQ(actual.file_pages, expected.file_pages);
	EXPECT_EQ(actual.block_pages, expected.block_pages);
	EXPECT_EQ(actual.reserve_pages, expected.reserve_pages);
	return 0;
}
