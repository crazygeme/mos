#include <int/int.h>
#include <mm/cache.h>
#include <mm/pagefault.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <mm/phymm.h>
#include <ps/ps.h>
#include <fs/cache.h>
#include <fs/fs.h>
#include <dev/dev.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <hw/time.h>
#include <config.h>
#include <macro.h>

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
static unsigned zero_page = 0;
static unsigned zero_page_phy = 0;

static void pf_process(intr_frame *frame);

void pf_init()
{
	int_register(0xe, pf_process, 0, 0);
	mm_cache_init();
	zero_page = vm_alloc(1);
	memset(zero_page, 0, PAGE_SIZE);
	zero_page_phy = VIRT_TO_PHY(zero_page);
}

static unsigned pf_read_file_page_direct(file *f, unsigned offset)
{
	unsigned page_idx;
	unsigned phy;

	if (!f || !f->f_inode || !f->f_inode->i_op ||
	    !f->f_inode->i_op->read_page)
		return 0;

	page_idx = phymm_alloc_user();
	if (page_idx == PHYMM_INVALID)
		return 0;

	phy = page_idx * PAGE_SIZE;
	if (mm_kmap_phys(phy) != 1) {
		phymm_free_user(page_idx);
		return 0;
	}

	if (f->f_inode->i_op->read_page(f->f_inode, offset,
					(void *)PHY_TO_VIRT(phy)) != 0) {
		phymm_free_user(page_idx);
		return 0;
	}

	return phy;
}

static int pf_file_uses_fs_page_cache(file *f)
{
	inode *node;

	if (!f)
		return 0;
	node = f->f_inode;
	return node && node->i_pgcache_tag && node->i_op &&
	       node->i_op->read_page;
}

typedef struct _pf_file_page_result {
	unsigned phy;
	int cache_hit;
	int needs_shared_registration;
} pf_file_page_result;

