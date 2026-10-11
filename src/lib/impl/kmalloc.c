/*
 * src/lib/kmalloc.c - Kernel heap allocator
 *
 * Algorithm: segregated explicit free-lists with exact small-size bins
 * with immediate right-coalescing on free().
 *
 * Block layout (allocated):
 *   ┌──────────┬─────────────────────────────┐
 *   │ hdr (8B) │ user data (8-aligned, n B)  │
 *   └──────────┴─────────────────────────────┘
 *   hdr = (total_block_size | ALLOC_BIT)
 *   total_block_size = max(MIN_BLK, ALIGN8(HDR_SZ + n))
 *   malloc(n) returns (blk + HDR_SZ); free(p) derives blk = p - HDR_SZ.
 *
 * Free block layout (total_size >= MIN_BLK, pointer-sized links):
 *   ┌──────────┬──────────┬──────────┬───────────┐
 *   │ hdr (8B) │ list_entry links  │  padding  │
 *   └──────────┴──────────┴──────────┴───────────┘
 *
 * Each new heap chunk gets an 8-byte epilogue sentinel (ALLOC_BIT only) at
 * its end, which stops right-coalescing at chunk boundaries.
 *
 * Blocks through 1024 bytes have one bin per aligned size. Larger blocks use
 * power-of-two ranges; the final bin holds blocks above 2 GiB.
 * A bitmap identifies nonempty bins without traversing undersized blocks.
 * Each bin is a circular doubly-linked list with a sentinel head node
 * (list_entry embedded after the header of each free block).
 *
 * Free is O(1). Small allocation searches a fixed-size bitmap. Large allocation
 * may scan blocks in its initial size range before selecting a higher bin.
 */

#include <lib/klib.h>
#include <lib/lock.h>
#include <mm/mm.h>

/* ── Constants ───────────────────────────────────────────────────────────── */

/* Keep payloads and every split block 8-aligned, including on i386.
 * A 4-byte header misaligns heap-backed 64-bit atomic CPU counters and can
 * make locked updates straddle cache lines (split locks).
 */
#define HDR_SZ 8u
#define ALLOC_BIT 1u
/* Header and two pointers total 16 bytes on i386, 24 bytes on x86-64. */
#define MIN_BLK (HDR_SZ + 2u * sizeof(void *))
/* Round up to nearest 8-byte boundary. */
#define ALIGN8(n) (((unsigned)(n) + 7u) & ~7u)
#define SMALL_BIN_MAX 1024u
#define SMALL_BIN_COUNT ((SMALL_BIN_MAX - MIN_BLK) / 8u + 1u)
#define LARGE_BIN_COUNT 22u
#define NUM_BINS (SMALL_BIN_COUNT + LARGE_BIN_COUNT)
#define BIN_BITMAP_WORDS ((NUM_BINS + 31u) / 32u)

/* ── Heap state ──────────────────────────────────────────────────────────── */

unsigned int heap_quota;
unsigned int heap_quota_high;
unsigned int heap_time;
vaddr_t cur_block_top = KHEAP_BEGIN;

static spinlock_t heap_lock;

/* ── Physical block helpers ──────────────────────────────────────────────── */

static inline unsigned blk_sz(void *b)
{
	return *(unsigned *)b & ~ALLOC_BIT;
}

static inline int blk_is_alloc(void *b)
{
	return *(unsigned *)b & ALLOC_BIT;
}

/* Write header; total_size includes HDR_SZ. */
static inline void set_hdr(void *b, unsigned total_size, int alloc)
{
	*(unsigned *)b = total_size | (alloc ? ALLOC_BIT : 0u);
}

/* Physical right neighbour. */
static inline void *blk_next(void *b)
{
	return (char *)b + blk_sz(b);
}

/* ── Free-list pointers (stored after header inside free blocks) ─────────── */

static inline list_entry *free_link(void *block)
{
	return (list_entry *)((char *)block + HDR_SZ);
}

static inline void *free_block(list_entry *link)
{
	return (char *)link - HDR_SZ;
}

static list_entry bins[NUM_BINS];
static unsigned bin_bitmap[BIN_BITMAP_WORDS];

/* Return the bin index for a block of the given total size. */
static unsigned size_to_bin(unsigned sz)
{
	unsigned bound = SMALL_BIN_MAX * 2u;
	unsigned i;

	if (sz <= SMALL_BIN_MAX)
		return (sz - MIN_BLK) / 8u;
	for (i = SMALL_BIN_COUNT; i < NUM_BINS - 1; i++, bound <<= 1) {
		if (sz <= bound)
			return i;
	}
	return NUM_BINS - 1;
}

static void fl_insert(void *block)
{
	unsigned bin = size_to_bin(blk_sz(block));
	list_insert_head(&bins[bin], free_link(block));
	bin_bitmap[bin / 32u] |= 1u << (bin % 32u);
}

static void fl_remove(void *block)
{
	unsigned bin = size_to_bin(blk_sz(block));
	list_remove_entry(free_link(block));
	if (list_is_empty(&bins[bin]))
		bin_bitmap[bin / 32u] &= ~(1u << (bin % 32u));
}

static void bins_init(void)
{
	for (unsigned i = 0; i < NUM_BINS; i++)
		list_init(&bins[i]);
	memset(bin_bitmap, 0, sizeof(bin_bitmap));
}

