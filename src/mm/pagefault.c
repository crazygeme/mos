#include <int/int.h>
#include <mm/pagefault.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <mm/phymm.h>
#include <ps/ps.h>
#include <fs/fs.h>
#include <lib/klib.h>
#include <lib/rbtree.h>
#include <lib/list.h>
#include <lib/lock.h>
#include <hw/time.h>
#include <config.h>
#include <macro.h>
#include <ext4.h>

unsigned page_fault_cow = 0;
unsigned page_fault_invalid = 0;
unsigned page_fault_file = 0;
unsigned page_fault_file_read = 0;
unsigned page_fault_perm = 0;
unsigned page_fault_file_cache_hit = 0;
unsigned long long page_fault_cow_spent = 0;
unsigned long long page_fault_invalid_spent = 0;
unsigned long long page_fault_file_spent = 0;
unsigned long long page_fault_file_search_spent = 0;
unsigned long long page_fault_perm_spent = 0;
extern unsigned cache_count;

/*
 * LRU mmap cache — for read-only MAP_PRIVATE file-backed pages only.
 *
 * Hash table for O(1) lookup keyed by (ino, offset).
 * Doubly-linked LRU list: tail = most recently used, head = least recently used.
 * On hit:  move entry to list tail (MRU position).
 * On miss + full: evict the head (LRU) entry, insert new entry at tail.
 */

typedef struct _mmap_cache_key {
	unsigned ino;
	unsigned offset;
} mmap_cache_key;

typedef struct _mmap_cache_entry {
	mmap_cache_key key; /* must be first — hash stores &entry->key */
	unsigned phy;
	list_entry lru;
} mmap_cache_entry;

static hash_table *mmap_cache = NULL;
static list_entry mmap_lru; /* tail = MRU, head = LRU */
static mutex_t mmap_cache_lock;
static unsigned zero_page = 0;
static unsigned zero_page_phy = 0;

/*
 * shared_map — non-evicting cache for MAP_SHARED pages.
 *
 * Keyed by (id, offset) where:
 *   - File-backed MAP_SHARED: id = inode number, offset = file offset
 *   - Anonymous MAP_SHARED:  id = anon_id | ANON_SHARED_BIT, offset = offset within mapping
 *
 * Entries are never evicted; the map holds a permanent reference to each page.
 * This ensures all processes sharing a region always see the same physical page.
 */
#define ANON_SHARED_BIT 0x80000000U

typedef struct _shared_map_entry {
	mmap_cache_key key; /* must be first */
	unsigned phy;
} shared_map_entry;

static hash_table *shared_map = NULL;
static mutex_t shared_map_lock;

static void pf_process(intr_frame *frame);

static int mmap_key_comp(const void *k1, const void *k2)
{
	const mmap_cache_key *key1 = k1;
	const mmap_cache_key *key2 = k2;
	int ret = key1->ino - key2->ino;
	if (ret == 0)
		ret = (int)key1->offset - (int)key2->offset;
	return ret;
}

static void mmap_entry_evict(const key_value_pair *pair)
{
	mmap_cache_entry *evict = pair->val;
	phymm_dereference_page(PHY_TO_PAGE_IDX(evict->phy));
	free(evict);
}

static void shared_map_evict(const key_value_pair *pair)
{
	shared_map_entry *evict = pair->val;
	phymm_dereference_page(PHY_TO_PAGE_IDX(evict->phy));
	free(evict);
}

void pf_init()
{
	int_register(0xe, pf_process, 0, 0);
	mmap_cache = hash_create(mmap_key_comp, mmap_entry_evict);
	list_init(&mmap_lru);
	mutex_init(&mmap_cache_lock);
	shared_map = hash_create(mmap_key_comp, shared_map_evict);
	mutex_init(&shared_map_lock);
	zero_page = vm_alloc(1);
	memset(zero_page, 0, PAGE_SIZE);
	zero_page_phy = VIRT_TO_PHY(zero_page);
}

/* Lookup (ino, offset) in the LRU cache.  On hit, promote to MRU position.
 * Returns the physical address, or 0 on miss. */
