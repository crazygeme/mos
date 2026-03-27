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
	ext4_file *ff;
	size_t rcnt = 0;
	int mmflag;
	unsigned long long begin = TestControl.profiling ? time_now_us() : 0;
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
			page_fault_file_search_spent += time_now_us() - begin;

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

	ff = f->f_inode->i_private;

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
		klog("FAIL: mmap: read to buffer %x, size %x\n", address,
		     PAGE_SIZE);

	if (rcnt < PAGE_SIZE)
		memset(address + rcnt, 0, PAGE_SIZE - rcnt);

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
		page_fault_file_spent += (time_now_us() - begin);
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
	unsigned long long begin = TestControl.profiling ? time_now_us() : 0;
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
		page_fault_invalid_spent += (time_now_us() - begin);
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
	if (!region)
		return 0;

	/* PROT_NONE: no access permitted — treat as unmapped. */
	if (region->prot == PROT_NONE)
		return 0;

	this_offset = region->offset + (this_begin - region->begin);

	if (region->fp != NULL) {
		pf_handle_invalid_file_map(this_begin, region->fp, this_offset,
					   region->prot, region->flag);
		cur->pf_major++;
	} else {
		pf_handle_invalid_memory(this_begin, region, this_offset);
		cur->pf_minor++;
	}

	return 1;
}

/*
 * Handle page fault which needs COW treatment.
 */
static void pf_handle_cow(unsigned cr2)
{
	unsigned vir = 0;
	unsigned new_mem = 0;
	int flag;
	unsigned long long begin = TestControl.profiling ? time_now_us() : 0;

	page_fault_cow++;

	vir = cr2;

	/*
	 * 1. alloc a new physical memory
	 */
	new_mem = vm_alloc(1);

	/*
	 * 2. copy cow value
	 */
	memcpy(new_mem, vir, PAGE_SIZE);

	/*
	 * 3. unmap origin address
	 */
	flag = mm_get_map_flag(vir);
	mm_unmap_page(vir);

	/*
	 * 4. unmap newly allocated physical memory
	 * note that should ref it first so that on one will use
	 * this physical memory
	 */
	phymm_reference_page(VIRT_TO_PAGE_IDX(new_mem));
	mm_kunmap_page(new_mem);

	/*
	 * 5. map newly allocated physical memory to origin virtual address
	 */
	flag |= PAGE_ENTRY_PRESENT;
	flag |= PAGE_ENTRY_WRITABLE;
	mm_map_page(vir, VIRT_TO_PHY(new_mem), flag);

	phymm_dereference_page(VIRT_TO_PAGE_IDX(new_mem));

	RELOAD_CR3();

	if (TestControl.profiling)
		page_fault_cow_spent += (time_now_us() - begin);
}

/*
 * Handle page fault which is originally readonly now writable.
 */
static void pf_handle_readonly(unsigned cr2)
{
	unsigned vir = 0;
	int flag;
	unsigned long long begin = TestControl.profiling ? time_now_us() : 0;

	page_fault_perm++;
	vir = cr2;
	flag = mm_get_map_flag(vir);
	flag |= PAGE_ENTRY_WRITABLE;
	mm_set_map_flag(vir, flag);
	INVLPG(vir);

	if (TestControl.profiling)
		page_fault_perm_spent += (time_now_us() - begin);
}

/*
 * Handle a write fault on a present but read-only page.
 *
 * MAP_SHARED mapping:
 *   Upgrade the PTE writable in place.  All processes that share the same
 *   physical page immediately see the write — that is the MAP_SHARED contract.
 *   No private copy is made.
 *
 * MAP_PRIVATE mapping (or fork-COW anonymous page):
 *   Two sub-cases:
 *   1. ref_count > 1: page is shared (fork-COW sibling or file-cache entry).
 *      Allocate a private copy so the sibling / cache is left untouched.
 *   2. ref_count = 1: sole owner, PTE still read-only after a prior COW
 *      resolution.  Just flip the writable bit.
 *
 * Anything else is a true protection violation → SIGSEGV.
 */
static int pf_handle_permission(unsigned cr2)
{
	task_struct *cur = CURRENT_TASK();
	vm_region *region;
	unsigned page_index;

	region = vm_find_map(cur->user->vm, cr2);

	/* No covering region, or region does not permit writes. */
	if (!region || !(region->prot & PROT_WRITE))
		return 0;

	/*
	 * MAP_SHARED: all sharers must see the same physical page, so never
	 * COW — just make this process's PTE writable.  If the region is
	 * file-backed, mark the physical page dirty so it gets flushed back
	 * to the file on munmap / process exit.
	 */
	if (region->flag & MAP_SHARED) {
		pf_handle_readonly(cr2);
		if (region->fp != NULL) {
			unsigned page_index = mm_get_attached_page_index(cr2);
			phymm_mark_dirty(page_index);
		}
		return 1;
	}

	/*
	 * MAP_PRIVATE: COW if the page is still shared (ref_count > 1),
	 * otherwise just upgrade the read-only PTE.
	 */
	page_index = mm_get_attached_page_index(cr2);
	if (phymm_is_cow(page_index))
		pf_handle_cow(cr2);
	else
		pf_handle_readonly(cr2);

	return 1;
}

static void pf_dump_region(vm_region *region, void *data)
{
	(void)data;
	klog("    %08x-%08x prot=%x flag=%x %s\n", region->begin, region->end,
	     region->prot, region->flag,
	     region->fp ? region->fp->f_name : "anon");
}

static void pf_process(intr_frame *frame)
{
	unsigned cr2;
	unsigned error = frame->error_code;
	int oldint;
	task_struct *cur;
	int int_enable = int_is_intr_enabled();
	(void)int_enable;

	/*
	 * Save old interrupt state first.
	 */
	oldint = sched_disable();

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
		if (pf_handle_permission(cr2))
			goto Done;
		goto NOT_HANDLED;
	}

NOT_HANDLED:
	cur = CURRENT_TASK();

	if (frame->cs != KERNEL_CODE_SELECTOR ||
	    (unsigned)frame->eip < KERNEL_OFFSET || cr2 < KERNEL_OFFSET) {
		/* FIXME(Ender): Currently we call process exit(-EFAULT)
		 * Unknown bugs cause user mode process r/w an unmaped page
		*/
		klog("FATAL: unhandled user page fault! error code %x, address %x, eip %x, cmd %s\n",
		     frame->error_code, cr2, frame->eip, cur->user->command);
		if (cur->user->vm) {
			klog("  vm regions:\n");
			vm_enum(cur->user->vm, pf_dump_region, NULL);
		}
		sys_exit(-EFAULT);
		goto Done;
	}

	klog("FATAL: unhandled kernel page fault! error code %x, address %x, eip %x\n",
	     frame->error_code, cr2, frame->eip);

	DIE();

Done:
	sched_set_level(oldint);
}
