/*
 * test/mm_test.c — unit tests for src/mm/mm.c
 *
 * Covers: vm_alloc/vm_free, mm_alloc_page_table/mm_free_page_table,
 *         mm_get_map_flag/mm_set_map_flag, mm_get_attached_page_index,
 *         name_get/name_put.
 *
 * Leak checks:
 *   phymm_used — counts reference-counted physical pages; must return to
 *                baseline after every vm_alloc/vm_free pair.
 *   heap_quota — counts live heap bytes; must return to baseline after
 *                every balanced malloc/free pair.
 */

#include <mm/mm.h>
#include <lib/klib.h>
#include <lib/list.h>
#include <config.h>
#include <test/test.h>

extern unsigned phymm_used;

/* ── vm_alloc / vm_free ──────────────────────────────────────────── */

KTEST(mm, vm_alloc_single)
{
	unsigned phys_before = phymm_used;

	unsigned addr = vm_alloc(1);
	ASSERT_NE(addr, 0u);
	EXPECT_GE(addr, KERNEL_OFFSET);
	EXPECT_EQ(phymm_used, phys_before + 1); /* one page referenced */

	/* Page must be writable */
	*(volatile unsigned *)addr = 0xdeadbeef;
	EXPECT_EQ(*(volatile unsigned *)addr, 0xdeadbeef);

	vm_free(addr, 1);
	EXPECT_EQ(phymm_used, phys_before); /* no leak */
	return 0;
}

KTEST(mm, vm_alloc_multi_page)
{
	unsigned phys_before = phymm_used;

	unsigned addr = vm_alloc(4);
	ASSERT_NE(addr, 0u);
	EXPECT_GE(addr, KERNEL_OFFSET);
	EXPECT_EQ(phymm_used, phys_before + 4);

	/* Touch first and last page */
	*(volatile unsigned *)addr = 0x11;
	*(volatile unsigned *)(addr + 3 * PAGE_SIZE) = 0x22;
	EXPECT_EQ(*(volatile unsigned *)addr, 0x11);
	EXPECT_EQ(*(volatile unsigned *)(addr + 3 * PAGE_SIZE), 0x22);

	vm_free(addr, 4);
	EXPECT_EQ(phymm_used, phys_before); /* no leak */
	return 0;
}

KTEST(mm, vm_alloc_distinct)
{
	unsigned phys_before = phymm_used;

	unsigned a = vm_alloc(1);
	unsigned b = vm_alloc(1);
	ASSERT_NE(a, 0u);
	ASSERT_NE(b, 0u);
	EXPECT_NE(a, b);
	EXPECT_EQ(phymm_used, phys_before + 2);

	vm_free(a, 1);
	vm_free(b, 1);
	EXPECT_EQ(phymm_used, phys_before); /* no leak */
	return 0;
}

KTEST(mm, vm_free_reuse)
{
	unsigned phys_before = phymm_used;

	unsigned a = vm_alloc(1);
	ASSERT_NE(a, 0u);
	vm_free(a, 1);
	EXPECT_EQ(phymm_used, phys_before);

	/* Allocator must still hand out pages after a free */
	unsigned b = vm_alloc(1);
	ASSERT_NE(b, 0u);
	vm_free(b, 1);
	EXPECT_EQ(phymm_used, phys_before); /* no leak */
	return 0;
}

/* ── mm_alloc_page_table / mm_free_page_table ────────────────────── */

KTEST(mm, page_table_alloc)
{
	unsigned heap_before = heap_quota;
	unsigned phys_before = phymm_used;

	unsigned pt = mm_alloc_page_table();
	ASSERT_NE(pt, 0u);
	/* Must fall inside the page-table cache region */
	EXPECT_GE(pt, PAGE_TABLE_CACHE_BEGIN);
	EXPECT_LT(pt, PAGE_TABLE_CACHE_END);

	mm_free_page_table(pt);
	/* Page-table cache uses pre-mapped memory — no heap or phys leak */
	EXPECT_EQ(heap_quota, heap_before);
	EXPECT_EQ(phymm_used, phys_before);
	return 0;
}

KTEST(mm, page_table_zeroed)
{
	unsigned pt = mm_alloc_page_table();
	ASSERT_NE(pt, 0u);

	/* mm_alloc_page_table memsets the page to 0 */
	unsigned *p = (unsigned *)pt;
	int i, all_zero = 1;
	for (i = 0; i < (int)(PAGE_SIZE / sizeof(unsigned)); i++) {
		if (p[i] != 0) {
			all_zero = 0;
			break;
		}
	}
	EXPECT_TRUE(all_zero);

	mm_free_page_table(pt);
	return 0;
}

