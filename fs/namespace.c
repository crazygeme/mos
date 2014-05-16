#include <fs/namespace.h>
#ifdef WIN32
#include <windows.h>
#include <osdep.h>
#else
#include <ps/ps.h>
#include <ps/lock.h>
#include <config.h>
#include <lib/klib.h>
#include <fs/mount.h>
#endif

static INODE fs_lookup_inode(char* path);
static unsigned fs_get_free_fd(INODE node);
static INODE fs_get_fd(unsigned id);
static INODE fs_clear_fd(unsigned fd);

unsigned fs_open(char* path)
{
    INODE node = fs_lookup_inode( path);
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
    return ret;
}

int fs_create(char* path, unsigned mode)
{  
	DIR dir;
	char dir_name[260] = { 0 };
	char* t;

	strcpy(dir_name, path);
	t = strrchr(dir_name, '/');
	if (!t)
		return 0;

	*t = '\0';
	t++;

	if (*dir_name == '\0')
		dir = fs_opendir("/");
	else
		dir = fs_opendir(dir_name);

	if (!dir)
		return 0;

	vfs_add_dir_entry(dir, mode, t);


	fs_closedir(dir);
    return 1;
}

int fs_delete(char* path)
{  

    return 0;
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
	
	node = fs_lookup_inode(path);
    if (!node) {
        return -1;
    }

    ret = vfs_copy_stat(node,s);
    vfs_free_inode(node);
    return ret;
}

DIR fs_opendir(char* path)
{
    INODE node = fs_lookup_inode(path);
    DIR ret;
    if (!node) {
        return 0;
    }
	if (!S_ISDIR(vfs_get_mode(node))){
		return 0;
	}

    ret = vfs_open_dir(node);
	vfs_free_inode(node);
	return ret;
}

int fs_readdir(DIR dir, char* name, unsigned* mode)
{
    INODE node = 0;

    if (!name || !mode) {
        return 0;
    }

    if (!dir) {
        *name = '\0';
        *mode = 0;
        return 0;
    }

    node = vfs_read_dir(dir);
	if (!node){
		*name = '\0';
		*mode = 0;
		return 0;
	}
	strcpy(name, vfs_get_name(node));
    *mode = vfs_get_mode(node);
    vfs_free_inode(node);
	return 1;
}

void fs_closedir(DIR dir)
{
    if (!dir) {
        return;
    }

    vfs_close_dir(dir);
}


static INODE fs_get_dirent_node(INODE node, char* name)
{
	DIR dir = 0;
	INODE entry = 0;

	if (!S_ISDIR(vfs_get_mode(node)))
	  return 0;
	
	dir = vfs_open_dir(node);
	entry = vfs_read_dir(dir);
	while(entry){
		if ( !strcmp(name, vfs_get_name(entry)) )
		  break;
		
		vfs_free_inode(entry);
		entry = vfs_read_dir(dir);
	}

	vfs_close_dir(dir);
	return entry;
}

static INODE fs_lookup_inode(char* path)
{
    struct filesys_type* type = 0;
	INODE root;
	char parent[256];
	char* tmp;
	if (!path || !*path)
	  return 0;
	
	// find root file system
	type = mount_lookup("/");
	if (!type)
	  return 0;
	
	root = vfs_get_root(type);
	if (!strcmp(path, "/")){
		return root;
	}
	
	strcpy(parent, path);
	tmp = parent;

	// FIXME
	// check mount point every time takes too much effert
	// linux will cache dirent with mount point info attached
	// we ignore different mount point now, fix it later
	//
	do{
		INODE p;
		INODE node; 
		char* slash = 0;

		tmp++;
		p = root;

		slash = strchr(tmp, '/');
		while (slash){
			*slash = '\0';
			node = fs_get_dirent_node(p, tmp);
			vfs_free_inode(p);
			if (!node)
			  return 0;
			p = node;
			tmp = slash+1;
			*slash = '/';
			slash = strchr(tmp, '/');
		}

		node = fs_get_dirent_node(p, tmp);
		vfs_free_inode(p);
		return node;
	}while(0);
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

#ifdef TEST_NS
static void list_dir(char* name, int depth)
{
	char file[32];
	unsigned mode;
	DIR dir = fs_opendir(name);
	while (fs_readdir(dir, file, &mode)){
		int i = 0;
		if (!strcmp(file, ".") || !strcmp(file, "..") )
		  continue;

		for (i = 0; i < depth; i++)
		  printf("\t");
		printf("%s\n", file);
		if (S_ISDIR(mode)){
			char path[32];
			strcpy(path, name);
			if ( strcmp(name, "/") )
				strcat(path, "/");
			strcat(path, file);
			list_dir(path, depth+1);
		}
	}
	fs_closedir(dir);
}

static void test_stat(char* path)
{
	struct stat s;
	fs_stat(path, &s);
	printk("%s: is dir %d, size %d\n", path, S_ISDIR(s.st_mode), s.st_size);
}

void test_ns()
{
	unsigned fd;
	char text[32];
	klogquota();
	printk("test_ns\n");

	fd = fs_open("/readme.txt");
	printk("fs_open %d\n", fd);
	fs_read(fd, 0, text, 31);
	printk("file context: %s\n", text);
	fs_close(fd);

	klogquota();
	list_dir("/", 1);

	test_stat("/bin/run");
	klogquota();
}
#endif
