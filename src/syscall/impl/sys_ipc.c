#include <ps/ps.h>
#include <fs/fs.h>
#include <mm/mmap.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <device/time.h>
#include <lib/klib.h>
#include <macro.h>
#include <mm/mmu.h>
#include <errno.h>
#include <lib/rbtree.h>
#include <lib/slots.h>
#include <syscall/syscall.h>
#include <syscall/ipc.h>

#define MOS_IPC_PRIVATE 0
#define MOS_IPC_CREAT 01000
#define MOS_IPC_EXCL 02000
#define MOS_IPC_RMID 0
#define MOS_IPC_STAT 2
#define MOS_IPC_64 0x0100

#define MOS_SHM_R 0400
#define MOS_SHM_W 0200
#define MOS_SHM_RDONLY 010000
#define MOS_SHM_RND 020000

#define MOS_SHM_SEGMENT_MAX 32
#define MOS_SHM_ATTACH_MAX 64

struct mos_shm_segment {
	int used;
	int removed;
	int key;
	int shmid;
	size_t size;
	size_t page_count;
	paddr_t *pages;
	unsigned owner_pid;
	unsigned creator_uid;
	unsigned creator_gid;
	unsigned mode;
	unsigned ctime;
	unsigned attach_count;
	struct rb_node id_node, key_node;
};

struct mos_shm_attach {
	struct rb_node address_node;
	int used;
	int shmid;
	unsigned owner_pid;
	vaddr_t addr;
	size_t size;
};

static struct mos_shm_segment mos_shm_segments[MOS_SHM_SEGMENT_MAX];
static struct mos_shm_attach mos_shm_attaches[MOS_SHM_ATTACH_MAX];
static spinlock_t mos_shm_lock;
static int mos_shm_next_id = 1;
static struct rb_root shm_ids = _RBTREE_ROOT_INIT;
static struct rb_root shm_keys = _RBTREE_ROOT_INIT;
static struct rb_root shm_addresses = _RBTREE_ROOT_INIT;
static slot_pool shm_segment_slots, shm_attach_slots;

static struct mos_shm_attach *shm_address_find(unsigned owner, vaddr_t address)
{
	struct rb_node *node = shm_addresses.rb_node;
	struct mos_shm_attach *match = NULL;
	while (node) {
		struct mos_shm_attach *attach =
			rb_entry(node, struct mos_shm_attach, address_node);
		if (owner == attach->owner_pid && address == attach->addr) {
			match = attach;
			node = node->rb_left;
		} else {
			int before = owner < attach->owner_pid ||
				     (owner == attach->owner_pid &&
				      address < attach->addr);
			node = before ? node->rb_left : node->rb_right;
		}
	}
	return match;
}

static void shm_address_insert(struct mos_shm_attach *attach)
{
	struct rb_node **link = &shm_addresses.rb_node, *parent = NULL;
	while (*link) {
		struct mos_shm_attach *other =
			rb_entry(*link, struct mos_shm_attach, address_node);
		parent = *link;
		int before =
			attach->owner_pid < other->owner_pid ||
			(attach->owner_pid == other->owner_pid &&
			 (attach->addr < other->addr ||
			  (attach->addr == other->addr && attach < other)));
		link = before ? &parent->rb_left : &parent->rb_right;
	}
	rb_init_node(&attach->address_node);
	rb_link_node(&attach->address_node, parent, link);
	rb_insert_color(&attach->address_node, &shm_addresses);
}

static void mos_shm_release_pages(struct mos_shm_segment *seg)
{
	size_t i;

	if (!seg || !seg->pages)
		return;

	for (i = 0; i < seg->page_count; ++i) {
		paddr_t phy = seg->pages[i];
		unsigned page_index;

		if (!phy)
			continue;

		page_index = PHY_TO_PAGE_IDX(phy);
		if (phymm_dereference_page(page_index) == 0)
			phymm_free_user(page_index);
	}

	kfree(seg->pages);
	seg->pages = NULL;
}

static void mos_shm_destroy_segment(struct mos_shm_segment *seg)
{
	if (!seg)
		return;

	rb_erase(&seg->id_node, &shm_ids);
	if (seg->key != MOS_IPC_PRIVATE && !seg->removed)
		rb_erase(&seg->key_node, &shm_keys);
	mos_shm_release_pages(seg);
	slot_return(&shm_segment_slots, seg - mos_shm_segments);
	memset(seg, 0, sizeof(*seg));
}

