/*
 * test/mmap_test.c — unit tests for the virtual memory / mmap subsystem.
 *
 *   ./run.sh test
 *   echo 1 > /proc/test && cat /proc/test
 */

#include <mm/mm.h>
#include <mm/mmap.h>
#include <mm/pagefault.h>
#include <mm/phymm.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <fs/cache.h>
#include <lib/klib.h>
#include <ps/ps.h>
#include <config.h>
#include <errno.h>
#include <syscall/syscall.h>
#include <test/test.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* A fixed user-space address well within the user zone, page-aligned. */
#define TEST_FIXED_ADDR 0x20000000u

static vm_struct_t cur_vm(void)
{
	return current->user->vm;
}

KTEST(mmap, direct_ram_alias)
{
	unsigned page = phymm_alloc_user();
	int fd;
	intptr_t address;
	paddr_t physical;
	unsigned references;

	ASSERT_NE(page, PHYMM_INVALID);
	physical = (paddr_t)page * PAGE_SIZE;
	phymm_reference_page(page);
	references = phymm_pages[page].ref_count;
	if (phymm_end > 0x100000U)
		EXPECT_GE(physical, 0x100000000ULL);
	fd = fs_open("/dev/mem", O_RDWR, 0);
	if (fd < 0) {
		EXPECT_GE(fd, 0);
		goto release;
	}
	address = do_mmap(TEST_FIXED_ADDR, PAGE_SIZE, PROT_READ | PROT_WRITE,
			  MAP_SHARED | MAP_FIXED, fd, physical);
	EXPECT_EQ(address, TEST_FIXED_ADDR);
	if (address != TEST_FIXED_ADDR)
		goto close;
	EXPECT_EQ(pf_resolve_task_page_fault(current, address, 1), 1);
	EXPECT_EQ(mm_virt_to_phys(address), physical);
	EXPECT_TRUE(mm_get_map_flag(address) & PAGE_ENTRY_DIRECT_PHYS);
	EXPECT_FALSE(mm_get_map_flag(address) & PAGE_ENTRY_CD);
	*(volatile unsigned *)address = 0x76543210;
	EXPECT_EQ(*(unsigned *)PHY_TO_VIRT(physical), 0x76543210U);
	do_munmap((void *)address, PAGE_SIZE);
	EXPECT_EQ(phymm_pages[page].ref_count, references);
close:
	fs_close(fd);
release:
	if (!phymm_dereference_page(page))
		phymm_free_user(page);
	return 0;
}

static int wide_cache_read_page(file *fp, uint64_t offset, void *buffer)
{
	(void)fp;
	memset(buffer, offset >= 0x100000000ULL ? 0xb6 : 0xa5, PAGE_SIZE);
	return 0;
}

KTEST(mmap, file_cache_wide_offsets)
{
	file_operations operations = { .read_page = wide_cache_read_page };
	inode node = { .i_ino = 1, .i_pgcache_tag = &node };
	file fp = { .f_inode = &node, .f_fop = &operations };
	paddr_t low = fs_page_cache_get(&fp, 0, NULL);
	paddr_t high = fs_page_cache_get(&fp, 0x100000000ULL, NULL);

	EXPECT_NE(low, 0);
	EXPECT_NE(high, 0);
	EXPECT_NE(low, high);
	if (low && mm_kmap_phys(low) == 1) {
		EXPECT_EQ(*(unsigned char *)PHY_TO_VIRT(low), 0xa5);
		mm_kunmap_phys(low);
	}
	if (high && mm_kmap_phys(high) == 1) {
		EXPECT_EQ(*(unsigned char *)PHY_TO_VIRT(high), 0xb6);
		mm_kunmap_phys(high);
	}
	if (low)
		fs_page_cache_put(low);
	if (high)
		fs_page_cache_put(high);
	fs_page_cache_invalidate(&fp);
	return 0;
}

/* This probe observes cache identity without retaining an I/O reference. */
static paddr_t cache_lookup_address(file *fp, uint64_t offset, int *hit)
{
	paddr_t phy = fs_page_cache_get(fp, offset, hit);
	if (phy)
		fs_page_cache_put(phy);
	return phy;
}

static unsigned cached_range_reads;

static int cached_range_read_page(file *fp, uint64_t offset, void *buffer)
{
	cached_range_reads++;
	return wide_cache_read_page(fp, offset, buffer);
}

