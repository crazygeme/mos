#ifndef _MM_MMAP_H
#define _MM_MMAP_H
#include <fs/fs.h>
#include <lib/lock.h>
#include <lib/rbtree.h>

typedef struct _user_enviroment user_enviroment;
typedef struct _mm_struct mm_struct;
typedef mm_struct *vm_struct_t;
typedef struct _vm_fault_lock vm_fault_lock;

#define VM_REGION_F_DIRECT_PHYS 0x1

typedef struct _vm_region {
	struct rb_node rb_node;
	unsigned begin;
	unsigned end;
	int prot;
	int flag;
	unsigned vm_flags; /* Internal VM metadata, not userspace mmap flags. */
	vm_fault_lock *
		fault_lock; /* Shared across fork/splits for fault serialization. */
	file *fp;
	int offset;
	unsigned anon_id; /* non-zero for MAP_SHARED|MAP_ANONYMOUS; shared across fork */
} vm_region;

/* Process address-space descriptor. */
struct _mm_struct {
	struct rb_root vma_index;
	spinlock_t vma_lock;
	unsigned page_dir;
	unsigned start_brk;
	unsigned brk;
	unsigned start_stack;
	unsigned mmap_base;
	unsigned task_size;
	unsigned users;
	unsigned count;
};

static inline mm_struct *vm_mm(vm_struct_t vm)
{
	return vm;
}
static inline void vm_set_page_dir(vm_struct_t vm, unsigned pd)
{
	if (vm)
		vm->page_dir = pd;
}
static inline unsigned vm_get_page_dir(vm_struct_t vm)
{
	return vm ? vm->page_dir : 0;
}
static inline void vm_set_brk(vm_struct_t vm, unsigned start, unsigned brk)
{
	if (vm) {
		vm->start_brk = start;
		vm->brk = brk;
	}
}
static inline void vm_set_stack(vm_struct_t vm, unsigned bottom)
{
	if (vm)
		vm->start_stack = bottom;
}

vm_struct_t vm_create();

void vm_get(vm_struct_t vm);
void vm_put(vm_struct_t vm);

void vm_destroy(vm_struct_t vm);

/**
 * map begin <= addr < end to virtual, with related fd fd can be
 * -1, which means it's not related with file when map to an
 * already maped reagion, it will split regions
 *
 * @author zhengender (8/3/2014)
 *
 * @param begin
 * @param end
 * @param fd
 */
void vm_add_map(vm_struct_t vm, unsigned begin, unsigned end, int prot,
		int flag, file *fp, int offset, unsigned anon_id);
void vm_add_map_clone(vm_struct_t vm, vm_region *src);

/* Extend the mapping that starts at @begin from @old_end to @new_end. */
int vm_extend_map(vm_struct_t vm, unsigned begin, unsigned old_end,
		  unsigned new_end);

/**
 * delete a mapped region which contains addr
 *
 * @author zhengender (8/3/2014)
 *
 * @param addr
 */
void vm_del_map(vm_struct_t vm, unsigned addr);

/* Update protection of [begin, end) without unmapping physical pages. */
void vm_mprotect(vm_struct_t vm, unsigned begin, unsigned end, int new_prot);

/**
 * find the region that contains addr
 *
 * @author zhengender (8/3/2014)
 *
 * @param addr
 *
 * @return vm_region*
 */
vm_region *vm_find_map(vm_struct_t vm, unsigned addr);

/*
 * vm_find_vma - return the mapping containing @addr, or the next mapping above
 * it if @addr is in a hole. Returns NULL when no VMA exists at or above @addr.
 */
vm_region *vm_find_vma(vm_struct_t vm, unsigned addr);

/*
 * Linux-style per-task last-hit cache around vm_find_vma/vm_find_map.
 * The cache stores the last vm_find_vma() result, which may be a containing
 * VMA or the next VMA above the probed address.
 */
vm_region *vm_find_vma_cached(user_enviroment *user, unsigned addr);
vm_region *vm_find_map_cached(user_enviroment *user, unsigned addr);
void vm_invalidate_user_cache(user_enviroment *user);
void vm_region_lock_fault(vm_region *region);
void vm_region_unlock_fault(vm_region *region);

/**
 * find a region large enough to contain @size
 * return the begin address
 *
 * @author zhengender (8/3/2014)
 *
 * @param size
 *
 * @return unsigned
 */
unsigned vm_disc_map(vm_struct_t vm, int size);

/**
 * dup vm maps from cur into new
 *
 * @author zhengender (9/7/2014)
 *
 * @param cur
 */
void vm_dup(vm_struct_t src, vm_struct_t dst);

/**
 * vm_enum - invoke fn(region, data) for every mapped region in vm.
 * Regions are visited in unspecified order.
 */
typedef void (*vm_enum_fn)(vm_region *region, void *data);
void vm_enum(vm_struct_t vm, vm_enum_fn fn, void *data);

/**
 * vm_flush_all_dirty - write back all dirty MAP_SHARED file-backed pages.
 * Called on process exit before the VM is torn down.
 */
void vm_flush_all_dirty(vm_struct_t vm);

/*
 * vm_flush_file_dirty - write back dirty MAP_SHARED file-backed pages for one
 * file identity within a VM map.
 */
void vm_flush_file_dirty(vm_struct_t vm, file *fp);

#endif