static void mos_shm_init(void)
{
	spinlock_init(&mos_shm_lock);
}

KERNEL_INIT(7, mos_shm_init);

static struct mos_shm_segment *shm_ids_find(int key)
{
	struct rb_node *node = shm_ids.rb_node;
	while (node) {
		struct mos_shm_segment *seg =
			rb_entry(node, struct mos_shm_segment, id_node);
		if (key == seg->shmid)
			return seg;
		node = key < seg->shmid ? node->rb_left : node->rb_right;
	}
	return NULL;
}
static void shm_ids_insert(struct mos_shm_segment *seg)
{
	struct rb_node **link = &shm_ids.rb_node, *parent = NULL;
	while (*link) {
		struct mos_shm_segment *other =
			rb_entry(*link, struct mos_shm_segment, id_node);
		parent = *link;
		link = seg->shmid < other->shmid ? &parent->rb_left :
						   &parent->rb_right;
	}
	rb_init_node(&seg->id_node);
	rb_link_node(&seg->id_node, parent, link);
	rb_insert_color(&seg->id_node, &shm_ids);
}

static struct mos_shm_segment *shm_keys_find(int key)
{
	struct rb_node *node = shm_keys.rb_node;
	while (node) {
		struct mos_shm_segment *seg =
			rb_entry(node, struct mos_shm_segment, key_node);
		if (key == seg->key)
			return seg;
		node = key < seg->key ? node->rb_left : node->rb_right;
	}
	return NULL;
}
static void shm_keys_insert(struct mos_shm_segment *seg)
{
	struct rb_node **link = &shm_keys.rb_node, *parent = NULL;
	while (*link) {
		struct mos_shm_segment *other =
			rb_entry(*link, struct mos_shm_segment, key_node);
		parent = *link;
		link = seg->key < other->key ? &parent->rb_left :
					       &parent->rb_right;
	}
	rb_init_node(&seg->key_node);
	rb_link_node(&seg->key_node, parent, link);
	rb_insert_color(&seg->key_node, &shm_keys);
}

static int mos_shm_create(int key, size_t size, unsigned mode)
{
	task_struct *cur = CURRENT_TASK();
	int i;
	size_t page_count;
	paddr_t *pages;

	page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	if (page_count == 0)
		page_count = 1;

	pages = kmalloc(sizeof(*pages) * page_count);
	if (!pages)
		return -ENOMEM;
	memset(pages, 0, sizeof(*pages) * page_count);

	i = slot_take(&shm_segment_slots, MOS_SHM_SEGMENT_MAX);
	if (i < 0) {
		kfree(pages);
		return -ENOSPC;
	}
	struct mos_shm_segment *seg = &mos_shm_segments[i];
	memset(seg, 0, sizeof(*seg));
	seg->used = 1;
	seg->key = key;
	seg->shmid = mos_shm_next_id++;
	seg->size = (size + PAGE_SIZE - 1) & PAGE_SIZE_MASK;
	if (seg->size == 0)
		seg->size = PAGE_SIZE;
	seg->page_count = page_count;
	seg->pages = pages;
	seg->owner_pid = cur->psid;
	seg->creator_uid = cur->user ? cur->user->euid : 0;
	seg->creator_gid = cur->user ? cur->user->egid : 0;
	seg->mode = mode & 0777;
	seg->ctime = (unsigned)(time_wall_us() / 1000000ULL);
	shm_ids_insert(seg);
	if (key != MOS_IPC_PRIVATE)
		shm_keys_insert(seg);
	return seg->shmid;
}

