/*
 * proc_pid.c — per-PID virtual files for /proc/{pid}/.
 *
 * Implements the directory and files visible under each numeric PID entry:
 *
 *   /proc/{pid}/          — directory: status stat cmdline maps fd
 *   /proc/{pid}/status    — human-readable process status (Linux-compatible)
 *   /proc/{pid}/stat      — single-line machine-readable stats
 *   /proc/{pid}/cmdline   — null-terminated command name
 *   /proc/{pid}/maps      — virtual memory region map
 *   /proc/{pid}/fd/       — directory of open file descriptors
 *
 * Entry point: proc_pid_lookup(pid, rest, flag) — called by procfs.c.
 */

#include <ps/ps.h>
#include <fs/fs.h>
#include <proc/proc.h>
#include <mm/mmap.h>
#include <mm/mm.h>
#include <lib/klib.h>
#include <macro.h>
#include <config.h>
#include <ext4.h>

/* ------------------------------------------------------------------ *
 * Shared buffer descriptor                                             *
 * ------------------------------------------------------------------ */

/*
 * pid_buf — backing store for a per-PID file or directory.
 *
 * Text files  (status, stat, cmdline, maps): buf = vm_alloc(1), is_page = 1.
 * Directories (fd/, per-PID dir):            buf = kmalloc(),   is_page = 0.
 */
typedef struct {
	char *buf;
	size_t len;
	int is_page; /* 1 = vm_alloc'd page, 0 = kmalloc'd */
} pid_buf;

/* ------------------------------------------------------------------ *
 * Shared file operations                                               *
 * ------------------------------------------------------------------ */

static ssize_t pid_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	pid_buf *pb = fp->f_inode->i_private;
	loff_t off = *pos;
	ssize_t left = (ssize_t)pb->len - (ssize_t)off;
	ssize_t n = (ssize_t)count < left ? (ssize_t)count : left;

	if (n <= 0)
		return 0;
	memcpy(buf, pb->buf + off, (size_t)n);
	*pos = off + n;
	return n;
}

static loff_t pid_llseek(file *fp, loff_t offset, int whence)
{
	pid_buf *pb = fp->f_inode->i_private;
	loff_t fsize = (loff_t)pb->len;
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		newpos = offset;
		break;
	case SEEK_CUR:
		newpos = fp->f_pos + offset;
		break;
	case SEEK_END:
		newpos = fsize + offset;
		break;
	default:
		return -EINVAL;
	}
	if (newpos < 0)
		return -EINVAL;
	fp->f_pos = newpos;
	return newpos;
}

static int pid_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT || type == FS_POLL_WRITE)
		return -1;
	return 0;
}

static int pid_release(file *fp)
{
	pid_buf *pb = fp->f_inode->i_private;
	if (pb->is_page)
		vm_free(pb->buf, 1);
	else
		kfree(pb->buf);
	free(pb);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static int pid_file_getattr(inode *node, struct stat *s)
{
	pid_buf *pb = node->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_size = (loff_t)pb->len;
	s->st_blksize = PAGE_SIZE;
	s->st_nlink = 1;
	s->st_dev = 0xb;
	s->st_ino = PROC_INODE;
	return 0;
}

static int pid_dir_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_blksize = PAGE_SIZE;
	s->st_nlink = 2;
	s->st_dev = 0xb;
	s->st_ino = PROC_INODE;
	return 0;
}

static const inode_operations pid_file_iops = { .getattr = pid_file_getattr };
static const inode_operations pid_dir_iops = { .getattr = pid_dir_getattr };

static const file_operations pid_file_fops = {
	.read = pid_read,
	.llseek = pid_llseek,
	.poll = pid_poll,
	.release = pid_release,
};

static const file_operations pid_dir_fops = {
	.read = pid_read,
	.llseek = pid_llseek,
	.poll = pid_poll,
	.release = pid_release,
};

/* ------------------------------------------------------------------ *
 * Constructor helpers                                                  *
 * ------------------------------------------------------------------ */

