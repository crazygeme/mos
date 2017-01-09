#include <console.h>
#include <klib.h>
#include <unistd.h>
#include <fs.h>
#include <tty.h>

static int console_write(void* inode, const void *buf, size_t size, size_t *wcnt);
static int console_close(void* inode);
void console_init()
{
    return 0;
}

static int console_write(void* inode, const void *buf, size_t size, size_t *wcnt)
{
    if (size < 1 || !buf)
        return 0;

    tty_write(buf, size);
    if (wcnt)
        *wcnt = size;
    return 0;
}

static int console_read(void* inode, const void *buf, size_t size, size_t *wcnt)
{
    memset(buf, '?', size);
    if (wcnt)
        *wcnt = size;

    return 0;
}

static int console_close(void* inode)
{
    return 0;
}

static int console_llseek(void* inode,  unsigned high, unsigned low, uint64_t* result, uint32_t origin)
{
    unsigned offset = low;
    switch (origin)
    {
        case SEEK_SET:
            klib_update_cursor(offset);
            break;
        case SEEK_CUR:
            klib_update_cursor(offset + klib_get_pos());
            break;
        case SEEK_END:
            klib_update_cursor(TTY_MAX_CHARS - offset);
            break;
    default:
        break;
    }
    klib_flush_cursor();
    if (result)
        *result = klib_get_pos();
}

static uint64_t console_tell(void* inode)
{
    return (uint64_t)klib_get_pos();
}

static int console_select(void* inode, unsigned type)
{
    if (type == FS_SELECT_EXCEPT || type == FS_SELECT_READ)
        return -1;

    // can write any time
    return 0;
}

static int console_stat(void* inode, struct stat* s)
{
    s->st_atime = time_now();
    s->st_mode = (S_IFCHR | S_IWUSR | S_IWGRP | S_IWOTH | S_IRUSR);
    s->st_blksize = 0;
    s->st_blocks = 400;
    s->st_ctime = time_now();
    s->st_dev = 0xb;
    s->st_gid = 0;
    s->st_ino = 0;
    s->st_mtime = 0;
    s->st_uid = 0;
    s->st_nlink = 1;
    s->st_rdev = 8004;
    return 0;
}

static fileop ttyop = {
    .write = console_write,
    .close = console_close,
    .stat = console_stat,
    .llseek = console_llseek,
    .select = console_select,
};

filep fs_alloc_filep_tty()
{
    filep fp = calloc(1, sizeof(*fp));
    fp->file_type = FILE_TYPE_CHAR;
    fp->inode = NULL;
    fp->ref_cnt = 0;
    fp->op = ttyop;
    fp->mode = 0;
    fp->istty = 1;
    return fp;
}