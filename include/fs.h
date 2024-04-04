#ifndef _FS_H_
#define _FS_H_
#include <mm.h>
#include <ext4.h>

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

#define S_ISLNK(m) (((m)&S_IFMT) == S_IFLNK)
#define S_ISREG(m) (((m)&S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m)&S_IFMT) == S_IFDIR)
#define S_ISCHR(m) (((m)&S_IFMT) == S_IFCHR)
#define S_ISBLK(m) (((m)&S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m)&S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m)&S_IFMT) == S_IFSOCK)

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

#define FS_SELECT_READ 0
#define FS_SELECT_WRITE 1
#define FS_SELECT_EXCEPT 2

typedef struct _fileop {
	int (*close)(void *inode);
	int (*read)(void *inode, void *buf, size_t size, size_t *rcnt);
	int (*write)(void *inode, const void *buf, size_t size, size_t *wcnt);
	int (*seek)(void *inode, uint64_t offset, uint32_t origin);
	int (*llseek)(void *inode, unsigned high, unsigned low,
		      uint64_t *result, uint32_t origin);
	int (*stat)(void *inode, struct stat *s);
	uint64_t (*tell)(void *inode);
	int (*select)(void *inode, unsigned type);
	int (*ioctl)(void *inode, unsigned cmd, void *buf);
} fileop;

typedef struct _file {
	int file_type;
	void *inode;
	unsigned ref_cnt;
	unsigned mode;
	int istty;
	fileop op;
	char *name;
} file;
typedef file *filep;

typedef struct _file_descriptor {
	filep fp;
	unsigned flag;
	unsigned file_off;
	unsigned used;
} file_descriptor;

struct linux_dirent {
	unsigned long d_ino; /* Inode number */
	unsigned long d_off; /* Offset to next linux_dirent */
	unsigned short d_reclen; /* Length of this linux_dirent */
	char d_name[]; /* Filename (null-terminated) */
};

#define ROUND_UP(x) (((x) + sizeof(long) - 1) & ~(sizeof(long) - 1))
#define NAME_OFFSET(de) ((int)((de)->d_name - (char *)(de)))

#define NAME_MAX 255
struct old_linux_dirent {
	unsigned long d_ino; /* inode number */
	unsigned long d_off; /* offset to this old_linux_dirent */
	unsigned short d_reclen; /* length of this d_name */
	char d_name[NAME_MAX + 1]; /* filename (null-terminated) */
};

#define MAX_FD ((PAGE_SIZE) / sizeof(file_descriptor))

int resolve_path(const char *old, char *new);

filep fs_alloc_filep_tty();
filep fs_alloc_filep_kb();
filep fs_alloc_filep_null();
int fs_alloc_filep_pipe(filep *pipes);

int ext4_blockdev_register(block *aux, char *name, int sec_size, int sec_cnt);

void fs_mount_root();

int fs_open(const char *path, int flag, char *mode);

int fs_close(int fd);

int fs_read(int fd, unsigned offset, char *buf, unsigned len);

int fs_write(int fd, unsigned offset, char *buf, unsigned len);

int fs_delete(const char *path);

int fs_stat(const char *path, struct stat *s);

int fs_fstat(int fd, struct stat *s);

int fs_pipe(int *pipefd);

int fs_dup(int fd);

int fs_dup2(int fd, int newfd);

int fs_llseek(int fd, unsigned offset_high, unsigned offset_low,
	      uint64_t *result, unsigned whence);

int fs_seek(int fd, unsigned offset, unsigned whence);

int fs_select(int fd, unsigned type);

int fs_ioctl(int fd, unsigned cmd, void *buf);

int fs_chmod(const char *pathname, uint32_t mode);

int fs_fchmod(int fd, uint32_t mode);

filep fs_open_file(const char *path, int flag, char *mode, int follow_link);

int fs_destroy(filep f);

filep fs_alloc_filep_normal(void *content);

filep fs_alloc_filep_dir(void *content);

#define fs_refrence(f) __sync_add_and_fetch(&(((filep)f)->ref_cnt), 1)

#endif