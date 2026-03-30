/*
 * net.c — /proc/net/ virtual directory for MOS.
 *
 * Mounts a sub-superblock at /net inside procfs that exposes:
 *   /proc/net/dev      — interface stats  (dev.c)
 *   /proc/net/if_inet6 — empty            (if_inet6.c)
 *   /proc/net/route    — routing table    (route.c)
 *   /proc/net/arp      — ARP cache        (arp.c)
 */
#include "proc_net.h"
#include <fs/fs.h>
#include <fs/vfs.h>
#include <proc/proc.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <macro.h>
#include <ext4.h>

/* ── /proc/net directory ────────────────────────────────────────────────────── */

static const char *net_dir_entries[] = { ".",	  "..",	 "dev", "if_inet6",
					 "route", "arp", NULL };

static const file_operations net_dir_fops;
static const inode_operations net_dir_iops;

static ssize_t net_dir_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	char *dbuf = (char *)fp->f_inode->i_private;
	loff_t fsize = (loff_t)fp->f_inode->i_size;
	loff_t offset = *pos;
	ssize_t left = (ssize_t)(fsize - offset);
	ssize_t n = (ssize_t)count < left ? (ssize_t)count : left;
	if (n <= 0)
		return 0;
	memcpy(buf, dbuf + offset, (size_t)n);
	*pos = offset + n;
	return n;
}

static int net_dir_release(file *fp)
{
	free(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static int net_dir_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_nlink = 2;
	s->st_blksize = PAGE_SIZE;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	return 0;
}

static const file_operations net_dir_fops = {
	.read = net_dir_read,
	.release = net_dir_release,
};

static const inode_operations net_dir_iops = {
	.getattr = net_dir_getattr,
};

static file *proc_net_open_root(super_block *sb, int flag)
{
	(void)sb;
	(void)flag;

	unsigned size = 0;
	int i;
	for (i = 0; net_dir_entries[i]; i++)
		size += ROUND_UP(NAME_OFFSET() + strlen(net_dir_entries[i]) +
				 1);

	char *buf = kmalloc(size);
	memset(buf, 0, size);
	char *p = buf;
	const char *begin = buf;

	for (i = 0; net_dir_entries[i]; i++) {
		struct linux_dirent *d = (struct linux_dirent *)p;
		d->d_ino = PROC_INODE;
		strcpy(d->d_name, net_dir_entries[i]);
		d->d_reclen = ROUND_UP(NAME_OFFSET() +
				       strlen(net_dir_entries[i]) + 1);
		d->d_off = (unsigned long)(p + d->d_reclen - begin);
		p += d->d_reclen;
	}

	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IXUSR |
		       S_IXGRP | S_IXOTH;
	node->i_op = &net_dir_iops;
	node->i_private = buf;
	node->i_size = size;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &net_dir_fops;
	return fp;
}

static file *proc_net_open(super_block *sb, const char *path, int flag)
{
	(void)sb;
	(void)flag;

	if (strcmp(path, "/dev") == 0)
		return open_net_dev();
	if (strcmp(path, "/if_inet6") == 0)
		return open_net_if_inet6();
	if (strcmp(path, "/route") == 0)
		return open_net_route();
	if (strcmp(path, "/arp") == 0)
		return open_net_arp();
	return NULL;
}

static super_operations proc_net_sops = {
	.open_root = proc_net_open_root,
	.open = proc_net_open,
};

/* ── registration ───────────────────────────────────────────────────────────── */
static void proc_net_register(super_block *proc_sb)
{
	vfs_mount(proc_sb, "/net", sget(&proc_net_sops));
}

PROC_INIT(proc_net_register);
