#include <mm/mm.h>
#include <mm/phymm.h>
#include <lib/list.h>
#include <lib/rbtree.h>
#include <lib/klib.h>
#include <mm/mmap.h>
#include <ps/ps.h>
#include <fs/fs.h>
#include <macro.h>
#include <config.h>
#include <errno.h>
#include <ext4.h>

/*
 * vm_key is the search key for the red-black tree that backs each process's
 * VM map.  The comparator treats any range overlap as equal (returns 0), so
 * hash_find() works as an overlap query: it returns any existing region that
 * intersects the probe key.
 */
typedef struct _vm_key {
	unsigned begin;
	unsigned end;
} vm_key;

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

/*
 * vm_can_merge - true if two adjacent regions can be folded into one.
 *
 * Anonymous regions (fp == NULL) only need matching prot and flag.
 * File-backed regions additionally require the same fp and contiguous
 * file offsets (right.offset == left.offset + left_size).
 */
static INLINE int vm_can_merge(const vm_region *left, const vm_region *right)
{
	if (left->prot != right->prot || left->flag != right->flag)
		return 0;
	if (left->fp != right->fp)
		return 0;
	if (left->fp != NULL &&
	    left->offset + (int)(left->end - left->begin) != right->offset)
		return 0;
	return 1;
}

static void vm_region_invalid(const key_value_pair *pair)
{
	vm_key *key = pair->key;
	vm_region *region = pair->val;

	if (region->fp)
		fs_put_file(region->fp);

	kfree(key);
	kfree(region);
}

vm_struct_t vm_create()
{
	return hash_create(vm_region_compare, vm_region_invalid);
}

void vm_destroy(vm_struct_t vm)
{
	hash_destroy((hash_table *)vm);
}

/*
 * vm_find_pair - return the tree pair whose region contains addr, or NULL.
 *
 * addr is rounded down to its page boundary before probing the tree.
 */
static INLINE key_value_pair *vm_find_pair(hash_table *table, unsigned addr)
{
	vm_key key;
	key.begin = addr & PAGE_SIZE_MASK;
	key.end = key.begin + PAGE_SIZE;
	return hash_find(table, &key);
}

/*
 * vm_coalesce - try to merge the region at @addr with its immediate neighbors.
 *
 * After inserting a new region, check whether the left neighbor (whose end
 * equals our begin) or the right neighbor (whose begin equals our end) can be
 * folded in.  If so, both are removed and vm_add_map() is called for the
 * combined range; that recursive call will coalesce further if needed.
 *
 * File ref-counting mirrors the vm_mprotect() pattern: bump once before
 * hash_remove() so the invalidator's fs_put_file() does not drop to zero,
 * then release after vm_add_map() has taken its own reference.
 */
