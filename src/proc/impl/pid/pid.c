/*
 * pid.c — entry point for /proc/{pid} virtual files.
 *
 * proc_pid_lookup(pid, rest, flag) is called by procfs.c for every path
 * under a numeric PID component.  It delegates content generation to the
 * other files in this directory:
 *
 *   common.c  — shared file/dir/symlink constructors, state helpers
 *   vm.c      — VM statistics (vm_get_stats, vm_fill_statm)
 *   status.c  — /status  /stat  /statm
 *   files.c   — /cmdline /environ /maps
 *   fd.c      — /fd/  and  /fd/{N}
 */
#include "proc_pid.h"
#include <ps/ps.h>
#include <config.h>
#include <macro.h>
#include <ext4.h>

/* ── /proc/{pid}/ directory listing ─────────────────────────────────── */

#define PID_FILL_DIRENT(pp, begin, name_str)                                   \
	do {                                                                   \
		struct linux_dirent *_d = (struct linux_dirent *)(*(pp));      \
		_d->d_ino = PROC_INODE;                                        \
		strcpy(_d->d_name, (name_str));                                \
		_d->d_reclen = ROUND_UP(NAME_OFFSET() + strlen(name_str) + 1); \
		_d->d_off = (unsigned long)((*(pp)) + _d->d_reclen - (begin)); \
		*(pp) += _d->d_reclen;                                         \
	} while (0)

static const char *const pid_dir_entries[] = { ".",    "..",	  "status",
					       "stat", "statm",	  "cmdline",
					       "maps", "environ", "fd",
					       NULL };

static file *pid_dir_open(task_struct *task)
{
	unsigned size = 0;
	int i;
	char *buf, *p;
	proc_buf_t *pb;

	for (i = 0; pid_dir_entries[i]; i++)
		size += ROUND_UP(NAME_OFFSET() + strlen(pid_dir_entries[i]) +
				 1);

	buf = p = kmalloc(size);
	memset(buf, 0, size);

	for (i = 0; pid_dir_entries[i]; i++)
		PID_FILL_DIRENT(&p, buf, pid_dir_entries[i]);

	pb = proc_buf_new();
	proc_buf_copy(pb, buf, size);
	free(buf);
	return make_pid_dir(pb);
}

#undef PID_FILL_DIRENT

/* ── Public entry point ──────────────────────────────────────────────── */

/*
 * proc_pid_lookup — open a file or directory under /proc/{pid}.
 *
 * @pid:  process ID parsed from the path
 * @rest: remainder after the PID component, e.g. "" "/" "/status" "/fd/"
 * @flag: open flags (passed through)
 *
 * Returns an open file* on success, NULL if the PID or entry does not exist.
 */
file *proc_pid_lookup(unsigned pid, const char *rest, int flag)
{
	task_struct *task = ps_find_process(pid);
	proc_buf_t *pb;

	if (!task)
		return NULL;

	/* "" or "/" → per-PID directory listing */
	if (rest[0] == '\0' || (rest[0] == '/' && rest[1] == '\0'))
		return pid_dir_open(task);

#define OPEN_TEXT_FILE(fill_fn)           \
	do {                              \
		pb = proc_buf_new();      \
		fill_fn(pb, task);        \
		return make_pid_file(pb); \
	} while (0)

	if (strcmp(rest, "/status") == 0)
		OPEN_TEXT_FILE(fill_status);
	if (strcmp(rest, "/stat") == 0)
		OPEN_TEXT_FILE(fill_stat);
	if (strcmp(rest, "/statm") == 0)
		OPEN_TEXT_FILE(fill_statm);
	if (strcmp(rest, "/cmdline") == 0)
		OPEN_TEXT_FILE(fill_cmdline);
	if (strcmp(rest, "/environ") == 0)
		OPEN_TEXT_FILE(fill_environ);
	if (strcmp(rest, "/maps") == 0)
		OPEN_TEXT_FILE(fill_maps);

#undef OPEN_TEXT_FILE

	/* /fd or /fd/ → fd directory listing */
	if (strcmp(rest, "/fd") == 0 || strcmp(rest, "/fd/") == 0)
		return pid_fd_dir_open(task);

	/* /fd/{N} → symlink to the fd's file path */
	if (strncmp(rest, "/fd/", 4) == 0) {
		const char *p = rest + 4;
		int fdno = 0;
		char anon[32];
		const char *target;

		if (*p < '0' || *p > '9')
			return NULL;
		while (*p >= '0' && *p <= '9')
			fdno = fdno * 10 + (*p++ - '0');
		if (*p != '\0')
			return NULL; /* trailing garbage */

		if (!task->fds || fdno >= MAX_FD || !task->fds[fdno])
			return NULL;

		target = (task->fds[fdno] && task->fds[fdno]->f_name) ?
				 task->fds[fdno]->f_name :
				 NULL;
		if (!target || !target[0]) {
			sprintf(anon, "pipe:[%d]", fdno);
			target = anon;
		}
		return make_pid_symlink(target);
	}

	return NULL;
}