static unsigned mmap_cache_find(unsigned ino, unsigned offset)
{
	mmap_cache_key tmp = { .ino = ino, .offset = offset };
	key_value_pair *pair;
	mmap_cache_entry *entry;

	if (PAGE_CACHE_SIZE == 0)
		return 0;

	if (ino == 0)
		return 0;

	mutex_lock(&mmap_cache_lock);
	pair = hash_find(mmap_cache, &tmp);
	if (!pair) {
		mutex_unlock(&mmap_cache_lock);
		return 0;
	}
	entry = pair->val;
	/* Promote to MRU (move to tail of LRU list). */
	list_remove_entry(&entry->lru);
	list_insert_tail(&mmap_lru, &entry->lru);
	mutex_unlock(&mmap_cache_lock);
	return entry->phy;
}

/* Insert (ino, offset, phy) into the LRU cache.
 * If the cache is full, evict the LRU entry first (single eviction). */
static void mmap_cache_add(unsigned ino, unsigned offset, unsigned phy)
{
	mmap_cache_entry *entry;
	mmap_cache_key tmp = { .ino = ino, .offset = offset };

	if (ino == 0 || phy == 0)
		return;

	if (PAGE_CACHE_SIZE == 0)
		return;

	mutex_lock(&mmap_cache_lock);

	/* Skip if already cached (e.g. two faults racing on the same page). */
	if (hash_find(mmap_cache, &tmp)) {
		mutex_unlock(&mmap_cache_lock);
		return;
	}

	/* Evict the single LRU entry when the cache is full. */
	if (hash_size(mmap_cache) >= PAGE_CACHE_SIZE) {
		mmap_cache_entry *evict = container_of(
			list_remove_head(&mmap_lru), mmap_cache_entry, lru);
		hash_remove(mmap_cache, &evict->key);
		cache_count--;
	}

	entry = malloc(sizeof(*entry));
	entry->key.ino = ino;
	entry->key.offset = offset;
	entry->phy = phy;
	list_insert_tail(&mmap_lru, &entry->lru);
	hash_insert(mmap_cache, &entry->key, entry);
	phymm_reference_page(PHY_TO_PAGE_IDX(phy));
	cache_count++;

	mutex_unlock(&mmap_cache_lock);
}

/* Lookup (id, offset) in the shared map.
 * Returns the physical address, or 0 on miss. */
static unsigned shared_map_find(unsigned id, unsigned offset)
{
	mmap_cache_key tmp = { .ino = id, .offset = offset };
	key_value_pair *pair;
	shared_map_entry *entry;

	mutex_lock(&shared_map_lock);
	pair = hash_find(shared_map, &tmp);
	if (!pair) {
		mutex_unlock(&shared_map_lock);
		return 0;
	}
	entry = pair->val;
	mutex_unlock(&shared_map_lock);
	return entry->phy;
}

/* Insert (id, offset, phy) into the shared map.
 * Bumps the page ref count so the page cannot be freed while in the map. */
static void shared_map_add(unsigned id, unsigned offset, unsigned phy)
{
	shared_map_entry *entry;
	mmap_cache_key tmp = { .ino = id, .offset = offset };

	if (id == 0 || phy == 0)
		return;

	mutex_lock(&shared_map_lock);

	/* Skip if already present (two faults on the same page before first one completes). */
	if (hash_find(shared_map, &tmp)) {
		mutex_unlock(&shared_map_lock);
		return;
	}

	entry = malloc(sizeof(*entry));
	entry->key.ino = id;
	entry->key.offset = offset;
	entry->phy = phy;
	hash_insert(shared_map, &entry->key, entry);
	phymm_reference_page(PHY_TO_PAGE_IDX(phy));

	mutex_unlock(&shared_map_lock);
}

/*
31                4                             0
+-----+-...-+-----+-----+-----+-----+-----+-----+
|     Reserved    | I/D | RSVD| U/S | W/R |  P  |
+-----+-...-+-----+-----+-----+-----+-----+-----+

*/
#define PF_MASK_P 0x00000001 // page valid
#define PF_MASK_RW 0x00000002 // write access
#define PF_MASK_US 0x00000004 // user mode access
#define PF_MASK_RSVD 0x00000008
extern phymm_page *phymm_pages;

/*
 * Handle page fault with fd attached.
 *
 * MAP_SHARED: pages are placed in shared_map (non-evicting) so every process
 * that faults the same (ino, offset) pair maps the same physical page.
 * Pages are mapped read-only; a subsequent write fault goes through
 * pf_handle_permission which flips the PTE writable and marks the page dirty.
 *
 * MAP_PRIVATE readonly: pages are placed in the LRU mmap_cache and mapped
 * read-only.  A write fault triggers COW.
 *
 * MAP_PRIVATE writable: each process gets its own private copy immediately.
 */
