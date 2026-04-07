#ifndef _FS_CACHE_H
#define _FS_CACHE_H

#include <fs/fs.h>

unsigned fs_page_cache_get(inode *inode, unsigned offset, int *cache_hit);
void fs_page_cache_invalidate(inode *inode);
ssize_t fs_page_cache_read(file *fp, void *buf, size_t size, loff_t *pos);

#endif