KTEST(mm, page_table_alloc_free_reuse)
{
	unsigned heap_before = heap_quota;
	unsigned phys_before = phymm_used;

	unsigned a = mm_alloc_page_table();
	ASSERT_NE(a, 0u);
	mm_free_page_table(a);

	/* After freeing, another alloc must succeed */
	unsigned b = mm_alloc_page_table();
	ASSERT_NE(b, 0u);
	mm_free_page_table(b);

	EXPECT_EQ(heap_quota, heap_before);
	EXPECT_EQ(phymm_used, phys_before);
	return 0;
}

/* ── mm_get_map_flag / mm_set_map_flag ───────────────────────────── */

KTEST(mm, map_flag_get_after_alloc)
{
	unsigned phys_before = phymm_used;

	unsigned addr = vm_alloc(1);
	ASSERT_NE(addr, 0u);

	unsigned flags = mm_get_map_flag(addr);
	/* Kernel data pages are present, writable and global. */
	EXPECT_TRUE(flags & PAGE_ENTRY_PRESENT);
	EXPECT_TRUE(flags & PAGE_ENTRY_WRITABLE);
	EXPECT_TRUE(flags & PAGE_ENTRY_GLOBAL);

	vm_free(addr, 1);
	EXPECT_EQ(phymm_used, phys_before); /* no leak */
	return 0;
}

KTEST(mm, map_flag_set)
{
	unsigned phys_before = phymm_used;

	unsigned addr = vm_alloc(1);
	ASSERT_NE(addr, 0u);

	/* Remove the writable bit */
	unsigned flags = mm_get_map_flag(addr);
	mm_set_map_flag(addr, flags & ~PAGE_ENTRY_WRITABLE);
	unsigned new_flags = mm_get_map_flag(addr);
	EXPECT_FALSE(new_flags & PAGE_ENTRY_WRITABLE);

	/* Restore and verify */
	mm_set_map_flag(addr, flags);
	EXPECT_TRUE(mm_get_map_flag(addr) & PAGE_ENTRY_WRITABLE);

	vm_free(addr, 1);
	EXPECT_EQ(phymm_used, phys_before); /* no leak */
	return 0;
}

/* ── mm_get_attached_page_index ──────────────────────────────────── */

KTEST(mm, attached_page_index)
{
	unsigned phys_before = phymm_used;

	unsigned addr = vm_alloc(1);
	ASSERT_NE(addr, 0u);

	unsigned idx = mm_get_attached_page_index(addr);
	/* Kernel heap addresses are direct-map aliases. */
	unsigned expected = (addr - KERNEL_OFFSET) / PAGE_SIZE;
	EXPECT_EQ(idx, expected);

	vm_free(addr, 1);
	EXPECT_EQ(phymm_used, phys_before); /* no leak */
	return 0;
}

/* ── name_get / name_put ─────────────────────────────────────────── */

KTEST(mm, name_get_nonnull)
{
	void *buf = name_get();
	ASSERT_NONNULL(buf);
	name_put(buf); /* returns buf to cache; node allocated on heap */
	return 0;
}

KTEST(mm, name_get_distinct)
{
	void *a = name_get();
	void *b = name_get();
	ASSERT_NONNULL(a);
	ASSERT_NONNULL(b);
	EXPECT_NE(a, b);
	name_put(a);
	name_put(b);
	return 0;
}

/* name_put_reuse: put then get must return the same buffer pointer. */
KTEST(mm, name_put_reuse)
{
	void *a = name_get();
	ASSERT_NONNULL(a);
	name_put(a);

	void *b = name_get();
	ASSERT_NONNULL(b);
	EXPECT_EQ(a, b); /* same buffer reused from cache */

	name_put(b);
	return 0;
}

/*
 * name_node_no_heap_leak: the pathname cache is page-backed, so balancing
 * name_get/name_put should not change heap_quota at all.
 */
KTEST(mm, name_node_no_heap_leak)
{
	unsigned heap_before = heap_quota;
	void *a = name_get();
	ASSERT_NONNULL(a);
	EXPECT_EQ(heap_quota, heap_before);

	name_put(a);
	EXPECT_EQ(heap_quota, heap_before);

	void *b = name_get();
	ASSERT_NONNULL(b);
	EXPECT_EQ(heap_quota, heap_before);

	name_put(b);
	EXPECT_EQ(heap_quota, heap_before);
	return 0;
}

