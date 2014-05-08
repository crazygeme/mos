#include <fs/namespace.h>
#include <ps/ps.h>
#include <ps/lock.h>
#include <config.h>
#include <lib/klib.h>
#include <fs/mount.h>

static INODE fs_lookup_inode(char* path_prefix, char* path_left);
static unsigned fs_get_free_fd(INODE node);
static INODE fs_get_fd(unsigned id);
static INODE fs_clear_fd(unsigned fd);

unsigned fs_open(char* path)
{
    INODE node = fs_lookup_inode("", path);
    unsigned fd;

    if (!node) {
        return MAX_FD;
    }

    fd = fs_get_free_fd(node);
    if (fd == MAX_FD) {
        vfs_free_inode(node);
    }

    return fd;
}

void fs_close(unsigned int fd)
{
    INODE node = fs_clear_fd(fd);

    if (node) {
        vfs_free_inode(node); 
    }
}

unsigned fs_read(unsigned fd, unsigned offset, void* buf, unsigned len)
{
    INODE node = fs_get_fd(fd);
    unsigned ret;

    if (!node) {
        return 0xffffffff;
    }

    ret = vfs_read_file(node, offset,buf,len);
    vfs_free_inode(node);
    return ret;
}

unsigned fs_write(unsigned fd, unsigned offset, void* buf, unsigned len)
{
    INODE node = fs_get_fd(fd);
    unsigned ret;

    if (!node) {
        return 0xffffffff;
    }

    ret = vfs_write_file(node, offset,buf,len);
    vfs_free_inode(node);
    return ret;
}

int fs_create(char* path)
{  
    UNIMPL;
    return 0;
}

int fs_delete(char* path)
{  
    UNIMPL;
    return 0;
}

int fs_rename(char* path, char* new)
{  
    UNIMPL;
    return 0;
}

int fs_stat(char* path, struct stat* s)
{
    INODE node = fs_lookup_inode("", path);
    int ret;

    if (!node) {
        return -1;
    }

    ret = vfs_copy_stat(node,s);
    vfs_free_inode(node);
    return ret;
}

DIR fs_opendir(char* path)
{
    INODE node = fs_lookup_inode("", path);
    DIR ret;

    if (!node) {
        return 0;
    }

    ret = vfs_open_dir(node);
    vfs_free_inode(node);
    return ret;
}

void fs_readdir(DIR dir, char* name, unsigned* mode)
{
    INODE node = 0;

    if (!name || !mode) {
        return;
    }

    if (!dir) {
        *name = '\0';
        *mode = 0;
        return;
    }

    node = vfs_read_dir(dir);
    strcpy(name, vfs_get_name(node));
    *mode = vfs_get_mode(node);
    vfs_free_inode(node);

}

void fs_closedir(DIR dir)
{
    if (!dir) {
        return;
    }

    vfs_close_dir(dir);
}



static INODE fs_lookup_inode(char* path_prefix, char* path_left)
{
    struct filesys_type* type = 0;

    if ( (!path_prefix || !*path_prefix) && (!strcmp(path_left, "/"))) {
        type = mount_lookup(path_left);
        if (!type) {
            return 0;
        }

        return vfs_get_root(type);
    }

    // TODO
    return 0; 
}

static unsigned fs_get_free_fd(INODE node)
{
    task_struct* cur = CURRENT_TASK();
    int i = 0;

    sema_wait(&cur->fd_lock);

    for (i = 0; i < MAX_FD; i++) {
        if (cur->fds[i] == 0) {
            cur->fds[i] = node;
            break;
        }
    }

    sema_trigger(&cur->fd_lock);

    return i;

}

static INODE fs_get_fd(unsigned fd)
{
    task_struct* cur = CURRENT_TASK();
    INODE node;

    if (fd >= MAX_FD) {
        return 0;
    }

    sema_wait(&cur->fd_lock);
    node = cur->fds[fd];
    sema_trigger(&cur->fd_lock);

    return node;
}

static INODE fs_clear_fd(unsigned fd)
{
    task_struct* cur = CURRENT_TASK();
    INODE node;

    if (fd >= MAX_FD) {
        return 0;
    }

    sema_wait(&cur->fd_lock);
    node = cur->fds[fd];
    cur->fds[fd] = 0;
    sema_trigger(&cur->fd_lock);

    return node;

}