/* Wrap a vm_alloc(1) page as a read-only regular file. */
static file *make_pid_file(char *page, size_t len)
{
	pid_buf *pb = zalloc(sizeof(*pb));
	inode *nd = zalloc(sizeof(*nd));
	file *fp = zalloc(sizeof(*fp));

	pb->buf = page;
	pb->len = len;
	pb->is_page = 1;

	nd->i_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
	nd->i_op = &pid_file_iops;
	nd->i_private = pb;

	fp->f_inode = nd;
	fp->f_count = 1;
	fp->f_fop = &pid_file_fops;
	return fp;
}

/* Wrap a kmalloc'd buffer as a directory file. */
static file *make_pid_dir(char *buf, size_t len)
{
	pid_buf *pb = zalloc(sizeof(*pb));
	inode *nd = zalloc(sizeof(*nd));
	file *fp = zalloc(sizeof(*fp));

	pb->buf = buf;
	pb->len = len;
	pb->is_page = 0;

	nd->i_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IXUSR | S_IXGRP |
		     S_IXOTH;
	nd->i_op = &pid_dir_iops;
	nd->i_private = pb;

	fp->f_inode = nd;
	fp->f_count = 1;
	fp->f_fop = &pid_dir_fops;
	return fp;
}

/* ------------------------------------------------------------------ *
 * Content fill functions                                               *
 * ------------------------------------------------------------------ */

static char pid_state_char(ps_status st)
{
	switch (st) {
	case ps_running:
		return 'R';
	case ps_ready:
		return 'R';
	case ps_waiting:
		return 'S';
	case ps_dying:
		return 'Z';
	default:
		return '?';
	}
}

static const char *pid_state_name(ps_status st)
{
	switch (st) {
	case ps_running:
		return "running";
	case ps_ready:
		return "runnable";
	case ps_waiting:
		return "sleeping";
	case ps_dying:
		return "zombie";
	default:
		return "unknown";
	}
}

/*
 * /proc/{pid}/status — human-readable, Linux-compatible.
 *
 * Fields: Name State Pid PPid Pgrp Session Threads MinFlt MajFlt
 *         voluntary_ctxt_switches nonvoluntary_ctxt_switches
 */
static void fill_status(char *buf, task_struct *task)
{
	sprintf(buf,
		"Name:\t%s\n"
		"State:\t%c (%s)\n"
		"Pid:\t%u\n"
		"PPid:\t%u\n"
		"Pgrp:\t%u\n"
		"Session:\t%u\n"
		"Threads:\t1\n"
		"MinFlt:\t%u\n"
		"MajFlt:\t%u\n"
		"voluntary_ctxt_switches:\t0\n"
		"nonvoluntary_ctxt_switches:\t%u\n",
		task->user->command ? (char *)task->user->command : "",
		pid_state_char(task->status), pid_state_name(task->status),
		task->psid, task->parent ? task->parent->psid : task->psid,
		task->user->group_id, task->user->session_id, task->pf_minor,
		task->pf_major, task->niv_switches);
}

/*
 * /proc/{pid}/stat — single-line format (subset of Linux fields):
 *   pid (comm) state ppid pgrp session tty_nr tpgid flags
 *   minflt cminflt majflt cmajflt utime stime cutime cstime
 *   priority nice num_threads itrealvalue starttime vsize rss
 */
static void fill_stat(char *buf, task_struct *task)
{
	sprintf(buf,
		"%u (%s) %c %u %u %u 0 0 0 "
		"%u 0 %u 0 %u %u 0 0 "
		"20 0 1 0 0 0 0\n",
		task->psid,
		task->user->command ? (char *)task->user->command : "",
		pid_state_char(task->status),
		task->parent ? task->parent->psid : task->psid,
		task->user->group_id, task->user->session_id, task->pf_minor,
		task->pf_major, task->user_tickets, task->kernel_tickets);
}

