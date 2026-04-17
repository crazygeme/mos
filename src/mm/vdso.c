#include <config.h>
#include <mm/vdso.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <mm/phymm.h>
#include <ps/ps.h>
#include <macro.h>

#define _VDSO __attribute__((used, section(".vdso")))
extern const unsigned __vdso_start;
extern const unsigned __vdso_end;

_VDSO NAKED void __kernel_vsyscall(void)
{
	asm volatile("int $0x80\n\t"
		     "ret");
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
		mm_map_page(vir, phy, PAGE_ENTRY_USER_CODE);
	}
	RELOAD_CR3();

	/* Register a VMA so fork's copy_page_range includes these pages. */
	{
		task_struct *cur = CURRENT_TASK();
		vm_add_map(cur->user->vm, VDSO_MM_REGION,
			   VDSO_MM_REGION + page_count * PAGE_SIZE,
			   PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS,
			   NULL, 0, 0);
	}
}

unsigned mm_vdso_fastcall_entry(void)
{
	unsigned vdso_start = (unsigned)&__vdso_start;
	unsigned vdso_end = (unsigned)&__vdso_end;

	if (vdso_end <= vdso_start)
		return 0;

	return VDSO_MM_REGION +
	       ((unsigned)&__kernel_vsyscall - (unsigned)&__vdso_start);
}

int mm_vdso_region(int phy)
{
	unsigned vdso_start = (unsigned)&__vdso_start - KERNEL_OFFSET;
	unsigned vdso_end = (unsigned)&__vdso_end - KERNEL_OFFSET;

	return phy >= vdso_start && phy < vdso_end;
}
