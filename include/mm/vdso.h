#ifndef _MM_VDSO_H
#define _MM_VDSO_H

#define VDSO_MM_REGION 0x10000

void mm_vdso_map();
unsigned mm_vdso_fastcall_entry(void);

int mm_vdso_region(int phy);

#endif
