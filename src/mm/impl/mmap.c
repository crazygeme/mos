#include <mm/mm.h>
#include <mm/cache.h>
#include <mm/phymm.h>
#include <lib/list.h>
#include <lib/rbtree.h>
#include <lib/klib.h>
#include <mm/mmap.h>
#include <dev/dev.h>
#include <ps/ps.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <macro.h>
#include <mm/mmu.h>
#include <config.h>
#include <errno.h>
#include <int/int.h>
#include <ext4.h>

/*
 * vm_key is the search key for the red-black tree that backs each process's
 * VM map.  The comparator treats any range overlap as equal (returns 0), so
 * tree lookup works as an overlap query: it returns any existing region that
 * intersects the probe key.
 */
typedef struct _vm_key {
	vaddr_t begin;
	vaddr_t end;
} vm_key;

struct _vm_fault_lock {
	ref_count_t ref;
	rmutex_t lock;
};

/*
 * vm_region_compare - interval comparator for the VM region tree.
 *
 * Returns -1  if region1 ends before region2 starts  (region1 strictly left),
 *          1  if region1 starts after region2 ends    (region1 strictly right),
 *          0  if the two ranges overlap in any way.
 */
static INLINE int vm_region_compare(const void *region1, const void *region2)
{
	const vm_key *key1 = region1;
	const vm_key *key2 = region2;

	if (key1->end <= key2->begin)
		return -1;
	if (key1->begin >= key2->end)
		return 1;
	return 0; /* overlap */
}

static void vm_flush_dirty_region(vm_region *region, vaddr_t begin,
				  vaddr_t end);
static void vm_add_map_with_lock(vm_struct_t vm, vaddr_t begin, vaddr_t end,
				 int prot, int flag, file *fp, uint64_t offset,
				 unsigned anon_id, vm_fault_lock *fault_lock);

static unsigned vm_region_flags_for_file(file *fp)
{
	if (!fp || !fp->f_inode)
		return 0;
	if (fp->f_inode->i_phys_size ||
	    (unsigned)(uintptr_t)fp->f_inode->i_private == DEV_MEM_RDEV)
		return VM_REGION_F_DIRECT_PHYS;
	return 0;
}

static void release_fault_lock(ref_count_t *ref)
{
	kfree(container_of(ref, vm_fault_lock, ref));
}

static void release_mm(ref_count_t *ref);

static vm_fault_lock *vm_fault_lock_new()
{
	vm_fault_lock *fault_lock = kmalloc(sizeof(*fault_lock));

	vm_lock_init(&fault_lock->lock);
	ref_count_init(&fault_lock->ref, release_fault_lock);
	return fault_lock;
}

static void vm_fault_lock_lock(vm_fault_lock *fault_lock)
{
	if (!fault_lock)
		return;
	vm_lock_enter(&fault_lock->lock, __func__);
}

static void vm_fault_lock_unlock(vm_fault_lock *fault_lock)
{
	if (!fault_lock)
		return;
	vm_lock_leave(&fault_lock->lock);
}

/* Counter for unique anonymous MAP_SHARED region identifiers. */
static unsigned g_anon_id_next = 0;

static void vm_region_free(vm_region *region)
{
	if (region->fp)
		fs_put_file(region->fp);
	if ((region->flag & MAP_SHARED) && region->fp == NULL &&
	    region->anon_id)
		mm_anon_shared_put(region->anon_id);
	ref_count_put(region->fault_lock);

	kfree(region);
}

static vm_region *vm_tree_find(mm_struct *mm, const vm_key *key)
{
	struct rb_node *node;
	int irq;

	spinlock_lock(&mm->vma_lock, &irq);
	node = mm->vma_index.rb_node;
	while (node) {
		vm_region *region = rb_entry(node, vm_region, rb_node);
		vm_key node_key = { region->begin, region->end };
		int comp = vm_region_compare(key, &node_key);
		if (comp < 0)
			node = node->rb_left;
		else if (comp > 0)
			node = node->rb_right;
		else {
			spinlock_unlock(&mm->vma_lock, irq);
			return region;
		}
	}
	spinlock_unlock(&mm->vma_lock, irq);
	return NULL;
}

static int vm_tree_insert(mm_struct *mm, vm_region *region)
{
	struct rb_node **link = &mm->vma_index.rb_node;
	struct rb_node *parent = NULL;
	vm_key key = { region->begin, region->end };
	int irq;

	spinlock_lock(&mm->vma_lock, &irq);
	while (*link) {
		vm_region *node_region = rb_entry(*link, vm_region, rb_node);
		vm_key node_key = { node_region->begin, node_region->end };
		int comp = vm_region_compare(&key, &node_key);
		parent = *link;
		if (comp < 0)
			link = &(*link)->rb_left;
		else if (comp > 0)
			link = &(*link)->rb_right;
		else {
			spinlock_unlock(&mm->vma_lock, irq);
			return 0;
		}
	}
	rb_link_node(&region->rb_node, parent, link);
	rb_insert_color(&region->rb_node, &mm->vma_index);
	mm->vma_generation++;
	spinlock_unlock(&mm->vma_lock, irq);
	return 1;
}

