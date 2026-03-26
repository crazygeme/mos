#include <mm/phymm.h>
#include <mm/mm.h>
#include <boot/multiboot.h>
#include <lib/lock.h>
#include <lib/klib.h>
#include <macro.h>

/*
 * Global state
 */

unsigned phymm_used = 0;

phymm_page *phymm_pages;

/* Buddy free lists: one singly-linked list per order (0..MAX_BUDDY_ORDER).
 * Each entry holds the index of the head page of a free block; the chain is
 * threaded through phymm_page.next_free / prev_free (doubly linked for O(1)
 * removal during coalescing). */
static unsigned buddy_lists[MAX_BUDDY_ORDER + 1];
static spinlock_t buddy_lock;

/*
 * Internal helpers
 * */

static unsigned ceil_log2(unsigned n)
{
	unsigned order = 0;
	unsigned size = 1;
	while (size < n) {
		size <<= 1;
		order++;
	}
	return order;
}

/* Push a free block at @idx of @order onto the head of the free list. */
static void buddy_push(unsigned idx, unsigned order)
{
	unsigned old_head = buddy_lists[order];

	phymm_pages[idx].order = (unsigned char)order;
	phymm_pages[idx].prev_free = PHYMM_INVALID;
	phymm_pages[idx].next_free = old_head;
	if (old_head != PHYMM_INVALID)
		phymm_pages[old_head].prev_free = idx;
	buddy_lists[order] = idx;
}

/* Unlink @idx from its current free list (order stored in phymm_pages[idx]). */
static void buddy_remove(unsigned idx)
{
	unsigned order = phymm_pages[idx].order;
	unsigned prev = phymm_pages[idx].prev_free;
	unsigned next = phymm_pages[idx].next_free;

	if (prev == PHYMM_INVALID)
		buddy_lists[order] = next;
	else
		phymm_pages[prev].next_free = next;

	if (next != PHYMM_INVALID)
		phymm_pages[next].prev_free = prev;
}

/* Pop the head of the free list for @order. Returns PHYMM_INVALID if empty. */
static unsigned buddy_pop(unsigned order)
{
	unsigned idx = buddy_lists[order];
	if (idx == PHYMM_INVALID)
		return PHYMM_INVALID;
	buddy_remove(idx);
	return idx;
}

/* Is @idx a free block head at exactly @order (ready to coalesce)? */
static int buddy_is_free_at_order(unsigned idx, unsigned order)
{
	if (idx < phymm_begin || idx >= phymm_end)
		return 0;
	return (phymm_pages[idx].ref_count == 0 &&
		phymm_pages[idx].order == (unsigned char)order);
}

/*
 * Core buddy allocator
 */

/*
 * Allocate a block of exactly 2^@order pages.
 * Returns the starting page index, or PHYMM_INVALID on failure.
 * Pages are removed from the free list; ref_count and order are left at 0 /
 * the allocated order so that phymm_reference_page (called later by the
 * mapping layer) can still check phymm_is_used()==0 on the first call.
 */
static unsigned buddy_alloc(unsigned order)
{
	unsigned o, idx, half;

	for (o = order; o <= MAX_BUDDY_ORDER; o++) {
		idx = buddy_pop(o);
		if (idx == PHYMM_INVALID)
			continue;

		/* Split down to requested order, pushing upper halves back */
		while (o > order) {
			o--;
			half = idx + (1u << o);
			if (half < phymm_end) {
				/* Mark split-off buddy as a free block head */
				phymm_pages[half].ref_count = 0;
				buddy_push(half, o);
			}
		}

		/* Store the allocated order on the head page for free_kernel */
		phymm_pages[idx].order = (unsigned char)order;
		/* Non-head pages within the block are marked PHYMM_ORDER_NONE */
		for (half = 1; half < (1u << order); half++) {
			if (idx + half < phymm_end)
				phymm_pages[idx + half].order =
					PHYMM_ORDER_NONE;
		}
		return idx;
	}
	return PHYMM_INVALID;
}

/*
 * Return a block starting at @idx of @order to the buddy system,
 * coalescing with free buddies as far as possible.
 *
 * Assumes caller holds buddy_lock and has verified the page is not reserved.
 */
