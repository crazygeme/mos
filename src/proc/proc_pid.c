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
#include "common/common.h"

/* ------------------------------------------------------------------ *
 * Shared buffer descriptor                                             *
 * ------------------------------------------------------------------ */

/*
 * Text files use proc_buf_t (dynamic, no size limit).
 * Directories use a kmalloc'd buffer wrapped in a plain {buf,len} struct.
 */
typedef struct {
	char *buf;
	size_t len;
} pid_dir_buf;

/* ------------------------------------------------------------------ *
 * Shared file operations                                               *
 * ------------------------------------------------------------------ */

static ssize_t pid_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	proc_buf_t *pb = fp->f_inode->i_private;
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
	proc_buf_t *pb = fp->f_inode->i_private;
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
	proc_buf_free(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static int pid_file_getattr(inode *node, struct stat *s)
{
	proc_buf_t *pb = node->i_private;
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

/* ── Symlink ops for /proc/{pid}/fd/{N} ──────────────────────────── */

static int pid_symlink_getattr(inode *node, struct stat *s)
{
	const char *target = (const char *)node->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_size = target ? (loff_t)strlen(target) : 0;
	s->st_nlink = 1;
	s->st_dev = 0xb;
	s->st_ino = PROC_INODE;
	return 0;
}

static int pid_symlink_release(file *fp)
{
	free(fp->f_inode->i_private); /* strdup'd target */
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const inode_operations pid_symlink_iops = {
	.getattr = pid_symlink_getattr,
};
static const file_operations pid_symlink_fops = {
	.release = pid_symlink_release,
};

/* Build a symlink file whose target is strdup'd from target. */
static file *make_pid_symlink(const char *target)
{
	inode *nd = zalloc(sizeof(*nd));
	file *fp = zalloc(sizeof(*fp));

	nd->i_mode = S_IFLNK | S_IRWXU | S_IRWXG | S_IRWXO;
	nd->i_op = &pid_symlink_iops;
	nd->i_private = strdup(target ? target : "(unknown)");

	fp->f_inode = nd;
	fp->f_count = 1;
	fp->f_fop = &pid_symlink_fops;
	return fp;
}

/* ------------------------------------------------------------------ *
 * Constructor helpers                                                  *
 * ------------------------------------------------------------------ */

/* Wrap a proc_buf_t as a read-only regular file. Ownership transfers. */
static file *make_pid_file(proc_buf_t *pb)
{
	inode *nd = zalloc(sizeof(*nd));
	file *fp = zalloc(sizeof(*fp));

	nd->i_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
	nd->i_op = &pid_file_iops;
	nd->i_private = pb;

	fp->f_inode = nd;
	fp->f_count = 1;
	fp->f_fop = &pid_file_fops;
	return fp;
}

/* Wrap a proc_buf_t as a directory file. Ownership transfers. */
static file *make_pid_dir(proc_buf_t *pb)
{
	inode *nd = zalloc(sizeof(*nd));
	file *fp = zalloc(sizeof(*fp));

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

typedef struct {
	unsigned total_kb;
	unsigned text_kb;
	unsigned data_kb;
	unsigned stk_kb;
	unsigned rss_file_kb;
	unsigned rss_anon_kb;
} vm_stats_t;

static void vm_get_stats(task_struct *task,
			 vm_stats_t *out); /* defined after statm_ctx */

/*
 * /proc/{pid}/status — human-readable, Linux-compatible.
 *
 * Fields: Name State Pid PPid Pgrp Session Threads MinFlt MajFlt
 *         voluntary_ctxt_switches nonvoluntary_ctxt_switches
 */
static void fill_status(proc_buf_t *pb, task_struct *task)
{
	const char *cmd =
		task->user->command ? (const char *)task->user->command : "";
	const char *slash = strrchr(cmd, '/');
	const char *name = slash ? slash + 1 : cmd;
	vm_stats_t vm;
	unsigned fdsize = task->fds ? 256 : 0;

	vm_get_stats(task, &vm);

	proc_buf_printf(pb, "Name:      %s\n", name);
	proc_buf_printf(pb, "State:     %c (%s)\n",
			pid_state_char(task->status),
			pid_state_name(task->status));
	proc_buf_printf(pb, "Tgid:      %u\n", task->psid);
	proc_buf_printf(pb, "Pid:       %u\n", task->psid);
	proc_buf_printf(pb, "PPid:      %u\n",
			task->parent ? task->parent->psid : task->psid);
	proc_buf_printf(pb, "TracerPid: 0\n");
	proc_buf_printf(pb, "Uid:       0\t0\t0\t0\n");
	proc_buf_printf(pb, "Gid:       0\t0\t0\t0\n");
	proc_buf_printf(pb, "FDSize:    %u\n", fdsize);
	proc_buf_printf(pb, "Groups:    0 1 2 3 4 6 10\n");
	proc_buf_printf(pb, "VmSize:    %u kB\n", vm.total_kb);
	proc_buf_printf(pb, "VmLck:     %u kB\n", 0);
	proc_buf_printf(pb, "VmRSS:     %u kB\n",
			vm.rss_anon_kb + vm.rss_file_kb);
	proc_buf_printf(pb, "VmData:    %u kB\n", vm.data_kb);
	proc_buf_printf(pb, "VmStk:     %u kB\n", vm.stk_kb);
	proc_buf_printf(pb, "VmExe:     %u kB\n", vm.text_kb);
	proc_buf_printf(pb, "VmLib:     %u kB\n", 0);
	proc_buf_printf(pb, "SigPnd: 0000000000000000\n");
	proc_buf_printf(pb, "SigBlk: 0000000000000000\n");
	proc_buf_printf(pb, "SigIgn: 0000000000000000\n");
	proc_buf_printf(pb, "SigCgt: 0000000000000000\n");
	proc_buf_printf(pb, "CapInh: 0000000000000000\n");
	proc_buf_printf(pb, "CapPrm: 0000000000000000\n");
	proc_buf_printf(pb, "CapEff: 0000000000000000\n");
	proc_buf_printf(pb, "voluntary_ctxt_switches:       %u\n",
			task->total_switches - task->niv_switches);
	proc_buf_printf(pb, "nonvoluntary_ctxt_switches:    %u\n",
			task->niv_switches);
}

/*
 * Look up the controlling TTY device number for a task.
 * Returns the encoded Linux tty_nr ((major<<8)|minor), or 0 if no TTY.
 * Linux virtual consoles are major 4, minor == tty index.
 */
static unsigned get_tty_nr(task_struct *task)
{
	int i;

	if (!task->fds)
		return 0;
	for (i = 0; i < 3; i++) {
		const char *name;
		const char *p;
		int idx;

		if (!task->fds[i].used || !task->fds[i].fp)
			continue;
		name = task->fds[i].fp->f_name;
		if (!name || strncmp(name, "/dev/tty", 8) != 0)
			continue;
		p = name + 8;
		if (*p < '0' || *p > '9')
			continue;
		idx = 0;
		while (*p >= '0' && *p <= '9')
			idx = idx * 10 + (*p++ - '0');
		if (*p != '\0')
			continue;
		return (4u << 8) | (unsigned)idx;
	}
	return 0;
}

/*
 * /proc/{pid}/stat — 39-field Linux 2.4 format:
 *   pid (comm) state ppid pgrp session tty_nr tpgid flags
 *   minflt cminflt majflt cmajflt utime stime cutime cstime
 *   priority nice num_threads itrealvalue starttime vsize rss rlim
 *   startcode endcode startstack kstkesp kstkeip signal blocked
 *   sigignore sigcatch wchan nswap cnswap exit_signal processor
 */
static void fill_stat(proc_buf_t *pb, task_struct *task)
{
	const char *cmd =
		task->user->command ? (const char *)task->user->command : "";
	const char *slash = strrchr(cmd, '/');
	const char *base = slash ? slash + 1 : cmd;
	char comm[16];
	unsigned tty_nr;
	int tpgid;
	vm_stats_t vm;
	unsigned vsize, rss_pages;
	unsigned stack_start;

	strncpy(comm, base, 15);
	comm[15] = '\0';

	tty_nr = get_tty_nr(task);
	tpgid = tty_nr ? (int)task->user->group_id : -1;

	vm_get_stats(task, &vm);
	vsize = vm.total_kb * 1024u;
	rss_pages = (vm.rss_anon_kb + vm.rss_file_kb) * 1024u / PAGE_SIZE;
	stack_start = KERNEL_OFFSET - USER_STACK_PAGES * PAGE_SIZE;

	proc_buf_printf(
		pb,
		"%u (%s) %c %u %u %u %u %d %lu "
		"%lu %lu %lu %lu %lu %lu %ld %ld "
		"%ld %ld %ld %ld %lu %lu %ld "
		"%lu %lu %lu %lu %lu %lu %lu %lu "
		"%lu %lu %lu %lu %d %d\n",
		/* 1  pid         */ task->psid,
		/* 2  comm        */ comm,
		/* 3  state       */ pid_state_char(task->status),
		/* 4  ppid        */ task->parent ? task->parent->psid :
						    task->psid,
		/* 5  pgrp        */ task->user->group_id,
		/* 6  session     */ task->user->session_id,
		/* 7  tty_nr      */ tty_nr,
		/* 8  tpgid       */ tpgid,
		/* 9  flags       */ (unsigned long)0,
		/* 10 minflt      */ (unsigned long)task->pf_minor,
		/* 11 cminflt     */ (unsigned long)0,
		/* 12 majflt      */ (unsigned long)task->pf_major,
		/* 13 cmajflt     */ (unsigned long)0,
		/* 14 utime       */ (unsigned long)task->user_tickets,
		/* 15 stime       */ (unsigned long)task->kernel_tickets,
		/* 16 cutime      */ (long)0,
		/* 17 cstime      */ (long)0,
		/* 18 priority    */ (long)20,
		/* 19 nice        */ (long)0,
		/* 20 num_threads */ (long)0,
		/* 21 itrealvalue */ (long)0,
		/* 22 starttime   */ (unsigned long)0,
		/* 23 vsize       */ (unsigned long)vsize,
		/* 24 rss         */ (long)rss_pages,
		/* 25 rlim        */ (unsigned long)0x7ffffffful,
		/* 26 startcode   */ (unsigned long)0,
		/* 27 endcode     */ (unsigned long)0,
		/* 28 startstack  */ (unsigned long)stack_start,
		/* 29 kstkesp     */ (unsigned long)0,
		/* 30 kstkeip     */ (unsigned long)0,
		/* 31 signal      */ (unsigned long)0,
		/* 32 blocked     */ (unsigned long)0,
		/* 33 sigignore   */ (unsigned long)0,
		/* 34 sigcatch    */ (unsigned long)0,
		/* 35 wchan       */ (unsigned long)0,
		/* 36 nswap       */ (unsigned long)0,
		/* 37 cnswap      */ (unsigned long)0,
		/* 38 exit_signal */ SIGCHLD,
		/* 39 processor   */ 0);
}

/* /proc/{pid}/cmdline — null-terminated command name */
static void fill_cmdline(proc_buf_t *pb, task_struct *task)
{
	if (task->user->command && task->user->cmd_len)
		proc_buf_copy(pb, (char *)task->user->command,
			      task->user->cmd_len);
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
	unsigned file; /* pages backed by a file (fp != NULL) */
	unsigned anon; /* pages with no file backing */
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
	if (region->fp)
		ctx->file += pages;
	else
		ctx->anon += pages;
}

/* Populate vm_stats_t for a task (vm regions + heap + stack). */
static void vm_get_stats(task_struct *task, vm_stats_t *out)
{
	statm_ctx ctx = { 0, 0, 0, 0, 0 };
	unsigned heap_pages = 0;

	if (task->user->vm)
		vm_enum(task->user->vm, statm_region_cb, &ctx);
	if (task->user->brk > task->user->start_brk)
		heap_pages =
			(task->user->brk - task->user->start_brk) / PAGE_SIZE;
	out->stk_kb = USER_STACK_PAGES * (PAGE_SIZE / 1024);
	out->text_kb = ctx.text * (PAGE_SIZE / 1024);
	out->data_kb = (ctx.data + heap_pages) * (PAGE_SIZE / 1024);
	out->total_kb = (ctx.total + heap_pages + USER_STACK_PAGES) *
			(PAGE_SIZE / 1024);
	out->rss_file_kb = ctx.file * (PAGE_SIZE / 1024);
	out->rss_anon_kb =
		(ctx.anon + heap_pages + USER_STACK_PAGES) * (PAGE_SIZE / 1024);
}

static void fill_statm(proc_buf_t *pb, task_struct *task)
{
	statm_ctx ctx = { 0, 0, 0, 0, 0 };
	unsigned heap_pages = 0, stack_pages = USER_STACK_PAGES;

	if (task->user->vm)
		vm_enum(task->user->vm, statm_region_cb, &ctx);

	if (task->user->brk > task->user->start_brk)
		heap_pages =
			(task->user->brk - task->user->start_brk) / PAGE_SIZE;

	ctx.total += heap_pages + stack_pages;
	ctx.data += heap_pages + stack_pages;

	proc_buf_printf(pb, "%u %u 0 %u 0 %u 0\n", ctx.total, ctx.total,
			ctx.text, ctx.data);
}

/* /proc/{pid}/environ — NUL-separated environment strings */
static void fill_environ(proc_buf_t *pb, task_struct *task)
{
	if (task->user->environment && task->user->env_len)
		proc_buf_copy(pb, task->user->environment, task->user->env_len);
}

/* /proc/{pid}/maps — one line per vm_region + stack + heap pseudo-regions */
typedef struct {
	proc_buf_t *pb;
	task_struct *task;
} maps_ctx;

static void maps_region_cb(vm_region *region, void *data)
{
	maps_ctx *ctx = data;
	char perms[5];
	const char *name;
	unsigned stack_begin = KERNEL_OFFSET - USER_STACK_PAGES * PAGE_SIZE;
	unsigned stack_end = KERNEL_OFFSET;
	int ino = 0;

	perms[0] = (region->prot & PROT_READ) ? 'r' : '-';
	perms[1] = (region->prot & PROT_WRITE) ? 'w' : '-';
	perms[2] = (region->prot & PROT_EXEC) ? 'x' : '-';
	perms[3] = (region->flag & MAP_SHARED) ? 's' : 'p';
	perms[4] = '\0';

	if (region->fp) {
		name = region->fp->f_name;
		ino = region->fp->f_inode->i_ino;
	} else if (region->begin >= ctx->task->user->start_brk &&
		   region->end <= ctx->task->user->brk)
		name = "[heap]";
	else if (region->begin >= stack_begin && region->end <= stack_end)
		name = "[stack]";
	else
		name = "";

	proc_buf_printf(ctx->pb, "%08x-%08x %s %08x 00:00 %-10d %s\n",
			region->begin, region->end, perms, region->offset, ino,
			name);
}

static void fill_maps(proc_buf_t *pb, task_struct *task)
{
	maps_ctx ctx = { pb, task };

	if (task->user->vm)
		vm_enum(task->user->vm, maps_region_cb, &ctx);
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

/* /proc/{pid}/fd/ directory — lists open file descriptor numbers. */
static file *pid_fd_dir_open(task_struct *task)
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
			if (task->fds[i].used) {
				size += ROUND_UP(NAME_OFFSET() +
						 sprintf(name, "%d", i) + 1);
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

	pb = proc_buf_new();
	proc_buf_copy(pb, buf, size);
	free(buf);
	return make_pid_dir(pb);
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

		if (!task->fds || fdno >= MAX_FD || !task->fds[fdno].used)
			return NULL;

		target = (task->fds[fdno].fp && task->fds[fdno].fp->f_name) ?
				 task->fds[fdno].fp->f_name :
				 NULL;
		if (!target || !target[0]) {
			sprintf(anon, "pipe:[%d]", fdno);
			target = anon;
		}
		return make_pid_symlink(target);
	}

	return NULL;
}
