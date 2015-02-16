#ifndef _CACHE_H_
#define _CACHE_H_

#include <lib/rbtree.h>
extern hash_table* inode_cache;

void file_cache_init();

void* file_cache_find(char* path);

int file_cache_read(void* cachefd, unsigned off, void* buf, unsigned len);

#endif
