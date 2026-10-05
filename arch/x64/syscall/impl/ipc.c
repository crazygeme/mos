#include <syscall/ipc.h>
#include <lib/klib.h>
struct native_shmid_ds {
	struct {
		int32_t key;
		uint32_t uid, gid, cuid, cgid;
		uint16_t mode, pad1, seq, pad2;
		uint64_t unused[2];
	} perm;
	uint64_t size, atime, dtime, ctime;
	int32_t cpid, lpid;
	uint64_t nattch, unused[2];
};
_Static_assert(sizeof(struct native_shmid_ds) == 112, "AMD64 shmid_ds layout");
int native_shmctl(int id, int command, void *buf)
{
	struct shm_status status;
	int ret = ps_shmctl(id, command, buf ? &status : NULL);
	if (ret || (command & ~0x100) != 2)
		return ret;
	struct native_shmid_ds *wire = buf;
	memset(wire, 0, sizeof(*wire));
	wire->perm.key = status.key;
	wire->perm.uid = wire->perm.cuid = status.uid;
	wire->perm.gid = wire->perm.cgid = status.gid;
	wire->perm.mode = status.mode;
	wire->size = status.size;
	wire->ctime = status.ctime;
	wire->cpid = status.cpid;
	wire->nattch = status.nattch;
	return 0;
}
