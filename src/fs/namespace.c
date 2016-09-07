#include <namespace.h>
#ifdef WIN32
#include <windows.h>
#include <osdep.h>
#elif MACOS
#include <osdep.h>
#else
#include <ps.h>
#include <lock.h>
#include <config.h>
#include <klib.h>
#include <mount.h>
#include <timer.h>
#include <errno.h>
#include <cyclebuf.h>
#endif
#include <unistd.h>
#include <cache.h>


static INODE fs_lookup_inode(char* path);
static unsigned fs_get_free_fd(INODE node, char* path);
static INODE fs_get_fd(unsigned id, char* path);
static void* fs_clear_fd(unsigned fd, int* isdir);

static unsigned fs_dir_get_free_fd(DIR node, char* path);
static DIR fs_dir_get_fd(unsigned id);

static unsigned fs_dup_to_free_fd(unsigned fd);

unsigned fs_open(char* path)
{
    INODE node = 0;
    DIR dir = 0;
    unsigned fd;
    char* fullPath = kmalloc(64);
    key_value_pair* pair;
    memset(fullPath, 0, 64);


    // FIXME
    if (!strcmp(path, "."))
    {
        sys_getcwd(fullPath, 64);
    }
    else
    {
        strcpy(fullPath, path);
    }

    node = fs_lookup_inode(fullPath);

    if (!node)
    {
        kfree(fullPath);
        return -ENOENT;
    }

    if (S_ISDIR(vfs_get_mode(node)))
    {
        dir = vfs_open_dir(node);
        fd = fs_dir_get_free_fd(dir, fullPath);
        vfs_free_inode(node);
        if (fd == MAX_FD)
        {
            vfs_close_dir(dir);
        }
    }
    else
    {
        fd = fs_get_free_fd(node, fullPath);
        if (fd == MAX_FD)
        {
            vfs_free_inode(node);
        }
    }

    if (fd == MAX_FD)
    {
        fd = -ENFILE;
    }

    kfree(fullPath);
    return fd;
}

int fs_dup(int oldfd)
{
    unsigned fd = fs_dup_to_free_fd(oldfd);
    if (fd == MAX_FD)
    {
        return 0xffffffff;
    }

    return fd;
}

int fs_dup2(int oldfd, int newfd)
{
    task_struct* cur = CURRENT_TASK();

    if (!cur->fds[oldfd].flag)
    {
        return -1;
    }


    if (cur->fds[newfd].flag)
    {
        fs_close(newfd);
    }

    sema_wait(&cur->fd_lock);


    cur->fds[newfd] = cur->fds[oldfd];
    cur->fds[newfd].path = strdup(cur->fds[oldfd].path);
    vfs_refrence(cur->fds[oldfd].file);


    sema_trigger(&cur->fd_lock);

    return 0;
}

void fs_close(unsigned int fd)
{
    INODE node = 0;
    DIR   dir = 0;
    int isdir;
    void* ret = 0;
    task_struct* cur = CURRENT_TASK();

    if (!cur->fds[fd].flag)
    {
        return;
    }

    ret = fs_clear_fd(fd, &isdir);


    if (isdir)
    {
        dir = ret;
        if (dir)
        {
            vfs_close_dir(dir);
        }
    }
    else
    {
        node = ret;
        if (node)
        {
            vfs_free_inode(node);
        }
    }
}

#ifndef WIN32
#ifndef MACOS
int fs_pipe(int* pipefd)
{
    task_struct *cur = CURRENT_TASK();
    cy_buf* buf = cyb_create("pipe");
    INODE reader = pipe_create_reader(buf);
    INODE writer = pipe_create_writer(buf);

    int readfd = fs_get_free_fd(reader, "pipefs");
    int writfd = fs_get_free_fd(writer, "pipefs");


    vfs_refrence(reader);
    vfs_refrence(writer);

    if (readfd == -1) return -1;

    if (writfd == -1) return -1;

    cur->fds[readfd].flag |= fd_flag_readonly;
    cur->fds[writfd].flag |= fd_flag_writonly;
    pipefd[0] = readfd;
    pipefd[1] = writfd;

    return 0;
}
#endif
#endif

unsigned fs_read(unsigned fd, unsigned offset, void* buf, unsigned len)
{
    INODE node = fs_get_fd(fd, 0);
    unsigned ret;

    if (fd == 0xffffffff)
    {
        return 0xffffffff;
    }

    if (!node)
    {
        return 0xffffffff;
    }

    ret = vfs_read_file(node, offset, buf, len);
    return ret;
}

