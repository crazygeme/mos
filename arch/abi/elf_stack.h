/* Initial stack construction for the selected ELF wire format. */
/* Symbolic values for the entries in the auxiliary table
   put on the initial stack */
#define AT_NULL 0 /* end of vector */
#define AT_IGNORE 1 /* entry should be ignored */
#define AT_EXECFD 2 /* file descriptor of program */
#define AT_PHDR 3 /* program headers for program */
#define AT_PHENT 4 /* size of program header entry */
#define AT_PHNUM 5 /* number of program headers */
#define AT_PAGESZ 6 /* system page size */
#define AT_BASE 7 /* base address of interpreter */
#define AT_FLAGS 8 /* flags */
#define AT_ENTRY 9 /* entry point of program */
#define AT_NOTELF 10 /* program is not ELF */
#define AT_UID 11 /* real uid */
#define AT_EUID 12 /* effective uid */
#define AT_GID 13 /* real gid */
#define AT_EGID 14 /* effective gid */
#define AT_PLATFORM 15 /* string identifying CPU for optimizations */
#define AT_HWCAP 16 /* arch dependent hints at CPU capabilities */
#define AT_CLKTCK 17 /* Frequency of times() */
#define AT_SECURE 23 /* secure dynamic loader execution */
#define AT_RANDOM 25 /* address of 16 random bytes */
#define AT_SYSINFO 32 /* address of kernel fast-syscall entry (vsyscall) */

static vaddr_t ELF_STACK(char *file, int argc, char **argv, int envc,
			 char **envp, vaddr_t top, mos_binfmt *exec)
{
	const unsigned width = sizeof(ELF_WORD);
	const char *platform = ELF_PLATFORM_NAME;
	vaddr_t sp = top;
	vaddr_t *av = kmalloc((argc + envc) * sizeof(vaddr_t));
	if (!av && argc + envc)
		do_exit(SIGKILL);
	for (int i = envc - 1; i >= 0; i--) {
		sp -= strlen(envp[i]) + 1;
		strcpy((char *)sp, envp[i]);
		av[argc + i] = sp;
	}
	for (int i = argc - 1; i >= 0; i--) {
		sp -= strlen(argv[i]) + 1;
		strcpy((char *)sp, argv[i]);
		av[i] = sp;
	}
	sp -= strlen(file) + 1;
	strcpy((char *)sp, file);
	vaddr_t filename = sp;
	sp -= strlen(platform) + 1;
	strcpy((char *)sp, platform);
	vaddr_t plat = sp;
	sp -= 16;
	vaddr_t random = sp;
	srand((unsigned)time_now_us());
	for (unsigned i = 0; i < 16; i++)
		((unsigned char *)sp)[i] = rand();
	uintptr_t aux[][2] = { { AT_PHDR, exec->elf_load_addr },
			       { AT_PHENT, exec->e_phent },
			       { AT_PHNUM, exec->e_phnum },
			       { AT_PAGESZ, PAGE_SIZE },
			       { AT_BASE, exec->interp_bias },
			       { AT_FLAGS, 0 },
			       { AT_ENTRY, exec->e_entry },
			       { AT_UID, current->credentials->uid },
			       { AT_EUID, current->credentials->euid },
			       { AT_GID, current->credentials->gid },
			       { AT_EGID, current->credentials->egid },
			       { AT_PLATFORM, plat },
			       { AT_HWCAP, ELF_CPU_HWCAP },
			       { AT_CLKTCK, 100 },
			       { ELF_SYSINFO_TYPE, ELF_SYSINFO_ENTRY },
			       { AT_RANDOM, random },
			       { 31, filename },
			       { AT_SECURE,
				 current->credentials->uid !=
						 current->credentials->euid ||
					 current->credentials->gid !=
						 current->credentials->egid },
			       { AT_NULL, 0 } };
	unsigned words =
		1 + argc + 1 + envc + 1 + sizeof(aux) / sizeof(uintptr_t);
	sp = (sp - words * width) & ~(vaddr_t)15;
	vaddr_t cursor = sp;
#define PUSH_WORD(value)                                  \
	do {                                              \
		*(ELF_WORD *)cursor = (uintptr_t)(value); \
		cursor += width;                          \
	} while (0)
	PUSH_WORD(argc);
	for (int i = 0; i < argc; i++)
		PUSH_WORD(av[i]);
	PUSH_WORD(0);
	for (int i = 0; i < envc; i++)
		PUSH_WORD(av[argc + i]);
	PUSH_WORD(0);
	for (unsigned i = 0; i < sizeof(aux) / sizeof(aux[0]); i++) {
		PUSH_WORD(aux[i][0]);
		PUSH_WORD(aux[i][1]);
	}
#undef PUSH_WORD
	kfree(av);
	return sp;
}