static void vm_tree_remove(mm_struct *mm, vm_region *region)
{
	int irq;
	spinlock_lock(&mm->vma_lock, &irq);
	rb_erase(&region->rb_node, &mm->vma_index);
	mm->vma_generation++;
	spinlock_unlock(&mm->vma_lock, irq);
	vm_region_free(region);
}

static vm_region *vm_tree_first(mm_struct *mm)
{
	struct rb_node *node;
	int irq;
	spinlock_lock(&mm->vma_lock, &irq);
	node = rb_first(&mm->vma_index);
	spinlock_unlock(&mm->vma_lock, irq);
	return node ? rb_entry(node, vm_region, rb_node) : NULL;
}

static vm_region *vm_tree_next(mm_struct *mm, vm_region *region)
{
	struct rb_node *node;
	int irq;
	spinlock_lock(&mm->vma_lock, &irq);
	node = rb_next(&region->rb_node);
	spinlock_unlock(&mm->vma_lock, irq);
	return node ? rb_entry(node, vm_region, rb_node) : NULL;
}

static void vm_tree_destroy(mm_struct *mm)
{
	/* Destruction is only used once the address space is detached.  Remove
	 * each node while holding the tree lock, then release the lock before
	 * dropping file/fault-lock references (those paths may acquire locks). */
	for (;;) {
		struct rb_node *node;
		vm_region *region;
		int irq;

		spinlock_lock(&mm->vma_lock, &irq);
		node = rb_first(&mm->vma_index);
		if (!node) {
			spinlock_unlock(&mm->vma_lock, irq);
			break;
		}
		region = rb_entry(node, vm_region, rb_node);
		rb_erase(node, &mm->vma_index);
		mm->vma_generation++;
		spinlock_unlock(&mm->vma_lock, irq);
		vm_region_free(region);
	}
}

vm_struct_t vm_create()
{
	mm_struct *mm = zalloc(sizeof(*mm));
	if (!mm)
		return NULL;
	mm->vma_index = _RBTREE_ROOT_INIT;
	spinlock_init(&mm->vma_lock);
	vm_lock_init(&mm->mapping_lock);
	mm->page_dir = 0;
	mm->start_brk = mm->brk = 0;
	mm->start_stack = 0;
	mm->mmap_base = TASK_UNMAPPED_BASE;
	mm->task_size = MOS_COMPAT_TASK_SIZE;
	mm->brk_limit = USER_HEAP_END;
	ref_count_init(&mm->ref, release_mm);
	memset(mm->ldt_desc, 0, sizeof(mm->ldt_desc));
	mm->ldt_present = 0;
	mm->vma_generation = 1;
	mm->command = vm_alloc(1);
	mm->environment = vm_alloc(1);
	if (!mm->command || !mm->environment) {
		ref_count_put(mm);
		return NULL;
	}
	mm->command[0] = mm->environment[0] = '\0';
	return mm;
}

static void release_mm(ref_count_t *ref)
{
	mm_struct *mm = container_of(ref, mm_struct, ref);
	if (mm->page_dir) {
		mm_destroy_user_map(mm->page_dir);
		vm_free(mm->page_dir, 1);
	}
	vm_tree_destroy(mm);
	if (mm->command)
		vm_free(mm->command, 1);
	if (mm->environment)
		vm_free(mm->environment, 1);
	if (mm->executable)
		fs_put_file(mm->executable);
	kfree(mm);
}

/*
 * vm_find_pair - return the tree pair whose region contains addr, or NULL.
 *
 * addr is rounded down to its page boundary before probing the tree.
 */
static INLINE vm_region *vm_find_region(mm_struct *mm, vaddr_t addr)
{
	vm_key key;
	key.begin = addr & PAGE_SIZE_MASK;
	key.end = key.begin + PAGE_SIZE;
	return vm_tree_find(mm, &key);
}

/*
 * vm_add_map - insert a new mapping [begin, end) into the VM map.
 *
 * If the new range overlaps any existing region, the overlapping region is
 * trimmed: portions outside [begin, end) are preserved as independent regions
 * and the overlapping portion is replaced by the new mapping.  The loop
 * handles arbitrarily many pre-existing overlapping regions one at a time
 * until no conflicts remain, then inserts the new region.
 */