unsigned fs_write(unsigned fd, unsigned offset, void* buf, unsigned len)
{
    INODE node = fs_get_fd(fd, 0);
    unsigned ret;

    if (fd == 0xffffffff)
    {
        return 0xffffffff;
    }

    if (!node)
    {
        return 0xffffffff;
    }

    ret = vfs_write_file(node, offset, buf, len);
    return ret;
}

int fs_create(char* path, unsigned mode)
{
    DIR dir;
    char dir_name[260] = {0};
    char* t;
    struct stat s;
    int stat_status;

    strcpy(dir_name, path);
    t = strrchr(dir_name, '/');
    if (!t)
        return 0;

    *t = '\0';
    t++;

    if (*dir_name == '\0')
    {
        dir = fs_opendir("/");
        stat_status = fs_stat("/", &s);
    }
    else
    {
        dir = fs_opendir(dir_name);
        stat_status = fs_stat(dir_name, &s);
    }

    if (!dir)
        return 0;

    if (stat_status == -1 || !S_ISDIR(s.st_mode))
    {
        fs_closedir(dir);
        return 0;
    }

    if (fs_stat(path, &s) != -1)
    {
        fs_closedir(dir);
        return 0;
    }

    vfs_add_dir_entry(dir, mode, t);


    fs_closedir(dir);
    return 1;
}

int fs_delete(char* path)
{
    DIR dir;
    char *dir_name;
    char* t;
    struct stat s;
    int stat_status;

    dir_name = kmalloc(64);
    strcpy(dir_name, path);
    t = strrchr(dir_name, '/');
    if (!t)
    {
        kfree(dir_name);
        return 0;
    }

    *t = '\0';
    t++;

    if (*dir_name == '\0')
    {
        dir = fs_opendir("/");
        stat_status = fs_stat("/", &s);
    }
    else
    {
        dir = fs_opendir(dir_name);
        stat_status = fs_stat(dir_name, &s);
    }

    if (!dir)
    {
        kfree(dir_name);
        return 0;
    }

    if (stat_status == -1 || !S_ISDIR(s.st_mode))
    {
        fs_closedir(dir);
        kfree(dir_name);
        return 0;
    }

    if (fs_stat(path, &s) == -1)
    {
        fs_closedir(dir);
        kfree(dir_name);
        return 0;
    }

    vfs_del_dir_entry(dir, t);


    fs_closedir(dir);
    kfree(dir_name);
    return 1;
}

int fs_rename(char* path, char* new)
{
    UNIMPL;
    return 0;
}

int fs_stat(char* path, struct stat* s)
{
    INODE node = 0;
    int ret;
    char* fullPath = kmalloc(64);
    memset(fullPath, 0, 64);


    // FIXME
    if (!strcmp(path, "."))
    {
        sys_getcwd(fullPath, 64);
    }
    else
    {
        strcpy(fullPath, path);
    }


    node = fs_lookup_inode(fullPath);
    if (!node)
    {
        kfree(fullPath);
        return -1;
    }

    ret = vfs_copy_stat(node, s, 0);
    vfs_free_inode(node);
    kfree(fullPath);
    return 0;
}

// FIXME!
// do not seperate INODE and DIR
// orelse ugly code will have to added
int fs_fstat(int fd, struct stat* s)
{
    INODE node = 0;
    DIR dir = 0;
    int ret;

#ifdef __VERBOS_SYSCALL__
    klog("fstat %d, ", fd);
#endif

    if (fd < 0 || fd >= MAX_FD)
    {
#ifdef __VERBOS_SYSCALL__
        klog_printf("ret %d\n", -1);
#endif
        return -1;
    }

    node = fs_get_fd(fd, 0);

    if (!node)
    {
        dir = fs_dir_get_fd(fd);
    }

    if (!node && !dir)
    {
#ifdef __VERBOS_SYSCALL__
        klog_printf("ret %d\n", -1);
#endif
        return -1;
    }

    if (node)
    {
        ret = vfs_copy_stat(node, s, 0);
    }
    else if (dir)
    {
        ret = vfs_copy_stat(dir, s, 1);
    }
#ifdef __VERBOS_SYSCALL__
    klog_printf(" (stat: mode %x, size %d, blocks %d, blksize %d) ret %d\n",
        s->st_mode, s->st_size, s->st_blocks, s->st_blksize, 0);
#endif

    return 1;
}