static void pf_handle_invalid_file_map(unsigned address, file *f, int offset,
				       int prot, int flag)
{
	unsigned phy = 0;
	size_t rcnt = 0;
	int mmflag;
	unsigned long long begin = TestControl.profiling ? time_wall_us() : 0;
	int readonly = !(prot & PROT_WRITE);
	int shared = flag & MAP_SHARED;
	unsigned ino = f->f_inode->i_ino;

	page_fault_file++;

	/* Try the appropriate cache first. */
	if (shared) {
		phy = shared_map_find(ino, offset);
		if (phy != 0) {
			page_fault_file_cache_hit++;
			/* Read-only: write fault handled by pf_handle_permission. */
			mm_map_page(address, phy, PAGE_ENTRY_USER_CODE);
			INVLPG(address);
			goto DONE;
		}
	} else if (readonly) {
		phy = mmap_cache_find(ino, offset);

		if (TestControl.profiling)
			page_fault_file_search_spent += time_wall_us() - begin;

		if (phy != 0) {
			page_fault_file_cache_hit++;
			mm_map_page(address, phy, PAGE_ENTRY_USER_CODE);
			INVLPG(address);
			goto DONE;
		}
	}

	/*
	 * Cache miss — allocate a fresh page, map it writable so we can fill it,
	 * then read the file content into it.
	 */
	phy = phymm_alloc_user() * PAGE_SIZE;

	mm_map_page(address, phy, PAGE_ENTRY_USER_DATA);
	INVLPG(address);

	if (f->f_inode->i_op && f->f_inode->i_op->read_page) {
		f->f_inode->i_op->read_page(f->f_inode, offset, address);
		rcnt = PAGE_SIZE;
	} else {
		ext4_file *ff = f->f_inode->i_private;

		/*
		 * Some programs map a region larger than the file size;
		 * return a zero page in that case.
		 */
		if (ext4_fseek(ff, offset, SEEK_SET) != EOK) {
			memset(address, 0, PAGE_SIZE);
			goto READ_DONE;
		}

		/*
		 * Read the file.  If fewer than PAGE_SIZE bytes are returned, zero the
		 * remainder (many programs depend on this).
		 */
		if (ext4_fread(ff, address, PAGE_SIZE, &rcnt) != EOK)
			klog("FAIL: mmap: read to buffer %x, size %x\n",
			     address, PAGE_SIZE);

		if (rcnt < PAGE_SIZE)
			memset(address + rcnt, 0, PAGE_SIZE - rcnt);
	}

READ_DONE:
	page_fault_file_read += rcnt;

	if (shared) {
		/*
		 * Strip the writable bit so the first write goes through
		 * pf_handle_permission, which marks the page dirty for
		 * writeback and then flips the PTE writable.
		 */
		mmflag = mm_get_map_flag(address);
		mmflag &= ~PAGE_ENTRY_WRITABLE;
		mm_set_map_flag(address, mmflag);
		INVLPG(address);
		shared_map_add(ino, offset, phy);
	} else if (readonly) {
		mmflag = mm_get_map_flag(address);
		mmflag &= ~PAGE_ENTRY_WRITABLE;
		mm_set_map_flag(address, mmflag);
		INVLPG(address);
		mmap_cache_add(ino, offset, phy);
	}
	/* else: writable MAP_PRIVATE — leave the PTE writable, no cache. */

DONE:
	if (TestControl.profiling)
		page_fault_file_spent += (time_wall_us() - begin);
}

/*
 * Handle page fault which has no physical page attached.
 *
 * MAP_SHARED anonymous (anon_id != 0):
 *   All processes sharing the same anon_id must map the same physical page.
 *   Use shared_map keyed by (anon_id | ANON_SHARED_BIT, offset).
 *   Pages are mapped read-only; write faults go through pf_handle_permission.
 *
 * MAP_PRIVATE writable: allocate a fresh zero page per fault.
 *
 * MAP_PRIVATE read-only: map the global zero page read-only; a write fault
 *   triggers COW.
 */
