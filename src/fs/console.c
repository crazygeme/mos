#include <console.h>
#include <klib.h>
#include <unistd.h>
#include <fs.h>

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
    return 0;
}

static int console_close(void* inode)
{
    return 0;
}

static fileop ttyop = {
    .write = console_write,
    .close = console_close,
};

filep fs_alloc_filep_tty()
{
    filep fp = calloc(1, sizeof(*fp));
    fp->file_type = FILE_TYPE_CHAR;
    fp->inode = NULL;
    fp->ref_cnt = 0;
    fp->file_off = 0;
    fp->flag = 0;
    fp->op = ttyop;
    return fp;
}