static pf_file_page_result pf_get_file_page(file *f, int offset, int flag)
{
	pf_file_page_result result = { 0 };
	int shared = (flag & MAP_SHARED) != 0;

	if (pf_file_uses_fs_page_cache(f)) {
		result.phy = fs_page_cache_get(f->f_inode, offset,
					       &result.cache_hit);
		return result;
	}

	if (shared) {
		result.phy = mm_file_shared_find(f, offset);
		if (result.phy != 0) {
			result.needs_shared_registration = 1;
			return result;
		}
	}

	result.phy = pf_read_file_page_direct(f, offset);
	if (result.phy != 0 && shared)
		result.needs_shared_registration = 1;
	return result;
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
 * File-backed faults are served from the filesystem page cache. This is closer
 * to Linux than the old pagefault-local cache: MAP_SHARED and MAP_PRIVATE both
 * start from the cached file page, and the semantic split happens on write.
 */
static int pf_handle_invalid_file_map(unsigned address, file *f, int offset,
				      int prot, int flag)
{
	pf_file_page_result page;
	int mmflag;
	unsigned long long begin = TestControl.profiling ? time_wall_us() : 0;

	page_fault_file++;

	/*
	 * /dev/mem must map the requested physical page directly.  Treating it
	 * like an ordinary file-backed mapping would populate a cached RAM page
	 * and only write it back on msync/munmap, which is wrong for MMIO/VRAM.
	 * XFree86's VESA driver mmaps the linear framebuffer through /dev/mem and
	 * expects stores to hit the BAR immediately.
	 */
	if (dev_mem_is_file(f)) {
		unsigned phy = (unsigned)offset & PAGE_SIZE_MASK;
		unsigned pte = PAGE_ENTRY_USER_CODE | PAGE_ENTRY_CD;

		if (prot & PROT_WRITE)
			pte |= PAGE_ENTRY_WRITABLE;
		if (mm_map_page_io(address, phy, pte) != 1)
			goto FAIL;
		INVLPG(address);
		if (TestControl.profiling)
			page_fault_file_spent += (time_wall_us() - begin);
		return 1;
	}

	/*
	 * Linux would populate from the filesystem page cache here. MOS now does
	 * the same at the fs layer: the returned page is shared across readers and
	 * private mappings fault COW on the first write.
	 */
	page = pf_get_file_page(f, offset, flag);
	if (TestControl.profiling)
		page_fault_file_search_spent += time_wall_us() - begin;

	if (page.phy == 0)
		goto FAIL;
	if (page.cache_hit)
		page_fault_file_cache_hit++;
	else
		page_fault_file_read += PAGE_SIZE;

	if (mm_map_page(address, page.phy, PAGE_ENTRY_USER_CODE) != 1)
		goto FAIL;
	INVLPG(address);

	if ((flag & MAP_SHARED) || !(prot & PROT_WRITE)) {
		mmflag = mm_get_map_flag(address);
		mmflag &= ~PAGE_ENTRY_WRITABLE;
		mm_set_map_flag(address, mmflag);
		INVLPG(address);
	}
	if ((flag & MAP_SHARED) && page.needs_shared_registration)
		mm_file_shared_add(f, offset, page.phy);

	if (TestControl.profiling)
		page_fault_file_spent += (time_wall_us() - begin);
	return 1;
FAIL:
	if (TestControl.profiling)
		page_fault_file_spent += (time_wall_us() - begin);
	return 0;
}

/*
 * Handle page fault which has no physical page attached.
 *
 * MAP_SHARED anonymous (anon_id != 0):
 *   All processes sharing the same anon_id must map the same physical page.
 *   Use anon_shared_map keyed by (anon_id | ANON_SHARED_BIT, offset).
 *   Pages are mapped read-only; write faults go through pf_handle_permission.
 *
 * MAP_PRIVATE writable: allocate a fresh zero page per fault.
 *
 * MAP_PRIVATE read-only: map the global zero page read-only; a write fault
 *   triggers COW.
 */
static int pf_handle_invalid_memory(unsigned address, vm_region *region,
				    int offset)
{
	int prot = region->prot;
	int flag = region->flag;
	unsigned long long begin = TestControl.profiling ? time_wall_us() : 0;
	unsigned page_idx;
	int handled = 0;

	page_fault_invalid++;

	if ((flag & MAP_SHARED) && region->anon_id != 0) {
		unsigned phy = mm_anon_shared_find(region->anon_id, offset);

		if (phy != 0) {
			/* Hit — map read-only so writes go through pf_handle_permission. */
			if (mm_map_page(address, phy, PAGE_ENTRY_USER_CODE) !=
			    1)
				goto DONE;
			INVLPG(address);
			handled = 1;
			goto DONE;
		}

		/* Miss — allocate, zero, strip writable, and register in anon_shared_map. */
		page_idx = phymm_alloc_user();
		if (page_idx == PHYMM_INVALID)
			goto DONE;
		phy = page_idx * PAGE_SIZE;
		if (mm_map_page(address, phy, PAGE_ENTRY_USER_DATA) != 1) {
			phymm_free_user(page_idx);
			goto DONE;
		}
		INVLPG(address);
		memset(address, 0, PAGE_SIZE);

		mm_anon_shared_add(region->anon_id, offset, phy);
		handled = 1;
		goto DONE;
	}

	if (prot & PROT_WRITE) {
		if (mm_map_page(address, 0, PAGE_ENTRY_USER_DATA) != 1)
			goto DONE;
		INVLPG(address);
		memset(address, 0, PAGE_SIZE);
	} else {
		/*
		 * For a read-only anonymous page, share the global zero page.
		 * mm_map_page bumps zero_page's ref_count to > 1, so
		 * phymm_is_cow() fires on the next write fault and COW kicks
		 * in — the shared zero page is never dirtied.
		 */
		if (mm_map_page(address, zero_page_phy, PAGE_ENTRY_USER_CODE) !=
		    1)
			goto DONE;
		INVLPG(address);
	}
	handled = 1;

DONE:
	if (TestControl.profiling)
		page_fault_invalid_spent += (time_wall_us() - begin);
	return handled;
}

static int pf_vma_is_stack(task_struct *task, vm_region *region)
{
	return region != NULL && region->fp == NULL &&
	       !(region->flag & MAP_SHARED) &&
	       region->begin == task->user->stack_bottom &&
	       (region->prot & PROT_WRITE);
}

/*
 * pf_find_vma - Linux-style fault lookup.
 *
 * First consult the task-local mmap_cache via vm_find_vma_cached(). If the
 * address falls in a hole and the next VMA is the grow-down stack mapping,
 * extend that mapping downward to cover the faulting page.
 */
static vm_region *pf_find_vma(task_struct *task, unsigned address)
{
	vm_region *region;

	region = vm_find_vma_cached(task->user, address);
	if (!region)
		return NULL;

	if (region->begin <= address && address < region->end)
		return region;

	if (!pf_vma_is_stack(task, region))
		return NULL;

	if (address < USER_ZONE_END || address >= task->user->stack_bottom)
		return NULL;

	{
		unsigned minimal_grow = USER_STACK_INIT_PAGES * PAGE_SIZE;
		unsigned required_grow = task->user->stack_bottom - address;
		unsigned grow_size = required_grow > minimal_grow ?
					     required_grow :
					     minimal_grow;

		task->user->stack_bottom -= grow_size;
		vm_add_map(task->user->vm, task->user->stack_bottom,
			   task->user->stack_bottom + grow_size,
			   PROT_READ | PROT_WRITE, MAP_FIXED, NULL, 0, 0);
		vm_invalidate_user_cache(task->user);
	}

	return vm_find_map_cached(task->user, address);
}

static vm_region *pf_lock_region(vm_region *region, unsigned address)
{
	if (!region)
		return NULL;

	vm_region_lock_fault(region);
	if (address < region->begin || address >= region->end) {
		vm_region_unlock_fault(region);
		return NULL;
	}

	return region;
}

static int pf_page_already_present(unsigned address)
{
	return (mm_get_map_flag(address) & PAGE_ENTRY_PRESENT) != 0;
}

/*
 * Handle page fault which has no physical page.
 */
static int pf_handle_page_invalid(task_struct *task, unsigned cr2)
{
	vm_region *region;
	int this_offset;

	region = pf_lock_region(vm_find_map_cached(task->user, cr2), cr2);
	if (!region)
		region = pf_lock_region(pf_find_vma(task, cr2), cr2);
	if (!region)
		return 0;

	/* PROT_NONE: no access permitted — treat as unmapped. */
	if (region->prot == PROT_NONE) {
		vm_region_unlock_fault(region);
		return 0;
	}

	if (pf_page_already_present(cr2)) {
		vm_region_unlock_fault(region);
		return 1;
	}

	this_offset = region->offset + (cr2 - region->begin);

	if (region->fp != NULL) {
		if (!pf_handle_invalid_file_map(cr2, region->fp, this_offset,
						region->prot, region->flag)) {
			vm_region_unlock_fault(region);
			return 0;
		}
		if (task->stats)
			task->stats->pf_major++;
	} else {
		if (!pf_handle_invalid_memory(cr2, region, this_offset)) {
			vm_region_unlock_fault(region);
			return 0;
		}
		if (task->stats)
			task->stats->pf_minor++;
	}

	vm_region_unlock_fault(region);
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
	unsigned vir = cr2 & PAGE_SIZE_MASK;
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
	unsigned vir = cr2 & PAGE_SIZE_MASK;
	int flag;
	unsigned long long begin = TestControl.profiling ? time_wall_us() : 0;

	page_fault_perm++;
	flag = mm_get_map_flag(vir);
	flag |= PAGE_ENTRY_WRITABLE;
	mm_set_map_flag(vir, flag);
	INVLPG(vir);

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
static int do_wp_page(task_struct *task, unsigned cr2)
{
	vm_region *region;
	unsigned page_index;
	unsigned vir = cr2 & PAGE_SIZE_MASK;

	region = pf_lock_region(vm_find_map_cached(task->user, vir), vir);

	if (!region || !(region->prot & PROT_WRITE)) {
		if (region)
			vm_region_unlock_fault(region);
		return 0;
	}

	{
		unsigned map_flag = mm_get_map_flag(vir);

		if (!(map_flag & PAGE_ENTRY_PRESENT)) {
			vm_region_unlock_fault(region);
			return 0;
		}
		if (map_flag & PAGE_ENTRY_WRITABLE) {
			vm_region_unlock_fault(region);
			return 1;
		}
	}

	if (region->flag & MAP_SHARED) {
		wp_page_reuse(vir);
		if (region->fp != NULL) {
			unsigned page_index = mm_get_attached_page_index(vir);
			phymm_mark_dirty(page_index);
		}

		vm_region_unlock_fault(region);
		return 1;
	}

	page_index = mm_get_attached_page_index(vir);
	if (phymm_is_cow(page_index))
		wp_page_copy(vir);
	else
		wp_page_reuse(vir);

	vm_region_unlock_fault(region);

	return 1;
}

/*
 * Resolve a fault in @task's user address space while borrowing its page
 * tables.  The scheduler and interrupts stay disabled for the duration so the
 * logical current task cannot diverge from the active CR3 mid-operation.
 */
int pf_resolve_task_page_fault(task_struct *task, unsigned addr, int write)
{
	unsigned old_cr3;
	unsigned target_cr3;
	unsigned old_level;
	int handled;

	if (!task || !task->user)
		return 0;

	target_cr3 = task->user->page_dir - KERNEL_OFFSET;
	old_level = int_intr_disable();
	sched_disable();
	LOAD_CR3(old_cr3);
	if (old_cr3 != target_cr3)
		SET_CR3(target_cr3);

	addr &= PAGE_SIZE_MASK;
	if (write)
		handled = do_wp_page(task, addr);
	else
		handled = pf_handle_page_invalid(task, addr);

	if (old_cr3 != target_cr3)
		SET_CR3(old_cr3);
	sched_enable();
	int_intr_setlevel(old_level);
	return handled;
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
		if (pf_handle_page_invalid(CURRENT_TASK(), cr2))
			goto Done;
		goto NOT_HANDLED;
	}

	if (error & PF_MASK_RW) {
		if (do_wp_page(CURRENT_TASK(), cr2))
			goto Done;
		goto NOT_HANDLED;
	}

NOT_HANDLED:
	cur = CURRENT_TASK();

	if ((unsigned)frame->eip < KERNEL_OFFSET ||
	    (cr2 < KERNEL_OFFSET && cr2 > 0x1000)) {
		klog("segfault: error code %x, address %x, eip %x\n",
		     frame->error_code, cr2, frame->eip);

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