KTEST(mm, name_buf_writable)
{
	char *buf = (char *)name_get();
	ASSERT_NONNULL(buf);

	/* Buffer must be at least MAX_PATH bytes and writable */
	memset(buf, 'x', MAX_PATH);
	EXPECT_EQ(buf[0], 'x');
	EXPECT_EQ(buf[MAX_PATH - 1], 'x');

	name_put(buf);
	return 0;
}

/* Direct-map leaves must translate offsets, remain supervisor-only, and
 * survive allocator frees without tearing down shared kernel mappings. */
KTEST(mm, large_direct_map)
{
	unsigned cr4, offset;
	pte_t *pd = (pte_t *)mm_get_pagedir();
	asm volatile("movl %%cr4, %0" : "=r"(cr4));
	if (!(cr4 & (1U << 4)))
		return 0; /* CPU without PSE uses the existing 4 KiB path. */
	for (offset = 0; offset < KERNEL_DIRECT_MAP_LIMIT;
	     offset += LARGE_PAGE_SIZE) {
		unsigned addr = KERNEL_OFFSET + offset;
		EXPECT_EQ(mm_virt_to_phys(addr), offset);
		EXPECT_EQ(mm_virt_to_phys(addr + LARGE_PAGE_SIZE - 1),
			  offset + LARGE_PAGE_SIZE - 1);
		EXPECT_FALSE(mm_get_map_flag(addr) & PAGE_ENTRY_DPL_USER);
	}
	/* The last direct-map PDE is untouched by heap permission tests. */
	EXPECT_TRUE(pd[ADDR_TO_PGT_OFFSET((KERNEL_KMAP_BEGIN - 1))] &
		    PAGE_ENTRY_LARGE);
	unsigned addr = vm_alloc(1);
	ASSERT_NE(addr, 0u);
	unsigned phys = mm_virt_to_phys(addr);
	vm_free(addr, 1);
	EXPECT_EQ(mm_virt_to_phys(addr), phys);
	EXPECT_EQ(mm_kmap_phys(phys), 1);
	return 0;
}

KTEST(mm, large_split_shared_permissions)
{
	unsigned cr4;
	asm volatile("movl %%cr4, %0" : "=r"(cr4));
	if (!(cr4 & (1U << 4)))
		return 0;
	/* Use an otherwise untouched leaf; no access to physical RAM is needed. */
	unsigned addr = KERNEL_KMAP_BEGIN - 2 * LARGE_PAGE_SIZE + PAGE_SIZE;
	unsigned pd_addr = vm_alloc(1);
	ASSERT_NE(pd_addr, 0u);
	mm_init_process_page_dir(pd_addr);
	pte_t *pd = (pte_t *)pd_addr;
	pte_t *current = (pte_t *)mm_get_pagedir();
	unsigned index = ADDR_TO_PGT_OFFSET(addr);
	EXPECT_TRUE(pd[index] & PAGE_ENTRY_LARGE);
	unsigned flags = mm_get_map_flag(addr);
	mm_set_map_flag_pd(pd_addr, addr, flags & ~PAGE_ENTRY_WRITABLE);
	EXPECT_FALSE(current[index] & PAGE_ENTRY_LARGE);
	EXPECT_EQ(pd[index], current[index]);
	EXPECT_FALSE(mm_get_map_flag(addr) & PAGE_ENTRY_WRITABLE);
	EXPECT_FALSE(mm_get_map_flag_pd(pd_addr, addr) & PAGE_ENTRY_WRITABLE);
	EXPECT_TRUE(mm_get_map_flag(addr - PAGE_SIZE) & PAGE_ENTRY_WRITABLE);
	EXPECT_TRUE(mm_get_map_flag(addr + PAGE_SIZE) & PAGE_ENTRY_WRITABLE);
	EXPECT_EQ(mm_virt_to_phys(addr + 123), addr + 123 - KERNEL_OFFSET);
	EXPECT_EQ(mm_get_attached_page_index(addr),
		  (addr - KERNEL_OFFSET) / PAGE_SIZE);
	mm_set_map_flag(addr, flags);
	EXPECT_TRUE(mm_get_map_flag_pd(pd_addr, addr) & PAGE_ENTRY_WRITABLE);
	vm_free(pd_addr, 1);
	return 0;
}
