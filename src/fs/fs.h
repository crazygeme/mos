#ifndef _FS_FS_H_
#define _FS_FS_H_

#include <mm/mm.h>
#include <lib/lock.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <fs/iovec.h>

typedef struct _block block;
#define S_IFMT 00170000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFBLK 0060000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000
#define S_ISUID 0004000
#define S_ISGID 0002000
#define S_ISVTX 0001000

#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

#define S_IRWXOGU (S_IRWXU | S_IRWXG | S_IRWXO)

#define EOF ((unsigned char)(-1))

#define FILE_TYPE_NORMAL 1
#define FILE_TYPE_PIPE 2
#define FILE_TYPE_CHAR 3
#define FILE_TYPE_DIR 4

/* Poll/select event mask bits used by file_operations::poll(). */
#define FS_POLL_READ (1U << 0)
#define FS_POLL_WRITE (1U << 1)
#define FS_POLL_EXCEPT (1U << 2)
#define FS_POLL_HUP (1U << 3)
#define FS_POLL_ERR (1U << 4)
#define FS_POLL_RDHUP (1U << 5)

typedef int64_t loff_t;
typedef int off_t;
typedef int ssize_t;

typedef struct _inode inode;
typedef struct _file file;
struct super_block;

/* Readiness subscription interfaces. */
typedef struct _task_struct task_struct;
typedef void (*poll_dereg_fn)(void *opaque, task_struct *task);

typedef void (*poll_wake_fn)(void *opaque);

typedef struct _poll_table_entry {
	void *opaque;
	poll_dereg_fn dereg;
	file *fp; /* Retained until this subscription is removed. */
	list_entry node;
	spinlock_t *lock;
	poll_wake_fn wake;
	void *wake_arg;
	task_struct *task;
} poll_table_entry;

typedef struct _poll_table {
	task_struct *task;
	unsigned nr;
	unsigned cap;
	int unsupported;
	int entries_owned;
	poll_table_entry *entries;
	poll_wake_fn wake;
	void *wake_arg;
} poll_table;

/*
 * file_operations - operations for an open file. All ops receive the open file
 * as first argument, even when they conceptually operate on inode metadata.
 */
typedef struct _file_operations {
	/* Custom dtor of file struct, will call regular kfree if not provided */
	int (*release)(file *file);
	int (*getattr)(file *file, struct stat *s);
	/* Borrowed parent identity and entry name: 1 for a parent, 0 for a root. */
	int (*notify_parent)(file *file, struct super_block **owner,
			     uint64_t *ino, const char **name);
	/* Follow a virtual link to an open object without resolving display text. */
	file *(*follow_link)(file *file, int flags);
	/* Allocate an independent open description for an anonymous object. */
	file *(*reopen)(file *file, int flags);
	int (*setattr)(file *file, uint32_t mode);
	int (*chown)(file *file, uint32_t uid, uint32_t gid);
	/* read/write: update *pos on success, return bytes transferred or -errno */
	ssize_t (*read)(file *file, void *buf, size_t size, loff_t *pos);
	/* Optional vectored reader for backends with per-buffer record semantics. */
	ssize_t (*readv)(file *file, const struct iovec *iov, int count);
	/* Linux directory-entry type; absent callbacks report DT_UNKNOWN. */
	unsigned char (*dirent_type)(file *file, const char *name);
	ssize_t (*write)(file *file, const void *buf, size_t size, loff_t *pos);
	/* llseek: return new position or -errno */
	loff_t (*llseek)(file *file, loff_t offset, int whence);
	/* read_page/write_page: transfer one PAGE_SIZE chunk at file offset @offset */
	int (*read_page)(file *file, uint64_t offset, void *buf);
	int (*write_page)(file *file, uint64_t offset, const void *buf);
	int (*ftruncate)(file *file, loff_t size);
	/*
	 * poll: return an FS_POLL_* readiness bitmask for the requested @events.
	 * If @pt is non-NULL, register the requested readiness queues even if
	 * ready. The table owns each subscription until poll_table_cleanup().
	 */
	unsigned (*poll)(file *file, unsigned events, poll_table *pt);
	int (*ioctl)(file *file, unsigned cmd, void *buf);
	int (*flush)(file *file);
	/* Return a referenced backing file and a validated byte offset. */
	int (*mmap_file)(file *file, uint64_t *offset, size_t size,
			 unsigned prot, unsigned flags, struct _file **backing);
	/* Return an allocator-owned physical page for a shared device mapping. */
	paddr_t (*map_page)(file *file, uint64_t offset);
} file_operations;