KTEST(mmap, file_cache_cached_range)
{
	file_operations operations = { .read_page = cached_range_read_page };
	inode node = { .i_ino = 1, .i_pgcache_tag = &node };
	file fp = { .f_inode = &node, .f_fop = &operations };
	paddr_t pages[4], first, last;

	cached_range_reads = 0;
	fs_page_cache_get_cached_range(&fp, 0x100000000ULL, pages, 4);
	for (unsigned i = 0; i < 4; i++)
		EXPECT_EQ(pages[i], 0);
	EXPECT_EQ(cached_range_reads, 0);
	first = fs_page_cache_get(&fp, 0x100000000ULL + PAGE_SIZE, NULL);
	last = fs_page_cache_get(&fp, 0x100000000ULL + 3 * PAGE_SIZE, NULL);
	ASSERT_NE(first, 0);
	ASSERT_NE(last, 0);
	fs_page_cache_put(first);
	fs_page_cache_put(last);
	EXPECT_EQ(cached_range_reads, 2);
	fs_page_cache_get_cached_range(&fp, 0x100000000ULL, pages, 4);
	EXPECT_EQ(pages[0], 0);
	EXPECT_EQ(pages[1], first);
	EXPECT_EQ(pages[2], 0);
	EXPECT_EQ(pages[3], last);
	EXPECT_EQ(cached_range_reads, 2);
	fs_page_cache_invalidate(&fp);
	for (unsigned i = 0; i < 4; i++) {
		if (!pages[i])
			continue;
		unsigned page = PHY_TO_PAGE_IDX(pages[i]);
		EXPECT_EQ(phymm_pages[page].ref_count, 1);
		fs_page_cache_put(pages[i]);
		EXPECT_EQ(phymm_is_used(page), 0);
	}
	return 0;
}

extern int copy_page_range(task_struct *parent, task_struct *child);

KTEST(mmap, proc_maps_format_widths)
{
	mm_struct *mm = cur_vm();
	vaddr_t base = mm->task_size > 0x100000000ULL ?
			       (vaddr_t)0x123450000ULL :
			       (vaddr_t)0x21000000;
	inode node = { .i_ino = 0x10000050467ULL };
	file backing = { .f_inode = &node,
			 .f_name = "/maps-width-probe",
			 .f_count = 1 };
	char *buffer = (char *)vm_alloc(2);
	int fd;
	int count;

	ASSERT_NONNULL(buffer);
	vm_add_map(mm, base, base + PAGE_SIZE, PROT_READ, MAP_PRIVATE, &backing,
		   0x1234567800001000ULL, 0);
	fd = fs_open("/proc/self/maps", O_RDONLY, 0);
	EXPECT_GE(fd, 0);
	if (fd < 0)
		goto cleanup;
	count = fs_read(fd, 0, buffer, 2 * PAGE_SIZE - 1);
	EXPECT_GT(count, 0);
	if (count > 0) {
		buffer[count] = 0;
		EXPECT_NONNULL(strstr(
			buffer,
			"r--p 1234567800001000 00:00 1099511956583 /maps-width-probe\n"));
		if (mm->task_size > 0x100000000ULL)
			EXPECT_NONNULL(strstr(buffer, "123450000-123451000 "));
		else
			EXPECT_NONNULL(strstr(buffer, "21000000-21001000 "));
	}
	fs_close(fd);
cleanup:
	do_munmap((void *)base, PAGE_SIZE);
	EXPECT_EQ(backing.f_count, 1);
	vm_free((vaddr_t)buffer, 2);
	return 0;
}

