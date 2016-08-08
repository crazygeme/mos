#include <pipechar.h>
#include <chardev.h>
#include <vfs.h>
#include <mount.h>
#include <unistd.h>
#include <klib.h>
#include <cyclebuf.h>

typedef struct _pipe_inode
{
    struct filesys_type* type;
    unsigned ref_count;
    cy_buf* buf;
    int readonly;
}pipe_inode;

static void pipe_free_inode(struct filesys_type*, INODE);
static INODE pipe_get_root(struct filesys_type*);
static unsigned pipe_get_mode(INODE inode);
static int pipe_copy_stat(INODE node, struct stat* s, int is_dir);
static unsigned pipe_read_file(INODE inode, unsigned int offset, char* buf, unsigned len);
static unsigned pipe_write_file(INODE inode, unsigned int offset, char* buf, unsigned len);

static struct super_operations pipe_super_operations = {
    0,
    0,
    0,
    0,
    pipe_free_inode,
    pipe_get_root
};

static struct inode_opreations pipe_inode_operations = {
    pipe_get_mode,
    pipe_read_file,
    pipe_write_file,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    pipe_copy_stat
};

void pipe_init()
{
    chardev* dev = 0;
    struct filesys_type* type = 0;

    dev = chardev_register(0, "pipe", 0, 0, 0);
    if (!dev)
        return;

    type = register_vfs(0, 0, dev, &pipe_super_operations, &pipe_inode_operations, "pipe");

    do_mount("pipefs", type);

}

INODE pipe_create_reader(cy_buf* buf)
{
    pipe_inode* n = kmalloc(sizeof(*n));
    n->type = mount_lookup("pipefs");
    n->ref_count = 0;
    n->buf = buf;
    n->readonly = 1;
    return n;
}

INODE pipe_create_writer(cy_buf* buf)
{
    pipe_inode* n = kmalloc(sizeof(*n));
    n->type = mount_lookup("pipefs");
    n->ref_count = 0;
    n->buf = buf;
    n->readonly = 0;
    return n;
}


static void pipe_free_inode(struct filesys_type* t, INODE n)
{
    pipe_inode* node = (pipe_inode*)n;
    klog("[pipe]node readonly %d, buf closed %d\n", node->readonly, cyb_is_writer_closed(node->buf));
    if (!node->readonly)
    {
        klog("[pipe] close writer\n");
        cyb_writer_close(node->buf);
    }

    if (node->readonly && cyb_is_writer_closed(node->buf))
    {
        klog("[pipe] free cycle buffer\n");
        kfree(node->buf);
    }

    klog("[pipe] node %x\n", n);
    kfree(n);
}

static INODE pipe_get_root(struct filesys_type* t)
{
    //  pipe_inode* n = kmalloc(sizeof(*n));
    //  n->type = t;
    //  n->ref_count = 0;
    //  n->buf = cyb_create("pipe");
    //  return n;
    return 0;
}

static unsigned pipe_get_mode(INODE inode)
{
    return (S_IFCHR | S_IWUSR | S_IWGRP | S_IWOTH | S_IRUSR | S_IRGRP | S_IROTH);
}


static unsigned pipe_read_file(INODE inode, unsigned int offset, char* buf, unsigned len)
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

    return i;
}

static unsigned pipe_write_file(INODE inode, unsigned int offset, char* buf, unsigned len)
{
    int i = 0;
    pipe_inode* n = (pipe_inode*)inode;
    unsigned char* tmp = buf;

    if (n->readonly)
        return 0;

    while (i < len)
    {
        cyb_putc(n->buf, *tmp);
        i++;
        tmp++;
    }
    return len;
}

static int pipe_copy_stat(INODE node, struct stat* s, int is_dir)
{
    s->st_atime = time(0);
    s->st_mode = pipe_get_mode(node);
    s->st_size = 0;
    s->st_blksize = 0;
    s->st_blocks = 0;
    s->st_ctime = time(0);
    s->st_dev = 0;
    s->st_gid = 0;
    s->st_ino = 0;
    s->st_mtime = 0;
    s->st_uid = 0;

    return 1;
}



