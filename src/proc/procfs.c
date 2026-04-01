/*
 * procfs.c — /proc filesystem.
 *
 * Serves:
 *   /proc            — directory listing (registered entries + live PIDs + "self")
 *   /proc/{pid}/     — per-PID directory (→ proc_pid.c)
 *   /proc/self/      — alias for /proc/{current_pid}/
 *   /proc/{name}     — static entries registered via PROC_INIT (cpuinfo, meminfo, ...)
 *
 * Static entries self-register by placing a pointer in the ".procfs_init"
 * ELF section (PROC_INIT macro).  procfs_init() iterates that section and
 * calls each function with the procfs superblock, which mounts the entry as
 * a child superblock at its chosen path.
 *
 * Per-PID paths are caught by proc_open(): anything that is not found in the
 * static child-mount table is treated as a potential PID or "self" alias and
 * delegated to proc_pid_lookup() in proc_pid.c.
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <proc/proc.h>
#include <ps/ps.h>
#include <lib/lock.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <macro.h>
#include <ext4.h>

/* Implemented in proc_pid.c */
file *proc_pid_lookup(unsigned pid, const char *rest, int flag);

/* ------------------------------------------------------------------ *
 * Root-directory private state (one per open(2) call on /proc)        *
 * ------------------------------------------------------------------ */

typedef struct {
	struct linux_dirent *buf;
	unsigned length;
} proc_root_dir;

/* ------------------------------------------------------------------ *
 * PID collection for directory generation                              *
 * ------------------------------------------------------------------ */

#define PROC_MAX_PIDS 512

typedef struct {
	unsigned *list;
	int count;
} pid_ctx_t;

static void proc_collect_pid(task_struct *task, void *ctx)
{
	pid_ctx_t *c = (pid_ctx_t *)ctx;
	if (task->psid != 0xffffffff && task->type != ps_kernel &&
	    c->count < PROC_MAX_PIDS)
		c->list[c->count++] = task->psid;
}

/* ------------------------------------------------------------------ *
 * inode/file operations for the /proc root directory                  *
 * ------------------------------------------------------------------ */

static ssize_t proc_root_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	proc_root_dir *rd = fp->f_inode->i_private;
	loff_t offset = *pos;
	ssize_t left, read_size = 0;

	if (!rd->buf || !rd->length)
		goto done;

	left = (ssize_t)rd->length - (ssize_t)offset;
	read_size = (ssize_t)count < left ? (ssize_t)count : left;
	if (read_size > 0)
		memcpy(buf, (char *)rd->buf + offset, read_size);
	else
		read_size = 0;
done:
	*pos = offset + read_size;
	return read_size;
}

static int proc_root_poll(file *fp, unsigned type)
{
	if (type == FS_POLL_EXCEPT || type == FS_POLL_WRITE)
		return -1;
	return 0;
}

static int proc_root_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_atime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mode = node->i_mode;
	s->st_blksize = PAGE_SIZE;
	s->st_dev = 0xb;
	s->st_nlink = 2;
	s->st_ino = PROC_INODE;
	return 0;
}

