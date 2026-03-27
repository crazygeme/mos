#ifndef _FS_FS_H_
#define _FS_FS_H_

#include <mm/mm.h>
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

/* Used by fs_select() and poll op */
#define FS_POLL_READ 0
#define FS_POLL_WRITE 1
#define FS_POLL_EXCEPT 2

typedef int64_t loff_t;
typedef int ssize_t;

typedef struct _inode inode;
typedef struct _file file;

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
	/* poll: return 0 if ready, -1 if not ready */
	int (*poll)(file *file, unsigned type);
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
	char *f_name;
};

typedef struct _file_descriptor {
	file *fp;
	unsigned flag;
	unsigned used;
} file_descriptor;

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

#define MAX_FD ((PAGE_SIZE) / sizeof(file_descriptor))

int resolve_path(const char *old, char *new);

/* DAC permission check: returns 0 if allowed, -EACCES if denied */
int fs_check_perm(const struct stat *s, int mask);

int fs_open(const char *path, int flag, char *mode);
file *fs_open_file(const char *path, int flag, char *mode, int follow_link);
int fs_put_file(file *f);
int fs_install_fd(file *fp, int flag); /* install a pre-built file as an fd */

int fs_close(int fd);

int fs_read(int fd, unsigned offset, char *buf, unsigned len);

int fs_write(int fd, unsigned offset, char *buf, unsigned len);

int fs_stat(const char *path, struct stat *s);

int fs_fstat(int fd, struct stat *s);

int fs_sync(int fd);

int fs_pipe(int *pipefd);

int fs_dup(int fd);

int fs_dup2(int fd, int newfd);

int fs_llseek(int fd, unsigned offset_high, unsigned offset_low,
	      uint64_t *result, unsigned whence);

int fs_seek(int fd, int offset, unsigned whence);

int fs_select(int fd, unsigned type);

int fs_ioctl(int fd, unsigned cmd, void *buf);

int fs_chmod(const char *pathname, uint32_t mode);

int fs_chown(const char *pathname, uint32_t uid, uint32_t gid);

int fs_fchown(int fd, uint32_t uid, uint32_t gid);

int fs_fchmod(int fd, uint32_t mode);

file *fs_open_file(const char *path, int flag, char *mode, int follow_link);

int fs_put_file(file *f);

/* Increment file reference count (analogous to Linux get_file()) */
#define fs_get_file(f) __sync_add_and_fetch(&(((file *)f)->f_count), 1)

#endif
