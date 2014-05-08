#ifndef _VFS_H_
#define _VFS_H_
#include "../list.h"

#define S_IFMT  00170000
#define S_IFSOCK 0140000
#define S_IFLNK	 0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000

#define S_ISLNK(m)	(((m) & S_IFMT) == S_IFLNK)
#define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)	(((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)	(((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m)	(((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m)	(((m) & S_IFMT) == S_IFSOCK)

#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

typedef void* INODE;
typedef void* DIR;
typedef void* SUPORBLOCK;
typedef void* DESC;
struct filesys_type;

struct super_operations {
	INODE (*create_inode) (struct filesys_type* type);
	void (*destroy_inode) (struct filesys_type*, INODE);
	void (*write_super) (struct filesys_type*, SUPORBLOCK);
	void (*write_desc) (struct filesys_type*, DESC);
	void (*free_inode)(struct filesys_type*,INODE);
	INODE (*get_root)(struct filesys_type*);
};

typedef void* DIR;
struct inode_opreations{
	unsigned (*get_mode)(INODE inode);
	unsigned (*read_file)(INODE inode, unsigned int offset, void* buf, unsigned len);
	unsigned (*write_file)(INODE inode, unsigned int offset, void* buf, unsigned len);
	DIR (*open_dir)(INODE inode);
	INODE (*read_dir)(DIR dir);
	void (*add_dir_entry)(DIR dir, INODE entry);
	void (*del_dir_entry)(DIR dir, INODE entry);
	void (*close_dir)(DIR dir);
	char* (*get_name)(INODE node);
	unsigned (*get_size)(INODE node);
};

struct filesys_type{
	void* sb;
	void* desc;
	void* dev;
	struct super_operations* super_ops;
	struct inode_opreations* ino_ops;
	char rootname[32];
	LIST_ENTRY fs_list;
};

void vfs_init();
struct filesys_type* register_vfs(void* sb, void* desc, void* dev, struct super_operations* ops, struct inode_opreations* inop, char* rootname); 
void vfs_trying_to_mount_root();

INODE vfs_create_inode (struct filesys_type* type);

void vfs_destroy_inode (struct filesys_type* type, INODE);

void vfs_write_super (struct filesys_type* type, SUPORBLOCK);

void vfs_write_desc (struct filesys_type* type, DESC);

void vfs_free_inode(struct filesys_type* type,INODE);

INODE vfs_get_root(struct filesys_type* type);

unsigned vfs_get_mode(struct filesys_type* type, INODE inode);

unsigned vfs_read_file(struct filesys_type* type, INODE inode, unsigned int offset, void* buf, unsigned len);

unsigned vfs_write_file(struct filesys_type* type, INODE inode, unsigned int offset, void* buf, unsigned len);

DIR vfs_open_dir(struct filesys_type* type, INODE inode);

INODE vfs_read_dir(struct filesys_type* type, DIR dir);

void vfs_add_dir_entry(struct filesys_type* type, DIR dir, INODE entry);

void vfs_del_dir_entry(struct filesys_type* type, DIR dir, INODE entry);

void vfs_close_dir(struct filesys_type* type, DIR dir);

char* vfs_get_name(struct filesys_type* type, INODE node);

unsigned vfs_get_size(struct filesys_type* type, INODE node);

#endif
