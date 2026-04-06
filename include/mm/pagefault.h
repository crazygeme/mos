#ifndef _MM_PAGEFAULT_H
#define _MM_PAGEFAULT_H

void pf_init();

void pf_enable();

void pf_disable();

void pf_invalidate_file_cache(unsigned ino);

#endif