static void vm_add_map_with_lock(vm_struct_t vm, vaddr_t begin, vaddr_t end,
				 int prot, int flag, file *fp, uint64_t offset,
				 unsigned anon_id, vm_fault_lock *fault_lock)
{
	mm_struct *mm = vm;
	LOCK_GUARD(&mm->mapping_lock);
	vm_key probe;
	vm_region *oregion;

	/*
	 * Resolve conflicts iteratively.  Each pass finds one overlapping
	 * region, saves its extent, removes it, then re-inserts the non-
	 * overlapping left and right remnants.  Those remnants never conflict
	 * with [begin, end), so their recursive vm_add_map calls insert
	 * directly without further iteration.
	 */
	probe.begin = begin;
	probe.end = end;
	while ((oregion = vm_tree_find(mm, &probe)) != NULL) {
		vaddr_t unmap_begin;
		vaddr_t unmap_end;
		vaddr_t vir;

		/* Snapshot all origin data before vm_del_map frees the structs. */
		vaddr_t o_begin = oregion->begin;
		vaddr_t o_end = oregion->end;
		int o_prot = oregion->prot;
		int o_flag = oregion->flag;
		file *o_fp = oregion->fp;
		uint64_t o_offset = oregion->offset;
		unsigned o_anon_id = oregion->anon_id;
		vm_fault_lock *o_fault_lock = oregion->fault_lock;

		if (o_fp)
			fs_get_file(o_fp);
		if ((o_flag & MAP_SHARED) && o_fp == NULL && o_anon_id)
			mm_anon_shared_get(o_anon_id);
		ref_count_get(o_fault_lock);
		vm_fault_lock_lock(o_fault_lock);

		unmap_begin = o_begin > begin ? o_begin : begin;
		unmap_end = o_end < end ? o_end : end;

		vm_flush_dirty_region(oregion, unmap_begin, unmap_end);
		for (vir = unmap_begin; vir < unmap_end; vir += PAGE_SIZE)
			mm_unmap_page(vir);

		vm_tree_remove(mm, oregion);

		/* Re-insert the left remnant [o_begin, begin), if any. */
		if (o_begin < begin)
			vm_add_map_with_lock(vm, o_begin, begin, o_prot, o_flag,
					     o_fp, o_offset, o_anon_id,
					     o_fault_lock);

		/*
		 * Re-insert the right remnant [end, o_end), if any.
		 * Its file offset advances by (end - o_begin) bytes.
		 */
		if (o_end > end)
			vm_add_map_with_lock(
				vm, end, o_end, o_prot, o_flag, o_fp,
				o_offset + (uint64_t)(end - o_begin), o_anon_id,
				o_fault_lock);

		if (o_fp)
			fs_put_file(o_fp);
		if ((o_flag & MAP_SHARED) && o_fp == NULL && o_anon_id)
			mm_anon_shared_put(o_anon_id);
		vm_fault_lock_unlock(o_fault_lock);
		ref_count_put(o_fault_lock);
	}

	/* No conflicts remain: insert the new region. */
	vm_region *region = kmalloc(sizeof(*region));
	rb_init_node(&region->rb_node);
	region->begin = begin;
	region->end = end;
	region->prot = prot;
	region->flag = flag;
	region->vm_flags = vm_region_flags_for_file(fp);
	if (fault_lock == NULL)
		fault_lock = vm_fault_lock_new();
	else
		ref_count_get(fault_lock);
	region->fault_lock = fault_lock;
	region->fp = fp;
	region->offset = offset;
	region->anon_id = anon_id;

	if (fp)
		fs_get_file(fp);
	if ((flag & MAP_SHARED) && fp == NULL && anon_id)
		mm_anon_shared_get(anon_id);

	vm_tree_insert(mm, region);
}

void vm_add_map(vm_struct_t vm, vaddr_t begin, vaddr_t end, int prot, int flag,
		file *fp, uint64_t offset, unsigned anon_id)
{
	vm_add_map_with_lock(vm, begin, end, prot, flag, fp, offset, anon_id,
			     NULL);
}

void vm_add_map_clone(vm_struct_t vm, vm_region *src)
{
	if (!src)
		return;
	vm_add_map_with_lock(vm, src->begin, src->end, src->prot, src->flag,
			     src->fp, src->offset, src->anon_id,
			     src->fault_lock);
}

/*
 * vm_extend_map - extend one existing VM descriptor in place.
 *
 * This is used by mremap() growth.  Unlike adding a second adjacent mapping,
 * extending the descriptor preserves userspace's expectation that the resized
 * range remains one mapping with one set of attributes.
 */
int vm_extend_map(vm_struct_t vm, vaddr_t begin, vaddr_t old_end,
		  vaddr_t new_end)
{
	mm_struct *mm = vm;
	vm_key probe;
	vm_region *region;
	int irq;

	if (!vm || begin >= old_end || old_end >= new_end)
		return 0;
	LOCK_GUARD(&mm->mapping_lock);

	region = vm_find_region(mm, begin);
	if (!region)
		return 0;

	if (region->begin != begin || region->end != old_end)
		return 0;

	probe.begin = old_end;
	probe.end = new_end;
	if (vm_tree_find(mm, &probe) != NULL)
		return 0;

	spinlock_lock(&mm->vma_lock, &irq);
	region->end = new_end;
	mm->vma_generation++;
	spinlock_unlock(&mm->vma_lock, irq);

	return 1;
}

/*
 * vm_del_map - remove the mapping that contains addr and unmap its pages.
 *
 * addr is rounded down to the nearest page boundary.  All hardware page-table
 * entries for the region are cleared and CR3 is reloaded to flush the TLB.
 */
