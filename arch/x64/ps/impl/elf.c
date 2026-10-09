#include <elf/format.h>
#include <ps/ps.h>
#include <int/int.h>
#include <lib/klib.h>
#include <mm/mm.h>
#include <mm/vdso.h>
#include <device/time.h>
#include <errno.h>
#define ELF_HEADER Elf64_Ehdr
#define ELF_PHDR Elf64_Phdr
#define ELF_MACHINE EM_X86_64
#include <arch/abi/elf_reader.h>
#define ELF_WORD uint64_t
#define ELF_STACK setup_stack
#define ELF_PLATFORM_NAME "x86_64"
#define ELF_CPU_HWCAP 0
#define ELF_SYSINFO_TYPE AT_IGNORE
#define ELF_SYSINFO_ENTRY 0
#include <arch/abi/elf_stack.h>
static void activate(task_struct *task)
{
	task->execution->robust_list_head = NULL;
	task->execution->robust_list_reader = NULL;
	task->execution->robust_list_size = 24;
	task->execution->arch.cs = USER64_CODE_SELECTOR;
}
const struct elf_format elf_amd64_format = {
	.elf_class = ELFCLASS64,
	.task_size = MOS_NATIVE_TASK_SIZE,
	.mmap_base = 0x100000000ULL,
	.brk_limit = MOS_COMPAT_TASK_SIZE,
	.exec_syscall = 59,
	.decode_header = decode_header,
	.read_phdrs = read_phdrs,
	.setup_stack = setup_stack,
	.activate = activate,
};