static int mos_shm_ensure_page(struct mos_shm_segment *seg, size_t page_no,
			       paddr_t *phy_out)
{
	unsigned page_index;
	paddr_t phy;

	if (!seg || page_no >= seg->page_count || !phy_out)
		return -EINVAL;

	phy = seg->pages[page_no];
	if (phy == 0) {
		page_index = phymm_alloc_user();
		if (page_index == PHYMM_INVALID) {
			phymm_reclaim_user_cache(32);
			page_index = phymm_alloc_user();
			if (page_index == PHYMM_INVALID) {
				klog("shm: phymm_alloc_user failed shmid=%d page=%u\n",
				     seg->shmid, page_no);
				return -ENOMEM;
			}
		}

		phy = page_index * PAGE_SIZE;
		if (mm_kmap_phys(phy) != 1) {
			phymm_free_user(page_index);
			klog("shm: mm_kmap_phys failed shmid=%d page=%u phy=%llx\n",
			     seg->shmid, page_no, (unsigned long long)phy);
			return -ENOMEM;
		}
		memset((void *)PHY_TO_VIRT(phy), 0, PAGE_SIZE);
		mm_kunmap_phys(phy);

		/* The segment itself owns one persistent reference. */
		phymm_reference_page(page_index);
		seg->pages[page_no] = phy;
	}

	*phy_out = phy;
	return 0;
}

static int mos_shm_map_pages(vaddr_t addr, struct mos_shm_segment *seg,
			     int prot)
{
	vaddr_t vir;
	size_t page_no = 0;
	unsigned pte_flag = (prot & PROT_WRITE) ? PAGE_ENTRY_USER_DATA :
						  PAGE_ENTRY_USER_CODE;

	for (vir = addr; vir < addr + seg->size; vir += PAGE_SIZE, ++page_no) {
		paddr_t phy;

		if (mos_shm_ensure_page(seg, page_no, &phy) != 0)
			return -ENOMEM;
		if (mm_map_page(vir, phy, pte_flag) != 1)
			return -ENOMEM;
		arch_mm_invalidate(vir);
	}

	return 0;
}

int sys_shmget(int key, size_t size, int shmflg)
{
	struct mos_shm_segment *seg;
	int irq;
	int ret;

	if (size == 0)
		return -EINVAL;

	spinlock_lock(&mos_shm_lock, &irq);

	if (key != MOS_IPC_PRIVATE) {
		seg = shm_keys_find(key);
		if (seg) {
			if ((shmflg & MOS_IPC_CREAT) &&
			    (shmflg & MOS_IPC_EXCL)) {
				spinlock_unlock(&mos_shm_lock, irq);
				return -EEXIST;
			}
			if (size > seg->size) {
				spinlock_unlock(&mos_shm_lock, irq);
				return -EINVAL;
			}
			ret = seg->shmid;
			spinlock_unlock(&mos_shm_lock, irq);
			return ret;
		}
		if (!(shmflg & MOS_IPC_CREAT)) {
			spinlock_unlock(&mos_shm_lock, irq);
			return -ENOENT;
		}
	}

	ret = mos_shm_create(key, size, (unsigned)shmflg);
	spinlock_unlock(&mos_shm_lock, irq);
	return ret;
}

static intptr_t mos_shmat(int shmid, const void *shmaddr, int shmflg,
			  vaddr_t *user_raddr)
{
	task_struct *cur = CURRENT_TASK();
	struct mos_shm_segment *seg;
	struct mos_shm_attach *attach = NULL;
	vaddr_t addr = (uintptr_t)shmaddr;
	intptr_t mapped;
	int prot = PROT_READ | PROT_WRITE;
	int flags = MAP_SHARED | MAP_ANONYMOUS;
	int i;
	int irq;

	if (!user_raddr)
		return -EFAULT;

	if (addr != 0) {
		if (shmflg & MOS_SHM_RND)
			addr &= PAGE_SIZE_MASK;
		else if (addr & ~PAGE_SIZE_MASK)
			return -EINVAL;
		flags |= MAP_FIXED;
	}

	if (shmflg & MOS_SHM_RDONLY)
		prot = PROT_READ;

	spinlock_lock(&mos_shm_lock, &irq);
	seg = shm_ids_find(shmid);
	if (!seg || seg->removed) {
		spinlock_unlock(&mos_shm_lock, irq);
		return -EINVAL;
	}

	i = slot_take(&shm_attach_slots, MOS_SHM_ATTACH_MAX);
	if (i < 0) {
		spinlock_unlock(&mos_shm_lock, irq);
		return -ENOSPC;
	}
	attach = &mos_shm_attaches[i];
	spinlock_unlock(&mos_shm_lock, irq);

	mapped = do_mmap(addr, seg->size, prot, flags, -1, 0);
	if ((uintptr_t)mapped >= (uintptr_t)-4095) {
		spinlock_lock(&mos_shm_lock, &irq);
		slot_return(&shm_attach_slots, i);
		spinlock_unlock(&mos_shm_lock, irq);
		return mapped;
	}

	if (mos_shm_map_pages(mapped, seg, prot) != 0) {
		do_munmap((void *)(uintptr_t)mapped, seg->size);
		spinlock_lock(&mos_shm_lock, &irq);
		slot_return(&shm_attach_slots, i);
		spinlock_unlock(&mos_shm_lock, irq);
		return -ENOMEM;
	}

	*user_raddr = mapped;

	spinlock_lock(&mos_shm_lock, &irq);
	seg = shm_ids_find(shmid);
	if (!seg || seg->removed) {
		slot_return(&shm_attach_slots, i);
		spinlock_unlock(&mos_shm_lock, irq);
		do_munmap((void *)(uintptr_t)mapped,
			  seg ? seg->size : PAGE_SIZE);
		return -EINVAL;
	}

	memset(attach, 0, sizeof(*attach));
	attach->used = 1;
	attach->shmid = shmid;
	attach->owner_pid = cur->psid;
	attach->addr = mapped;
	attach->size = seg->size;
	shm_address_insert(attach);
	seg->attach_count++;
	spinlock_unlock(&mos_shm_lock, irq);
	return 0;
}