static int proc_root_release(file *fp)
{
	proc_root_dir *rd = fp->f_inode->i_private;
	kfree(rd->buf);
	free(rd);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const inode_operations proc_root_iops = {
	.getattr = proc_root_getattr,
};

static const file_operations proc_root_fops = {
	.read = proc_root_read,
	.is_ready = proc_root_poll,
	.release = proc_root_release,
};

/* ------------------------------------------------------------------ *
 * proc_dir_gen — build the packed linux_dirent buffer for /proc       *
 * ------------------------------------------------------------------ */

/*
 * Generate entries:  "."  ".."  "self"  <static mounts>  <live PIDs>
 *
 * The static mounts come from sb->s_mounts whose keys are absolute paths
 * like "/meminfo"; we strip the leading '/' when emitting the dirent name.
 *
 * Live PIDs are collected via ps_enum_all into the module-static array
 * (proc_pid_list / pid_ctx.count); acceptable for a single-CPU context.
 */
static void proc_dir_gen(super_block *sb, proc_root_dir *rd)
{
	key_value_pair *kv;
	unsigned size = 0;
	int i;
	char pidbuf[12];
	char *buf, *p;
	const char *begin;
	struct linux_dirent *dirp;

	/* Collect live PIDs */
	unsigned *proc_pid_list = zalloc(sizeof(unsigned) * PROC_MAX_PIDS);
	pid_ctx_t pid_ctx = { proc_pid_list, 0 };
	ps_enum_all(proc_collect_pid, &pid_ctx);

	/* ---- Size calculation ---- */
	size += ROUND_UP(NAME_OFFSET() + 2); /* "."    strlen=1 +1 */
	size += ROUND_UP(NAME_OFFSET() + 3); /* ".."   strlen=2 +1 */
	size += ROUND_UP(NAME_OFFSET() + 5); /* "self" strlen=4 +1 */

	mutex_lock(&sb->s_lock);
	for (kv = hash_first(sb->s_mounts); kv;
	     kv = hash_next(sb->s_mounts, kv)) {
		/* key is "/name"; display "name" (key+1) */
		size += ROUND_UP(NAME_OFFSET() + strlen((char *)kv->key + 1) +
				 1);
	}
	mutex_unlock(&sb->s_lock);

	for (i = 0; i < pid_ctx.count; i++)
		size += ROUND_UP(NAME_OFFSET() +
				 sprintf(pidbuf, "%u", pid_ctx.list[i]) + 1);

	/* ---- Allocate and fill ---- */
	buf = p = kmalloc(size);
	begin = buf;
	memset(buf, 0, size);
	rd->buf = (struct linux_dirent *)buf;
	rd->length = size;

#define FILL_ENTRY(name_str)                                               \
	do {                                                               \
		dirp = (struct linux_dirent *)p;                           \
		dirp->d_ino = PROC_INODE;                                  \
		strcpy(dirp->d_name, (name_str));                          \
		dirp->d_reclen =                                           \
			ROUND_UP(NAME_OFFSET() + strlen(name_str) + 1);    \
		dirp->d_off = (unsigned long)(p + dirp->d_reclen - begin); \
		p += dirp->d_reclen;                                       \
	} while (0)

	FILL_ENTRY(".");
	FILL_ENTRY("..");
	FILL_ENTRY("self");

	mutex_lock(&sb->s_lock);
	for (kv = hash_first(sb->s_mounts); kv;
	     kv = hash_next(sb->s_mounts, kv))
		FILL_ENTRY((char *)kv->key + 1);
	mutex_unlock(&sb->s_lock);

	for (i = 0; i < pid_ctx.count; i++) {
		sprintf(pidbuf, "%u", pid_ctx.list[i]);
		FILL_ENTRY(pidbuf);
	}

	free(proc_pid_list);

#undef FILL_ENTRY
}

/* ------------------------------------------------------------------ *
 * super_operations for /proc                                           *
 * ------------------------------------------------------------------ */

/*
 * proc_open_root — called when /proc itself is opened.
 * Generates a fresh directory listing and returns a file backed by it.
 */
static file *proc_open_root(super_block *sb, int flag)
{
	proc_root_dir *rd = zalloc(sizeof(*rd));
	inode *node = zalloc(sizeof(*node));
	file *fp = zalloc(sizeof(*fp));

	proc_dir_gen(sb, rd);

	node->i_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IXUSR |
		       S_IXGRP | S_IXOTH;
	node->i_op = &proc_root_iops;
	node->i_private = rd;

	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &proc_root_fops;
	return fp;
}

/*
 * proc_open — called for paths under /proc not matched by static child mounts.
 *
 * Handles:
 *   /self            → current task's PID directory
 *   /self/{file}     → file inside current task's PID directory
 *   /{pid}           → per-PID directory
 *   /{pid}/{file}    → file inside that PID's directory
 *
 * path is relative to the procfs root and always starts with '/'.
 */
static file *proc_open(super_block *sb, const char *path, int flag)
{
	const char *p = path;
	unsigned pid;
	const char *rest;

	if (*p != '/')
		return NULL;
	p++;

	/* /self or /self/... */
	if (strncmp(p, "self", 4) == 0 && (p[4] == '/' || p[4] == '\0')) {
		pid = CURRENT_TASK()->psid;
		rest = p + 4; /* "" or "/status" etc. */
		return proc_pid_lookup(pid, rest, flag);
	}

	/* /NNN or /NNN/... */
	if (*p >= '0' && *p <= '9') {
		pid = 0;
		while (*p >= '0' && *p <= '9')
			pid = pid * 10 + (unsigned)(*p++ - '0');
		rest = p; /* "" or "/status" or "/fd/0" … */
		return proc_pid_lookup(pid, rest, flag);
	}

	return NULL;
}

static void proc_release_super(super_block *sb)
{
	free(sb);
}

/*
 * proc_readlink — resolve /proc/{pid}/fd/{N} or /proc/self/fd/{N} to the
 * path of the underlying open file, matching Linux /proc/<pid>/fd behaviour.
 */
static int proc_readlink(super_block *sb, const char *path, char *buf,
			 size_t bufsiz, size_t *rcnt)
{
	const char *p = path;
	unsigned pid;
	int fdno;
	task_struct *task;
	const char *fname;
	char anon[32];
	size_t n;

	if (*p != '/')
		return -1;
	p++;

	if (strncmp(p, "self", 4) == 0 && p[4] == '/') {
		pid = CURRENT_TASK()->psid;
		p += 4;
	} else if (*p >= '0' && *p <= '9') {
		pid = 0;
		while (*p >= '0' && *p <= '9')
			pid = pid * 10 + (unsigned)(*p++ - '0');
	} else {
		return -1;
	}

	if (strncmp(p, "/fd/", 4) != 0)
		return -1;
	p += 4;

	fdno = 0;
	while (*p >= '0' && *p <= '9')
		fdno = fdno * 10 + (*p++ - '0');
	if (*p != '\0')
		return -1;

	task = ps_find_process(pid);
	if (!task || !task->fds)
		return -1;
	if (fdno >= MAX_FD || !task->fds[fdno].used)
		return -1;

	fname = (task->fds[fdno].fp && task->fds[fdno].fp->f_name) ?
			task->fds[fdno].fp->f_name :
			NULL;
	if (!fname || !fname[0]) {
		sprintf(anon, "pipe:[%d]", fdno);
		fname = anon;
	}

	n = strlen(fname);
	if (n > bufsiz)
		n = bufsiz;
	memcpy(buf, fname, n);
	if (rcnt)
		*rcnt = n;
	return 0;
}

static int proc_statfs(super_block *sb, struct statfs *buf)
{
	memset(buf, 0, sizeof(*buf));
	buf->f_type = 0x9FA0; /* PROC_SUPER_MAGIC */
	buf->f_bsize = PAGE_SIZE;
	buf->f_namelen = 255;
	return 0;
}

static super_operations proc_sops = {
	.open_root = proc_open_root,
	.open = proc_open,
	.readlink = proc_readlink,
	.release = proc_release_super,
	.statfs = proc_statfs,
};

/* ------------------------------------------------------------------ *
 * Initialisation                                                       *
 * ------------------------------------------------------------------ */

static void procfs_init(void)
{
	task_struct *cur = CURRENT_TASK();
	super_block *root = cur->root;
	proc_init_fn_t *fn;
	super_block *sb = sget(&proc_sops);

	printk("mnt: Mounting procfs on /proc\n");
	vfs_mount(root, "/proc", sb);
	vfs_mount_record("proc", "/proc", "proc", "rw,relatime");

	/* Let each static entry self-register under the procfs superblock. */
	for (fn = __procfs_init_start; fn < __procfs_init_end; fn++)
		(*fn)(sb);
}

KERNEL_INIT(5, procfs_init);