/*
 * inode - in-memory representation of a filesystem object.
 * i_private holds the backend-specific handle (e.g. ext4_file *).
 */
struct _inode {
	uint32_t i_mode;
	uint64_t i_ino;
	uint64_t i_size;
	uint32_t i_phys_base;
	uint64_t i_phys_size; /* MMIO window; zero for ordinary files */
	void *i_private;
	void *i_pgcache_tag; /* stable address_space-style identity for page cache */

	/* flock state — lazily initialised on first sys_flock call */
	int i_flock_inited;
	file *i_flock_ex_owner; /* file * holding LOCK_EX, NULL if none */
	int i_flock_sh; /* number of LOCK_SH holders */
	list_entry i_flock_wait; /* tasks sleeping in sys_flock */
	spinlock_t i_flock_lock; /* guards all i_flock_* fields */
};

/*
 * file - open file instance, one per open(). Tracks position (f_pos)
 * separately from the underlying inode, matching Linux struct file semantics.
 */
struct _file {
	inode *f_inode;
	const file_operations *f_fop;
	loff_t f_pos;
	unsigned f_count;
	unsigned f_mode; /* O_RDONLY / O_WRONLY / O_RDWR (set by fs_open) */
	unsigned f_flag; /* file status flags, including O_NONBLOCK */
	unsigned f_state;
	unsigned f_mount_flags;
	int f_owner; /* async I/O owner set via fcntl(F_SETOWN) */
	int f_sigio; /* signal number for async I/O (0 => SIGIO) */
	char *f_name;
	/* The owning backend is retained independently of aliases and mount views. */
	struct super_block *f_sb;
	char *f_relative_path;
	struct super_block *f_notify_root;
	list_entry f_notify_node;
	unsigned f_notify_epoch;
	struct inotify_node *f_notify;
	struct epitem *f_ep_links;
	int f_flock; /* current flock: 0=none, LOCK_SH, or LOCK_EX */
};

struct linux_dirent {
	uint32_t d_ino; /* Inode number */
	uint32_t d_off; /* Offset to next linux_dirent */
	unsigned short d_reclen; /* Length of this linux_dirent */
	char d_name[]; /* Filename (null-terminated) */
};

#define NAME_OFFSET() offset_of(struct linux_dirent, d_name)

struct linux_dirent64 {
	unsigned long long d_ino; /* 64-bit inode number */
	unsigned long long d_off; /* Offset to next entry */
	unsigned short d_reclen; /* Length of this entry */
	unsigned char d_type; /* File type */
	char d_name[]; /* Filename (null-terminated) */
};

#define NAME64_OFFSET() offset_of(struct linux_dirent64, d_name)

#define MAX_FD 1024
#define FD_TABLE_PAGES ((MAX_FD * sizeof(file *) + PAGE_SIZE - 1) / PAGE_SIZE)
#define FD_BITMAP_BITS (8 * sizeof(unsigned long))
#define FD_BITMAP_WORDS ((MAX_FD + FD_BITMAP_BITS - 1) / FD_BITMAP_BITS)

static inline int fd_bitmap_test(const unsigned long *map, int fd)
{
	return map && fd >= 0 && fd < MAX_FD &&
	       ((map[fd / FD_BITMAP_BITS] >> (fd % FD_BITMAP_BITS)) & 1UL);
}

static inline void fd_bitmap_set(unsigned long *map, int fd)
{
	if (map && fd >= 0 && fd < MAX_FD)
		map[fd / FD_BITMAP_BITS] |= 1UL << (fd % FD_BITMAP_BITS);
}