static void buddy_free_block(unsigned idx, unsigned order)
{
	while (order < MAX_BUDDY_ORDER) {
		unsigned buddy = idx ^ (1u << order);

		if (!buddy_is_free_at_order(buddy, order))
			break;

		/* Coalesce: remove buddy from its list, merge into lower index */
		buddy_remove(buddy);
		if (buddy < idx)
			idx = buddy;
		order++;
	}
	phymm_pages[idx].ref_count = 0;
	phymm_pages[idx].flags = 0;
	buddy_push(idx, order);
}

/*
 * Public allocation / free API
 */

unsigned phymm_alloc_kernel(unsigned page_count)
{
	unsigned order = ceil_log2(page_count ? page_count : 1);
	unsigned idx;

	spinlock_lock(&buddy_lock);
	idx = buddy_alloc(order);
	spinlock_unlock(&buddy_lock);
	return idx;
}

unsigned phymm_alloc_user(void)
{
	unsigned idx;

	spinlock_lock(&buddy_lock);
	idx = buddy_alloc(0);
	spinlock_unlock(&buddy_lock);
	return idx;
}

void phymm_free_kernel(unsigned page_index, unsigned page_count)
{
	unsigned order;

	if (page_index == PHYMM_INVALID || page_index < phymm_begin ||
	    page_index >= phymm_end)
		return;

	/* Recover the order stored by phymm_alloc_kernel */
	order = phymm_pages[page_index].order;
	if (order > MAX_BUDDY_ORDER)
		order = ceil_log2(page_count ? page_count : 1);

	spinlock_lock(&buddy_lock);
	buddy_free_block(page_index, order);
	spinlock_unlock(&buddy_lock);
}

void phymm_free_user(unsigned page_index)
{
	if (page_index == PHYMM_INVALID || page_index < phymm_begin ||
	    page_index >= phymm_end)
		return;

	spinlock_lock(&buddy_lock);
	buddy_free_block(page_index, 0);
	spinlock_unlock(&buddy_lock);
}

/*
 * Reference counting (COW / sharing)
 */

unsigned phymm_reference_page(unsigned page_index)
{
	unsigned ret =
		__sync_add_and_fetch(&phymm_pages[page_index].ref_count, 1);
	if (ret == 1)
		phymm_used++;
	return ret;
}

unsigned phymm_dereference_page(unsigned page_index)
{
	unsigned ret =
		__sync_add_and_fetch(&phymm_pages[page_index].ref_count, -1);
	if (ret == 0)
		phymm_used--;
	return ret;
}

int phymm_is_cow(unsigned page_index)
{
	return ((int)__sync_add_and_fetch(&phymm_pages[page_index].ref_count,
					  0) > 1);
}

int phymm_is_used(unsigned page_index)
{
	return ((int)__sync_add_and_fetch(&phymm_pages[page_index].ref_count,
					  0) > 0);
}

void phymm_mark_dirty(unsigned page_index)
{
	__sync_fetch_and_or(&phymm_pages[page_index].flags, PHYMM_PAGE_DIRTY);
}

int phymm_is_dirty(unsigned page_index)
{
	return (phymm_pages[page_index].flags & PHYMM_PAGE_DIRTY) != 0;
}

void phymm_clear_dirty(unsigned page_index)
{
	__sync_fetch_and_and(&phymm_pages[page_index].flags,
			     (unsigned char)(~PHYMM_PAGE_DIRTY));
}

/*
 * Boot-time setup
 */

unsigned phymm_get_mgmt_pages(unsigned highest_mm_addr)
{
	unsigned page_count = highest_mm_addr / PAGE_SIZE;
	unsigned size = page_count * sizeof(phymm_page);
	return ((size - 1) / PAGE_SIZE) + 1;
}