static void pf_handle_invalid_memory(unsigned address, vm_region *region,
				     int offset)
{
	int prot = region->prot;
	int flag = region->flag;
	unsigned long long begin = TestControl.profiling ? time_wall_us() : 0;
	int mmflag;

	page_fault_invalid++;

	if ((flag & MAP_SHARED) && region->anon_id != 0) {
		unsigned id = region->anon_id | ANON_SHARED_BIT;
		unsigned phy = shared_map_find(id, offset);

		if (phy != 0) {
			/* Hit — map read-only so writes go through pf_handle_permission. */
			mm_map_page(address, phy, PAGE_ENTRY_USER_CODE);
			INVLPG(address);
			goto DONE;
		}

		/* Miss — allocate, zero, strip writable, and register in shared_map. */
		phy = phymm_alloc_user() * PAGE_SIZE;
		mm_map_page(address, phy, PAGE_ENTRY_USER_DATA);
		INVLPG(address);
		memset(address, 0, PAGE_SIZE);

		mmflag = mm_get_map_flag(address);
		mmflag &= ~PAGE_ENTRY_WRITABLE;
		mm_set_map_flag(address, mmflag);
		INVLPG(address);

		shared_map_add(id, offset, phy);
		goto DONE;
	}

	if (prot & PROT_WRITE) {
		mm_map_page(address, 0, PAGE_ENTRY_USER_DATA);
		INVLPG(address);
		memset(address, 0, PAGE_SIZE);
	} else {
		/*
		 * For a read-only anonymous page, share the global zero page.
		 * mm_map_page bumps zero_page's ref_count to > 1, so
		 * phymm_is_cow() fires on the next write fault and COW kicks
		 * in — the shared zero page is never dirtied.
		 */
		mm_map_page(address, zero_page_phy, PAGE_ENTRY_USER_CODE);
		INVLPG(address);
	}

DONE:
	if (TestControl.profiling)
		page_fault_invalid_spent += (time_wall_us() - begin);
}

/*
 * pf_prefetch_pages — read-ahead for readonly MAP_PRIVATE file-backed pages.
 *
 * After a demand fault on 'fault_addr', prefetch up to (PAGE_PREFETCH_N - 1)
 * pages before and PAGE_PREFETCH_N pages after it.  Each prefetched page is:
 *   - mapped into the current process's page table (no future fault needed), and
 *   - inserted into the LRU page cache at the MRU end.
 *
 * LRU policy: MRU insertion.  For sequential workloads this is optimal —
 * prefetched pages will be accessed soon and should stay hot.  Pages that are
 * never accessed drift naturally to the LRU head and are evicted first.
 */
static void pf_prefetch_pages(unsigned fault_addr, file *f, vm_region *region)
{
	unsigned ino = f->f_inode->i_ino;
	uint64_t file_size = f->f_inode->i_size;
	int i;

	for (i = -(PAGE_PREFETCH_N - 1); i <= (int)PAGE_PREFETCH_N; i++) {
		unsigned addr;
		int file_offset;
		unsigned phy;
		unsigned page_idx;
		size_t rcnt;
		int mmflag;

		if (i == 0)
			continue;

		/* Guard against unsigned underflow when scanning backwards. */
		if (i < 0 && fault_addr < (unsigned)((-i) * (int)PAGE_SIZE))
			continue;

		addr = fault_addr + (unsigned)(i * (int)PAGE_SIZE);

		/* Clamp to vm_region bounds. */
		if (addr < region->begin || addr >= region->end)
			continue;

		/* Skip pages beyond the end of the file. */
		file_offset = region->offset + (int)(addr - region->begin);
		if ((uint64_t)file_offset >= file_size)
			continue;

		/* Skip if already present in the current page table. */
		if (mm_get_map_flag(addr) != 0)
			continue;

		/* Cache hit: map read-only without touching the disk. */
		phy = mmap_cache_find(ino, file_offset);
		if (phy != 0) {
			mm_map_page(addr, phy, PAGE_ENTRY_USER_CODE);
			INVLPG(addr);
			continue;
		}

		/* Cache miss: allocate a page, read from disk, cache, and map. */
		page_idx = phymm_alloc_user();
		if (page_idx == PHYMM_INVALID)
			break; /* OOM — stop prefetching */
		phy = page_idx * PAGE_SIZE;

		mm_map_page(addr, phy, PAGE_ENTRY_USER_DATA);
		INVLPG(addr);

		if (f->f_inode->i_op && f->f_inode->i_op->read_page) {
			f->f_inode->i_op->read_page(f->f_inode, file_offset,
						    (void *)addr);
		} else {
			ext4_file *ff = f->f_inode->i_private;
			if (ext4_fseek(ff, file_offset, SEEK_SET) != EOK) {
				mm_unmap_page(addr);
				INVLPG(addr);
				continue;
			}
			rcnt = 0;
			if (ext4_fread(ff, (void *)addr, PAGE_SIZE, &rcnt) !=
			    EOK)
				klog("prefetch: read fail ino=%u off=%d\n", ino,
				     file_offset);
			if (rcnt < PAGE_SIZE)
				memset((void *)(addr + rcnt), 0,
				       PAGE_SIZE - rcnt);
		}

		/* Strip writable bit, then insert into LRU cache. */
		mmflag = mm_get_map_flag(addr);
		mmflag &= ~PAGE_ENTRY_WRITABLE;
		mm_set_map_flag(addr, mmflag);
		INVLPG(addr);
		mmap_cache_add(ino, file_offset, phy);
	}
}

