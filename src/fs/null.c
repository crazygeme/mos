#include <null.h>
#include <chardev.h>
#include <vfs.h>
#include <mount.h>
#include <unistd.h>
#include <klib.h>

static void null_free_inode(struct filesys_type*, INODE);
static INODE null_get_root(struct filesys_type*);
static unsigned null_get_mode(INODE inode);
static int null_copy_stat(INODE node, struct stat* s, int is_dir);
static unsigned null_read_file(INODE inode, unsigned int offset, char* buf, unsigned len);
static unsigned null_write_file(INODE inode, unsigned int offset, char* buf, unsigned len);
static struct super_operations null_super_operations = {
    0,
    0,
    0,
    0,
    null_free_inode,
    null_get_root
};

static struct inode_opreations null_inode_operations = {
    null_get_mode,
    null_read_file,
    null_write_file,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    null_copy_stat
};

void null_init()
{
    chardev* dev = 0;
    struct filesys_type* type = 0;

    dev = chardev_register(0, "null", 0, 0, 0);
    if (!dev)
        return;

    type = register_vfs(0, 0, dev, &null_super_operations, &null_inode_operations, "null");

    do_mount("/dev/null", type);

}



static void null_free_inode(struct filesys_type* t, INODE n)
{
    kfree(n);
}

static INODE null_get_root(struct filesys_type* t)
{
    INODE n = kmalloc(sizeof(*n));
    n->type = t;
    n->ref_count = 0;
    return n;
}

static unsigned null_get_mode(INODE inode)
{
    return (S_IFCHR | S_IWUSR | S_IWGRP | S_IWOTH | S_IRUSR | S_IRGRP | S_IROTH);
}

static unsigned null_read_file(INODE inode, unsigned int offset, char* buf, unsigned len)
{
    return 0;
}

static unsigned null_write_file(INODE inode, unsigned int offset, char* buf, unsigned len)
{
    return len;
}

static int null_copy_stat(INODE node, struct stat* s, int is_dir)
{
    s->st_atime = time_now();
    s->st_mode = null_get_mode(node);
    s->st_size = 0;
    s->st_blksize = 0;
    s->st_blocks = 0;
    s->st_ctime = time_now();
    s->st_dev = 0;
    s->st_gid = 0;
    s->st_ino = 0;
    s->st_mtime = 0;
    s->st_uid = 0;

    return 1;
}


