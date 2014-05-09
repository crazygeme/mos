#ifndef _NAMESPACE_H_
#define _NAMESPACE_H_
#include <fs/vfs.h>


struct directory{
	DIR vfs_dir;
	INODE vfs_node;
};

unsigned fs_open(char* path);

void fs_close(unsigned int fd);

unsigned fs_read(unsigned fd, unsigned offset, void* buf, unsigned len);

unsigned fs_write(unsigned fd, unsigned offset, void* buf, unsigned len);

int fs_create(char* path);

int fs_delete(char* path);

int fs_rename(char* path, char* new);

int fs_stat(char* path, struct stat* s);

struct directory* fs_opendir(char* path);

int fs_readdir(struct directory* dir, char* name, unsigned* mode);

void fs_closedir(struct directory* dir);

#endif
