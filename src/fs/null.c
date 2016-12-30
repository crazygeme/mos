#include <null.h>
#include <unistd.h>
#include <klib.h>
#include <fs.h>

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

static fileop nullop = {
    .read = null_read,
    .write = null_write,
    .close = null_close,
};


filep fs_alloc_filep_null()
{
    filep fp = calloc(1, sizeof(*fp));
    fp->file_type = FILE_TYPE_CHAR;
    fp->inode = NULL;
    fp->ref_cnt = 0;
    fp->file_off = 0;
    fp->flag = 0;
    fp->op = nullop;
    return fp;
}
