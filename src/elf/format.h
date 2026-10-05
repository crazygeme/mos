#ifndef MOS_ELF_FORMAT_H
#define MOS_ELF_FORMAT_H
#include <elf/elf.h>
struct _task_struct;
struct elf_format {
	unsigned elf_class;
	vaddr_t task_size, mmap_base, brk_limit;
	unsigned exec_syscall;
	int (*read_header)(file *, Elf64_Ehdr *);
	int (*read_phdr)(file *, unsigned, Elf64_Phdr *);
	vaddr_t (*setup_stack)(char *, int, char **, int, char **, vaddr_t,
			       mos_binfmt *);
	void (*activate)(struct _task_struct *);
};
extern const struct elf_format elf_i386_format;
extern const struct elf_format elf_amd64_format;
const struct elf_format *arch_elf_format(unsigned elf_class);
const struct elf_format *elf_image_format(const elf_image *);
int elf_read(file *, unsigned, void *, int);
#endif
