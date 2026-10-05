#include <syscall/ipc.h>
#include <lib/klib.h>
#include <errno.h>
#include <syscall/syscall.h>
#include <macro.h>
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

int i386_shmctl(int id, int command, void *buf)
{
	struct shm_status status;
	int ret = ps_shmctl(id, command, buf ? &status : NULL);
	if (ret || (command & ~0x100) != 2)
		return ret;
	struct mos_shmid_ds *wire = buf;
	memset(wire, 0, sizeof(*wire));
	wire->shm_perm.key = status.key;
	wire->shm_perm.uid = wire->shm_perm.cuid = status.uid;
	wire->shm_perm.gid = wire->shm_perm.cgid = status.gid;
	wire->shm_perm.mode = status.mode;
	wire->shm_segsz = status.size;
	wire->shm_ctime = status.ctime;
	wire->shm_cpid = status.cpid;
	wire->shm_nattch = status.nattch;
	return 0;
}

#define MOS_SHMGET 23
#define MOS_SHMAT 21
#define MOS_SHMDT 22
#define MOS_SHMCTL 24
static int ipc_shmget(int first, int second, int third, void *ptr)
{
	return sys_shmget(first, (unsigned)second, third);
}

static int ipc_shmat(int first, int second, int third, void *ptr)
{
	unsigned *output = (void *)(uintptr_t)(uint32_t)third;
	if (!output)
		return -EFAULT;
	intptr_t mapped = sys_shmat(first, ptr, second);
	if ((uintptr_t)mapped >= (uintptr_t)-4095)
		return mapped;
	*output = mapped;
	return 0;
}

static int ipc_shmdt(int first, int second, int third, void *ptr)
{
	return sys_shmdt(ptr);
}

static int ipc_shmctl(int first, int second, int third, void *ptr)
{
	return i386_shmctl(first, second, ptr);
}

int sys_ipc(unsigned call, int first, int second, int third, void *ptr,
	    long fifth)
{
	unsigned version = call >> 16;

	call &= 0xffff;
	(void)fifth;

	if (TEST_LOG(TEST_LOG_INFO))
		klog("ipc(call=%u, version=%u, first=%x, second=%x, third=%x, ptr=%x, fifth=%x)\n",
		     call, version, first, second, third, ptr, fifth);

	static int (*const calls[])(int, int, int, void *) = {
		[MOS_SHMGET] = ipc_shmget,
		[MOS_SHMAT] = ipc_shmat,
		[MOS_SHMDT] = ipc_shmdt,
		[MOS_SHMCTL] = ipc_shmctl,
	};
	if (call >= sizeof(calls) / sizeof(calls[0]) || !calls[call])
		return -ENOSYS;
	return calls[call](first, second, third, ptr);
}
