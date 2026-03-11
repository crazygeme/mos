#include <config.h>
#include <lock.h>
#include <int.h>
#include <pagefault.h>
#include <mm.h>
#include <ps.h>
#include <mmap.h>
#include <phymm.h>
#include <fs.h>
#include <klib.h>
#include <time.h>
#include <macro.h>
#include <rbtree.h>
#include <list.h>
#include <errno.h>

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

/* -------------------------------------------------------------------------
 * LRU mmap cache
 *
 * Hash table for O(1) lookup keyed by (ino, offset).
 * Doubly-linked LRU list: head = most recently used, tail = least recently used.
 * On hit:  move entry to list head.
 * On miss + full: evict the tail (LRU) entry, insert new entry at head.
 * ------------------------------------------------------------------------- */

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
static list_entry mmap_lru; /* head = MRU, tail = LRU */
static mutex_t mmap_cache_lock;
static unsigned zero_page = 0;
static unsigned zero_page_phy = 0;

static void pf_process(intr_frame *frame);

static int mmap_key_comp(void *k1, void *k2)
{
	mmap_cache_key *key1 = k1;
	mmap_cache_key *key2 = k2;
	int ret = key1->ino - key2->ino;
	if (ret == 0)
		ret = (int)key1->offset - (int)key2->offset;
	return ret;
}

void pf_init()
{
	int_register(0xe, pf_process, 0, 0);
	mmap_cache = hash_create(mmap_key_comp);
	list_init(&mmap_lru);
	mutex_init(&mmap_cache_lock);
	zero_page = vm_alloc(1);
	memset(zero_page, 0, PAGE_SIZE);
	zero_page_phy = VIRT_TO_PHY(zero_page);
}

/* Lookup (ino, offset) in the cache.  On hit, promote to MRU position.
 * Returns the physical address, or 0 on miss. */
static unsigned mmap_cache_find(unsigned ino, unsigned offset)
{
	mmap_cache_key tmp = { .ino = ino, .offset = offset };
	key_value_pair *pair;
	mmap_cache_entry *entry;

	if (ino == 0)
		return 0;

	mutex_lock(&mmap_cache_lock);
	pair = hash_find(mmap_cache, &tmp);
	if (!pair) {
		mutex_unlock(&mmap_cache_lock);
		return 0;
	}
	entry = pair->val;
	/* Promote to MRU (move to head of LRU list). */
	list_remove_entry(&entry->lru);
	list_insert_tail(&mmap_lru, &entry->lru);
	mutex_unlock(&mmap_cache_lock);
	return entry->phy;
}

/* Insert (ino, offset, phy) into the cache.
 * If the cache is full, evict the LRU entry first (single eviction). */
static void mmap_cache_add(unsigned ino, unsigned offset, unsigned phy)
{
	mmap_cache_entry *entry;
	mmap_cache_key tmp = { .ino = ino, .offset = offset };

	if (ino == 0 || phy == 0)
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
		phymm_dereference_page(PHY_TO_PAGE_IDX(evict->phy));
		free(evict);
	}

	entry = malloc(sizeof(*entry));
	entry->key.ino = ino;
	entry->key.offset = offset;
	entry->phy = phy;
	list_insert_tail(&mmap_lru, &entry->lru);
	hash_insert(mmap_cache, &entry->key, entry);
	phymm_reference_page(PHY_TO_PAGE_IDX(phy));

	mutex_unlock(&mmap_cache_lock);
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
 */
static int pf_handle_invalid_file_map(unsigned address, file *f,
				      unsigned offset, int prot, int flag)
{
	int ret = 0;
	unsigned phy = 0;
	ext4_file *ff;
	size_t rcnt = 0;
	unsigned long long begin = TestControl.profiling ? time_now_us() : 0;

	page_fault_file++;

	/*
	 * Find a cached map first.
	 * Some of the file map are globally same, especially those
	 * runtime libraries.
	 */
	phy = mmap_cache_find(f->f_inode->i_ino, offset);

	if (TestControl.profiling)
		page_fault_file_search_spent += time_now_us() - begin;

	if (phy != NULL) {
		page_fault_file_cache_hit++;
		mm_add_dynamic_map(address, phy, PAGE_ENTRY_USER_CODE);
		INVLPG(address);
		goto DONE;
	}

	/*
	 * We don't use mm_add_dynamic_map with NULL physical address here
	 * because we want to know exactly what physical page and cache it.
	 */
	phy = phymm_alloc_user() * PAGE_SIZE;

	mm_add_dynamic_map(address, phy, PAGE_ENTRY_USER_CODE);
	INVLPG(address);

	ff = f->f_inode->i_private;

	/*
	 * Some program will map a region larger than file size.
	 * we return a zero page in this case.
	 */
	ret = ext4_fseek(ff, offset, SEEK_SET);
	if (ret != EOK) {
		bzero(address);
		// memset(address, 0, PAGE_SIZE);
		goto READ_DONE;
	}

	/*
	 * Read the file. Note that not always PAGE_SIZE bytes read, we
	 * will fill with zero in this case.
	 * Usually this "fill zero" operation is not nessasary, but lots
	 * of program rely on this.
	 */
	ret = ext4_fread(ff, address, PAGE_SIZE, &rcnt);
	if (ret != EOK) {
		klog("FAIL: mmap: read to buffer %x, size %x\n", address,
		     PAGE_SIZE);
	}

	if (rcnt < PAGE_SIZE) {
		memset(address + rcnt, 0, PAGE_SIZE - rcnt);
	}

READ_DONE:
	mmap_cache_add(f->f_inode->i_ino, offset, phy);
	page_fault_file_read += rcnt;

DONE:
	if (TestControl.profiling)
		page_fault_file_spent += (time_now_us() - begin);
}