void vm_del_map(vm_struct_t vm, vaddr_t addr)
{
	mm_struct *mm = vm;
	LOCK_GUARD(&mm->mapping_lock);
	vm_region *region;
	vaddr_t vir;

	addr &= PAGE_SIZE_MASK;

	region = vm_find_region(mm, addr);
	if (!region)
		return;
	vm_fault_lock *fault_lock = region->fault_lock;

	/* FIXME(Ender:) flush file if has one */

	ref_count_get(fault_lock);
	vm_fault_lock_lock(fault_lock);

	/* Unmap every page in the region from the hardware page tables. */
	for (vir = region->begin; vir < region->end; vir += PAGE_SIZE)
		mm_unmap_page(vir);
	vm_tree_remove(mm, region);
	vm_fault_lock_unlock(fault_lock);
	ref_count_put(fault_lock);
}

/*
 * vm_find_map - find the region that contains addr.
 *
 * Returns a pointer to the live vm_region (caller must not free it),
 * or NULL if no region covers addr.
 */
vm_region *vm_find_map(vm_struct_t vm, vaddr_t addr)
{
	addr &= PAGE_SIZE_MASK;
	return vm_find_region(vm, addr);
}

vm_region *vm_find_vma(vm_struct_t vm, vaddr_t addr)
{
	mm_struct *mm = vm;
	struct rb_node *node;
	vm_region *candidate = NULL;
	int irq;

	addr &= PAGE_SIZE_MASK;
	spinlock_lock(&mm->vma_lock, &irq);
	for (node = mm->vma_index.rb_node; node != NULL;) {
		vm_region *region = rb_entry(node, vm_region, rb_node);

		/*
		 * vm_region descriptors live in kernel heap. If the tree ever hands
		 * us a user-space pointer here, the tree is already corrupted; bail
		 * out rather than faulting again while holding table->lock and
		 * deadlocking in the nested page-fault path.
		 */
		if ((uintptr_t)region < MOS_NATIVE_TASK_SIZE) {
			spinlock_unlock(&mm->vma_lock, irq);
			return NULL;
		}

		if (addr < region->begin) {
			candidate = region;
			node = node->rb_left;
		} else if (addr >= region->end) {
			node = node->rb_right;
		} else {
			spinlock_unlock(&mm->vma_lock, irq);
			return region;
		}
	}
	spinlock_unlock(&mm->vma_lock, irq);

	return candidate;
}

void vm_invalidate_task_cache(task_struct *task)
{
	if (task && task->execution) {
		task->execution->mmap_cache = NULL;
		task->execution->mmap_cache_vm = NULL;
		task->execution->mmap_cache_generation = 0;
	}
}

vm_region *vm_find_vma_cached(task_struct *task, vaddr_t addr)
{
	if (!task || !task->execution || !task->memory)
		return NULL;

	mm_struct *mm = task->memory;
	LOCK_GUARD(&mm->mapping_lock);
	vaddr_t page = addr & PAGE_SIZE_MASK;
	/* Validate generation before touching the cached raw pointer. */
	if (task->execution->mmap_cache_vm == mm &&
	    task->execution->mmap_cache_generation == mm->vma_generation &&
	    task->execution->mmap_cache &&
	    task->execution->mmap_cache->begin <= page &&
	    page < task->execution->mmap_cache->end)
		return task->execution->mmap_cache;
	vm_region *region = vm_find_vma(mm, page);
	task->execution->mmap_cache = region;
	task->execution->mmap_cache_vm = mm;
	task->execution->mmap_cache_generation = mm->vma_generation;
	return region;
}

vm_region *vm_find_map_cached(task_struct *task, vaddr_t addr)
{
	vm_region *region = vm_find_vma_cached(task, addr);

	addr &= PAGE_SIZE_MASK;
	if (region && region->begin <= addr && addr < region->end)
		return region;
	return NULL;
}

/*
 * vm_disc_map - find a free virtual address range of at least @size bytes
 *               within [TASK_UNMAPPED_BASE, KERNEL_OFFSET).
 *
 * Iterates the sorted region list and returns the start of the first gap
 * that is large enough.  The candidate start is always clamped to at least
 * TASK_UNMAPPED_BASE so that existing mappings below the user zone (text,
 * stack set up by the process loader) are skipped over.  Returns 0 if no
 * suitable gap exists.
 */
vaddr_t vm_disc_map(vm_struct_t vm, size_t size)
{
	mm_struct *mm = vm;
	LOCK_GUARD(&mm->mapping_lock);
	vm_region *region = vm_tree_first(mm);
	vaddr_t candidate = mm->mmap_base;

	while (region) {
		/* Gap before this region is large enough — use it. */
		if (candidate + size <= region->begin)
			return candidate;

		/* Advance candidate past this region if it overlaps. */
		if (region->end > candidate)
			candidate = region->end;

		region = vm_tree_next(mm, region);
	}

	/* Check for room after the last region. */
	if (candidate + size <= mm->task_size - USER_STACK_PAGES * PAGE_SIZE)
		return candidate;

	return 0;
}

