#include <fs/console.h>
#include <drivers/chardev.h>
#include <fs/vfs.h>
#include <fs/mount.h>
#include <lib/klib.h>
#include <syscall/unistd.h>

static int console_write(void* aux, void* buf, unsigned len);

static void console_free_inode(struct filesys_type*, INODE);
static INODE console_get_root(struct filesys_type*);
static unsigned console_get_mode(INODE inode);
static unsigned console_write_file(INODE inode, unsigned int offset, char* buf, unsigned len);
static int console_copy_stat(INODE node, struct stat* s, int is_dir);

static struct super_operations console_super_operations = {
	0,
	0,
	0,
	0,
	console_free_inode,
	console_get_root
};

static struct inode_opreations console_inode_operations = {
	console_get_mode,
	0,
	console_write_file,
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	console_copy_stat
};

void console_init()
{
	chardev* dev = 0;
	struct filesys_type* type = 0;

	dev = chardev_register(0, "console", 0, console_write, 0);
	if(!dev)
		return;

	type = register_vfs(0, 0, dev, &console_super_operations, &console_inode_operations, "tty0" );

	do_mount("/dev/tty0", type);

}

static int console_write(void* aux, void* buf, unsigned len)
{
	if(len < 1 || !buf)
		return 0;

	tty_write(buf, len);
	return len;
}

static void console_free_inode(struct filesys_type* t, INODE n)
{
	kfree(n);
}

static INODE console_get_root(struct filesys_type* t)
{
	INODE n = kmalloc(sizeof(*n));
	n->type = t;
	n->ref_count = 0;
	return n;
}

static unsigned console_get_mode(INODE inode)
{
	return (S_IFCHR | S_IWUSR | S_IWGRP | S_IWOTH | S_IRUSR);
}

static unsigned console_write_file(INODE inode, unsigned int offset, char* buf, unsigned len)
{
	chardev* dev = inode->type->dev;

	if(!dev || !dev->write)
		return 0;

	return dev->write(dev->aux, buf, len);
}

static int console_copy_stat(INODE node, struct stat* s, int is_dir)
{
	s->st_atime = time(0);
	s->st_mode = console_get_mode(node);
	s->st_size = 0;
	s->st_blksize = 0;
	s->st_blocks = 400;
	s->st_ctime = time(0);
	s->st_dev = 0xb;
	s->st_gid = 0;
	s->st_ino = 0;
	s->st_mtime = 0;
	s->st_uid = 0;
	s->st_nlink = 1;
	s->st_rdev = 8004;
	
	return 1;
}

