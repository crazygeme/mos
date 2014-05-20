#include <fs/vfs.h>
#ifdef WIN32
#include <windows.h>
#include <osdep.h>
#else
#include <lib/klib.h>
#include <ps/lock.h>
#endif

static LIST_ENTRY vfs_list;
static semaphore vfs_lock;

void vfs_init()
{
	InitializeListHead(&vfs_list);
	sema_init(&vfs_lock, "vfs", 0);
}

struct filesys_type* register_vfs(void* sb, void* desc, void* dev, 
                                  struct super_operations* ops, struct inode_opreations* inop, 
                                  char* rootname) 
{
	struct filesys_type* fs = kmalloc(sizeof(*fs));
	fs->sb = sb;
	fs->desc = desc;
	fs->dev = dev;
	fs->super_ops = ops;
    fs->ino_ops = inop;
    strcpy(fs->rootname, rootname);
	sema_wait(&vfs_lock);
	InsertTailList(&vfs_list, &fs->fs_list);
	sema_trigger(&vfs_lock);
	return fs;
}

void vfs_trying_to_mount_root()
{
    PLIST_ENTRY entry;
    struct filesys_type* fs;
    int found = 0;
    sema_wait(&vfs_lock);
    entry = vfs_list.Flink;
    while (entry != &vfs_list) {
        fs = CONTAINER_OF(entry, struct filesys_type, fs_list);
        if (!strcmp(fs->rootname, "rootfs")) {
            found = 1;
            break;
        }
        entry = entry->Flink;
    }
    sema_trigger(&vfs_lock); 

    if (!found) {
        return;
    }

    do_mount("/", fs);
}

INODE vfs_create_inode (struct filesys_type* type)
{
    if (!type || !type->super_ops) {
        return 0;
    }

    return type->super_ops->create_inode(type);
}

void vfs_destroy_inode (INODE node)
{
    struct filesys_type* type = node->type;
    if (!type || !type->super_ops) {
        return ;
    }

    return type->super_ops->destroy_inode(type, node);
}

void vfs_write_super (struct filesys_type* type)
{

    if (!type || !type->super_ops) {
        return ;
    }

    return type->super_ops->write_super(type, type->sb);
}

void vfs_write_desc (struct filesys_type* type)
{
    if (!type || !type->super_ops) {
        return ;
    }

    return type->super_ops->write_desc(type, type->desc);
}

void vfs_free_inode(INODE node)
{
    struct filesys_type* type = node->type;

    if (!type || !type->super_ops) {
        return ;
    }

    return type->super_ops->free_inode(type, node);
}

INODE vfs_get_root(struct filesys_type* type)
{
    if (!type || !type->super_ops) {
        return 0;
    }

    return type->super_ops->get_root(type);
}


unsigned vfs_get_mode(INODE inode)
{
    struct filesys_type* type = inode->type;
    if (!type || !type->ino_ops) {
        return 0;
    }

    return type->ino_ops->get_mode(inode);
}

unsigned vfs_read_file(INODE inode, unsigned int offset, void* buf, unsigned len)
{
    struct filesys_type* type = inode->type;

    if (!type || !type->ino_ops) {
        return 0;
    }

    return type->ino_ops->read_file(inode,offset,buf,len);
}

unsigned vfs_write_file(INODE inode, unsigned int offset, void* buf, unsigned len)
{
    struct filesys_type* type = inode->type;

    if (!type || !type->ino_ops) {
        return 0;
    }

    return type->ino_ops->write_file(inode,offset,buf,len);
}

DIR vfs_open_dir(INODE inode)
{
    struct filesys_type* type = inode->type;

    if (!type || !type->ino_ops) {
        return 0;
    }

    return type->ino_ops->open_dir(inode);
}

INODE vfs_read_dir( DIR dir)
{
    struct filesys_type* type = dir->type;

    if (!type || !type->ino_ops) {
        return 0;
    }

    return type->ino_ops->read_dir(dir);
}

void vfs_add_dir_entry(DIR dir, unsigned mode, char* name)
{
    struct filesys_type* type = dir->type;

    if (!type || !type->ino_ops) {
        return ;
    }

    return type->ino_ops->add_dir_entry(dir,mode, name);
}

void vfs_del_dir_entry(DIR dir, char* name)
{
    struct filesys_type* type = dir->type;

    if (!type || !type->ino_ops) {
        return ;
    }

    return type->ino_ops->del_dir_entry(dir,name);
}

void vfs_close_dir(DIR dir)
{
    struct filesys_type* type = dir->type;

    if (!type || !type->ino_ops) {
        return ;
    }

    return type->ino_ops->close_dir(dir);
}

char* vfs_get_name(INODE node)
{
    struct filesys_type* type = node->type;

    if (!type || !type->ino_ops) {
        return 0;
    }

    return type->ino_ops->get_name(node);
}

unsigned vfs_get_size(INODE node)
{
    struct filesys_type* type = node->type;

    if (!type || !type->ino_ops) {
        return 0;
    }

    return type->ino_ops->get_size( node);

}

int vfs_copy_stat(INODE node, struct stat* s)
{
    struct filesys_type* type = node->type;

    if (!type || !type->ino_ops) {
        return 0;
    }

    return type->ino_ops->copy_stat(node, s);
}

void vfs_close(struct filesys_type* type)
{
	if (!type || !type->super_ops) {
		return ;
	}

	type->super_ops->write_super(type, type->sb);
	type->super_ops->write_desc(type, type->desc);
}