KTEST(mmap, brk_contiguous_growth)
{
	mm_struct *mm = cur_vm();
	vaddr_t saved_start = mm->start_brk;
	vaddr_t saved_brk = mm->brk;
	vaddr_t saved_limit = mm->brk_limit;
	const vaddr_t base = TEST_FIXED_ADDR;
	vm_region *region;
	vm_fault_lock *fault_lock;

	vm_set_brk(mm, base, base);
	mm->brk_limit = base + 64 * PAGE_SIZE;
	EXPECT_EQ(sys_brk(base + PAGE_SIZE), base + PAGE_SIZE);
	region = vm_find_map(mm, base);
	EXPECT_NONNULL(region);
	if (!region)
		goto cleanup;
	fault_lock = region->fault_lock;
	EXPECT_EQ(pf_resolve_task_page_fault(current, base, 1), 1);
	*(volatile unsigned *)base = 0x12345678;
	for (unsigned i = 2; i <= 16; i++) {
		EXPECT_EQ(sys_brk(base + i * PAGE_SIZE), base + i * PAGE_SIZE);
		EXPECT_EQ(vm_find_map(mm, base + (i - 1) * PAGE_SIZE), region);
		EXPECT_EQ(region->end, base + i * PAGE_SIZE);
		EXPECT_EQ(region->fault_lock, fault_lock);
	}
	EXPECT_EQ(*(volatile unsigned *)base, 0x12345678u);
	/* Shrink to a partial page, then extend the surviving region again. */
	EXPECT_EQ(sys_brk(base + 3 * PAGE_SIZE + 7), base + 3 * PAGE_SIZE + 7);
	region = vm_find_map(mm, base);
	EXPECT_EQ(region->end, base + 4 * PAGE_SIZE);
	EXPECT_EQ(vm_find_map(mm, base + 4 * PAGE_SIZE), NULL);
	EXPECT_EQ(sys_brk(base + 20 * PAGE_SIZE), base + 20 * PAGE_SIZE);
	EXPECT_EQ(vm_find_map(mm, base + 19 * PAGE_SIZE), region);
	EXPECT_EQ(region->end, base + 20 * PAGE_SIZE);
	EXPECT_EQ(*(volatile unsigned *)base, 0x12345678u);
cleanup:
	do_munmap((void *)base, 64 * PAGE_SIZE);
	mm->start_brk = saved_start;
	mm->brk = saved_brk;
	mm->brk_limit = saved_limit;
	return 0;
}

KTEST(mmap, brk_preserves_protected_tail)
{
	mm_struct *mm = cur_vm();
	vaddr_t saved_start = mm->start_brk;
	vaddr_t saved_brk = mm->brk;
	vaddr_t saved_limit = mm->brk_limit;
	const vaddr_t base = TEST_FIXED_ADDR;
	vm_region *tail;
	vm_region *growth;

	vm_set_brk(mm, base, base);
	mm->brk_limit = base + 8 * PAGE_SIZE;
	EXPECT_EQ(sys_brk(base + 2 * PAGE_SIZE), base + 2 * PAGE_SIZE);
	vm_mprotect(mm, base + PAGE_SIZE, base + 2 * PAGE_SIZE, PROT_READ);
	tail = vm_find_map(mm, base + PAGE_SIZE);
	EXPECT_EQ(sys_brk(base + 3 * PAGE_SIZE), base + 3 * PAGE_SIZE);
	growth = vm_find_map(mm, base + 2 * PAGE_SIZE);
	EXPECT_NE(growth, tail);
	EXPECT_EQ(tail->prot, PROT_READ);
	EXPECT_EQ(tail->end, base + 2 * PAGE_SIZE);
	EXPECT_EQ(growth->prot, PROT_READ | PROT_WRITE | PROT_EXEC);

	do_munmap((void *)base, 8 * PAGE_SIZE);
	mm->start_brk = saved_start;
	mm->brk = saved_brk;
	mm->brk_limit = saved_limit;
	return 0;
}

KTEST(mmap, sparse_clone_teardown)
{
	const vaddr_t base = TEST_FIXED_ADDR;
	const unsigned indexes[] = { 0, 127, 1023 };
	user_enviroment user = { 0 };
	task_struct child = { .user = &user };
	pfn_t pages[3];
	unsigned references[3];
	int ret;

	ASSERT_EQ(do_mmap(base, 1024 * PAGE_SIZE, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0),
		  base);
	for (unsigned i = 0; i < 3; i++) {
		vaddr_t address = base + indexes[i] * PAGE_SIZE;
		ASSERT_EQ(pf_resolve_task_page_fault(current, address, 1), 1);
		pages[i] = PHY_TO_PAGE_IDX(mm_virt_to_phys(address));
		references[i] = phymm_pages[pages[i]].ref_count;
	}
	user.vm = vm_create();
	ASSERT_NE(user.vm, NULL);
	user.vm->page_dir = vm_alloc(1);
	ASSERT_NE(user.vm->page_dir, 0);
	vm_set_page_dir(user.vm, user.vm->page_dir);
	ret = copy_page_range(current, &child);
	EXPECT_EQ(ret, 0);
	if (!ret)
		for (unsigned i = 0; i < 3; i++)
			EXPECT_EQ(phymm_pages[pages[i]].ref_count,
				  references[i] + 1);
	vm_put(user.vm);
	for (unsigned i = 0; i < 3; i++)
		EXPECT_EQ(phymm_pages[pages[i]].ref_count, references[i]);
	do_munmap((void *)base, 1024 * PAGE_SIZE);
	for (unsigned i = 0; i < 3; i++)
		EXPECT_EQ(phymm_is_used(pages[i]), 0);
	return 0;
}