static inline void fd_bitmap_clear(unsigned long *map, int fd)
{
	if (map && fd >= 0 && fd < MAX_FD)
		map[fd / FD_BITMAP_BITS] &= ~(1UL << (fd % FD_BITMAP_BITS));
}

typedef struct {
	struct linux_dirent *buf;
	unsigned length;
	int node_count;
	size_t total_size;
} memory_dir;

#define FILL_ENTRY(name_str, inode)                                        \
	do {                                                               \
		dirp = (struct linux_dirent *)p;                           \
		dirp->d_ino = inode;                                       \
		strcpy(dirp->d_name, (name_str));                          \
		dirp->d_reclen =                                           \
			ROUND_UP(NAME_OFFSET() + strlen(name_str) + 1);    \
		dirp->d_off = (unsigned long)(p + dirp->d_reclen - begin); \
		p += dirp->d_reclen;                                       \
	} while (0)

int resolve_path(const char *old, char *new);

/* Wake all tasks sleeping on @in's flock wait queue.  Caller holds i_flock_lock. */
void flock_wake_all_locked(inode *in);
/* Release any flock held by @f; wakes blocked waiters.  Called by fs_put_file
 * when the last reference to an open file description is dropped. */
void fs_flock_release(file *f);

/* DAC permission check: returns 0 if allowed, -EACCES if denied */
int fs_check_perm(const struct stat *s, int mask);
int fs_check_perm_ids(const struct stat *s, int mask, unsigned uid,
		      unsigned gid);

int fs_open(const char *path, int flag, umode_t mode);
file *fs_open_file(const char *path, int flag, umode_t mode);
int fs_put_file(file *f);
int fs_install_fd(file *fp, int flag); /* install a pre-built file as an fd */
void fs_cancel_io(task_struct *task);
int fs_install_fd_unsafe(file *fp, int flag); /* caller holds files->lock */

int fs_close(int fd);

int fs_read(int fd, unsigned offset, char *buf, unsigned len);
int fs_ftruncate(int fd, uint64_t length);
int fs_readv_special(int fd, const struct iovec *iov, int count, int *handled);

int fs_write(int fd, unsigned offset, const char *buf, unsigned len);

int fs_pread(int fd, loff_t offset, char *buf, unsigned len);

int fs_pwrite(int fd, loff_t offset, const char *buf, unsigned len);

int fs_stat(const char *path, struct stat *s);

int fs_fstat(int fd, struct stat *s);

int fs_sync(int fd);

int fs_pipe(int *pipefd);

int fs_dup(int fd);
int fs_dup_from(int fd, int minfd);
int fs_dup_from_flags(int fd, int minfd, int flags);

int fs_dup2(int fd, int newfd);
int fs_dup3(int fd, int newfd, int flags);

int fs_llseek(int fd, unsigned offset_high, unsigned offset_low,
	      uint64_t *result, unsigned whence);

int fs_seek(int fd, int offset, unsigned whence);

unsigned fs_fd_poll(int fd, unsigned events, poll_table *pt);

int fs_ioctl(int fd, unsigned cmd, void *buf);

void poll_table_init(poll_table *pt, task_struct *task,
		     poll_table_entry *entries, unsigned cap);

void poll_table_cleanup(poll_table *pt);

int poll_table_add(poll_table *pt, void *opaque, poll_dereg_fn dereg);
/* Queue callbacks execute with the supplied producer lock held. */
void poll_subscribe(poll_table *pt, list_entry *head, spinlock_t *lock);
void poll_notify(list_entry *head);

int fs_chmod(const char *pathname, uint32_t mode);

int fs_chown(const char *pathname, uint32_t uid, uint32_t gid);

int fs_fchown(int fd, uint32_t uid, uint32_t gid);

int fs_fchmod(int fd, uint32_t mode);

int fs_put_file(file *f);

/* Increment file reference count (analogous to Linux get_file()) */
#define fs_get_file(f) __sync_add_and_fetch(&(((file *)f)->f_count), 1)

#endif
