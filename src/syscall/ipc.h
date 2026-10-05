#ifndef MOS_SYSCALL_IPC_H
#define MOS_SYSCALL_IPC_H
#include <arch/types.h>
#include <stddef.h>
struct shm_status {
	int key;
	unsigned uid, gid, mode, ctime, cpid, nattch;
	size_t size;
};
int ps_shmctl(int shmid, int command, struct shm_status *status);
#endif