KTEST(mmap, file_cache_invalidate_only_inode)
{
	file_operations operations = { .read_page = wide_cache_read_page };
	inode nodes[3] = {
		{ .i_ino = 10, .i_pgcache_tag = &operations },
		{ .i_ino = 20, .i_pgcache_tag = &operations },
		{ .i_ino = 30, .i_pgcache_tag = &operations },
	};
	file files[3];
	paddr_t physical[3];
	int hit;

	memset(files, 0, sizeof(files));
	for (unsigned i = 0; i < 3; i++) {
		files[i].f_inode = &nodes[i];
		files[i].f_fop = &operations;
		physical[i] = cache_lookup_address(&files[i], 0, NULL);
		EXPECT_NE(physical[i], 0);
	}
	EXPECT_NE(cache_lookup_address(&files[1], 0x100000000ULL, NULL), 0);
	fs_page_cache_invalidate(&files[1]);
	EXPECT_EQ(cache_lookup_address(&files[0], 0, &hit), physical[0]);
	EXPECT_EQ(hit, 1);
	EXPECT_EQ(cache_lookup_address(&files[2], 0, &hit), physical[2]);
	EXPECT_EQ(hit, 1);
	EXPECT_NE(cache_lookup_address(&files[1], 0, &hit), 0);
	EXPECT_EQ(hit, 0);
	EXPECT_NE(cache_lookup_address(&files[1], 0x100000000ULL, &hit), 0);
	EXPECT_EQ(hit, 0);
	for (unsigned i = 0; i < 3; i++)
		fs_page_cache_invalidate(&files[i]);
	return 0;
}

KTEST(mmap, file_cache_retained_during_invalidation)
{
	file_operations operations = { .read_page = wide_cache_read_page };
	inode node = { .i_ino = 1, .i_pgcache_tag = &node };
	file fp = { .f_inode = &node, .f_fop = &operations };
	paddr_t phy = fs_page_cache_get(&fp, 0, NULL);
	EXPECT_NE(phy, 0);
	if (!phy)
		return 0;
	unsigned page = PHY_TO_PAGE_IDX(phy);
	fs_page_cache_invalidate(&fp);
	EXPECT_EQ(phymm_pages[page].ref_count, 1);
	unsigned other = phymm_alloc_cache();
	EXPECT_NE(other, page);
	if (mm_kmap_phys(phy) == 1) {
		EXPECT_EQ(*(unsigned char *)PHY_TO_VIRT(phy), 0xa5);
		mm_kunmap_phys(phy);
	}
	if (other != PHYMM_INVALID)
		phymm_free_user(other);
	fs_page_cache_put(phy);
	EXPECT_EQ(phymm_is_used(page), 0);
	return 0;
}

/* ── AnonAutoAddr ─────────────────────────────────────────────────────────
 * do_mmap with addr=0 picks an address inside the user zone.
 */
KTEST(mmap, anon_auto_addr)
{
	vaddr_t addr = (vaddr_t)do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	ASSERT_NE(addr, 0u);
	EXPECT_GE(addr, TASK_UNMAPPED_BASE);
	EXPECT_LT(addr, cur_vm()->task_size - USER_STACK_PAGES * PAGE_SIZE);

	do_munmap((void *)addr, PAGE_SIZE);
	return 0;
}

/* ── AnonFixed ────────────────────────────────────────────────────────────
 * MAP_FIXED returns exactly the requested address.
 */
KTEST(mmap, anon_fixed)
{
	vaddr_t addr = (vaddr_t)do_mmap(TEST_FIXED_ADDR, PAGE_SIZE,
					PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
					-1, 0);

	EXPECT_EQ(addr, TEST_FIXED_ADDR);

	do_munmap((void *)addr, PAGE_SIZE);
	return 0;
}

/* ── RegionTracked ────────────────────────────────────────────────────────
 * After mmap the vm region is findable via vm_find_map.
 */