/*
 * Handle page fault which has no physical page.
 */
static int pf_handle_page_invalid(unsigned cr2)
{
	vm_region *region;
	int this_offset;
	unsigned this_begin;
	task_struct *cur = CURRENT_TASK();

	this_begin = cr2;

	/*
	 * Find out the map region first. The `region` structure
	 * will tell us whether it's file map or not.
	 */
	region = vm_find_map(cur->user->vm, this_begin);
	if (region)
		goto found;

	/*
	 * Stack auto-grow: if the fault is just below the current
	 * stack bottom and within the allowed stack zone, extend the
	 * stack mapping downward to cover the faulting page.
	 */
	if (cur->user->stack_bottom > USER_ZONE_END &&
	    this_begin >= USER_ZONE_END &&
	    this_begin < cur->user->stack_bottom) {
		unsigned minimal_growse = USER_STACK_INIT_PAGES * PAGE_SIZE;
		unsigned required_grow = cur->user->stack_bottom - this_begin;
		unsigned grow_size = required_grow > minimal_growse ?
					     required_grow :
					     minimal_growse;
		cur->user->stack_bottom = cur->user->stack_bottom - grow_size;
		do_mmap_kernel(cur->user->stack_bottom, grow_size,
			       PROT_READ | PROT_WRITE, MAP_FIXED, NULL, 0);
		region = vm_find_map(cur->user->vm, this_begin);
	}

	if (region)
		goto found;

	return 0;

found:
	/* PROT_NONE: no access permitted — treat as unmapped. */
	if (region->prot == PROT_NONE)
		return 0;

	this_offset = region->offset + (this_begin - region->begin);

	if (region->fp != NULL) {
		pf_handle_invalid_file_map(this_begin, region->fp, this_offset,
					   region->prot, region->flag);
		cur->pf_major++;
		if (PAGE_PREFETCH_N > 0 && !(region->prot & PROT_WRITE) &&
		    !(region->flag & MAP_SHARED))
			pf_prefetch_pages(this_begin, region->fp, region);
	} else {
		pf_handle_invalid_memory(this_begin, region, this_offset);
		cur->pf_minor++;
	}

	return 1;
}

/*
 * wp_page_copy — COW resolution: allocate a private page and copy content.
 *
 * Called when a write fault hits a shared (ref_count > 1) page.  Mirrors
 * Linux's wp_page_copy(): allocate, copy, swap in the new PTE.
 */
static void wp_page_copy(unsigned cr2)
{
	unsigned vir = cr2;
	unsigned new_mem;
	int flag;
	unsigned long long begin = TestControl.profiling ? time_wall_us() : 0;

	page_fault_cow++;

	/* Allocate a new page mapped into kernel space for the copy. */
	new_mem = vm_alloc(1);
	memcpy(new_mem, vir, PAGE_SIZE);

	/* Unmap the shared page from this process. */
	flag = mm_get_map_flag(vir);
	mm_unmap_page(vir);

	/*
	 * Pin the new page before removing its kernel mapping so the physical
	 * page cannot be reclaimed between mm_kunmap_page and mm_map_page.
	 */
	phymm_reference_page(VIRT_TO_PAGE_IDX(new_mem));
	mm_kunmap_page(new_mem);

	/* Install the private writable PTE. */
	flag |= PAGE_ENTRY_PRESENT | PAGE_ENTRY_WRITABLE;
	mm_map_page(vir, VIRT_TO_PHY(new_mem), flag);

	phymm_dereference_page(VIRT_TO_PAGE_IDX(new_mem));

	RELOAD_CR3();

	if (TestControl.profiling)
		page_fault_cow_spent += (time_wall_us() - begin);
}