DIR fs_opendir(char* path)
{
    INODE node = fs_lookup_inode(path);
    DIR ret;
    if (!node)
    {
        return 0;
    }
    if (!S_ISDIR(vfs_get_mode(node)))
    {
        return 0;
    }

    ret = vfs_open_dir(node);
    vfs_free_inode(node);
    return ret;
}

int fs_readdir(DIR dir, char* name, unsigned* mode)
{
    INODE node = 0;

    if (!name || !mode)
    {
        return 0;
    }

    if (!dir)
    {
        *name = '\0';
        *mode = 0;
        return 0;
    }

    node = vfs_read_dir(dir);
    if (!node)
    {
        *name = '\0';
        *mode = 0;
        return 0;
    }
    strcpy(name, vfs_get_name(node));
    *mode = vfs_get_mode(node);
    vfs_free_inode(node);
    return 1;
}

int sys_readdir(unsigned fd, struct dirent* entry)
{
    DIR dir = fs_dir_get_fd(fd);
    int ret = fs_readdir(dir, entry->d_name, &entry->d_mode);
    if (ret)
    {
        entry->d_namlen = strlen(entry->d_name);
        entry->d_ino = 1;
        entry->d_reclen = ((entry->d_namlen + 10) / 4 + 1) * 4;

    }

    return ret;
}

void fs_closedir(DIR dir)
{
    if (!dir)
    {
        return;
    }

    vfs_close_dir(dir);
}


static INODE fs_get_dirent_node(INODE node, char* name)
{
    DIR dir = 0;
    INODE entry = 0;
    int found = 0;

    if (!S_ISDIR(vfs_get_mode(node)))
        return 0;

    dir = vfs_open_dir(node);
    entry = vfs_read_dir(dir);
    while (entry)
    {
        if (!strcmp(name, vfs_get_name(entry)))
        {
            found = 1;
            break;
        }

        vfs_free_inode(entry);
        entry = vfs_read_dir(dir);
    }

    vfs_close_dir(dir);
    if (found)
    {
        return entry;
    }
    else
    {
        return 0;
    }
}

static INODE fs_check_mountpoint(char* path)
{
    struct filesys_type* type = 0;
    INODE node = 0;
    type = mount_lookup(path);
    if (!type || !type->super_ops || !type->super_ops->get_root)
    {
        return 0;
    }


    node = type->super_ops->get_root(type);
    if (node)
    {
        vfs_refrence(node);
    }
    return node;

}


static void fs_add_inode_cache(INODE node, char* path)
{
    vfs_refrence(node);
    hash_insert(inode_cache, strdup(path), node);
}

static INODE fs_lookup_inode(char* path)
{
    struct filesys_type* type = (void*)0;
    INODE root;
    char* parent = 0;
    char* tmp;
    key_value_pair* pair = 0;

    if (!path || !*path)
        return 0;

    pair = hash_find(inode_cache, path);
    if (pair)
    {
        INODE ret = pair->val;
        vfs_refrence(ret);
        //printk("find cached inode %s\n", path);
        return ret;
    }

    // find root file system
    type = mount_lookup("/");
    if (!type)
        return 0;

    root = vfs_get_root(type);
    if (!strcmp(path, "/"))
    {
        fs_add_inode_cache(root, path);
        return root;
    }

    parent = kmalloc(64);
    memset(parent, 0, 64);

    strcpy(parent, path);
    tmp = parent;

    do
    {
        INODE p;
        INODE node;
        char* slash = 0;

        tmp++;
        p = root;

        slash = strchr(tmp, '/');
        while (slash)
        {
            *slash = '\0';
            node = fs_check_mountpoint(parent);
            if (!node)
            {
                node = fs_get_dirent_node(p, tmp);
            }
            else
            {
                fs_add_inode_cache(node, parent);
            }
            vfs_free_inode(p);
            if (!node)
            {
                *slash = '/';
                node = fs_check_mountpoint(parent);
                if (node)
                {
                    fs_add_inode_cache(node, parent);
                }
                kfree(parent);
                return node;
            }
            p = node;
            tmp = slash + 1;
            *slash = '/';
            slash = strchr(tmp, '/');
        }
        node = fs_check_mountpoint(parent);
        if (!node)
        {
            node = fs_get_dirent_node(p, tmp);
            if (node)
            {
                fs_add_inode_cache(node, path);
            }
        }
        else
        {
            fs_add_inode_cache(node, parent);
        }
        vfs_free_inode(p);
        kfree(parent);
        return node;
    }
    while (0);
}

