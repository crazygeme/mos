/*
 * src/lib/kmalloc.c - Kernel heap allocator
 *
 * Algorithm: segregated explicit free-lists (NUM_BINS power-of-two bins)
 * with immediate right-coalescing on free().
 *
 * Block layout (allocated):
 *   ┌──────────┬─────────────────────────────┐
 *   │ hdr (4B) │ user data (8-aligned, n B)  │
 *   └──────────┴─────────────────────────────┘
 *   hdr = (total_block_size | ALLOC_BIT)
 *   total_block_size = HDR_SZ + ALIGN8(n)
 *   malloc(n) returns (blk + HDR_SZ); free(p) derives blk = p - HDR_SZ.
 *
 * Free block layout (total_size >= MIN_BLK = 12):
 *   ┌──────────┬──────────┬──────────┬───────────┐
 *   │ hdr (4B) │ next (4) │ prev (4) │  padding  │
 *   └──────────┴──────────┴──────────┴───────────┘
 *
 * Each new heap chunk gets a 4-byte epilogue sentinel (ALLOC_BIT only) at
 * its end, which stops right-coalescing at chunk boundaries.
 *
 * Bins: bin[i] holds free blocks of total size in (MIN_BLK<<(i-1), MIN_BLK<<i],
 * bin[NUM_BINS-1] is the catch-all for large blocks.
 * Each bin is a circular doubly-linked list with a sentinel head node
 * (bin_t, which shares the hdr+next+prev layout of real free blocks).
 *
 * Complexity: O(1) free (with right-coalesce), O(k) malloc (k = blocks scanned).
 */

#include <lib/klib.h>
#include <lib/lock.h>
#include <mm/mm.h>

/* ── Constants ───────────────────────────────────────────────────────────── */

#define HDR_SZ 4u
#define ALLOC_BIT 1u
/* Free block needs header + two pointers; must be at least 12 bytes total. */
#define MIN_BLK 12u
/* Round up to nearest 8-byte boundary. */
#define ALIGN8(n) (((unsigned)(n) + 7u) & ~7u)
#define NUM_BINS 16

/* ── Heap state ──────────────────────────────────────────────────────────── */

unsigned int heap_quota;
unsigned int heap_quota_high;
unsigned int heap_time;
unsigned int cur_block_top = KHEAP_BEGIN;

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

static inline void **fl_next(void *b)
{
	return (void **)((char *)b + HDR_SZ);
}

static inline void **fl_prev(void *b)
{
	return (void **)((char *)b + HDR_SZ + 4);
}

/* ── Segregated bins ─────────────────────────────────────────────────────── */

/*
 * Sentinel node for each bin.  Layout is intentionally compatible with a
 * real free block so that fl_next/fl_prev work uniformly:
 *   offset 0  : h    (dummy header, not a real size)
 *   offset 4  : next (== fl_next on &bins[i])
 *   offset 8  : prev (== fl_prev on &bins[i])
 */
typedef struct {
	unsigned h;
	void *next, *prev;
} bin_t;

static bin_t bins[NUM_BINS];

/* Return the bin index for a block of the given total size. */
static int size_to_bin(unsigned sz)
{
	unsigned bound = MIN_BLK;
	int i;

	for (i = 0; i < NUM_BINS - 1; i++, bound <<= 1) {
		if (sz <= bound)
			return i;
	}
	return NUM_BINS - 1;
}

static void fl_insert(void *b)
{
	int i = size_to_bin(blk_sz(b));
	void *head = &bins[i];
	void *first = *fl_next(head);

	*fl_next(b) = first;
	*fl_prev(b) = head;
	*fl_next(head) = b;
	*fl_prev(first) = b;
}

static void fl_remove(void *b)
{
	void *nx = *fl_next(b);
	void *pv = *fl_prev(b);

	*fl_next(pv) = nx;
	*fl_prev(nx) = pv;
}

static void bins_init(void)
{
	int i;

	for (i = 0; i < NUM_BINS; i++)
		bins[i].next = bins[i].prev = &bins[i];
}

/* ── Heap extension ──────────────────────────────────────────────────────── */

static unsigned kblk_raw(unsigned page_count)
{
	unsigned ret = cur_block_top;

	if (page_count == 0)
		return 0;
	if (cur_block_top + page_count * PAGE_SIZE >= KHEAP_END)
		return vm_alloc(page_count);
	cur_block_top += page_count * PAGE_SIZE;
	return ret;
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
	unsigned addr = kblk_raw(pages);

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
 * Searches bin[size_to_bin(need)] with first-fit, then higher bins
 * (all blocks there are guaranteed >= need).
 */
static void *find_free(unsigned need)
{
	int start = size_to_bin(need);
	void *head, *b;
	int i;

	/* First-fit scan within the starting bin */
	head = &bins[start];
	b = *fl_next(head);
	while (b != head) {
		if (blk_sz(b) >= need) {
			fl_remove(b);
			return b;
		}
		b = *fl_next(b);
	}

	/* Higher bins: any block is guaranteed >= need; take the first. */
	for (i = start + 1; i < NUM_BINS; i++) {
		head = &bins[i];
		b = *fl_next(head);
		if (b != head) {
			fl_remove(b);
			return b;
		}
	}
	return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void *malloc(unsigned size)
{
	unsigned need;
	void *blk;

	if (!size)
		return NULL;

	need = ALIGN8(size + HDR_SZ);
	if (need < MIN_BLK)
		need = MIN_BLK;

	spinlock_lock(&heap_lock);

	blk = find_free(need);
	if (!blk) {
		blk = extend_heap(need);
		if (!blk) {
			spinlock_unlock(&heap_lock);
			return NULL;
		}
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

	spinlock_unlock(&heap_lock);
	return (char *)blk + HDR_SZ;
}

void free(void *ptr)
{
	void *blk;
	unsigned sz;

	if (!ptr)
		return;

	spinlock_lock(&heap_lock);

	blk = (char *)ptr - HDR_SZ;
	sz = blk_sz(blk);
	heap_quota -= sz;

	set_hdr(blk, sz, 0);
	blk = coalesce_right(blk);
	fl_insert(blk);

	spinlock_unlock(&heap_lock);
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