int sys_shmdt(const void *shmaddr)
{
	task_struct *cur = CURRENT_TASK();
	struct mos_shm_segment *seg;
	vaddr_t addr = (uintptr_t)shmaddr;
	size_t size = 0;
	int shmid = -1;
	int irq;

	spinlock_lock(&mos_shm_lock, &irq);
	struct mos_shm_attach *attach = shm_address_find(cur->psid, addr);
	if (attach) {
		size = attach->size;
		shmid = attach->shmid;
		rb_erase(&attach->address_node, &shm_addresses);
		slot_return(&shm_attach_slots, attach - mos_shm_attaches);
		memset(attach, 0, sizeof(*attach));
	}

	if (size == 0) {
		spinlock_unlock(&mos_shm_lock, irq);
		return -EINVAL;
	}

	seg = shm_ids_find(shmid);
	if (seg) {
		if (seg->attach_count > 0)
			seg->attach_count--;
		if (seg->removed && seg->attach_count == 0)
			mos_shm_destroy_segment(seg);
	}
	spinlock_unlock(&mos_shm_lock, irq);

	return do_munmap((void *)(uintptr_t)addr, size);
}

static int shm_remove(struct mos_shm_segment *seg, void *buf)
{
	if (!seg->removed && seg->key != MOS_IPC_PRIVATE)
		rb_erase(&seg->key_node, &shm_keys);
	seg->removed = 1;
	if (!seg->attach_count)
		mos_shm_destroy_segment(seg);
	return 0;
}

static int shm_stat(struct mos_shm_segment *seg, void *buf)
{
	if (!buf)
		return -EFAULT;
	struct shm_status *status = buf;
	*status = (struct shm_status){ .key = seg->key,
				       .uid = seg->creator_uid,
				       .gid = seg->creator_gid,
				       .mode = seg->mode,
				       .ctime = seg->ctime,
				       .cpid = seg->owner_pid,
				       .nattch = seg->attach_count,
				       .size = seg->size };
	return 0;
}

int ps_shmctl(int shmid, int cmd, struct shm_status *buf)
{
	static int (*const calls[])(struct mos_shm_segment *, void *) = {
		[MOS_IPC_RMID] = shm_remove,
		[MOS_IPC_STAT] = shm_stat,
	};
	struct mos_shm_segment *seg;
	int irq, ret;
	cmd &= ~MOS_IPC_64;
	spinlock_lock(&mos_shm_lock, &irq);
	seg = shm_ids_find(shmid);
	if (!seg)
		ret = -EINVAL;
	else if ((unsigned)cmd >= sizeof(calls) / sizeof(calls[0]) ||
		 !calls[cmd])
		ret = -ENOSYS;
	else
		ret = calls[cmd](seg, buf);
	spinlock_unlock(&mos_shm_lock, irq);
	return ret;
}

intptr_t sys_shmat(int shmid, const void *address, int flags)
{
	vaddr_t mapped;
	intptr_t ret = mos_shmat(shmid, address, flags, &mapped);
	return ret ? ret : (intptr_t)mapped;
}