KTEST(mmap, region_tracked)
{
	vaddr_t addr = (vaddr_t)do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
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
	vaddr_t addr = (vaddr_t)do_mmap(0, PAGE_SIZE, prot,
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
	vaddr_t addr = (vaddr_t)do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
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
	vaddr_t addr = (vaddr_t)do_mmap(TEST_FIXED_ADDR, 1, PROT_READ,
					MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
					-1, 0);
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
	vaddr_t addr = (vaddr_t)do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_NE(addr, 0u);

	int ret = do_munmap((void *)addr, PAGE_SIZE);
	EXPECT_EQ(ret, 0);

	vm_region *r = vm_find_map(cur_vm(), addr);
	EXPECT_NULL(r);
	return 0;
}

/* ── MunmapInvalid ────────────────────────────────────────────────────────
 * munmap on an address that was never mapped is a no-op and returns 0,
 * matching Linux behaviour (POSIX does not require EINVAL here).
 */
KTEST(mmap, munmap_invalid)
{
	/* Use an address we know is not mapped. */
	vaddr_t addr = 0x30000000u;

	/* Make sure it really isn't mapped. */
	vm_region *r = vm_find_map(cur_vm(), addr);
	ASSERT_NULL(r);

	int ret = do_munmap((void *)addr, PAGE_SIZE);
	EXPECT_EQ(ret, 0);
	return 0;
}

/* ── TwoMapsDistinct ──────────────────────────────────────────────────────
 * Two back-to-back anonymous mmaps return different non-overlapping addresses.
 */
KTEST(mmap, two_maps_distinct)
{
	vaddr_t a = (vaddr_t)do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
				     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	vaddr_t b = (vaddr_t)do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
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

/* A fixed address for merge/split tests — distinct from TEST_FIXED_ADDR. */
#define TEST_MERGE_BASE 0x21000000u

/* ── LargeMapping ─────────────────────────────────────────────────────────
 * A multi-page mapping creates a single contiguous region.
 */
KTEST(mmap, large_mapping)
{
	unsigned size = 16 * PAGE_SIZE;
	vaddr_t addr = (vaddr_t)do_mmap(0, size, PROT_READ | PROT_WRITE,
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

/* ── MergeAdjacent ────────────────────────────────────────────────────────
 * Current vm_add_map() behavior keeps adjacent anonymous regions distinct
 * even when prot/flags match.
 */
KTEST(mmap, merge_adjacent)
{
	vaddr_t base = TEST_MERGE_BASE;
	int prot = PROT_READ | PROT_WRITE;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;

	do_mmap(base, PAGE_SIZE, prot, flags, -1, 0);
	do_mmap(base + PAGE_SIZE, PAGE_SIZE, prot, flags, -1, 0);

	vm_region *r = vm_find_map(cur_vm(), base);
	ASSERT_NONNULL(r);
	EXPECT_EQ(r->begin, base);
	EXPECT_EQ(r->end, base + PAGE_SIZE);

	vm_region *r2 = vm_find_map(cur_vm(), base + PAGE_SIZE);
	ASSERT_NONNULL(r2);
	EXPECT_EQ(r2->begin, base + PAGE_SIZE);
	EXPECT_EQ(r2->end, base + 2 * PAGE_SIZE);
	EXPECT_NE(r2->begin, r->begin);

	do_munmap((void *)base, 2 * PAGE_SIZE);
	return 0;
}

/* ── MergeNoDiffProt ──────────────────────────────────────────────────────
 * Adjacent regions with different prot must NOT be merged.
 */
KTEST(mmap, merge_no_diff_prot)
{
	vaddr_t base = TEST_MERGE_BASE;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;

	do_mmap(base, PAGE_SIZE, PROT_READ, flags, -1, 0);
	do_mmap(base + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE, flags, -1,
		0);

	vm_region *r = vm_find_map(cur_vm(), base);
	ASSERT_NONNULL(r);
	EXPECT_EQ(r->begin, base);
	EXPECT_EQ(r->end,
		  base + PAGE_SIZE); /* must not extend into next page */

	do_munmap((void *)base, 2 * PAGE_SIZE);
	return 0;
}

/* ── MergeThreeWay ────────────────────────────────────────────────────────
 * Filling the gap between two same-prot regions keeps three distinct VMAs.
 */
KTEST(mmap, merge_three_way)
{
	vaddr_t base = TEST_MERGE_BASE;
	int prot = PROT_READ | PROT_WRITE;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;

	do_mmap(base, PAGE_SIZE, prot, flags, -1, 0);
	do_mmap(base + 2 * PAGE_SIZE, PAGE_SIZE, prot, flags, -1, 0);
	/* Fill the gap — should coalesce all three into one. */
	do_mmap(base + PAGE_SIZE, PAGE_SIZE, prot, flags, -1, 0);

	vm_region *r = vm_find_map(cur_vm(), base);
	ASSERT_NONNULL(r);
	EXPECT_EQ(r->begin, base);
	EXPECT_EQ(r->end, base + PAGE_SIZE);

	vm_region *mid = vm_find_map(cur_vm(), base + PAGE_SIZE);
	ASSERT_NONNULL(mid);
	EXPECT_EQ(mid->begin, base + PAGE_SIZE);
	EXPECT_EQ(mid->end, base + 2 * PAGE_SIZE);

	vm_region *right = vm_find_map(cur_vm(), base + 2 * PAGE_SIZE);
	ASSERT_NONNULL(right);
	EXPECT_EQ(right->begin, base + 2 * PAGE_SIZE);
	EXPECT_EQ(right->end, base + 3 * PAGE_SIZE);

	do_munmap((void *)base, 3 * PAGE_SIZE);
	return 0;
}

/* ── SplitMiddle ──────────────────────────────────────────────────────────
 * munmap of the middle of a region leaves two separate remnants.
 */
KTEST(mmap, split_middle)
{
	vaddr_t base = TEST_MERGE_BASE;
	int prot = PROT_READ | PROT_WRITE;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;

	do_mmap(base, 4 * PAGE_SIZE, prot, flags, -1, 0);
	do_munmap((void *)(base + PAGE_SIZE), 2 * PAGE_SIZE);

	vm_region *rl = vm_find_map(cur_vm(), base);
	ASSERT_NONNULL(rl);
	EXPECT_EQ(rl->begin, base);
	EXPECT_EQ(rl->end, base + PAGE_SIZE);

	vm_region *rr = vm_find_map(cur_vm(), base + 3 * PAGE_SIZE);
	ASSERT_NONNULL(rr);
	EXPECT_EQ(rr->begin, base + 3 * PAGE_SIZE);
	EXPECT_EQ(rr->end, base + 4 * PAGE_SIZE);

	EXPECT_NULL(vm_find_map(cur_vm(), base + PAGE_SIZE));
	EXPECT_NULL(vm_find_map(cur_vm(), base + 2 * PAGE_SIZE));

	do_munmap((void *)base, PAGE_SIZE);
	do_munmap((void *)(base + 3 * PAGE_SIZE), PAGE_SIZE);
	return 0;
}

/* ── SplitLeftTrim ────────────────────────────────────────────────────────
 * munmap of the left portion leaves only the right remnant.
 */
KTEST(mmap, split_left_trim)
{
	vaddr_t base = TEST_MERGE_BASE;
	int prot = PROT_READ | PROT_WRITE;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;

	do_mmap(base, 4 * PAGE_SIZE, prot, flags, -1, 0);
	do_munmap((void *)base, 2 * PAGE_SIZE);

	EXPECT_NULL(vm_find_map(cur_vm(), base));
	EXPECT_NULL(vm_find_map(cur_vm(), base + PAGE_SIZE));

	vm_region *r = vm_find_map(cur_vm(), base + 2 * PAGE_SIZE);
	ASSERT_NONNULL(r);
	EXPECT_EQ(r->begin, base + 2 * PAGE_SIZE);
	EXPECT_EQ(r->end, base + 4 * PAGE_SIZE);

	do_munmap((void *)(base + 2 * PAGE_SIZE), 2 * PAGE_SIZE);
	return 0;
}

/* ── SplitRightTrim ───────────────────────────────────────────────────────
 * munmap of the right portion leaves only the left remnant.
 */
KTEST(mmap, split_right_trim)
{
	vaddr_t base = TEST_MERGE_BASE;
	int prot = PROT_READ | PROT_WRITE;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;

	do_mmap(base, 4 * PAGE_SIZE, prot, flags, -1, 0);
	do_munmap((void *)(base + 2 * PAGE_SIZE), 2 * PAGE_SIZE);

	vm_region *r = vm_find_map(cur_vm(), base);
	ASSERT_NONNULL(r);
	EXPECT_EQ(r->begin, base);
	EXPECT_EQ(r->end, base + 2 * PAGE_SIZE);

	EXPECT_NULL(vm_find_map(cur_vm(), base + 2 * PAGE_SIZE));
	EXPECT_NULL(vm_find_map(cur_vm(), base + 3 * PAGE_SIZE));

	do_munmap((void *)base, 2 * PAGE_SIZE);
	return 0;
}

/* ── SplitByFixed ─────────────────────────────────────────────────────────
 * MAP_FIXED over the middle with a different prot splits the original
 * region into three: left remnant | new middle | right remnant.
 */
KTEST(mmap, split_by_fixed)
{
	vaddr_t base = TEST_MERGE_BASE;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;

	do_mmap(base, 4 * PAGE_SIZE, PROT_READ, flags, -1, 0);
	/* Overwrite middle 2 pages with a different prot. */
	do_mmap(base + PAGE_SIZE, 2 * PAGE_SIZE, PROT_READ | PROT_WRITE, flags,
		-1, 0);

	vm_region *rl = vm_find_map(cur_vm(), base);
	ASSERT_NONNULL(rl);
	EXPECT_EQ(rl->begin, base);
	EXPECT_EQ(rl->end, base + PAGE_SIZE);
	EXPECT_EQ(rl->prot, PROT_READ);

	vm_region *rm = vm_find_map(cur_vm(), base + PAGE_SIZE);
	ASSERT_NONNULL(rm);
	EXPECT_EQ(rm->begin, base + PAGE_SIZE);
	EXPECT_EQ(rm->end, base + 3 * PAGE_SIZE);
	EXPECT_EQ(rm->prot, PROT_READ | PROT_WRITE);

	vm_region *rr = vm_find_map(cur_vm(), base + 3 * PAGE_SIZE);
	ASSERT_NONNULL(rr);
	EXPECT_EQ(rr->begin, base + 3 * PAGE_SIZE);
	EXPECT_EQ(rr->end, base + 4 * PAGE_SIZE);
	EXPECT_EQ(rr->prot, PROT_READ);

	do_munmap((void *)base, 4 * PAGE_SIZE);
	return 0;
}

KTEST(mmap, mremap_grow_extends_region)
{
	vaddr_t base = TEST_MERGE_BASE;
	int prot = PROT_READ | PROT_WRITE;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;

	do_mmap(base, 2 * PAGE_SIZE, prot, flags, -1, 0);

	EXPECT_EQ(sys_mremap(base, 2 * PAGE_SIZE, 4 * PAGE_SIZE, 0, 0),
		  (int)base);

	vm_region *r = vm_find_map(cur_vm(), base);
	ASSERT_NONNULL(r);
	EXPECT_EQ(r->begin, base);
	EXPECT_EQ(r->end, base + 4 * PAGE_SIZE);
	EXPECT_EQ(vm_find_map(cur_vm(), base + 3 * PAGE_SIZE), r);

	do_munmap((void *)base, 4 * PAGE_SIZE);
	return 0;
}

KTEST(mmap, mremap_grow_rejects_later_overlap)
{
	vaddr_t base = TEST_MERGE_BASE;
	int prot = PROT_READ | PROT_WRITE;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;

	do_mmap(base, PAGE_SIZE, prot, flags, -1, 0);
	do_mmap(base + 2 * PAGE_SIZE, PAGE_SIZE, prot, flags, -1, 0);

	EXPECT_EQ(sys_mremap(base, PAGE_SIZE, 4 * PAGE_SIZE, 0, 0), -ENOMEM);

	vm_region *r = vm_find_map(cur_vm(), base);
	ASSERT_NONNULL(r);
	EXPECT_EQ(r->begin, base);
	EXPECT_EQ(r->end, base + PAGE_SIZE);

	do_munmap((void *)base, PAGE_SIZE);
	do_munmap((void *)(base + 2 * PAGE_SIZE), PAGE_SIZE);
	return 0;
}

/* One resolver call must finish a first-write file fault, without making the
 * page cache writable. Also exercise a later COW fault and slot reuse. */
KTEST(mmap, file_first_write_private)
{
	int fd = fs_open("/bin/true", O_RDONLY, 0);
	vaddr_t base = 0x21000000;
	vaddr_t scratch = vm_alloc(1);
	paddr_t cached, copied;
	unsigned char first;
	int mapped = 0;

	EXPECT_GE(fd, 0);
	EXPECT_NE(scratch, 0u);
	if (fd < 0 || !scratch)
		goto out;
	for (int i = 0; i < 3; i++) {
		int prot = i == 2 ? PROT_READ : PROT_READ | PROT_WRITE;
		int ret = do_mmap(base + i * PAGE_SIZE, PAGE_SIZE, prot,
				  MAP_PRIVATE | MAP_FIXED, fd, 0);
		EXPECT_EQ((vaddr_t)ret, base + i * PAGE_SIZE);
		if ((vaddr_t)ret != base + i * PAGE_SIZE)
			goto out;
		mapped++;
	}
	EXPECT_EQ(pf_resolve_task_page_fault(current, base, 0), 1);
	cached = mm_virt_to_phys(base);
	EXPECT_NE(cached, 0u);
	if (!cached)
		goto out;
	EXPECT_FALSE(mm_get_map_flag(base) & PAGE_ENTRY_WRITABLE);
	EXPECT_EQ(mm_copy_phys_page(VIRT_TO_PHY(scratch), cached), 1);
	first = *(unsigned char *)scratch;

	EXPECT_EQ(pf_resolve_task_page_fault(current, base + PAGE_SIZE, 1), 1);
	EXPECT_TRUE(mm_get_map_flag(base + PAGE_SIZE) & PAGE_ENTRY_WRITABLE);
	copied = mm_virt_to_phys(base + PAGE_SIZE);
	EXPECT_NE(copied, cached);
	EXPECT_NE(copied, 0u);
	if (!copied || copied == cached)
		goto out;
	EXPECT_EQ(mm_copy_phys_page(VIRT_TO_PHY(scratch), copied), 1);
	EXPECT_EQ(*(unsigned char *)scratch, first);
	*(unsigned char *)scratch = first ^ 0xff;
	EXPECT_EQ(mm_copy_phys_page(copied, VIRT_TO_PHY(scratch)), 1);
	EXPECT_EQ(mm_copy_phys_page(VIRT_TO_PHY(scratch), cached), 1);
	EXPECT_EQ(*(unsigned char *)scratch, first);
	EXPECT_EQ(mm_copy_phys_page(VIRT_TO_PHY(scratch), copied), 1);
	EXPECT_EQ(*(unsigned char *)scratch, (unsigned char)(first ^ 0xff));

	/* A write to an existing read-only private mapping still uses COW. */
	EXPECT_EQ(pf_resolve_task_page_fault(current, base, 1), 1);
	EXPECT_TRUE(mm_get_map_flag(base) & PAGE_ENTRY_WRITABLE);
	EXPECT_NE(mm_virt_to_phys(base), cached);
	EXPECT_EQ(pf_resolve_task_page_fault(current, base + 2 * PAGE_SIZE, 1),
		  0);
	EXPECT_FALSE(mm_get_map_flag(base + 2 * PAGE_SIZE) &
		     PAGE_ENTRY_PRESENT);

out:
	while (mapped)
		do_munmap((void *)(base + --mapped * PAGE_SIZE), PAGE_SIZE);
	if (scratch)
		vm_free(scratch, 1);
	if (fd >= 0)
		fs_close(fd);
	return 0;
}

KTEST(mmap, execute_permissions)
{
	intptr_t result = do_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	ASSERT_GT(result, 0);
	vaddr_t address = (vaddr_t)result;
	EXPECT_EQ(pf_resolve_task_page_fault(current, address, 0), 1);
	EXPECT_EQ((mm_get_map_flag(address) & PAGE_ENTRY_NO_EXEC) != 0,
		  MOS_PAGE_NO_EXEC != 0);

	EXPECT_EQ(sys_mprotect((void *)address, PAGE_SIZE,
			       PROT_READ | PROT_EXEC),
		  0);
	EXPECT_EQ(mm_get_map_flag(address) & PAGE_ENTRY_NO_EXEC, 0);
	EXPECT_EQ(sys_mprotect((void *)address, PAGE_SIZE, PROT_READ), 0);
	EXPECT_EQ((mm_get_map_flag(address) & PAGE_ENTRY_NO_EXEC) != 0,
		  MOS_PAGE_NO_EXEC != 0);

	do_mmap_update(address, PROT_READ | PROT_EXEC,
		       MAP_PRIVATE | MAP_ANONYMOUS);
	EXPECT_EQ(mm_get_map_flag(address) & PAGE_ENTRY_NO_EXEC, 0);
	do_mmap_update(address, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS);
	EXPECT_EQ((mm_get_map_flag(address) & PAGE_ENTRY_NO_EXEC) != 0,
		  MOS_PAGE_NO_EXEC != 0);
	do_munmap((void *)address, PAGE_SIZE);
	return 0;
}
