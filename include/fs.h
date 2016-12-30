#ifndef _FS_H_
#define _FS_H_
#include <mm.h>
#include <block.h>
#include <lwext4/include/ext4.h>

typedef struct _block block;
#define S_IFMT  00170000
#define S_IFSOCK 0140000
#define S_IFLNK         0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000

#define S_ISLNK(m)     (((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)     (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)     (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)     (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)     (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m)    (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m)    (((m) & S_IFMT) == S_IFSOCK)

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

#define EOF ((unsigned char)(-1))

#define FILE_TYPE_NORMAL    1
#define FILE_TYPE_PIPE      2
#define FILE_TYPE_CHAR      3
#define FILE_TYPE_DIR       4



#define MAX_FD ((PAGE_SIZE)/sizeof(file*))

typedef struct _fileop
{
    int (*close)(void* inode);
    int (*read)(void* inode, void *buf, size_t size, size_t *rcnt);
    int (*write)(void* inode, const void *buf, size_t size, size_t *wcnt);
    int (*seek)(void* inode, uint64_t offset, uint32_t origin);
}fileop;

typedef struct _file
{
    int file_type;
    void    *inode;
    unsigned ref_cnt;
    unsigned file_off;
    unsigned flag;
    unsigned mode;
    fileop op;
}file;
typedef file* filep;

filep fs_alloc_filep_tty();
filep fs_alloc_filep_kb();
filep fs_alloc_filep_null();
int fs_alloc_filep_pipe(filep* pipes);

int ext4_blockdev_register(block* aux, char* name, int sec_size, int sec_cnt);

void fs_mount_root();


#define fs_refrence(f)\
    __sync_add_and_fetch(&(((filep)f)->ref_cnt), 1)


#endif