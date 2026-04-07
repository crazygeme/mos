#include <ps/ps.h>
#include <fs/fs.h>
#include <mm/mmap.h>
#include <mm/mm.h>
#include <mm/cache.h>
#include <hw/time.h>
#include <lib/klib.h>
#include <macro.h>
#include <errno.h>

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

#define MOS_SHMGET 23
#define MOS_SHMAT 21
#define MOS_SHMDT 22
#define MOS_SHMCTL 24

#define MOS_SHM_SEGMENT_MAX 32
#define MOS_SHM_ATTACH_MAX 64

struct mos_ipc_perm {
	int key;
	unsigned uid;
	unsigned gid;
	unsigned cuid;
	unsigned cgid;
	unsigned short mode;
	unsigned short seq;
};

struct mos_shmid_ds {
	struct mos_ipc_perm shm_perm;
	unsigned shm_segsz;
	unsigned shm_atime;
	unsigned shm_dtime;
	unsigned shm_ctime;
	unsigned shm_cpid;
	unsigned shm_lpid;
	unsigned shm_nattch;
	unsigned short unused1;
	unsigned short unused2;
};

struct mos_shm_segment {
	int used;
	int removed;
	int key;
	int shmid;
	unsigned anon_id;
	unsigned size;
	unsigned owner_pid;
	unsigned creator_uid;
	unsigned creator_gid;
	unsigned mode;
	unsigned ctime;
	unsigned attach_count;
};

struct mos_shm_attach {
	int used;
	int shmid;
	unsigned owner_pid;
	unsigned addr;
	unsigned size;
};

static struct mos_shm_segment mos_shm_segments[MOS_SHM_SEGMENT_MAX];
static struct mos_shm_attach mos_shm_attaches[MOS_SHM_ATTACH_MAX];
static spinlock_t mos_shm_lock;
static int mos_shm_next_id = 1;

static void mos_shm_init(void)
{
	spinlock_init(&mos_shm_lock);
}

KERNEL_INIT(7, mos_shm_init);

static struct mos_shm_segment *mos_shm_find_by_id(int shmid)
{
	int i;

	for (i = 0; i < MOS_SHM_SEGMENT_MAX; ++i) {
		if (mos_shm_segments[i].used &&
		    mos_shm_segments[i].shmid == shmid)
			return &mos_shm_segments[i];
	}

	return NULL;
}

static struct mos_shm_segment *mos_shm_find_by_key(int key)
{
	int i;

	for (i = 0; i < MOS_SHM_SEGMENT_MAX; ++i) {
		if (mos_shm_segments[i].used && !mos_shm_segments[i].removed &&
		    mos_shm_segments[i].key == key)
			return &mos_shm_segments[i];
	}

	return NULL;
}

static int mos_shm_create(int key, unsigned size, unsigned mode)
{
	task_struct *cur = CURRENT_TASK();
	int i;

	for (i = 0; i < MOS_SHM_SEGMENT_MAX; ++i) {
		struct mos_shm_segment *seg = &mos_shm_segments[i];

		if (seg->used)
			continue;

		memset(seg, 0, sizeof(*seg));
		seg->used = 1;
		seg->key = key;
		seg->shmid = mos_shm_next_id++;
		seg->anon_id = (unsigned)seg->shmid;
		seg->size = (size + PAGE_SIZE - 1) & PAGE_SIZE_MASK;
		if (seg->size == 0)
			seg->size = PAGE_SIZE;
		seg->owner_pid = cur->psid;
		seg->creator_uid = cur->user ? cur->user->euid : 0;
		seg->creator_gid = cur->user ? cur->user->egid : 0;
		seg->mode = mode & 0777;
		seg->ctime = (unsigned)(time_wall_us() / 1000000ULL);
		return seg->shmid;
	}

	return -ENOSPC;
}

static void mos_shm_rebind_region(unsigned addr, unsigned size,
				  unsigned anon_id)
{
	task_struct *cur = CURRENT_TASK();
	vm_region *region;
	unsigned old_anon_id;
	unsigned vir;

	if (!cur || !cur->user || anon_id == 0)
		return;

	region = vm_find_map(cur->user->vm, addr);
	if (!region || region->begin != addr || region->end < addr + size)
		return;

	old_anon_id = region->anon_id;
	if (old_anon_id == anon_id)
		return;

	mm_anon_shared_get(anon_id);
	region->anon_id = anon_id;

	for (vir = region->begin; vir < region->end; vir += PAGE_SIZE) {
		unsigned mmflag = mm_get_map_flag(vir);

		if (mmflag == 0)
			continue;

		mm_anon_shared_add(anon_id, vir - region->begin,
				   mm_get_attached_page_index(vir) * PAGE_SIZE);
	}

	if (old_anon_id != 0)
		mm_anon_shared_put(old_anon_id);
}

