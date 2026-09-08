#ifndef _MM_CACHE_H
#define _MM_CACHE_H

#include <arch/types.h>
#include <fs/fs.h>

void mm_cache_init(void);

paddr_t mm_anon_shared_find(unsigned anon_id, unsigned offset);
void mm_anon_shared_add(unsigned anon_id, unsigned offset, paddr_t phy);
void mm_anon_shared_get(unsigned anon_id);
void mm_anon_shared_put(unsigned anon_id);

paddr_t mm_file_shared_find(file *f, unsigned offset);
void mm_file_shared_add(file *f, unsigned offset, paddr_t phy);

#endif
