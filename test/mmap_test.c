/*
 * test/mmap_test.c — unit tests for the virtual memory / mmap subsystem.
 *
 *   ./run.sh test
 *   echo 1 > /proc/test && cat /proc/test
 */

#include <mm/mm.h>
#include <mm/mmap.h>
#include <ps/ps.h>
#include <config.h>
#include <errno.h>
#include <test/test.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* A fixed user-space address well within the user zone, page-aligned. */
#define TEST_FIXED_ADDR 0x20000000u

static vm_struct_t cur_vm(void)
{
	return CURRENT_TASK()->user->vm;
}

/* ── AnonAutoAddr ─────────────────────────────────────────────────────────
 * do_mmap with addr=0 picks an address inside the user zone.
 */
KTEST(mmap, anon_auto_addr)
{
	unsigned addr = (unsigned)do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
					  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	ASSERT_NE(addr, 0u);
	EXPECT_GE(addr, TASK_UNMAPPED_BASE);
	EXPECT_LT(addr, USER_ZONE_END);

	do_munmap((void *)addr, PAGE_SIZE);
	return 0;
}

/* ── AnonFixed ────────────────────────────────────────────────────────────
 * MAP_FIXED returns exactly the requested address.
 */
KTEST(mmap, anon_fixed)
{
	unsigned addr = (unsigned)do_mmap(
		TEST_FIXED_ADDR, PAGE_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

	EXPECT_EQ(addr, TEST_FIXED_ADDR);

	do_munmap((void *)addr, PAGE_SIZE);
	return 0;
}

/* ── RegionTracked ────────────────────────────────────────────────────────
 * After mmap the vm region is findable via vm_find_map.
 */
KTEST(mmap, region_tracked)
{
	unsigned addr = (unsigned)do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
					  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(addr, 0u);

	vm_region *r = vm_find_map(cur_vm(), addr);
	ASSERT_NONNULL(r);
	EXPECT_GE(addr, r->begin);
	EXPECT_LT(addr, r->end);

	do_munmap((void *)addr, PAGE_SIZE);
	return 0;
}

/* ── RegionProt ───────────────────────────────────────────────────────────
 * The region remembers the prot flags that were passed to mmap.
 */
KTEST(mmap, region_prot)
{
	int prot = PROT_READ | PROT_WRITE;
	unsigned addr = (unsigned)do_mmap(0, PAGE_SIZE, prot,
					  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(addr, 0u);

	vm_region *r = vm_find_map(cur_vm(), addr);
	ASSERT_NONNULL(r);
	EXPECT_EQ(r->prot, prot);

	do_munmap((void *)addr, PAGE_SIZE);
	return 0;
}

/* ── RegionAnon ───────────────────────────────────────────────────────────
 * Anonymous mapping: region->node must be NULL (no file backing).
 */
KTEST(mmap, region_anon)
{
	unsigned addr = (unsigned)do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
					  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(addr, 0u);

	vm_region *r = vm_find_map(cur_vm(), addr);
	ASSERT_NONNULL(r);
	EXPECT_NULL(r->fp);

	do_munmap((void *)addr, PAGE_SIZE);
	return 0;
}

/* ── SizeRoundup ──────────────────────────────────────────────────────────
 * Mapping 1 byte still reserves a full page-aligned region.
 */
KTEST(mmap, size_roundup)
{
	unsigned addr = (unsigned)do_mmap(
		TEST_FIXED_ADDR, 1, PROT_READ,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	ASSERT_EQ(addr, TEST_FIXED_ADDR);

	vm_region *r = vm_find_map(cur_vm(), addr);
	ASSERT_NONNULL(r);
	/* Region must cover at least one full page */
	EXPECT_EQ(r->begin, TEST_FIXED_ADDR);
	EXPECT_GE(r->end, TEST_FIXED_ADDR + PAGE_SIZE);

	do_munmap((void *)addr, 1);
	return 0;
}

/* ── MunmapRemoves ────────────────────────────────────────────────────────
 * After munmap the region is gone from the vm map.
 */
KTEST(mmap, munmap_removes)
{
	unsigned addr = (unsigned)do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
					  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(addr, 0u);

	int ret = do_munmap((void *)addr, PAGE_SIZE);
	EXPECT_EQ(ret, 0);

	vm_region *r = vm_find_map(cur_vm(), addr);
	EXPECT_NULL(r);
	return 0;
}

/* ── MunmapInvalid ────────────────────────────────────────────────────────
 * munmap on an address that was never mapped returns -EINVAL.
 */
KTEST(mmap, munmap_invalid)
{
	/* Use an address we know is not mapped. */
	unsigned addr = 0x30000000u;

	/* Make sure it really isn't mapped. */
	vm_region *r = vm_find_map(cur_vm(), addr);
	ASSERT_NULL(r);

	int ret = do_munmap((void *)addr, PAGE_SIZE);
	EXPECT_EQ(ret, -EINVAL);
	return 0;
}

/* ── TwoMapsDistinct ──────────────────────────────────────────────────────
 * Two back-to-back anonymous mmaps return different non-overlapping addresses.
 */
KTEST(mmap, two_maps_distinct)
{
	unsigned a = (unsigned)do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	unsigned b = (unsigned)do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	ASSERT_NE(a, 0u);
	ASSERT_NE(b, 0u);
	EXPECT_NE(a, b);
	/* Regions must not overlap */
	EXPECT_TRUE(b >= a + PAGE_SIZE || a >= b + PAGE_SIZE);

	do_munmap((void *)a, PAGE_SIZE);
	do_munmap((void *)b, PAGE_SIZE);
	return 0;
}

/* ── LargeMapping ─────────────────────────────────────────────────────────
 * A multi-page mapping creates a single contiguous region.
 */
KTEST(mmap, large_mapping)
{
	unsigned size = 16 * PAGE_SIZE;
	unsigned addr = (unsigned)do_mmap(0, size, PROT_READ | PROT_WRITE,
					  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(addr, 0u);

	/* Check the start and end of the region */
	vm_region *r_start = vm_find_map(cur_vm(), addr);
	vm_region *r_end = vm_find_map(cur_vm(), addr + size - PAGE_SIZE);

	ASSERT_NONNULL(r_start);
	ASSERT_NONNULL(r_end);
	/* Both addresses must fall inside the same region */
	EXPECT_EQ(r_start->begin, r_end->begin);
	EXPECT_GE(r_start->end - r_start->begin, size);

	do_munmap((void *)addr, size);
	return 0;
}