static void vm_coalesce(hash_table *table, unsigned addr)
{
	key_value_pair *pair = vm_find_pair(table, addr);
	vm_key probe;

	if (!pair)
		return;

	vm_region *region = pair->val;
	vm_key    *rkey   = pair->key;

	/* --- left neighbor: probe [begin-1, begin) ---------------------- */
	if (region->begin > 0) {
		probe.begin = region->begin - 1;
		probe.end   = region->begin;
		key_value_pair *lpair = hash_find(table, &probe);
		if (lpair) {
			vm_region *lregion = lpair->val;
			if (lregion->end == region->begin &&
			    vm_can_merge(lregion, region)) {
				unsigned new_begin  = lregion->begin;
				unsigned new_end    = region->end;
				int      new_prot   = region->prot;
				int      new_flag   = region->flag;
				file    *new_fp     = region->fp;
				int      new_offset = lregion->offset;

				if (new_fp)
					fs_get_file(new_fp);
				hash_remove(table, lpair->key);
				hash_remove(table, rkey);
				vm_add_map(table, new_begin, new_end, new_prot,
					   new_flag, new_fp, new_offset);
				if (new_fp)
					fs_put_file(new_fp);
				return;
			}
		}
	}

	/* --- right neighbor: probe [end, end+1) ------------------------- */
	probe.begin = region->end;
	probe.end   = region->end + 1;
	{
		key_value_pair *rpair = hash_find(table, &probe);
		if (rpair) {
			vm_region *rregion = rpair->val;
			if (rregion->begin == region->end &&
			    vm_can_merge(region, rregion)) {
				unsigned new_begin  = region->begin;
				unsigned new_end    = rregion->end;
				int      new_prot   = region->prot;
				int      new_flag   = region->flag;
				file    *new_fp     = region->fp;
				int      new_offset = region->offset;

				if (new_fp)
					fs_get_file(new_fp);
				hash_remove(table, rkey);
				hash_remove(table, rpair->key);
				vm_add_map(table, new_begin, new_end, new_prot,
					   new_flag, new_fp, new_offset);
				if (new_fp)
					fs_put_file(new_fp);
				return;
			}
		}
	}
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
void vm_add_map(vm_struct_t vm, unsigned begin, unsigned end, int prot,
		int flag, file *fp, int offset)
{
	hash_table *table = vm;
	vm_key probe;
	key_value_pair *pair;

	/*
	 * Resolve conflicts iteratively.  Each pass finds one overlapping
	 * region, saves its extent, removes it, then re-inserts the non-
	 * overlapping left and right remnants.  Those remnants never conflict
	 * with [begin, end), so their recursive vm_add_map calls insert
	 * directly without further iteration.
	 */
	probe.begin = begin;
	probe.end = end;
	while ((pair = hash_find(table, &probe)) != NULL) {
		vm_key *okey = pair->key;
		vm_region *oregion = pair->val;

		/* Snapshot all origin data before vm_del_map frees the structs. */
		unsigned o_begin = okey->begin;
		unsigned o_end = okey->end;
		int o_prot = oregion->prot;
		int o_flag = oregion->flag;
		file *o_fp = oregion->fp;
		int o_offset = oregion->offset;

		vm_del_map(vm, o_begin);

		/* Re-insert the left remnant [o_begin, begin), if any. */
		if (o_begin < begin)
			vm_add_map(vm, o_begin, begin, o_prot, o_flag, o_fp,
				   o_offset);

		/*
		 * Re-insert the right remnant [end, o_end), if any.
		 * Its file offset advances by (end - o_begin) bytes.
		 */
		if (o_end > end)
			vm_add_map(vm, end, o_end, o_prot, o_flag, o_fp,
				   o_offset + (int)(end - o_begin));
	}

	/* No conflicts remain: insert the new region. */
	vm_key *key = kmalloc(sizeof(*key));
	key->begin = begin;
	key->end = end;

	vm_region *region = kmalloc(sizeof(*region));
	region->begin = begin;
	region->end = end;
	region->prot = prot;
	region->flag = flag;
	region->fp = fp;
	region->offset = offset;

	if (fp)
		fs_get_file(fp);

	hash_insert(table, key, region);
	vm_coalesce(table, begin);
}

/*
 * vm_del_map - remove the mapping that contains addr and unmap its pages.
 *
 * addr is rounded down to the nearest page boundary.  All hardware page-table
 * entries for the region are cleared and CR3 is reloaded to flush the TLB.
 */
void vm_del_map(vm_struct_t vm, unsigned addr)
{
	hash_table *table = vm;
	key_value_pair *pair;
	vm_key *key;
	vm_region *region;
	unsigned vir;

	addr &= PAGE_SIZE_MASK;

	pair = vm_find_pair(table, addr);
	if (!pair)
		return;

	key = pair->key;
	region = pair->val;

	/* FIXME(Ender:) flush file if has one */

	/* Unmap every page in the region from the hardware page tables. */
	for (vir = region->begin; vir < region->end; vir += PAGE_SIZE)
		mm_unmap_page(vir);
	RELOAD_CR3();

	hash_remove(table, key);
}

/*
 * vm_find_map - find the region that contains addr.
 *
 * Returns a pointer to the live vm_region (caller must not free it),
 * or NULL if no region covers addr.
 */
vm_region *vm_find_map(vm_struct_t vm, unsigned addr)
{
	hash_table *table = vm;
	key_value_pair *pair;

	addr &= PAGE_SIZE_MASK;
	pair = vm_find_pair(table, addr);
	return pair ? pair->val : 0;
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
unsigned vm_disc_map(vm_struct_t vm, int size)
{
	hash_table *table = vm;
	key_value_pair *pair = hash_first(table);
	unsigned candidate = TASK_UNMAPPED_BASE;
	vm_key *key;

	while (pair) {
		key = pair->key;

		/* Gap before this region is large enough — use it. */
		if (candidate + (unsigned)size <= key->begin)
			return candidate;

		/* Advance candidate past this region if it overlaps. */
		if (key->end > candidate)
			candidate = key->end;

		pair = hash_next(table, pair);
	}

	/* Check for room after the last region. */
	if (candidate + (unsigned)size <= KERNEL_OFFSET)
		return candidate;

	return 0;
}

/*
 * vm_dup - copy all VM mappings from @src into @dst.
 *
 * Used during fork() to duplicate the parent's address space descriptor into
 * the child.  vm_add_map() handles the fs_get_file() reference for each node.
 */
void vm_dup(vm_struct_t src, vm_struct_t dst)
{
	hash_table *table = src;
	key_value_pair *pair = hash_first(table);

	while (pair) {
		vm_region *region = pair->val;
		vm_add_map(dst, region->begin, region->end, region->prot,
			   region->flag, region->fp, region->offset);
		pair = hash_next(table, pair);
	}
}

void vm_enum(vm_struct_t vm, vm_enum_fn fn, void *data)
{
	hash_table *table = (hash_table *)vm;
	key_value_pair *kv;

	if (!vm || !fn)
		return;
	for (kv = hash_first(table); kv; kv = hash_next(table, kv))
		fn((vm_region *)kv->val, data);
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
 * File reference counting: we temporarily bump the ref before hash_remove()
 * drops it via vm_region_invalid(), then release our bump at the end.
 */
void vm_mprotect(vm_struct_t vm, unsigned begin, unsigned end, int new_prot)
{
	hash_table *table = vm;
	vm_key probe;
	key_value_pair *pair;

	probe.begin = begin;
	probe.end = end;

	while ((pair = hash_find(table, &probe)) != NULL) {
		vm_key *okey = pair->key;
		vm_region *oregion = pair->val;

		unsigned r_begin = oregion->begin;
		unsigned r_end = oregion->end;
		int r_prot = oregion->prot;
		int r_flag = oregion->flag;
		file *r_fp = oregion->fp;
		int r_offset = oregion->offset;

		/* Intersection of the region with [begin, end) */
		unsigned upd_begin = r_begin > begin ? r_begin : begin;
		unsigned upd_end = r_end < end ? r_end : end;

		/* Temporarily hold a file ref across the hash_remove() drop. */
		if (r_fp)
			fs_get_file(r_fp);

		/* Remove descriptor only — physical pages stay mapped. */
		hash_remove(table, okey);

		/* Preserve left remnant [r_begin, upd_begin) at original prot. */
		if (r_begin < upd_begin)
			vm_add_map(vm, r_begin, upd_begin, r_prot, r_flag, r_fp,
				   r_offset);

		/* Re-insert updated portion [upd_begin, upd_end) with new prot. */
		vm_add_map(vm, upd_begin, upd_end, new_prot, r_flag, r_fp,
			   r_offset + (int)(upd_begin - r_begin));

		/* Preserve right remnant [upd_end, r_end) at original prot. */
		if (upd_end < r_end)
			vm_add_map(vm, upd_end, r_end, r_prot, r_flag, r_fp,
				   r_offset + (int)(upd_end - r_begin));

		if (r_fp)
			fs_put_file(r_fp);

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
 */
int do_mmap_kernel(unsigned int _addr, unsigned int _len, unsigned int prot,
		   unsigned int flags, file *fp, unsigned int offset)
{
	unsigned addr = _addr & PAGE_SIZE_MASK;
	unsigned last_addr = (_addr + _len - 1) & PAGE_SIZE_MASK;
	unsigned page_count = (last_addr - addr) / PAGE_SIZE + 1;
	unsigned size = page_count * PAGE_SIZE;
	task_struct *cur = CURRENT_TASK();
	hash_table *table = cur->user->vm;
	vm_key probe;

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
			if (hash_find(table, &probe) != NULL)
				addr = 0; /* fall through to vm_disc_map */
		}
		if (addr == 0)
			addr = vm_disc_map(cur->user->vm, size);
	}

	vm_add_map(cur->user->vm, addr, addr + size, prot, flags, fp, offset);

	if (TestControl.verbos) {
		klog("mmap: file %s, addr %x, offset %x, prot %x, flags %x, len %x at addr %x\n",
		     fp ? fp->f_name : "ANON", _addr, offset, prot, flags, _len,
		     addr);
	}

	return addr;
}

void do_mmap_update(unsigned int _addr, unsigned int prot, unsigned int flags)
{
	unsigned addr = _addr & PAGE_SIZE_MASK;
	task_struct *cur = CURRENT_TASK();
	vm_region *region;
	unsigned vir;

	region = vm_find_map(cur->user->vm, addr);
	if (region) {
		region->prot = prot;
		region->flag = flags;
	}

	/* Also update actual mmap flag */
	for (vir = region->begin; vir < region->end; vir += PAGE_SIZE) {
		unsigned mmflag = mm_get_map_flag(vir);
		if (prot & PROT_WRITE)
			mmflag |= PAGE_ENTRY_WRITABLE;
		else
			mmflag &= ~PAGE_ENTRY_WRITABLE;

		mm_set_map_flag(vir, mmflag);
	}

	RELOAD_CR3();
}

/*
 * do_mmap - syscall handler for mmap(2).
 *
 * Resolves the file descriptor to an inode pointer (NULL for anonymous
 * mappings where fd == -1) and delegates to do_mmap_kernel().
 */
int do_mmap(unsigned int _addr, unsigned int _len, unsigned int prot,
	    unsigned int flags, int fd, unsigned int offset)
{
	task_struct *cur = CURRENT_TASK();
	void *node = (fd != -1 && cur->fds[fd].used != 0) ? cur->fds[fd].fp : 0;

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
static void vm_flush_dirty_region(vm_region *region, unsigned begin,
				  unsigned end)
{
	ext4_file *ff;
	unsigned vir;

	if (!(region->flag & MAP_SHARED) || region->fp == NULL)
		return;

	ff = region->fp->f_inode->i_private;

	for (vir = begin; vir < end; vir += PAGE_SIZE) {
		unsigned page_index;
		int file_offset;
		size_t wcnt = 0;

		/* Skip pages that were never faulted in. */
		if (mm_get_map_flag(vir) == 0)
			continue;

		page_index = mm_get_attached_page_index(vir);
		if (!phymm_is_dirty(page_index))
			continue;

		file_offset = region->offset + (int)(vir - region->begin);
		if (ext4_fseek(ff, file_offset, SEEK_SET) != EOK)
			continue;

		if (ext4_fwrite(ff, (void *)vir, PAGE_SIZE, &wcnt) == EOK)
			phymm_clear_dirty(page_index);
	}
}

/* vm_enum callback wrapper for vm_flush_all_dirty. */
static void vm_flush_region_cb(vm_region *region, void *data)
{
	(void)data;
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
int do_munmap(void *addr, unsigned length)
{
	task_struct *cur = CURRENT_TASK();
	unsigned begin = ((unsigned)addr) & PAGE_SIZE_MASK;
	/* Round length up to a page count, then compute end — avoids the
	 * off-by-one that (addr+length+PAGE_SIZE-1)&PAGE_MASK produces when
	 * length is already a multiple of PAGE_SIZE. */
	unsigned pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
	unsigned end = begin + pages * PAGE_SIZE;
	vm_key probe;
	key_value_pair *pair;
	unsigned vir;

	if (length == 0)
		return 0;

	probe.begin = begin;
	probe.end = end;

	while ((pair = hash_find(cur->user->vm, &probe)) != NULL) {
		vm_region *region = pair->val;
		vm_key *key = pair->key;
		unsigned r_begin = region->begin;
		unsigned r_end = region->end;
		int r_prot = region->prot;
		int r_flag = region->flag;
		file *r_fp = region->fp;
		int r_offset = region->offset;

		/* Intersection of this region with the unmap range. */
		unsigned unmap_begin = r_begin > begin ? r_begin : begin;
		unsigned unmap_end = r_end < end ? r_end : end;

		/* Flush dirty MAP_SHARED pages before physical unmap. */
		vm_flush_dirty_region(region, unmap_begin, unmap_end);

		/* Unmap physical pages only in the intersection [unmap_begin, unmap_end).
		 * Pages in remnant portions are left untouched in the page tables. */
		for (vir = unmap_begin; vir < unmap_end; vir += PAGE_SIZE)
			mm_unmap_page(vir);

		/* Hold a file ref across the evict() drop inside hash_remove(). */
		if (r_fp)
			fs_get_file(r_fp);

		/* Remove this vm_region descriptor from the tree. */
		hash_remove(cur->user->vm, key);

		/* Preserve the left remnant [r_begin, unmap_begin) if any.
		 * Its physical pages were not unmapped above. */
		if (r_begin < unmap_begin)
			vm_add_map(cur->user->vm, r_begin, unmap_begin, r_prot,
				   r_flag, r_fp, r_offset);

		/* Preserve the right remnant [unmap_end, r_end) if any. */
		if (r_end > unmap_end)
			vm_add_map(cur->user->vm, unmap_end, r_end, r_prot,
				   r_flag, r_fp,
				   r_offset + (int)(unmap_end - r_begin));

		if (r_fp)
			fs_put_file(r_fp);
	}

	RELOAD_CR3();
	return 0;
}
