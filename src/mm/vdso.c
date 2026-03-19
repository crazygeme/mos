#include <mm/vdso.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <macro.h>

#define _VDSO __attribute__((used, section(".vdso")))
#define VDSO_MM_REGION 0x10000
extern const unsigned __vdso_start;
extern const unsigned __vdso_end;

_VDSO void test_sig_handler(int signo)
{
	asm volatile("mov $102, %eax");
	asm volatile("mov %0, %%ebx" : : "m"(signo));
	asm volatile("int $0x80");
	return;
}

_VDSO NAKED void vdso_sigreturn_tramp()
{
	asm volatile("mov $119, %eax");
	asm volatile("int $0x80");
	NOP();
}

void mm_vdso_map()
{
	unsigned vdso_start = (unsigned)&__vdso_start;
	unsigned vdso_end = (unsigned)&__vdso_end;
	unsigned size = vdso_end - vdso_start;
	unsigned page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	unsigned i;

	if (size == 0)
		return;

	for (i = 0; i < page_count; i++) {
		unsigned vir = VDSO_MM_REGION + i * PAGE_SIZE;
		unsigned phy = vdso_start + i * PAGE_SIZE - KERNEL_OFFSET;
		mm_add_dynamic_map(vir, phy, PAGE_ENTRY_USER_CODE);
	}
	RELOAD_CR3();
}

int mm_vdso_region(int phy)
{
	unsigned vdso_start = (unsigned)&__vdso_start - KERNEL_OFFSET;
	unsigned vdso_end = (unsigned)&__vdso_end - KERNEL_OFFSET;

	return phy >= vdso_start && phy < vdso_end;
}

unsigned mm_vdso_translate(unsigned kernel_code)
{
	return VDSO_MM_REGION + (kernel_code - (unsigned)&__vdso_start);
}