/* /proc/{pid}/cmdline — null-terminated command name */
static void fill_cmdline(char *buf, task_struct *task)
{
	if (task->user->command && task->user->cmd_len)
		memcpy(buf, (char *)task->user->command, task->user->cmd_len);
}

/*
 * /proc/{pid}/statm — memory usage in pages (Linux-compatible):
 *   size resident shared text lib data dt
 *
 * size:     total virtual pages (vm regions + heap + stack)
 * resident: approximated as size (no swap in this kernel)
 * shared:   0 (not tracked)
 * text:     executable pages (PROT_EXEC regions)
 * lib:      0
 * data:     writable non-exec pages + heap + stack
 * dt:       0
 */
typedef struct {
	unsigned total;
	unsigned text;
	unsigned data;
} statm_ctx;

static void statm_region_cb(vm_region *region, void *arg)
{
	statm_ctx *ctx = arg;
	unsigned pages = (region->end - region->begin) / PAGE_SIZE;

	ctx->total += pages;
	if (region->prot & PROT_EXEC)
		ctx->text += pages;
	else if (region->prot & PROT_WRITE)
		ctx->data += pages;
}

static void fill_statm(char *buf, task_struct *task)
{
	statm_ctx ctx = { 0, 0, 0 };
	unsigned heap_pages = 0, stack_pages = USER_STACK_PAGES;

	if (task->user->vm)
		vm_enum(task->user->vm, statm_region_cb, &ctx);

	if (task->user->heap_top > USER_HEAP_BEGIN)
		heap_pages =
			(task->user->heap_top - USER_HEAP_BEGIN) / PAGE_SIZE;

	ctx.total += heap_pages + stack_pages;
	ctx.data += heap_pages + stack_pages;

	sprintf(buf, "%u %u 0 %u 0 %u 0\n", ctx.total, ctx.total, ctx.text,
		ctx.data);
}

/* /proc/{pid}/environ — NUL-separated environment strings */
static void fill_environ(char *buf, task_struct *task)
{
	if (task->user->environment && task->user->env_len)
		memcpy(buf, task->user->environment, task->user->env_len);
}

/* /proc/{pid}/maps — one line per vm_region + stack + heap pseudo-regions */
typedef struct {
	char *buf;
	size_t pos;
	size_t size;
} maps_ctx;

static void maps_region_cb(vm_region *region, void *data)
{
	maps_ctx *ctx = data;
	char perms[5];
	char *p;

	if (ctx->pos + 80 >= ctx->size)
		return;

	perms[0] = (region->prot & PROT_READ) ? 'r' : '-';
	perms[1] = (region->prot & PROT_WRITE) ? 'w' : '-';
	perms[2] = (region->prot & PROT_EXEC) ? 'x' : '-';
	perms[3] = (region->flag & MAP_SHARED) ? 's' : 'p';
	perms[4] = '\0';

	p = ctx->buf + ctx->pos;
	sprintf(p, "%08x-%08x %s %08x 00:00 0\n", region->begin, region->end,
		perms, region->offset);
	ctx->pos += strlen(p);
}

static void fill_maps(char *buf, size_t size, task_struct *task)
{
	maps_ctx ctx = { buf, 0, size };
	char *p;

	if (task->user->vm)
		vm_enum(task->user->vm, maps_region_cb, &ctx);

	/* Heap pseudo-region */
	if (task->user->heap_top > USER_HEAP_BEGIN && ctx.pos + 80 < size) {
		p = buf + ctx.pos;
		sprintf(p, "%08x-%08x rw-p 00000000 00:00 0          [heap]\n",
			USER_HEAP_BEGIN, task->user->heap_top);
		ctx.pos += strlen(p);
	}

	/* Stack pseudo-region */
	if (ctx.pos + 80 < size) {
		p = buf + ctx.pos;
		sprintf(p, "%08x-%08x rw-p 00000000 00:00 0          [stack]\n",
			(unsigned)KERNEL_OFFSET -
				USER_STACK_PAGES * (unsigned)PAGE_SIZE,
			(unsigned)KERNEL_OFFSET);
		ctx.pos += strlen(p);
	}
}