/*
 * Handle page fault which has no physical page attached.
 */
static int pf_handle_invalid_memory(unsigned address, int prot, int flag)
{
	unsigned long long begin = TestControl.profiling ? time_now_us() : 0;

	if (prot & PROT_WRITE) {
		mm_add_dynamic_map(address, 0, PAGE_ENTRY_USER_DATA);
		INVLPG(address);
		//memset(address, 0, PAGE_SIZE);
		bzero(address);
	} else {
		/*
		 * For read only zero page, we map with a globally shared
		 * zero page first, and let COW allocate new one if write.
		 */
		mm_add_dynamic_map(address, zero_page_phy,
				   PAGE_ENTRY_USER_CODE);
		INVLPG(address);
	}

	if (TestControl.profiling) {
		page_fault_invalid++;
		page_fault_invalid_spent += (time_now_us() - begin);
	}
}

/*
 * Handle page fault which has no physical page.
 */
static int pf_handle_page_invalid(unsigned cr2)
{
	vm_region *region;
	unsigned this_offset;
	unsigned this_begin;
	task_struct *cur = CURRENT_TASK();

	this_begin = cr2 & PAGE_SIZE_MASK;

	/*
	 * Find out the map region first. The `region` structure
	 * will tell us whether it's file map or not.
	 */
	region = vm_find_map(cur->user.vm, this_begin);
	if (!region)
		return 0;

	this_offset = region->offset + (this_begin - region->begin);

	if (region->node != 0) {
		pf_handle_invalid_file_map(this_begin, region->node,
					   this_offset, region->prot,
					   region->flag);
		cur->pf_major++;
	} else {
		pf_handle_invalid_memory(this_begin, region->prot,
					 region->flag);
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
	vir = (vir & PAGE_SIZE_MASK);
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
	mm_del_dynamic_map(vir);

	/*
	 * 4. unmap newly allocated physical memory
	 * note that should ref it first so that on one will use
	 * this physical memory
	 */
	phymm_reference_page(VIRT_TO_PAGE_IDX(new_mem));
	mm_del_direct_map(new_mem);

	/*
	 * 5. map newly allocated physical memory to origin virtual address
	 */
	flag |= PAGE_ENTRY_PRESENT;
	flag |= PAGE_ENTRY_WRITABLE;
	mm_add_dynamic_map(vir, VIRT_TO_PHY(new_mem), flag);

	phymm_dereference_page(VIRT_TO_PAGE_IDX(new_mem));

	RELOAD_CR3();

	if (TestControl.profiling)
		page_fault_cow_spent += (time_now_us() - begin);
}

/*
 * Handle page fault which is originally readonly now writable.
 * FIXME(Ender): Remove this function.
 * This is wrong because every page should be managed by mmap and
 * mprotect system call, but we havn't implement mprotect yet.
 */
static void pf_handle_readonly(unsigned cr2)
{
	unsigned vir = 0;
	int flag;
	unsigned long long begin = TestControl.profiling ? time_now_us() : 0;

	page_fault_perm++;
	vir = cr2;
	vir = (vir & PAGE_SIZE_MASK);
	flag = mm_get_map_flag(vir);
	flag |= PAGE_ENTRY_WRITABLE;
	mm_set_map_flag(vir, flag);
	INVLPG(vir);

	if (TestControl.profiling)
		page_fault_perm_spent += (time_now_us() - begin);
}

/*
 * Handle page fault which is permission deny.
 */
static int pf_handle_permission(unsigned cr2)
{
	unsigned page_index = mm_get_attached_page_index(cr2);
	unsigned cow;
	int flag = 0;

	cow = phymm_is_cow(page_index);

	if (cow)
		pf_handle_cow(cr2);
	else
		pf_handle_readonly(cr2);

	return 1;
}

static void pf_process(intr_frame *frame)
{
	unsigned cr2;
	unsigned error = frame->error_code;
	int oldint;

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

	if (!((error & PF_MASK_P) == PF_MASK_P)) {
		if (pf_handle_page_invalid(cr2))
			goto Done;

		goto NOT_HANDLED;
	}

	if ((error & PF_MASK_RW) == PF_MASK_RW) {
		if (pf_handle_permission(cr2))
			goto Done;

		goto NOT_HANDLED;
	}

NOT_HANDLED:

	if (frame->cs != KERNEL_CODE_SELECTOR) {
		/* FIXME(Ender): Currently we call process exit(-EFAULT)
		 * Because we haven't implement signal yet, and it's hard to return
		 * from page fault handler to user code. We will implement signal
		 * and return from page fault handler later, and then we can send
		 * SIGSEGV to user process here.
		*/
		sys_exit(-EFAULT);
		goto Done;
	}

	do {
		task_struct *cur = CURRENT_TASK();
		printk("[%d]page fault! error code %x, address %x, eip %x\n",
		       cur->psid, frame->error_code, cr2, frame->eip);
	} while (0);

	DIE();

Done:
	if (oldint)
		sched_enable();
	else
		sched_disable();
}