/*
 * vm_dup - copy all VM mappings from @src into @dst.
 *
 * Used during fork() to duplicate the parent's address space descriptor into
 * the child.  vm_add_map() handles the fs_get_file() reference for each node.
 * anon_id is preserved so MAP_SHARED anonymous pages are shared across fork.
 */
void vm_dup(vm_struct_t src, vm_struct_t dst)
{
	mm_struct *mm = src;
	LOCK_GUARD(&mm->mapping_lock);
	vm_region *region = vm_tree_first(mm);

	while (region) {
		vm_add_map_with_lock(dst, region->begin, region->end,
				     region->prot, region->flag, region->fp,
				     region->offset, region->anon_id,
				     region->fault_lock);
		region = vm_tree_next(mm, region);
	}
}

void vm_region_lock_fault(vm_region *region)
{
	if (!region || !region->fault_lock)
		return;
	vm_fault_lock_lock(region->fault_lock);
}

void vm_region_unlock_fault(vm_region *region)
{
	if (!region || !region->fault_lock)
		return;
	vm_fault_lock_unlock(region->fault_lock);
}

void vm_enum(vm_struct_t vm, vm_enum_fn fn, void *data)
{
	mm_struct *mm = vm;
	vm_region *region;

	if (!vm || !fn)
		return;
	LOCK_GUARD(&mm->mapping_lock);
	for (region = vm_tree_first(mm); region;
	     region = vm_tree_next(mm, region))
		fn(region, data);
}

/*
 * vm_mprotect - update protection flags for [begin, end) without unmapping pages.
 *
 * Iterates all VM regions that overlap [begin, end).  For each overlapping
 * region the portion outside [begin, end) is re-inserted with its original
 * protection, while the overlapping portion is re-inserted with @new_prot.
 * Physical page mappings are left intact; only the VM descriptors and page-
 * table permission bits change (done by the caller after this returns).
 *
 * File reference counting: we temporarily bump the ref before removing the
 * region, then release our bump at the end.
 */
void vm_mprotect(vm_struct_t vm, vaddr_t begin, vaddr_t end, int new_prot)
{
	mm_struct *mm = vm;
	LOCK_GUARD(&mm->mapping_lock);
	vm_key probe;
	vm_region *oregion;

	probe.begin = begin;
	probe.end = end;

	/* Visit mappings in address order before advancing the range cursor. */
	while ((oregion = vm_find_vma(mm, probe.begin)) != NULL &&
	       oregion->begin < end) {
		vaddr_t r_begin = oregion->begin;
		vaddr_t r_end = oregion->end;
		int r_prot = oregion->prot;
		int r_flag = oregion->flag;
		file *r_fp = oregion->fp;
		uint64_t r_offset = oregion->offset;
		unsigned r_anon_id = oregion->anon_id;
		vm_fault_lock *r_fault_lock = oregion->fault_lock;

		/* Intersection of the region with [begin, end) */
		vaddr_t upd_begin = r_begin > begin ? r_begin : begin;
		vaddr_t upd_end = r_end < end ? r_end : end;

		/* Temporarily hold references across descriptor removal. */
		if (r_fp)
			fs_get_file(r_fp);
		if ((r_flag & MAP_SHARED) && r_fp == NULL && r_anon_id)
			mm_anon_shared_get(r_anon_id);
		ref_count_get(r_fault_lock);
		vm_fault_lock_lock(r_fault_lock);

		/* Remove descriptor only — physical pages stay mapped. */
		vm_tree_remove(mm, oregion);

		/* Preserve left remnant [r_begin, upd_begin) at original prot. */
		if (r_begin < upd_begin)
			vm_add_map_with_lock(vm, r_begin, upd_begin, r_prot,
					     r_flag, r_fp, r_offset, r_anon_id,
					     r_fault_lock);

		/* Re-insert updated portion [upd_begin, upd_end) with new prot. */
		vm_add_map_with_lock(vm, upd_begin, upd_end, new_prot, r_flag,
				     r_fp,
				     r_offset + (uint64_t)(upd_begin - r_begin),
				     r_anon_id, r_fault_lock);

		/* Preserve right remnant [upd_end, r_end) at original prot. */
		if (upd_end < r_end)
			vm_add_map_with_lock(
				vm, upd_end, r_end, r_prot, r_flag, r_fp,
				r_offset + (uint64_t)(upd_end - r_begin),
				r_anon_id, r_fault_lock);

		if (r_fp)
			fs_put_file(r_fp);
		if ((r_flag & MAP_SHARED) && r_fp == NULL && r_anon_id)
			mm_anon_shared_put(r_anon_id);
		vm_fault_lock_unlock(r_fault_lock);
		ref_count_put(r_fault_lock);

		/* Advance probe past the portion we just handled. */
		probe.begin = upd_end;
	}
}

/*
 * do_mmap_kernel - kernel-internal mmap implementation.
 *
 * Maps the page-aligned range covering [_addr, _addr+_len) into the current
 * task's address space.  If _addr is 0, an appropriate free range is located
 * automatically via vm_disc_map().  Returns the mapped virtual address.
 *
 * For MAP_SHARED|MAP_ANONYMOUS, assigns a unique anon_id so that the region
 * is identifiable across fork() for shared-page lookup.
 */