/*
 * wp_page_reuse — sole owner: upgrade a read-only PTE to writable in place.
 *
 * Called when ref_count == 1 (this process is the only user of the page).
 * Mirrors Linux's wp_page_reuse(): no copy needed, just flip the PTE bit.
 */
static void wp_page_reuse(unsigned cr2)
{
	int flag;
	unsigned long long begin = TestControl.profiling ? time_wall_us() : 0;

	page_fault_perm++;
	flag = mm_get_map_flag(cr2);
	flag |= PAGE_ENTRY_WRITABLE;
	mm_set_map_flag(cr2, flag);
	INVLPG(cr2);

	if (TestControl.profiling)
		page_fault_perm_spent += (time_wall_us() - begin);
}

/*
 * do_wp_page — handle a write fault on a present but read-only PTE.
 *
 * Mirrors Linux's do_wp_page():
 *
 * MAP_SHARED: upgrade the PTE writable in place — all sharers see the same
 *   physical page (MAP_SHARED contract).  Mark dirty for file-backed regions.
 *
 * MAP_PRIVATE (ref_count > 1): page is still shared with a fork sibling or
 *   the file cache.  Call wp_page_copy() to get a private copy.
 *
 * MAP_PRIVATE (ref_count == 1): sole owner; the PTE was left read-only by
 *   fork but the sibling has since broken away.  Call wp_page_reuse().
 *
 * No VMA or VMA without PROT_WRITE → SIGSEGV.
 */
static int do_wp_page(unsigned cr2)
{
	task_struct *cur = CURRENT_TASK();
	vm_region *region;
	unsigned page_index;

	region = vm_find_map(cur->user->vm, cr2);

	if (!region || !(region->prot & PROT_WRITE))
		return 0;

	if (region->flag & MAP_SHARED) {
		wp_page_reuse(cr2);
		if (region->fp != NULL) {
			unsigned page_index = mm_get_attached_page_index(cr2);
			phymm_mark_dirty(page_index);
		}
		return 1;
	}

	page_index = mm_get_attached_page_index(cr2);
	if (phymm_is_cow(page_index))
		wp_page_copy(cr2);
	else
		wp_page_reuse(cr2);

	return 1;
}

static void pf_dump_region(vm_region *region, void *data)
{
	(void)data;
	klog("    %08x-%08x prot=%x flag=%x %s\n", region->begin, region->end,
	     region->prot, region->flag,
	     region->fp ? region->fp->f_name : "anon");
}

extern void do_signal(intr_frame *frame);
extern void do_exit(unsigned encoded_status);

static void pf_process(intr_frame *frame)
{
	unsigned cr2;
	unsigned error = frame->error_code;
	task_struct *cur;
	int int_enable = 0;

	/*
	 * Save old interrupt state first.
	 */
	sched_disable();
	int_enable = int_intr_enable();

	/*
	 * cr2 tells you which page is invalid.
	 * frame->error_code tells you what type of error we are handling.
	 * `error_code` is actually pushed by CPU.
	 */

	LOAD_CR2(cr2);
	cr2 = cr2 & PAGE_SIZE_MASK;

	if (!(error & PF_MASK_P)) {
		if (pf_handle_page_invalid(cr2))
			goto Done;
		goto NOT_HANDLED;
	}

	if (error & PF_MASK_RW) {
		if (do_wp_page(cr2))
			goto Done;
		goto NOT_HANDLED;
	}

NOT_HANDLED:
	cur = CURRENT_TASK();

	if ((unsigned)frame->eip < KERNEL_OFFSET ||
	    (cr2 < KERNEL_OFFSET && cr2 > 0x1000)) {
		klog("segfault: error code %x, address %x, eip %x, cmd %s\n",
		     frame->error_code, cr2, frame->eip, cur->user->command);
		if (cur->user->vm) {
			klog("  vm regions:\n");
			vm_enum(cur->user->vm, pf_dump_region, NULL);
		}

		cur->signal->sig_pending |= (1UL << (SIGSEGV - 1));
		do_signal(frame);
		/* Falls through only if SIGSEGV is masked or SIG_IGN; force-terminate. */
		do_exit(SIGSEGV);
		goto Done;
	}

	klog("segfault: error code %x, address %x, eip %x\n", frame->error_code,
	     cr2, frame->eip);

	DIE();

Done:
	int_intr_setlevel(int_enable);
	sched_enable();
}
