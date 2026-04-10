#ifndef _FS_FS_H_
#define _FS_FS_H_

#include <mm/mm.h>
#include <lib/lock.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>

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

typedef int64_t loff_t;
typedef int off_t;
typedef int ssize_t;

typedef struct _inode inode;
typedef struct _file file;

/* Forward declarations for poll/select hooks */
typedef struct _task_struct task_struct;
typedef void (*poll_dereg_fn)(void *opaque, task_struct *task);

typedef struct _poll_table_entry {
	void *opaque;
	poll_dereg_fn dereg;
} poll_table_entry;

typedef struct _poll_table {
	task_struct *task;
	unsigned nr;
	unsigned cap;
	int unsupported;
	poll_table_entry *entries;
} poll_table;

/*
 * file_operations - per-open-file operations, analogous to Linux
 * struct file_operations. All ops receive the open file as first argument.
 */
typedef struct _file_operations {
	/* Custom dtor of file struct, will call regular kfree if not provided */
	int (*release)(file *file);
	/* read/write: update *pos on success, return bytes transferred or -errno */
	ssize_t (*read)(file *file, void *buf, size_t size, loff_t *pos);
	ssize_t (*write)(file *file, const void *buf, size_t size, loff_t *pos);
	/* llseek: return new position or -errno */
	loff_t (*llseek)(file *file, loff_t offset, int whence);
	/*
	 * poll: return an FS_POLL_* readiness bitmask for the requested @events.
	 * If @pt is non-NULL, the implementation may register wakeups for the
	 * current task through poll_table helpers; deregistration is handled by
	 * the generic poll/select layer.
	 */
	unsigned (*poll)(file *file, unsigned events, poll_table *pt);
	int (*ioctl)(file *file, unsigned cmd, void *buf);
	int (*flush)(file *file);
} file_operations;

/*
 * inode_operations - inode-level metadata operations, analogous to Linux
 * struct inode_operations.
 */
typedef struct _inode_operations {
	int (*getattr)(inode *inode, struct stat *s);
	int (*setattr)(inode *inode, uint32_t mode);
	int (*chown)(inode *inode, uint32_t uid, uint32_t gid);
	/* read_page/write_page: transfer one PAGE_SIZE chunk at file offset @offset */
	int (*read_page)(inode *inode, unsigned offset, void *buf);
	int (*write_page)(inode *inode, unsigned offset, const void *buf);
	int (*ftruncate)(inode *inode, loff_t size);
} inode_operations;

/*
 * inode - in-memory representation of a filesystem object.
 * i_private holds the backend-specific handle (e.g. ext4_file *).
 */
struct _inode {
	uint32_t i_mode;
	uint64_t i_ino;
	uint64_t i_size;
	const inode_operations *i_op;
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
	char *f_name;
	int f_flock; /* current flock: 0=none, LOCK_SH, or LOCK_EX */
};

#define FS_FILE_UNLINK_ON_CLOSE 0x1u

struct linux_dirent {
	unsigned long d_ino; /* Inode number */
	unsigned long d_off; /* Offset to next linux_dirent */
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

#define MAX_FD ((PAGE_SIZE) / sizeof(file *))
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

int fs_open(const char *path, int flag, umode_t mode);
file *fs_open_file(const char *path, int flag, umode_t mode);
int fs_put_file(file *f);
int fs_install_fd(file *fp, int flag); /* install a pre-built file as an fd */

int fs_close(int fd);

int fs_read(int fd, unsigned offset, char *buf, unsigned len);

int fs_write(int fd, unsigned offset, const char *buf, unsigned len);

int fs_pread(int fd, unsigned offset, char *buf, unsigned len);

int fs_pwrite(int fd, unsigned offset, const char *buf, unsigned len);

int fs_stat(const char *path, struct stat *s);

int fs_fstat(int fd, struct stat *s);

int fs_sync(int fd);

int fs_pipe(int *pipefd);

int fs_dup(int fd);

int fs_dup2(int fd, int newfd);

int fs_llseek(int fd, unsigned offset_high, unsigned offset_low,
	      uint64_t *result, unsigned whence);

int fs_seek(int fd, int offset, unsigned whence);

unsigned fs_fd_poll(int fd, unsigned events, poll_table *pt);

int fs_ioctl(int fd, unsigned cmd, void *buf);

void poll_table_init(poll_table *pt, task_struct *task,
		     poll_table_entry *entries, unsigned cap);
void poll_table_cleanup(poll_table *pt);
int poll_table_add(poll_table *pt, void *opaque, poll_dereg_fn dereg);
void poll_table_note_unsupported(poll_table *pt);

int fs_chmod(const char *pathname, uint32_t mode);

int fs_chown(const char *pathname, uint32_t uid, uint32_t gid);

int fs_fchown(int fd, uint32_t uid, uint32_t gid);

int fs_fchmod(int fd, uint32_t mode);

int fs_put_file(file *f);

/* Increment file reference count (analogous to Linux get_file()) */
#define fs_get_file(f) __sync_add_and_fetch(&(((file *)f)->f_count), 1)

#endif
