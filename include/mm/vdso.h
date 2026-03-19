#ifndef _MM_VDSO_H
#define _MM_VDSO_H

void mm_vdso_map();

int mm_vdso_region(int phy);

unsigned mm_vdso_translate(unsigned kernel_code);

#endif