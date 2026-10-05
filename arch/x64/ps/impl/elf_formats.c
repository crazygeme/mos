#include <elf/format.h>
const struct elf_format *arch_elf_format(unsigned elf_class)
{
	static const struct elf_format *const formats[ELFCLASSNUM] = {
		[ELFCLASS32] = &elf_i386_format,
		[ELFCLASS64] = &elf_amd64_format,
	};
	return elf_class < ELFCLASSNUM ? formats[elf_class] : 0;
}
