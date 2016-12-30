#include <kbchar.h>
#include <klib.h>
#include <keyboard.h>
#include <unistd.h>
#include <fs.h>

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

static fileop kbop = {
    .read = kb_read,
    .close = kb_close,
};

filep fs_alloc_filep_kb()
{
    filep fp = calloc(1, sizeof(*fp));
    fp->file_type = FILE_TYPE_CHAR;
    fp->inode = NULL;
    fp->ref_cnt = 0;
    fp->file_off = 0;
    fp->flag = 0;
    fp->op = kbop;
    return fp;
}