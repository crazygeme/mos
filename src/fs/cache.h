#ifndef _FS_CACHE_H
#define _FS_CACHE_H

#include <fs/fs.h>

/* Return a retained physical page; each successful get requires one put. */
paddr_t fs_page_cache_get(file *fp, uint64_t offset, int *cache_hit);
void fs_page_cache_put(paddr_t phy);
void fs_page_cache_invalidate(file *fp);
unsigned fs_page_cache_reclaim(unsigned target_pages);
ssize_t fs_page_cache_read(file *fp, void *buf, size_t size, loff_t *pos);

#endif
