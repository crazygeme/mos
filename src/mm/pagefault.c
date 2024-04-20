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

unsigned page_fault_cow = 0;
unsigned page_fault_invalid = 0;
unsigned page_fault_file = 0;
unsigned page_fault_file_read = 0;
unsigned page_fault_perm = 0;
unsigned long long page_fault_cow_spent = 0;
unsigned long long page_fault_invalid_spent = 0;
unsigned long long page_fault_file_spent = 0;
unsigned long long page_fault_perm_spent = 0;

static hash_table *mmap_cache = NULL;
static mutex_t mmap_cache_lock;
static unsigned zero_page = NULL;
static mutex_t zero_page_lock;

static void pf_process(intr_frame *frame);

typedef struct _mmap_cache_key {
	char *path;
	unsigned offset;
} mmap_cache_key;

static int mmap_key_comp(void *k1, void *k2)
{
	mmap_cache_key *key1 = k1;
	mmap_cache_key *key2 = k2;

	int ret = strcmp(key1->path, key2->path);
	if (ret == 0) {
		ret = (int)key1->offset - (int)key2->offset;
	}

	return ret;
}

void pf_init()
{
	int_register(0xe, pf_process, 0, 0);
	mmap_cache = hash_create(mmap_key_comp);
	mutex_init(&mmap_cache_lock);
	mutex_init(&zero_page_lock);
}

static unsigned mmap_cache_find(const char *path, unsigned offset)
{
	key_value_pair *pair = NULL;
	mmap_cache_key tmp = { .path = path, .offset = offset };

	mutex_lock(&mmap_cache_lock);
	pair = hash_find(mmap_cache, &tmp);
	mutex_unlock(&mmap_cache_lock);

	return pair ? pair->val : NULL;
}

static void mmap_cache_add(const char *path, unsigned offset, unsigned phy)
{
	unsigned size = 0;
	mmap_cache_key *key = malloc(sizeof(*key));

	/*
	 * Currently NO cache size limit
	 */
	key->path = strdup(path);
	key->offset = offset;
	mutex_lock(&mmap_cache_lock);
	hash_insert(mmap_cache, key, phy);
	phymm_reference_page(PHY_TO_PAGE_IDX(phy));
	mutex_unlock(&mmap_cache_lock);
}

static unsigned zero_page_get()
{
	unsigned ret = 0;

	mutex_lock(&zero_page_lock);
	ret = zero_page;
	mutex_unlock(&zero_page_lock);

	return ret;
}

static unsigned zero_page_put(unsigned phy)
{
	mutex_lock(&zero_page_lock);
	if (zero_page) {
		phymm_dereference_page(PHY_TO_PAGE_IDX(zero_page));
	}

	zero_page = phy;
	phymm_reference_page(PHY_TO_PAGE_IDX(zero_page));
	mutex_unlock(&zero_page_lock);
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

static int pf_handle_invalid_file_map(unsigned address, filep f,
				      unsigned offset)
{
	int ret = 0;
	unsigned phy = NULL;
	ext4_file *ff;
	size_t rcnt = 0;
	unsigned long long begin = time_now_us();

	page_fault_file++;

	phy = mmap_cache_find(f->name, offset);
	if (phy != NULL) {
		mm_add_dynamic_map(address, phy, PAGE_ENTRY_USER_CODE);
		RELOAD_CR3();

	} else {
		phy = phymm_alloc_user() * PAGE_SIZE;

		mm_add_dynamic_map(address, phy, PAGE_ENTRY_USER_CODE);
		RELOAD_CR3();

		ff = f->inode;
		ret = ext4_fseek(ff, offset, SEEK_SET);
		if (ret != EOK) {
			klog("FAIL: mmap: seek to off %x\n", offset);
		}

		ret = ext4_fread(ff, address, PAGE_SIZE, &rcnt);
		if (ret != EOK) {
			klog("FAIL: mmap: read to buffer %x, size %x\n",
			     address, PAGE_SIZE);
		}

		mmap_cache_add(f->name, offset, phy);
		page_fault_file_read += rcnt;
	}

	page_fault_file_spent += (time_now_us() - begin);
}

static int pf_handle_invalid_memory(unsigned address)
{
	unsigned long long begin = time_now_us();

	mm_add_dynamic_map(address, 0, PAGE_ENTRY_USER_DATA);
	RELOAD_CR3();
	memset(address, 0, PAGE_SIZE);
	page_fault_invalid++;
	page_fault_invalid_spent += (time_now_us() - begin);
}

static int pf_handle_page_invalid(unsigned cr2)
{
	vm_region *region;
	unsigned this_offset;
	unsigned this_begin;
	task_struct *cur = CURRENT_TASK();

	this_begin = cr2 & PAGE_SIZE_MASK;
	region = vm_find_map(cur->user.vm, this_begin);
	if (!region)
		return 0;

	this_offset = region->offset + (this_begin - region->begin);

	if (region->node != 0)
		pf_handle_invalid_file_map(this_begin, region->node,
					   this_offset);
	else
		pf_handle_invalid_memory(this_begin);

	return 1;
}

static void pf_handle_cow(unsigned cr2)
{
	unsigned vir = 0;
	unsigned new_mem = 0;
	int flag;
	unsigned long long begin = time_now_us();

	page_fault_cow++;

	vir = cr2;
	vir = (vir & PAGE_SIZE_MASK);
	// 1. alloc a new physical memory
	new_mem = vm_alloc(1);

	// 2. copy cow value
	memcpy(new_mem, vir, PAGE_SIZE);

	// 3. unmap origin address
	flag = mm_get_map_flag(vir);
	mm_del_dynamic_map(vir);

	// 4. unmap newly allocated physical memory
	// note that should ref it first so that on one will use
	// this physical memory
	phymm_reference_page(VIRT_TO_PAGE_IDX(new_mem));
	mm_del_direct_map(new_mem);

	// 5. map newly allocated physical memory to origin virtual address
	flag |= PAGE_ENTRY_PRESENT;
	flag |= PAGE_ENTRY_WRITABLE;
	mm_add_dynamic_map(vir, VIRT_TO_PHY(new_mem), flag);

	phymm_dereference_page(VIRT_TO_PAGE_IDX(new_mem));

	RELOAD_CR3();

	page_fault_cow_spent += (time_now_us() - begin);
}

static void pf_handle_readonly(unsigned cr2)
{
	unsigned vir = 0;
	int flag;
	unsigned long long begin = time_now_us();

	page_fault_perm++;
	vir = cr2;
	vir = (vir & PAGE_SIZE_MASK);
	flag = mm_get_map_flag(vir);
	flag |= PAGE_ENTRY_WRITABLE;
	mm_set_map_flag(vir, flag);
	RELOAD_CR3();

	page_fault_perm_spent += (time_now_us() - begin);
}

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
	int page_valid;
	int write_access;
	unsigned oldint;

	oldint = int_intr_disable();
	LOAD_CR2(cr2);
	page_valid = ((error & PF_MASK_P) == PF_MASK_P);
	write_access = ((error & PF_MASK_RW) == PF_MASK_RW);

	if (!page_valid) {
		if (pf_handle_page_invalid(cr2))
			goto Done;

		goto HLT;
	} else if (write_access) {
		if (pf_handle_permission(cr2))
			goto Done;

		goto HLT;
	}

HLT:
	do {
		task_struct *cur = CURRENT_TASK();
		printk("[%d]page fault! error code %x, address %x, eip %x\n",
		       cur->psid, frame->error_code, cr2, frame->eip);
	} while (0);

	DIE();

Done:
	if (oldint) {
		int_intr_enable();
	}
}