static int mos_shmget(int key, unsigned size, int shmflg)
{
	struct mos_shm_segment *seg;
	int irq;
	int ret;

	if (size == 0)
		return -EINVAL;

	spinlock_lock(&mos_shm_lock, &irq);

	if (key != MOS_IPC_PRIVATE) {
		seg = mos_shm_find_by_key(key);
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

static int mos_shmat(int shmid, const void *shmaddr, int shmflg,
		     unsigned *user_raddr)
{
	task_struct *cur = CURRENT_TASK();
	struct mos_shm_segment *seg;
	struct mos_shm_attach *attach = NULL;
	unsigned addr = (unsigned)shmaddr;
	unsigned mapped;
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
	seg = mos_shm_find_by_id(shmid);
	if (!seg || seg->removed) {
		spinlock_unlock(&mos_shm_lock, irq);
		return -EINVAL;
	}

	for (i = 0; i < MOS_SHM_ATTACH_MAX; ++i) {
		if (!mos_shm_attaches[i].used) {
			attach = &mos_shm_attaches[i];
			break;
		}
	}
	if (!attach) {
		spinlock_unlock(&mos_shm_lock, irq);
		return -ENOSPC;
	}
	spinlock_unlock(&mos_shm_lock, irq);

	mapped = do_mmap(addr, seg->size, prot, flags, -1, 0);
	if ((int)mapped < 0)
		return (int)mapped;

	mos_shm_rebind_region(mapped, seg->size, seg->anon_id);

	*user_raddr = mapped;

	spinlock_lock(&mos_shm_lock, &irq);
	seg = mos_shm_find_by_id(shmid);
	if (!seg || seg->removed) {
		spinlock_unlock(&mos_shm_lock, irq);
		do_munmap((void *)mapped, seg ? seg->size : PAGE_SIZE);
		return -EINVAL;
	}

	memset(attach, 0, sizeof(*attach));
	attach->used = 1;
	attach->shmid = shmid;
	attach->owner_pid = cur->psid;
	attach->addr = mapped;
	attach->size = seg->size;
	seg->attach_count++;
	spinlock_unlock(&mos_shm_lock, irq);
	return 0;
}

static int mos_shmdt(const void *shmaddr)
{
	task_struct *cur = CURRENT_TASK();
	struct mos_shm_segment *seg;
	unsigned addr = (unsigned)shmaddr;
	unsigned size = 0;
	int shmid = -1;
	int i;
	int irq;

	spinlock_lock(&mos_shm_lock, &irq);
	for (i = 0; i < MOS_SHM_ATTACH_MAX; ++i) {
		if (!mos_shm_attaches[i].used)
			continue;
		if (mos_shm_attaches[i].owner_pid != cur->psid)
			continue;
		if (mos_shm_attaches[i].addr != addr)
			continue;

		size = mos_shm_attaches[i].size;
		shmid = mos_shm_attaches[i].shmid;
		memset(&mos_shm_attaches[i], 0, sizeof(mos_shm_attaches[i]));
		break;
	}

	if (size == 0) {
		spinlock_unlock(&mos_shm_lock, irq);
		return -EINVAL;
	}

	seg = mos_shm_find_by_id(shmid);
	if (seg) {
		if (seg->attach_count > 0)
			seg->attach_count--;
		if (seg->removed && seg->attach_count == 0)
			memset(seg, 0, sizeof(*seg));
	}
	spinlock_unlock(&mos_shm_lock, irq);

	return do_munmap((void *)addr, size);
}

static int mos_shmctl(int shmid, int cmd, void *buf)
{
	struct mos_shm_segment *seg;
	int irq;

	cmd &= ~MOS_IPC_64;

	spinlock_lock(&mos_shm_lock, &irq);
	seg = mos_shm_find_by_id(shmid);
	if (!seg) {
		spinlock_unlock(&mos_shm_lock, irq);
		return -EINVAL;
	}

	switch (cmd) {
	case MOS_IPC_RMID:
		seg->removed = 1;
		if (seg->attach_count == 0)
			memset(seg, 0, sizeof(*seg));
		spinlock_unlock(&mos_shm_lock, irq);
		return 0;
	case MOS_IPC_STAT:
		if (!buf) {
			spinlock_unlock(&mos_shm_lock, irq);
			return -EFAULT;
		}

		memset(buf, 0, sizeof(struct mos_shmid_ds));
		((struct mos_shmid_ds *)buf)->shm_perm.key = seg->key;
		((struct mos_shmid_ds *)buf)->shm_perm.uid = seg->creator_uid;
		((struct mos_shmid_ds *)buf)->shm_perm.gid = seg->creator_gid;
		((struct mos_shmid_ds *)buf)->shm_perm.cuid = seg->creator_uid;
		((struct mos_shmid_ds *)buf)->shm_perm.cgid = seg->creator_gid;
		((struct mos_shmid_ds *)buf)->shm_perm.mode = seg->mode;
		((struct mos_shmid_ds *)buf)->shm_segsz = seg->size;
		((struct mos_shmid_ds *)buf)->shm_ctime = seg->ctime;
		((struct mos_shmid_ds *)buf)->shm_cpid = seg->owner_pid;
		((struct mos_shmid_ds *)buf)->shm_nattch = seg->attach_count;
		spinlock_unlock(&mos_shm_lock, irq);
		return 0;
	default:
		spinlock_unlock(&mos_shm_lock, irq);
		return -ENOSYS;
	}
}

int sys_ipc(unsigned call, int first, int second, int third, void *ptr,
	    long fifth)
{
	unsigned version = call >> 16;

	call &= 0xffff;
	(void)fifth;

	if (TestControl.verbos)
		klog("ipc(call=%u, version=%u, first=%x, second=%x, third=%x, ptr=%x, fifth=%x)\n",
		     call, version, first, second, third, ptr, fifth);

	switch (call) {
	case MOS_SHMGET:
		return mos_shmget(first, (unsigned)second, third);
	case MOS_SHMAT:
		return mos_shmat(first, ptr, second, (unsigned *)third);
	case MOS_SHMDT:
		return mos_shmdt(ptr);
	case MOS_SHMCTL:
		return mos_shmctl(first, second, ptr);
	default:
		return -ENOSYS;
	}
}
