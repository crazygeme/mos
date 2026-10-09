#include <elf/format.h>
#include <ps/ps.h>
#include <int/int.h>
#include <lib/klib.h>
#include <mm/mm.h>
#include <mm/vdso.h>
#include <device/time.h>
#include <errno.h>
#define ELF_HEADER Elf32_Ehdr
#define ELF_PHDR Elf32_Phdr
#define ELF_MACHINE EM_386
#include <arch/abi/elf_reader.h>
#define ELF_WORD uint32_t
#define ELF_STACK setup_stack
#define ELF_PLATFORM_NAME "i686"
#define ELF_CPU_HWCAP (0x0183FBFFU | (1U << 29))
#define ELF_SYSINFO_TYPE AT_SYSINFO
#define ELF_SYSINFO_ENTRY mm_vdso_fastcall_entry()
#include <arch/abi/elf_stack.h>
static void activate(task_struct *task)
{
	task->execution->robust_list_head = NULL;
	task->execution->robust_list_reader = NULL;
	task->execution->robust_list_size = 12;
	task->execution->arch.cs = USER_CODE_SELECTOR;
	mm_vdso_map();
}
const struct elf_format elf_i386_format = {
	.elf_class = ELFCLASS32,
	.task_size = MOS_COMPAT_TASK_SIZE,
	.mmap_base = USER_HEAP_END,
	.brk_limit = USER_HEAP_END,
	.exec_syscall = 11,
	.decode_header = decode_header,
	.read_phdrs = read_phdrs,
	.setup_stack = setup_stack,
	.activate = activate,
};