static unsigned next_nonempty_bin(unsigned start)
{
	if (start >= NUM_BINS)
		return NUM_BINS;
	unsigned word = start / 32u;
	unsigned active = bin_bitmap[word] & (~0u << (start % 32u));
	for (;;) {
		if (active)
			return word * 32u + __builtin_ctz(active);
		if (++word == BIN_BITMAP_WORDS)
			return NUM_BINS;
		active = bin_bitmap[word];
	}
}

/* ── Heap extension ──────────────────────────────────────────────────────── */

static vaddr_t kblk_raw(unsigned page_count)
{
	vaddr_t ret = 0;
	int irq;

	if (page_count == 0)
		return 0;
	spinlock_lock(&heap_lock, &irq);
	if (cur_block_top + page_count * PAGE_SIZE < KHEAP_END) {
		ret = cur_block_top;
		cur_block_top += page_count * PAGE_SIZE;
	}
	spinlock_unlock(&heap_lock, irq);
	/* Physical allocation may reclaim caches whose records use free(). */
	return ret ? ret : vm_alloc(page_count);
}

/*
 * Allocate at least min_sz bytes of free block from the OS.
 * Places an ALLOC_BIT epilogue sentinel at the chunk end to stop
 * right-coalescing at the boundary.
 * Returns NULL on failure; otherwise returns the free block (not inserted
 * into any bin yet).
 */
static void *extend_heap(unsigned min_sz)
{
	unsigned pages = (min_sz + HDR_SZ + PAGE_SIZE - 1) / PAGE_SIZE;
	unsigned chunk = pages * PAGE_SIZE;
	vaddr_t addr = kblk_raw(pages);

	if (!addr)
		return NULL;

	/* Epilogue sentinel at end of chunk (just ALLOC_BIT, size = 0 sentinel) */
	*(unsigned *)(addr + chunk - HDR_SZ) = ALLOC_BIT;

	/* Free block occupies the chunk minus the sentinel */
	unsigned sz = chunk - HDR_SZ;
	void *blk = (void *)addr;

	set_hdr(blk, sz, 0);
	return blk;
}

/* ── Coalescing ──────────────────────────────────────────────────────────── */

/*
 * Right-coalesce: if the physical right neighbour is free, merge it into b.
 * The neighbour is removed from its bin before merging.
 * Returns b (possibly grown).
 */
static void *coalesce_right(void *b)
{
	void *next = blk_next(b);

	if (!blk_is_alloc(next)) {
		fl_remove(next);
		set_hdr(b, blk_sz(b) + blk_sz(next), 0);
	}
	return b;
}

/* ── Free-list search ────────────────────────────────────────────────────── */

/*
 * Find and remove a free block with total size >= need.
 * Small bins contain only fitting blocks. Large bins require first-fit in
 * the initial range. Every block in a higher bin fits the request.
 */
static void *find_free(unsigned need)
{
	unsigned start = size_to_bin(need);
	unsigned bin;
	if (start >= SMALL_BIN_COUNT) {
		for (list_entry *node = bins[start].next; node != &bins[start];
		     node = node->next) {
			void *block = free_block(node);
			if (blk_sz(block) >= need) {
				fl_remove(block);
				return block;
			}
		}
		start++;
	}
	bin = next_nonempty_bin(start);
	if (bin == NUM_BINS)
		return NULL;
	void *block = free_block(bins[bin].next);
	fl_remove(block);
	return block;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void *malloc(unsigned size)
{
	unsigned need;
	void *blk;
	int irq;

	if (!size)
		return NULL;

	need = ALIGN8(size + HDR_SZ);
	if (need < MIN_BLK)
		need = MIN_BLK;

	spinlock_lock(&heap_lock, &irq);

	blk = find_free(need);
	if (!blk) {
		spinlock_unlock(&heap_lock, irq);
		blk = extend_heap(need);
		if (!blk)
			return NULL;
		spinlock_lock(&heap_lock, &irq);
	}

	/* Split if the remainder would form a valid free block. */
	unsigned rem = blk_sz(blk) - need;

	if (rem >= MIN_BLK) {
		set_hdr(blk, need, 1);
		void *lo = (char *)blk + need;

		set_hdr(lo, rem, 0);
		fl_insert(lo);
	} else {
		set_hdr(blk, blk_sz(blk), 1);
	}

	heap_quota += blk_sz(blk);
	if (heap_quota > heap_quota_high)
		heap_quota_high = heap_quota;

	spinlock_unlock(&heap_lock, irq);
	return (char *)blk + HDR_SZ;
}

void free(void *ptr)
{
	void *blk;
	unsigned sz;
	int irq;

	if (!ptr)
		return;

	spinlock_lock(&heap_lock, &irq);

	blk = (char *)ptr - HDR_SZ;
	sz = blk_sz(blk);
	heap_quota -= sz;

	set_hdr(blk, sz, 0);
	blk = coalesce_right(blk);
	fl_insert(blk);

	spinlock_unlock(&heap_lock, irq);
}

void *zalloc(unsigned size)
{
	void *p = malloc(size);

	if (p)
		memset(p, 0, size);
	return p;
}

void kmalloc_init(void)
{
	spinlock_init(&heap_lock);
	bins_init();
}
