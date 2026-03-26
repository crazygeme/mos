/*
 * fd.c — /proc/{pid}/fd/ directory listing and symlink lookup.
 */
#include "proc_pid.h"
#include <config.h>
#include <macro.h>
#include <ext4.h>

/* Helper: emit one linux_dirent into *pp, advance *pp, from base begin. */
#define PID_FILL_DIRENT(pp, begin, name_str)                                   \
	do {                                                                   \
		struct linux_dirent *_d = (struct linux_dirent *)(*(pp));      \
		_d->d_ino = PROC_INODE;                                        \
		strcpy(_d->d_name, (name_str));                                \
		_d->d_reclen = ROUND_UP(NAME_OFFSET() + strlen(name_str) + 1); \
		_d->d_off = (unsigned long)((*(pp)) + _d->d_reclen - (begin)); \
		*(pp) += _d->d_reclen;                                         \
	} while (0)

/* /proc/{pid}/fd/ — directory listing of open file descriptor numbers. */
file *pid_fd_dir_open(task_struct *task)
{
	unsigned size = 0;
	char name[12];
	char *buf, *p;
	proc_buf_t *pb;
	int i;

	size += ROUND_UP(NAME_OFFSET() + 2); /* "."  */
	size += ROUND_UP(NAME_OFFSET() + 3); /* ".." */

	if (task->fds) {
		for (i = 0; i < MAX_FD; i++) {
			if (task->fds[i].used)
				size += ROUND_UP(NAME_OFFSET() +
						 sprintf(name, "%d", i) + 1);
		}
	}

	buf = p = kmalloc(size);
	memset(buf, 0, size);

	PID_FILL_DIRENT(&p, buf, ".");
	PID_FILL_DIRENT(&p, buf, "..");

	if (task->fds) {
		for (i = 0; i < MAX_FD; i++) {
			if (task->fds[i].used) {
				sprintf(name, "%d", i);
				PID_FILL_DIRENT(&p, buf, name);
			}
		}
	}

	pb = proc_buf_new();
	proc_buf_copy(pb, buf, size);
	free(buf);
	return make_pid_dir(pb);
}

#undef PID_FILL_DIRENT
