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
    pipe_inode* n = calloc(1, sizeof(*n));
    n->buf = buf;
    n->readonly = 1;
    return n;
}

static void* pipe_create_writer(cy_buf* buf)
{
    pipe_inode* n = calloc(1, sizeof(*n));
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
    else{
        cyb_reader_close(node->buf);
    }

    free(n);

    return 0;
}


static int pipe_read(void* inode, const void *buf, size_t len, size_t *wcnt)
{
    unsigned char c;
    int i = 0;
    int remain = 0;
    pipe_inode* n = (pipe_inode*)inode;
    unsigned char* tmp = buf;
    int cache_len;
    if (!n->readonly)
        return -1;

    if ((cyb_writer_count(n->buf) == 0)){
        if (cyb_isempty(n->buf)){
            if (wcnt) *wcnt = 0;
            return 0;
        }
    }

    while (i < len)
    {
        c = cyb_getc(n->buf);
        if (c == EOF) {
            break;
        }
        *tmp = c;
        i++;
        tmp++;
    }

    if (wcnt) *wcnt = i;
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

static int pipe_stat(void* inode, struct stat* s)
{
    s->st_atime = time_now();
    s->st_mode = S_IFIFO | S_IRUSR | S_IWUSR;
    s->st_size = 0;
    s->st_blksize = PAGE_SIZE;
    s->st_blocks = 0;
    s->st_ctime = time_now();
    s->st_dev = 0;
    s->st_gid = 0;
    s->st_ino = 0;
    s->st_mtime = 0;
    s->st_uid = 0;
    return 0;
}

static int pipe_select(void* inode, unsigned type)
{
    pipe_inode* n = (pipe_inode*)inode;
    if (type == FS_SELECT_EXCEPT)
        return -1;

    if (type == FS_SELECT_READ)
        return cyb_isempty(n->buf) ? -1 : 0;

    if (type == FS_SELECT_WRITE)
        return cyb_isfull(n->buf) ? -1 : 0;

    return -1;
}

static int pipe_llseek(void* inode,  unsigned high, unsigned low, uint64_t* result, uint32_t origin)
{
    // FIXME
    return 0;
}

static fileop readop = {
    .read = pipe_read,
    .close = pipe_close,
    .stat = pipe_stat,
    .llseek = pipe_llseek,
    .select = pipe_select,
};

static fileop writeop = {
    .write = pipe_write,
    .close = pipe_close,
    .stat = pipe_stat,
    .llseek = pipe_llseek,
    .select = pipe_select,
};

int fs_alloc_filep_pipe(filep* pipes)
{
    cy_buf* buf = cyb_create("pipe");
    filep fp_read = calloc(1, sizeof(*fp_read));
    fp_read->file_type = FILE_TYPE_PIPE;
    fp_read->inode = pipe_create_reader(buf);
    fp_read->ref_cnt = 0;
    fp_read->op = readop;
    fp_read->istty = 0;

    filep fp_write = calloc(1, sizeof(*fp_write));
    fp_write->file_type = FILE_TYPE_PIPE;
    fp_write->inode = pipe_create_writer(buf);
    fp_write->ref_cnt = 0;
    fp_write->op = writeop;
    fp_write->istty = 0;

    pipes[0] = fp_read;
    pipes[1] = fp_write;

    return 0;
}



