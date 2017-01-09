#include <null.h>
#include <unistd.h>
#include <klib.h>
#include <fs.h>
#include <include/fs.h>

static int null_read(void* inode, const void *buf, size_t size, size_t *wcnt);
static int null_write(void* inode, const void *buf, size_t size, size_t *wcnt);
static int null_close(void* inode);

void null_init()
{
}


static int null_read(void* inode, const void *buf, size_t size, size_t *wcnt)
{
    memset(buf, 0, size);
    if (wcnt)
        *wcnt = size;
}

static int null_write(void* inode, const void *buf, size_t size, size_t *wcnt)
{
    // do nothing!
    if (wcnt)
        *wcnt = size;
    return 0;
}

static int null_close(void* inode)
{
    return 0;
}

static int null_select(void* inode, unsigned type)
{
    if (type == FS_SELECT_EXCEPT)
        return -1;
    
    return 0;
}

static int null_stat(void* inode, struct stat* s)
{
    s->st_atime = time_now();
    s->st_mode = (S_IFCHR | S_IWUSR | S_IWGRP | S_IWOTH | S_IRUSR | S_IRGRP | S_IROTH);
    s->st_size = 0;
    s->st_blksize = 0;
    s->st_blocks = 0;
    s->st_ctime = time_now();
    s->st_dev = 0;
    s->st_gid = 0;
    s->st_ino = 0;
    s->st_mtime = 0;
    s->st_uid = 0;
    return 0;
}

static fileop nullop = {
    .read = null_read,
    .write = null_write,
    .close = null_close,
    .stat = null_stat,
    .select = null_select,
};


filep fs_alloc_filep_null()
{
    filep fp = calloc(1, sizeof(*fp));
    fp->file_type = FILE_TYPE_CHAR;
    fp->inode = NULL;
    fp->ref_cnt = 0;
    fp->op = nullop;
    fp->istty = 0;
    return fp;
}