vaddr_t do_mmap_kernel(vaddr_t _addr, size_t _len, unsigned int prot,
		       unsigned int flags, file *fp, uint64_t offset)
{
	vaddr_t addr = _addr & PAGE_SIZE_MASK;
	vaddr_t last_addr = (_addr + _len - 1) & PAGE_SIZE_MASK;
	size_t page_count = (last_addr - addr) / PAGE_SIZE + 1;
	size_t size = page_count * PAGE_SIZE;
	task_struct *cur = CURRENT_TASK();
	mm_struct *mm = cur->memory;
	LOCK_GUARD(&mm->mapping_lock);
	vm_key probe;
	unsigned anon_id = 0;

	if (flags & MAP_FIXED) {
		/*
		 * POSIX MAP_FIXED: map at addr exactly.  Any existing mappings
		 * that overlap [addr, addr+size) are silently discarded and
		 * replaced.  vm_add_map() handles this via its conflict-
		 * resolution loop, so no pre-processing is needed here.
		 */
	} else {
		/*
		 * addr is a hint.  If addr == 0 or the hinted range overlaps an
		 * existing region, pick a free range automatically.
		 */
		if (addr != 0) {
			probe.begin = addr;
			probe.end = addr + size;
			if (vm_tree_find(mm, &probe) != NULL)
				addr = 0; /* fall through to vm_disc_map */
		}
		if (addr == 0)
			addr = vm_disc_map(cur->memory, size);
	}

	if (!addr || addr >= mm->task_size || size > mm->task_size - addr)
		return 0;
	/*
	 * Assign a unique ID for anonymous MAP_SHARED regions so that all
	 * processes sharing this mapping (e.g. after fork) can locate the
	 * same physical pages via the anonymous shared-page cache in mm/cache.c.
	 */
	if ((flags & MAP_SHARED) && fp == NULL)
		anon_id = ++g_anon_id_next;

	vm_add_map(cur->memory, addr, addr + size, prot, flags, fp, offset,
		   anon_id);
	vm_invalidate_task_cache(cur);

	if (TEST_LOG(TEST_LOG_INFO)) {
		klog("mmap: file %s, addr %x, offset %llx, prot %x, flags %x, len %x at addr %x\n",
		     fp ? fp->f_name : "ANON", _addr,
		     (unsigned long long)offset, prot, flags, _len, addr);
	}

	return addr;
}

void do_mmap_update(vaddr_t _addr, unsigned int prot, unsigned int flags)
{
	vaddr_t addr = _addr & PAGE_SIZE_MASK;
	task_struct *cur = CURRENT_TASK();
	LOCK_GUARD(&cur->memory->mapping_lock);
	vm_region *region;
	vaddr_t vir;

	region = vm_find_map(cur->memory, addr);
	if (!region)
		return;
	region->prot = prot;
	region->flag = flags;

	/* Also update actual mmap flag */
	for (vir = region->begin; vir < region->end; vir += PAGE_SIZE) {
		unsigned mmflag = mm_get_map_flag(vir);
		if (!mmflag)
			continue;
		if (prot == PROT_NONE) {
			mmflag &= ~(PAGE_ENTRY_DPL_USER | PAGE_ENTRY_WRITABLE);
			mm_set_map_flag(vir, mmflag);
			continue;
		}

#if MOS_PAGE_NO_EXEC
		mmflag = prot & PROT_EXEC ? mmflag & ~PAGE_ENTRY_NO_EXEC :
					    mmflag | PAGE_ENTRY_NO_EXEC;
#endif
		mmflag |= PAGE_ENTRY_DPL_USER;
		if (!(prot & PROT_WRITE))
			mmflag &= ~PAGE_ENTRY_WRITABLE;
		else if ((region->vm_flags & VM_REGION_F_DIRECT_PHYS) ||
			 ((region->flag & MAP_SHARED) && region->fp &&
			  region->fp->f_fop && region->fp->f_fop->map_page))
			mmflag |= PAGE_ENTRY_WRITABLE;

		mm_set_map_flag(vir, mmflag);
	}

	vm_invalidate_task_cache(cur);
}

/*
 * do_mmap - syscall handler for mmap(2).
 *
 * Resolves the file descriptor to an inode pointer (NULL for anonymous
 * mappings where fd == -1) and delegates to do_mmap_kernel().
 */