static unsigned fs_get_free_fd(INODE node, char* path)
{
    task_struct* cur = CURRENT_TASK();
    int i = 0;

    sema_wait(&cur->fd_lock);

    for (i = 0; i < MAX_FD; i++)
    {
        if (cur->fds[i].flag == 0)
        {
            cur->fds[i].file = node;
            cur->fds[i].file_off = 0;
            cur->fds[i].flag |= fd_flag_used;
            cur->fds[i].path = strdup(path);
            break;
        }
    }

    sema_trigger(&cur->fd_lock);

    return i;

}

static unsigned fs_dup_to_free_fd(unsigned fd)
{
    task_struct* cur = CURRENT_TASK();
    int i = 0;

    if (cur->fds[fd].flag == 0)
    {
        return MAX_FD;
    }


    sema_wait(&cur->fd_lock);

    for (i = 0; i < MAX_FD; i++)
    {
        if (cur->fds[i].flag != 0)
        {
            continue;
        }

        cur->fds[i] = cur->fds[fd];
        cur->fds[i].path = strdup(cur->fds[fd].path);
        vfs_refrence(cur->fds[fd].file);
        break;
    }

    sema_trigger(&cur->fd_lock);

    return i;
}

static INODE fs_get_fd(unsigned fd, char* path)
{
    task_struct* cur = CURRENT_TASK();
    INODE node;

    if (fd >= MAX_FD)
    {
        return 0;
    }


    sema_wait(&cur->fd_lock);
    if (cur->fds[fd].flag & fd_flag_isdir)
    {
        node = 0;
    }
    else
    {
        node = cur->fds[fd].file;
        if (path)
        {
            strcpy(path, cur->fds[fd].path);
        }
    }
    sema_trigger(&cur->fd_lock);

    return node;
}

static unsigned fs_dir_get_free_fd(DIR node, char* path)
{
    task_struct* cur = CURRENT_TASK();
    int i = 0;

    sema_wait(&cur->fd_lock);

    for (i = 0; i < MAX_FD; i++)
    {
        if (cur->fds[i].flag == 0)
        {
            cur->fds[i].dir = node;
            cur->fds[i].file_off = 0;
            cur->fds[i].flag |= fd_flag_used;
            cur->fds[i].flag |= fd_flag_isdir;
            cur->fds[i].path = strdup(path);
            break;
        }
    }

    sema_trigger(&cur->fd_lock);

    return i;
}

static DIR fs_dir_get_fd(unsigned fd)
{
    task_struct* cur = CURRENT_TASK();
    DIR node;

    if (fd >= MAX_FD)
    {
        return 0;
    }


    sema_wait(&cur->fd_lock);
    if (!(cur->fds[fd].flag & fd_flag_isdir))
    {
        node = 0;
    }
    else
    {
        node = cur->fds[fd].dir;
    }
    sema_trigger(&cur->fd_lock);

    return node;
}

static void* fs_clear_fd(unsigned fd, int* isdir)
{
    task_struct* cur = CURRENT_TASK();
    INODE node;

    if (fd >= MAX_FD)
    {
        return 0;
    }

    sema_wait(&cur->fd_lock);


    *isdir = (cur->fds[fd].flag & fd_flag_isdir);
    node = cur->fds[fd].file;
    cur->fds[fd].file = 0;
    cur->fds[fd].file_off = 0;
    cur->fds[fd].flag = 0;
    kfree(cur->fds[fd].path);
    cur->fds[fd].path = 0;
    sema_trigger(&cur->fd_lock);

    return node;

}

void fs_flush(char* filesys)
{
    struct filesys_type* type = mount_lookup("/");
}

INODE fs_open_log()
{
    INODE ret = fs_lookup_inode("/krn.log");
    INODE root = 0;
    DIR d = 0;
    if (ret == 0)
    {
        root = fs_lookup_inode("/");
        d = vfs_open_dir(root);
        vfs_free_inode(root);
        vfs_add_dir_entry(d, S_IRWXU | S_IRWXG | S_IRWXO, "krn.log");
        vfs_close_dir(d);
        ret = fs_lookup_inode("/krn.log");
    }

    return ret;
}