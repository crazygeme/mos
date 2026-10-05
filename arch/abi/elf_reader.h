static int read_header(file *fp, Elf64_Ehdr *output)
{
	ELF_HEADER input;
	if (elf_read(fp, 0, &input, sizeof(input)) != sizeof(input) ||
	    input.e_machine != ELF_MACHINE || input.e_ehsize != sizeof(input) ||
	    input.e_phentsize != sizeof(ELF_PHDR))
		return -ENOEXEC;
	memset(output, 0, sizeof(*output));
	memcpy(output->e_ident, input.e_ident, EI_NIDENT);
#define COPY(field) output->field = input.field
	COPY(e_type);
	COPY(e_machine);
	COPY(e_version);
	COPY(e_entry);
	COPY(e_phoff);
	COPY(e_ehsize);
	COPY(e_phentsize);
	COPY(e_phnum);
#undef COPY
	return 0;
}
static int read_phdr(file *fp, unsigned offset, Elf64_Phdr *output)
{
	ELF_PHDR input;
	if (elf_read(fp, offset, &input, sizeof(input)) != sizeof(input))
		return -ENOEXEC;
#define COPY(field) output->field = input.field
	COPY(p_type);
	COPY(p_flags);
	COPY(p_offset);
	COPY(p_vaddr);
	COPY(p_paddr);
	COPY(p_filesz);
	COPY(p_memsz);
	COPY(p_align);
#undef COPY
	return 0;
}
