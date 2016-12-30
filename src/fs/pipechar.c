#include <pipechar.h>
#include <unistd.h>
#include <klib.h>
#include <cyclebuf.h>
#include <fs.h>

typedef struct _pipe_inode
{
    cy_buf* buf;
    int readonly;
}pipe_inode;


void pipe_init()
{

}

static void* pipe_create_reader(cy_buf* buf)
{
    pipe_inode* n = kmalloc(sizeof(*n));
    n->buf = buf;
    n->readonly = 1;
    return n;
}

static void* pipe_create_writer(cy_buf* buf)
{
    pipe_inode* n = kmalloc(sizeof(*n));
    n->buf = buf;
    n->readonly = 0;
    return n;
}


static int pipe_close(void* n)
{
    pipe_inode* node = (pipe_inode*)n;
    if (!node->readonly)
    {
        cyb_writer_close(node->buf);
    }

    if (node->readonly && cyb_is_writer_closed(node->buf))
    {
        kfree(node->buf);
    }

    kfree(n);

    return 0;
}


static int pipe_read(void* inode, const void *buf, size_t len, size_t *wcnt)
{
    int i = 0;
    pipe_inode* n = (pipe_inode*)inode;
    unsigned char* tmp = buf;

    if (!n->readonly)
        return 0;

    while (i < len)
    {
        *tmp = cyb_getc(n->buf);
        if (*tmp == EOF)
            break;
        i++;
        tmp++;
    }

    if (wcnt)
        *wcnt = i;
    return 0;
}

static int pipe_write(void* inode, const void *buf, size_t len, size_t *wcnt)
{
    int i = 0;
    pipe_inode* n = (pipe_inode*)inode;
    unsigned char* tmp = buf;

    if (n->readonly)
        return 0;

    cyb_putbuf(n->buf, tmp, len);

    if (wcnt)
        *wcnt = len;
    return 0;
}

static fileop readop = {
    .read = pipe_read,
    .close = pipe_close,
};

static fileop writeop = {
    .write = pipe_write,
    .close = pipe_close
};

int fs_alloc_filep_pipe(filep* pipes)
{
    cy_buf* buf = cyb_create("pipe");
    filep fp_read = calloc(1, sizeof(*fp_read));
    fp_read->file_type = FILE_TYPE_PIPE;
    fp_read->inode = pipe_create_reader(buf);
    fp_read->ref_cnt = 0;
    fp_read->file_off = 0;
    fp_read->flag = 0;
    fp_read->op = readop;

    filep fp_write = calloc(1, sizeof(*fp_write));
    fp_write->file_type = FILE_TYPE_PIPE;
    fp_write->inode = pipe_create_reader(buf);
    fp_write->ref_cnt = 0;
    fp_write->file_off = 0;
    fp_write->flag = 0;
    fp_write->op = writeop;

    pipes[0] = fp_read;
    pipes[1] = fp_write;

    return 0;
}