intptr_t do_mmap(vaddr_t _addr, size_t _len, unsigned int prot,
		 unsigned int flags, int fd, uint64_t offset)
{
	task_struct *cur = CURRENT_TASK();
	file *node = NULL;

	if (!_len || _len > cur->memory->task_size ||
	    _addr >= cur->memory->task_size ||
	    _len > cur->memory->task_size - _addr)
		return -EINVAL;
	if (fd != -1 && (offset > 0x7fffffffffffffffULL ||
			 _len > 0x7fffffffffffffffULL - offset))
		return -EINVAL;
	/* A fixed mapping must avoid the architecture's reserved ranges. */
	if (!arch_mm_user_range_valid(_addr, _len)) {
		if (flags & MAP_FIXED)
			return -EINVAL;
		/* A hint, including zero, does not fix the resulting range. */
		_addr = 0;
	}
	/*
	 * This kernel historically treats fd == -1 as an anonymous mapping
	 * even when callers omit MAP_ANONYMOUS (e.g. exec stack setup).
	 * Preserve that behavior and only validate an fd when one is supplied.
	 */
	if (fd != -1) {
		if (fd < 0 || fd >= MAX_FD || cur->files->fds[fd] == NULL)
			return -EBADF;

		node = cur->files->fds[fd];
		if (node->f_inode == NULL)
			return -ENODEV;
		if (!S_ISREG(node->f_inode->i_mode) &&
		    !S_ISCHR(node->f_inode->i_mode))
			return -ENODEV;
	}

	if (node && node->f_inode->i_phys_size) {
		if (!cur->memory || cur->credentials->euid != 0 ||
		    (node->f_flag & O_PATH))
			return -EACCES;
		uint64_t end = (uint64_t)offset + _len;
		if ((node->f_inode->i_phys_base & (PAGE_SIZE - 1)) ||
		    end > node->f_inode->i_phys_size ||
		    (uint64_t)node->f_inode->i_phys_base + end >
			    PHYMM_ADDRESS_LIMIT)
			return -EINVAL;
		if ((prot & PROT_WRITE) && node->f_mode == O_RDONLY)
			return -EACCES;
		offset += node->f_inode->i_phys_base;
	}
	if (node && vm_region_flags_for_file(node) & VM_REGION_F_DIRECT_PHYS) {
		if (offset >= PHYMM_ADDRESS_LIMIT ||
		    _len > PHYMM_ADDRESS_LIMIT - offset)
			return -EINVAL;
	}

	if (node && node->f_fop && node->f_fop->mmap_file) {
		file *backing = NULL;
		intptr_t result = node->f_fop->mmap_file(node, &offset, _len,
							 prot, flags, &backing);
		if (result < 0)
			return result;
		result = do_mmap_kernel(_addr, _len, prot, flags, backing,
					offset);
		fs_put_file(backing);
		return result;
	}
	return do_mmap_kernel(_addr, _len, prot, flags, node, offset);
}

/*
 * vm_flush_dirty_region - write dirty MAP_SHARED pages in [begin, end) back
 * to the underlying file.
 *
 * Only acts on regions that are both MAP_SHARED and file-backed.  Pages that
 * were never faulted in (not present in the page table) are skipped.  The
 * dirty flag is cleared after a successful write so that a repeated flush
 * (e.g. partial unmap followed by exit) does not re-write clean pages.
 *
 * Called with the user's page tables still active so that (void *)vir is a
 * valid kernel-readable address.
 */
static void vm_flush_dirty_region(vm_region *region, vaddr_t begin, vaddr_t end)
{
	vaddr_t vir;
	if (!(region->flag & MAP_SHARED) || region->fp == NULL)
		return;
	if ((region->vm_flags & VM_REGION_F_DIRECT_PHYS) ||
	    (region->fp->f_fop && region->fp->f_fop->map_page))
		return;

	for (vir = begin; vir < end; vir += PAGE_SIZE) {
		unsigned page_index;
		uint64_t file_offset;

		/* Skip pages that were never faulted in. */
		if (mm_get_map_flag(vir) == 0)
			continue;

		page_index = mm_get_attached_page_index(vir);
		if (page_index < phymm_begin || page_index >= phymm_end ||
		    phymm_pages[page_index].ref_count == PHYMM_RESERVED)
			continue;
		if (!phymm_is_dirty(page_index))
			continue;

		file_offset = region->offset + (uint64_t)(vir - region->begin);

		if (region->fp->f_fop && region->fp->f_fop->write_page) {
			if (region->fp->f_fop->write_page(
				    region->fp, file_offset, (void *)vir) == 0)
				phymm_clear_dirty(page_index);
		} else {
			inode *node = region->fp->f_inode;
			ext4_file *ff = node->i_private;
			size_t wcnt = 0;

			if (ext4_fseek(ff, file_offset, SEEK_SET) != EOK)
				continue;
			if (ext4_fwrite(ff, (void *)vir, PAGE_SIZE, &wcnt) ==
			    EOK)
				phymm_clear_dirty(page_index);
		}
	}
}

/* vm_enum callback wrapper for vm_flush_all_dirty. */
static void vm_flush_region_cb(vm_region *region, void *data)
{
	(void)data;
	vm_flush_dirty_region(region, region->begin, region->end);
}

static int vm_same_file_identity(file *left, file *right)
{
	void *left_tag;
	void *right_tag;
	inode *left_inode;
	inode *right_inode;

	if (!left || !right)
		return 0;

	left_inode = left->f_inode;
	right_inode = right->f_inode;
	if (!left_inode || !right_inode)
		return 0;

	left_tag = left_inode->i_pgcache_tag ? left_inode->i_pgcache_tag :
					       left_inode->i_private;
	right_tag = right_inode->i_pgcache_tag ? right_inode->i_pgcache_tag :
						 right_inode->i_private;

	return left_tag == right_tag && left_inode->i_ino == right_inode->i_ino;
}

