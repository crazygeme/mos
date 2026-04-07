#ifndef _MM_CACHE_H
#define _MM_CACHE_H

#include <fs/fs.h>

void mm_cache_init(void);

unsigned mm_anon_shared_find(unsigned anon_id, unsigned offset);
void mm_anon_shared_add(unsigned anon_id, unsigned offset, unsigned phy);
void mm_anon_shared_get(unsigned anon_id);
void mm_anon_shared_put(unsigned anon_id);

unsigned mm_file_shared_find(file *f, unsigned offset);
void mm_file_shared_add(file *f, unsigned offset, unsigned phy);

#endif
