#include <kbchar.h>
#include <klib.h>
#include <keyboard.h>
#include <unistd.h>
#include <fs.h>
#include <tty.h>

static int kb_read(void* inode, const void *buf, size_t size, size_t *wcnt);
static int kb_close(struct ext4_blockdev *bdev);

void kbchar_init()
{
}

static int kb_read(void* inode, const void *buf, size_t size, size_t *wcnt)
{
    char d;
    char* tmp = buf;
    if (size < 1 || !buf)
        return 0;

    d = kb_buf_get();
    *tmp = d;
    if (wcnt)
        *wcnt = 1;
    return 0;
}

static int kb_close(struct ext4_blockdev *bdev)
{
    return 0;
}

static int kb_select(void* inode, unsigned type)
{
    if (type == FS_SELECT_WRITE || type == FS_SELECT_EXCEPT)
        return -1;

    if (kb_can_read())
        return 0;

    return 0;
}

static int kb_stat(void* inode, struct stat* s)
{
    s->st_atime = time_now();
    s->st_mode = (S_IFCHR | S_IRUSR | S_IRGRP | S_IROTH);
    s->st_blksize = PAGE_SIZE;
    s->st_blocks = 0;
    s->st_ctime = time_now();
    s->st_dev = 0;
    s->st_rdev = 5;
    s->st_gid = 0;
    s->st_ino = 0;
    s->st_mtime = 0;
    s->st_uid = 0;
    s->st_size = PAGE_SIZE;
    return 0;
}

static fileop kbop = {
    .read = kb_read,
    .close = kb_close,
    .stat = kb_stat,
    .select = kb_select,
    .ioctl = tty_ioctl,
};

filep fs_alloc_filep_kb()
{
    filep fp = calloc(1, sizeof(*fp));
    fp->file_type = FILE_TYPE_CHAR;
    fp->inode = NULL;
    fp->ref_cnt = 0;
    fp->op = kbop;
    fp->mode = 0;
    fp->istty = 0;
    return fp;
}