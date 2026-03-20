#include <mm/mm.h>
#include <lib/list.h>
#include <lib/rbtree.h>
#include <lib/klib.h>
#include <mm/mmap.h>
#include <ps/ps.h>
#include <fs/fs.h>
#include <macro.h>
#include <config.h>
#include <errno.h>

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
		mm_del_dynamic_map(vir);
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
 *               within [USER_ZONE_BEGIN, KERNEL_OFFSET).
 *
 * Iterates the sorted region list and returns the start of the first gap
 * that is large enough.  The candidate start is always clamped to at least
 * USER_ZONE_BEGIN so that existing mappings below the user zone (text,
 * stack set up by the process loader) are skipped over.  Returns 0 if no
 * suitable gap exists.
 */
unsigned vm_disc_map(vm_struct_t vm, int size)
{
	hash_table *table = vm;
	key_value_pair *pair = hash_first(table);
	unsigned candidate = USER_ZONE_BEGIN;
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
	task_struct *cur = CURRENT_TASK();

	if (_addr == 0)
		addr = vm_disc_map(cur->user->vm, page_count * PAGE_SIZE);

	vm_add_map(cur->user->vm, addr, addr + page_count * PAGE_SIZE, prot,
		   flags, fp, offset);

	if (TestControl.verbos) {
		klog("mmap: file %s, addr %x, offset %x, len %x at addr %x\n",
		     fp ? fp->f_name : "ANON", _addr, offset, _len, addr);
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
 * do_munmap - syscall handler for munmap(2).
 *
 * Unmaps the page-aligned range [begin, end) from the current task's address
 * space.  If the range is a strict subset of an existing region, the region is
 * split and the portions outside [begin, end) are re-inserted as independent
 * mappings.
 *
 * Returns 0 on success, -EINVAL if no region covers addr or the range extends
 * beyond the containing region.
 */
int do_munmap(void *addr, unsigned length)
{
	task_struct *cur = CURRENT_TASK();
	unsigned begin = ((unsigned)addr) & PAGE_SIZE_MASK;
	unsigned end = ((unsigned)addr + length + PAGE_SIZE - 1) &
		       PAGE_SIZE_MASK;
	vm_region *region;
	unsigned r_begin, r_end;
	int r_prot, r_flag;
	void *r_fp;
	int r_offset;

	region = vm_find_map(cur->user->vm, (unsigned)addr);
	if (!region)
		return -EINVAL;

	/* The unmap range must be fully contained within one existing region. */
	if (begin < region->begin || end > region->end)
		return -EINVAL;

	/* Snapshot region data before vm_del_map() frees the region struct. */
	r_begin = region->begin;
	r_end = region->end;
	r_prot = region->prot;
	r_flag = region->flag;
	r_fp = region->fp;
	r_offset = region->offset;

	vm_del_map(cur->user->vm, (unsigned)addr);

	/* Preserve the left remnant [r_begin, begin), if any. */
	if (r_begin < begin)
		vm_add_map(cur->user->vm, r_begin, begin, r_prot, r_flag, r_fp,
			   r_offset);

	/* Preserve the right remnant [end, r_end), if any. */
	if (end < r_end)
		vm_add_map(cur->user->vm, end, r_end, r_prot, r_flag, r_fp,
			   r_offset + (int)(end - r_begin));

	return 0;
}