typedef struct _vm_flush_file_ctx {
	file *fp;
} vm_flush_file_ctx;

static void vm_flush_file_region_cb(vm_region *region, void *data)
{
	vm_flush_file_ctx *ctx = data;

	if (!ctx || !vm_same_file_identity(region->fp, ctx->fp))
		return;

	vm_flush_dirty_region(region, region->begin, region->end);
}

/*
 * vm_flush_all_dirty - flush every dirty MAP_SHARED page in the VM map.
 *
 * Called on process exit before the VM is torn down.
 */
void vm_flush_all_dirty(vm_struct_t vm)
{
	vm_enum(vm, vm_flush_region_cb, NULL);
}

void vm_flush_file_dirty(vm_struct_t vm, file *fp)
{
	vm_flush_file_ctx ctx;

	if (!vm || !fp)
		return;

	ctx.fp = fp;
	vm_enum(vm, vm_flush_file_region_cb, &ctx);
}

/*
 * do_munmap - syscall handler for munmap(2).
 *
 * Unmaps the page-aligned range [begin, end) from the current task's address
 * space.  Handles ranges that span multiple vm_regions by iterating over all
 * overlapping regions.  For each region the intersection with [begin, end) is
 * physically unmapped; portions outside [begin, end) are re-inserted as
 * independent mappings with their physical pages left intact.
 *
 * Returns 0 on success.
 */
int do_munmap(void *addr, size_t length)
{
	task_struct *cur = CURRENT_TASK();
	vaddr_t begin = ((vaddr_t)(uintptr_t)addr) & PAGE_SIZE_MASK;
	/* Round length up to a page count, then compute end — avoids the
	 * off-by-one that (addr+length+PAGE_SIZE-1)&PAGE_MASK produces when
	 * length is already a multiple of PAGE_SIZE. */
	size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
	vaddr_t end = begin + pages * PAGE_SIZE;
	mm_struct *mm = cur->memory;
	LOCK_GUARD(&mm->mapping_lock);
	vm_key probe;
	vm_region *region;
	vaddr_t vir;

	if (begin >= mm->task_size || length > mm->task_size - begin)
		return -EINVAL;
	if (!arch_mm_user_range_valid(begin, length))
		return -EINVAL;
	if (length == 0)
		return 0;

	probe.begin = begin;
	probe.end = end;

	while ((region = vm_tree_find(mm, &probe)) != NULL) {
		vaddr_t r_begin = region->begin;
		vaddr_t r_end = region->end;
		int r_prot = region->prot;
		int r_flag = region->flag;
		file *r_fp = region->fp;
		uint64_t r_offset = region->offset;
		unsigned r_anon_id = region->anon_id;
		vm_fault_lock *r_fault_lock = region->fault_lock;

		/* Intersection of this region with the unmap range. */
		vaddr_t unmap_begin = r_begin > begin ? r_begin : begin;
		vaddr_t unmap_end = r_end < end ? r_end : end;

		/* Flush dirty MAP_SHARED pages before physical unmap. */
		vm_flush_dirty_region(region, unmap_begin, unmap_end);

		/* Unmap physical pages only in the intersection [unmap_begin, unmap_end).
		 * Pages in remnant portions are left untouched in the page tables. */
		for (vir = unmap_begin; vir < unmap_end; vir += PAGE_SIZE)
			mm_unmap_page(vir);

		/* Hold references across descriptor removal. */
		if (r_fp)
			fs_get_file(r_fp);
		if ((r_flag & MAP_SHARED) && r_fp == NULL && r_anon_id)
			mm_anon_shared_get(r_anon_id);
		ref_count_get(r_fault_lock);
		vm_fault_lock_lock(r_fault_lock);

		/* Remove this vm_region descriptor from the tree. */
		vm_tree_remove(mm, region);

		/* Preserve the left remnant [r_begin, unmap_begin) if any.
		 * Its physical pages were not unmapped above. */
		if (r_begin < unmap_begin)
			vm_add_map_with_lock(cur->memory, r_begin, unmap_begin,
					     r_prot, r_flag, r_fp, r_offset,
					     r_anon_id, r_fault_lock);

		/* Preserve the right remnant [unmap_end, r_end) if any. */
		if (r_end > unmap_end)
			vm_add_map_with_lock(
				cur->memory, unmap_end, r_end, r_prot, r_flag,
				r_fp,
				r_offset + (uint64_t)(unmap_end - r_begin),
				r_anon_id, r_fault_lock);

		if (r_fp)
			fs_put_file(r_fp);
		if ((r_flag & MAP_SHARED) && r_fp == NULL && r_anon_id)
			mm_anon_shared_put(r_anon_id);
		vm_fault_lock_unlock(r_fault_lock);
		ref_count_put(r_fault_lock);
	}

	vm_invalidate_task_cache(cur);
	return 0;
}
