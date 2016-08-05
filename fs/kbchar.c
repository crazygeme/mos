#include <fs/kbchar.h>
#include <drivers/chardev.h>
#include <fs/vfs.h>
#include <fs/mount.h>
#include <lib/klib.h>
#include <drivers/keyboard.h>
#include <syscall/unistd.h>

static int kbchar_read(void* aux, void* buf, unsigned len);

static void kb_free_inode(struct filesys_type*, INODE);
static INODE kb_get_root(struct filesys_type*);
static unsigned kb_get_mode(INODE inode);
static unsigned kb_read_file(INODE inode, unsigned int offset, char* buf, unsigned len);
static int kb_copy_stat(INODE node, struct stat* s, int is_dir);

static struct super_operations kb_super_operations = {
    0,
    0,
    0,
    0,
    kb_free_inode,
    kb_get_root
};

static struct inode_opreations kb_inode_operations = {
    kb_get_mode,
    kb_read_file,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    kb_copy_stat
};

void kbchar_init()
{
    chardev* dev = 0;
    struct filesys_type* type = 0;

    dev = chardev_register(0, "keyboard", kbchar_read, 0, 0);
    if (!dev)
        return;

    type = register_vfs(0, 0, dev, &kb_super_operations, &kb_inode_operations, "kb0");

    do_mount("/dev/kb0", type);

}

static int kbchar_read(void* aux, void* buf, unsigned len)
{
    char d;
    char* tmp = buf;
    if (len < 1 || !buf)
        return 0;

    d = kb_buf_get();
    *tmp = d;
    return 1;
}

static void kb_free_inode(struct filesys_type* t, INODE n)
{
    kfree(n);
}

static INODE kb_get_root(struct filesys_type* t)
{
    INODE n = kmalloc(sizeof(*n));
    n->type = t;
    n->ref_count = 0;
    return n;
}

static unsigned kb_get_mode(INODE inode)
{
    return (S_IFCHR | S_IRUSR | S_IRGRP | S_IROTH);
}

static unsigned kb_read_file(INODE inode, unsigned int offset, char* buf, unsigned len)
{
    chardev* dev = inode->type->dev;

    if (!dev || !dev->read)
        return 0;

    return dev->read(dev->aux, buf, len);
}

static int kb_copy_stat(INODE node, struct stat* s, int is_dir)
{
    s->st_atime = time(0);
    s->st_mode = kb_get_mode(node);
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