void phymm_setup_mgmt_pages(unsigned start_page)
{
	unsigned i;
	unsigned addr;

	for (i = (start_page * PAGE_SIZE); i < (phymm_begin * PAGE_SIZE);
	     i += PAGE_SIZE) {
		addr = i + KERNEL_OFFSET;
		mm_kmap_page(addr);
	}
	RELOAD_CR3();
	addr = (start_page * PAGE_SIZE) + KERNEL_OFFSET;
	memset((void *)addr, 0, (phymm_begin - start_page) * PAGE_SIZE);
	phymm_pages = (phymm_page *)(addr);
}

/*
 * Add free pages in the range [@start, @end) (page indices) to the buddy
 * system, using the largest aligned blocks that fit.  Pages must already have
 * ref_count == 0.
 */
static void buddy_add_free_range(unsigned start, unsigned end)
{
	unsigned p = start;
	while (p < end) {
		unsigned order = MAX_BUDDY_ORDER;
		/* Shrink order until block is aligned and fits within [p, end) */
		while (order > 0) {
			unsigned block = 1u << order;
			if ((p & (block - 1)) == 0 && p + block <= end)
				break;
			order--;
		}
		/* Final check for order 0 */
		if (p + (1u << order) > end)
			break;
		buddy_push(p, order);
		p += 1u << order;
	}
}

/*
 * phymm_init – called once after phymm_setup_mgmt_pages.
 *
 * @mmap_addr: physical address of the multiboot memory map (accessible at
 *             mmap_addr + KERNEL_OFFSET since paging is now active).
 * @mmap_len:  total byte length of the memory map.
 *
 * Steps:
 *  1. Initialise buddy free lists and lock.
 *  2. Mark all pages in [phymm_begin, phymm_end) as reserved.
 *  3. Walk the multiboot mmap; for each type-1 (usable) region intersecting
 *     [phymm_begin, phymm_end), clear the reserved flag (ref_count = 0) and
 *     add the pages to the buddy free lists.
 */
void phymm_init(unsigned mmap_addr, unsigned mmap_len)
{
	unsigned i;
	memory_map_t *map;
	unsigned vmap_addr;

	/* 1. Initialise buddy system */
	spinlock_init(&buddy_lock);
	for (i = 0; i <= MAX_BUDDY_ORDER; i++)
		buddy_lists[i] = PHYMM_INVALID;

	/* 2. Presume every page in the managed range is reserved/non-RAM */
	for (i = phymm_begin; i < phymm_end; i++) {
		phymm_pages[i].ref_count = PHYMM_RESERVED;
		phymm_pages[i].order = PHYMM_ORDER_NONE;
		phymm_pages[i].next_free = PHYMM_INVALID;
		phymm_pages[i].prev_free = PHYMM_INVALID;
	}

	if (!mmap_addr || !mmap_len)
		return;

	/* Access the mmap through the kernel virtual window (phys + KERNEL_OFFSET).
	 * The initial 8 MB mapping covers physical 0..8MB; GRUB places the mmap
	 * in low memory so this is always valid. */
	vmap_addr = mmap_addr + KERNEL_OFFSET;
	map = (memory_map_t *)vmap_addr;

	/* 3. Walk all type-1 (usable RAM) entries */
	while ((unsigned)map < vmap_addr + mmap_len) {
		unsigned long long base, top;
		unsigned page_start, page_end;

		if (map->type == 1) {
			base = (unsigned long long)map->base_addr_low |
			       ((unsigned long long)map->base_addr_high << 32);
			top = base +
			      ((unsigned long long)map->length_low |
			       ((unsigned long long)map->length_high << 32));

			/* Convert to page indices, clamp to managed range */
			page_start = (unsigned)(base / PAGE_SIZE);
			page_end = (unsigned)(top / PAGE_SIZE);

			if (page_start < phymm_begin)
				page_start = phymm_begin;
			if (page_end > phymm_end)
				page_end = phymm_end;

			if (page_start >= page_end)
				goto next;

			/* Clear reserved flag for usable pages */
			for (i = page_start; i < page_end; i++) {
				phymm_pages[i].ref_count = 0;
				phymm_pages[i].order = PHYMM_ORDER_NONE;
			}

			/* Add to buddy free lists */
			buddy_add_free_range(page_start, page_end);
		}
next:
		map = (memory_map_t *)((unsigned)map + map->size +
				       sizeof(unsigned int));
	}
}