/* ------------------------------------------------------------------ *
 * Per-PID directory builders                                           *
 * ------------------------------------------------------------------ */

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

/* Fixed entries visible in every /proc/{pid}/ directory. */
static const char *const pid_dir_entries[] = { ".",    "..",	  "status",
					       "stat", "statm",	  "cmdline",
					       "maps", "environ", "fd",
					       NULL };

/* /proc/{pid}/ directory listing. */
static file *pid_dir_open(task_struct *task)
{
	unsigned size = 0;
	int i;
	char *buf, *p;

	for (i = 0; pid_dir_entries[i]; i++)
		size += ROUND_UP(NAME_OFFSET() + strlen(pid_dir_entries[i]) +
				 1);

	buf = p = kmalloc(size);
	memset(buf, 0, size);

	for (i = 0; pid_dir_entries[i]; i++)
		PID_FILL_DIRENT(&p, buf, pid_dir_entries[i]);

	return make_pid_dir(buf, size);
}

/* /proc/{pid}/fd/ directory — lists open file descriptor numbers. */
static file *pid_fd_dir_open(task_struct *task)
{
	unsigned size = 0;
	char name[12];
	char *buf, *p;
	int i;

	size += ROUND_UP(NAME_OFFSET() + 2); /* "."  */
	size += ROUND_UP(NAME_OFFSET() + 3); /* ".." */

	if (task->fds) {
		for (i = 0; i < MAX_FD; i++) {
			if (task->fds[i].used) {
				sprintf(name, "%d", i);
				size += ROUND_UP(NAME_OFFSET() + strlen(name) +
						 1);
			}
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

	return make_pid_dir(buf, size);
}

#undef PID_FILL_DIRENT

/* ------------------------------------------------------------------ *
 * Public entry point                                                   *
 * ------------------------------------------------------------------ */

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
	char *page;

	if (!task)
		return NULL;

	/* "" or "/" → per-PID directory listing */
	if (rest[0] == '\0' || (rest[0] == '/' && rest[1] == '\0'))
		return pid_dir_open(task);

	/* /status */
	if (strcmp(rest, "/status") == 0) {
		page = vm_alloc(1);
		memset(page, 0, PAGE_SIZE);
		fill_status(page, task);
		return make_pid_file(page, strlen(page));
	}

	/* /stat */
	if (strcmp(rest, "/stat") == 0) {
		page = vm_alloc(1);
		memset(page, 0, PAGE_SIZE);
		fill_stat(page, task);
		return make_pid_file(page, strlen(page));
	}

	/* /statm */
	if (strcmp(rest, "/statm") == 0) {
		page = vm_alloc(1);
		memset(page, 0, PAGE_SIZE);
		fill_statm(page, task);
		return make_pid_file(page, strlen(page));
	}

	/* /cmdline */
	if (strcmp(rest, "/cmdline") == 0) {
		page = vm_alloc(1);
		memset(page, 0, PAGE_SIZE);
		fill_cmdline(page, task);
		return make_pid_file(page, strlen(page));
	}

	/* /environ */
	if (strcmp(rest, "/environ") == 0) {
		page = vm_alloc(1);
		memset(page, 0, PAGE_SIZE);
		fill_environ(page, task);
		return make_pid_file(page, strlen(page));
	}

	/* /maps */
	if (strcmp(rest, "/maps") == 0) {
		page = vm_alloc(1);
		memset(page, 0, PAGE_SIZE);
		fill_maps(page, PAGE_SIZE, task);
		return make_pid_file(page, strlen(page));
	}

	/* /fd or /fd/ → fd directory listing */
	if (strcmp(rest, "/fd") == 0 || strcmp(rest, "/fd/") == 0)
		return pid_fd_dir_open(task);

	return NULL;
}
