#ifndef _NAMESPACE_H_
#define _NAMESPACE_H_
#include <fs/vfs.h>



unsigned fs_open(char* path);

void fs_close(unsigned int fd);

unsigned fs_read(unsigned fd, unsigned offset, void* buf, unsigned len);

unsigned fs_write(unsigned fd, unsigned offset, void* buf, unsigned len);

int fs_create(char* path, unsigned mode);

int fs_delete(char* path);

int fs_rename(char* path, char* new);

int fs_stat(char* path, struct stat* s);

DIR fs_opendir(char* path);

int fs_readdir(DIR dir, char* name, unsigned* mode);

void fs_closedir(DIR dir);

void fs_flush(char* filesys);

int sys_readdir(unsigned fd, struct dirent* entry);


#endif
