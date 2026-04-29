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

static unsigned mm_vdso_size(void)
{
	unsigned vdso_start = (unsigned)&__vdso_start;
	unsigned vdso_end = (unsigned)&__vdso_end;

	if (vdso_end <= vdso_start)
		return 0;
	return vdso_end - vdso_start;
}

static unsigned mm_vdso_base(void)
{
	unsigned size = mm_vdso_size();
	unsigned page_count;

	if (size == 0)
		return 0;

	page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;

	/*
	 * Keep the VDSO just below the mmap/stack boundary, well away from
	 * legacy low-memory layouts such as XFree86 int10's 0x00000-0x9ffff
	 * shared-memory attach. The non-NPTL branch has no live VDSO code, so
	 * placing it in low memory created an NPTL-only regression.
	 */
	return USER_ZONE_END - page_count * PAGE_SIZE;
}

void mm_vdso_map()
{
	unsigned vdso_start = (unsigned)&__vdso_start;
	unsigned size = mm_vdso_size();
	unsigned base = mm_vdso_base();
	unsigned page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
	unsigned i;

	if (size == 0)
		return;

	for (i = 0; i < page_count; i++) {
		unsigned vir = base + i * PAGE_SIZE;
		unsigned phy = VIRT_TO_PHY(vdso_start + i * PAGE_SIZE);
		mm_map_page(vir, phy, PAGE_ENTRY_USER_CODE);
	}
	RELOAD_CR3();

	/* Register a VMA so fork's copy_page_range includes these pages. */
	{
		task_struct *cur = CURRENT_TASK();
		vm_add_map(cur->user->vm, base, base + page_count * PAGE_SIZE,
			   PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS,
			   NULL, 0, 0);
	}
}

unsigned mm_vdso_fastcall_entry(void)
{
	unsigned vdso_start = (unsigned)&__vdso_start;
	unsigned base = mm_vdso_base();

	if (base == 0)
		return 0;

	return base + ((unsigned)&__kernel_vsyscall - vdso_start);
}

int mm_vdso_region(int phy)
{
	unsigned vdso_start = VIRT_TO_PHY(&__vdso_start);
	unsigned vdso_end = VIRT_TO_PHY(&__vdso_end);

	return phy >= vdso_start && phy < vdso_end;
}
