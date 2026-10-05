#include <elf/exec.h>
#include <device/time.h>
#include <elf/elf.h>
#include <dev/blockdev.h>
#include <ps/ps.h>
#include <int/int.h>
#include <dev/tty.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <mm/vdso.h>
#include <fs/fcntl.h>
#include <fs/fs.h>
#include <fs/mount.h>
#include <lib/klib.h>
#include <config.h>
#include <unistd.h>
#include <macro.h>
#include <mm/mmu.h>
#include <ps/task.h>
#include <errno.h>

/*
 * cleanup - tear down the current process's user-space state before exec
 *
 * Called at the start of execve, before the new image is loaded:
 *   1. If this task was created by vfork(), signal the parent that the
 *      address space is about to be replaced (wakes the blocked parent).
 *      For vfork: allocate a fresh page directory for the child (copying
 *      only kernel mappings) and switch CR3 — the parent's page directory
 *      and vm are left untouched.
 *   2. Close all file descriptors that have O_CLOEXEC set — POSIX requires
 *      these to be closed across exec.
 *   3. Unmap all user virtual-memory regions (code, data, stack, heap).
 *   4. Reset the heap pointer and replace the VM descriptor with a fresh one
 *      so the new image starts with a clean address space.
 */
static void cleanup()
{
	task_struct *cur = CURRENT_TASK();
	mm_struct *old_mm = cur->user->vm;
	mm_struct *new_mm;
	intr_frame *frame = (intr_frame *)((char *)cur + KERNEL_TASK_BYTES -
					   sizeof(*frame));
	vaddr_t new_pd;
	int i = 0;

	if (cur->fork_flag & FORK_FLAG_VFORK) {
		cond_notify(&cur->vfork_event);
		cur->fork_flag &= ~FORK_FLAG_VFORK;
	}

	/* Build and activate a completely new address space before dropping the
	 * old one.  This is the exec-time equivalent of Linux's exec_mmap(). */
	new_mm = vm_create();
	if (!new_mm)
		DIE();
	new_pd = vm_alloc(1);
	if (!new_pd)
		DIE();
	mm_init_process_page_dir(new_pd);
	vm_set_page_dir(new_mm, new_pd);
	cur->user->vm = new_mm;
	arch_mm_activate(VIRT_TO_PHY(new_pd));
	vm_put(old_mm);

	/* Close all O_CLOEXEC file descriptors. */
	for (i = 0; i < MAX_FD; i++) {
		if (cur->fds[i] && fd_bitmap_test(cur->fd_cloexec, i)) {
			fs_close(i);
		}
	}

	/*
	 * Signal state is handled by execve() itself using Linux semantics:
	 * caught handlers reset to SIG_DFL, ignored handlers stay SIG_IGN,
	 * pending signals and the signal mask are preserved, and the altstack
	 * is disabled.
	 * Do not wipe the whole signal context here or we'd lose SIG_IGN.
	 */

	/*
	 * execve replaces the user image, so any thread-local descriptor state
	 * from the old program must not leak into the new one. Otherwise the new
	 * image can hit set_thread_area(entry=-1) with all three slots already
	 * appearing occupied before it has installed its own TLS.
	 */
	arch_task_reset_tls(cur, frame);
}

/*
 * count_strv - count entries in a NULL-terminated string array
 *
 * Stops at a NULL pointer or an empty string, matching execve ABI convention.
 */
static unsigned count_strv(char **v)
{
	unsigned n = 0;
	if (v)
		while (v[n])
			n++;
	return n;
}

/*
 * dup_strv - deep-copy @n strings from @v into kernel heap memory
 *
 * User-supplied pointers become invalid after cleanup() tears down the
 * address space, so every string is strdup()'d here.  Returns a
 * heap-allocated array of @n strings, or NULL if @n is zero.
 * Free with free_v(). The pointer array and all strings share one allocation.
 */
static char **dup_strv(char **v, unsigned n)
{
	unsigned i, bytes = 0;
	char **ret;
	if (!n)
		return NULL;
	for (i = 0; i < n; i++)
		bytes += strlen(v[i]) + 1;
	ret = kmalloc(n * sizeof(char *) + bytes);
	if (!ret)
		return NULL;
	char *strings = (char *)(ret + n);
	for (i = 0; i < n; i++) {
		unsigned len = strlen(v[i]) + 1;
		ret[i] = strings;
		memcpy(strings, v[i], len);
		strings += len;
	}
	return ret;
}

/*
 * parse_shebang - parse a "#!" interpreter directive in-place
 *
 * Terminates @line at the first newline or carriage-return, then extracts
 * two tokens from "#!/path/to/interp[ arg...]":
 *   *@interp     — the interpreter path
 *   *@interp_arg — everything after the first whitespace gap, as one string
 *                  (NULL if absent)
 *
 * The entire remainder after the interpreter is treated as a single argument,
 * matching Linux kernel behaviour (unlike FreeBSD which splits it).  So
 * "#!/usr/bin/python -u -O" yields interp_arg = "-u -O", not two tokens.
 *
 * Caller must copy the results (e.g. with strdup) before freeing @line.
 */
static void parse_shebang(char *line, const char **interp,
			  const char **interp_arg)
{
	char *p, *lf, *cr;

	lf = strchr(line, '\n');
	if (lf)
		*lf = '\0';
	cr = strchr(line, '\r');
	if (cr)
		*cr = '\0';

	p = line + 2;
	while (*p == ' ' || *p == '\t')
		p++;
	*interp = p;

	while (*p && *p != ' ' && *p != '\t')
		p++;

	*interp_arg = NULL;
	if (*p) {
		*p++ = '\0';
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p)
			*interp_arg = p;
	}
}

/*
 * free_v - free a deep-copied argv/envp array previously made by
 *             save_argv() or save_envp()
 *
 * Frees each individual string and then the pointer array itself.
 * Safe to call with a NULL @v (no-op).
 */
static void free_v(char **v, unsigned size)
{
	(void)size;
	if (!v) {
		return;
	}
	kfree(v);
}

/* TODO; fix this */
/* bit 29: HWCAP_I386_TLS — kernel supports set_thread_area; causes ld-linux
 * to search /lib/tls/ for NPTL-optimised libraries (e.g. /lib/tls/libc.so.6) */
#define ELF_HWCAP (0x0183FBFF | (1 << 29))

/* This yields a string that ld.so will use to load implementation
specific libraries for optimization.  This is more specific in
intent than poking at uname or /proc/cpuinfo.

For the moment, we have only optimizations for the Intel generations,
but that could change... */

#define ELF_PLATFORM "i686"
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

/*
 * setup_user_stack - build the initial user stack layout required by the ELF ABI
 *
 * Constructs the stack that the dynamic linker (or a static binary) expects
 * when it receives control.  The layout, growing downward from @top, is:
 *
 *   [high address = top]
 *     <4-byte end sentinel (0)>
 *     <filename string>
 *     <envp strings>          ← each NUL-terminated
 *     <argv strings>          ← each NUL-terminated
 *     <ELF_PLATFORM string>   ("i686")
 *     <16 random bytes>      (AT_RANDOM)
 *     <16-byte alignment pad>
 *     <auxiliary vector>      ← AT_NULL terminator at the top of this block,
 *                               then AT_PLATFORM, AT_HWCAP/PAGESZ/CLKTCK,
 *                               and the 10 AT_* entries the linker needs
 *     <envp pointer array>    ← NULL-terminated array of pointers into strings
 *     <argv pointer array>    ← NULL-terminated array of pointers into strings
 *     <argc>                  ← pushed last (lowest address)
 *   [returned esp]
 *
 * NEW_AUX_ENT writes one (type, value) pair into the auxiliary vector.
 * The macro __put_user writes a single word at a given stack address.
 *
 * Returns the new stack pointer (esp) that should be given to the entry point.
 */
static void stack_word(vaddr_t address, uintptr_t value, unsigned width)
{
	if (width == 4)
		*(uint32_t *)address = value;
	else
		*(uint64_t *)address = value;
}

static vaddr_t setup_user_stack(char *file, int argc, char **argv, int envc,
				char **envp, vaddr_t top, mos_binfmt *exec)
{
	unsigned width = current->user->abi == MOS_ABI_I386 ? 4 : 8;
	const char *platform = width == 4 ? "i686" : "x86_64";
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
			       { AT_UID, current->user->uid },
			       { AT_EUID, current->user->euid },
			       { AT_GID, current->user->gid },
			       { AT_EGID, current->user->egid },
			       { AT_PLATFORM, plat },
			       { AT_HWCAP, width == 4 ? ELF_HWCAP : 0 },
			       { AT_CLKTCK, 100 },
			       { width == 4 ? AT_SYSINFO : AT_IGNORE,
				 width == 4 ? mm_vdso_fastcall_entry() : 0 },
			       { AT_RANDOM, random },
			       { 31, filename },
			       { AT_SECURE,
				 current->user->uid != current->user->euid ||
					 current->user->gid !=
						 current->user->egid },
			       { AT_NULL, 0 } };
	unsigned words =
		1 + argc + 1 + envc + 1 + sizeof(aux) / sizeof(uintptr_t);
	sp = (sp - words * width) & ~(vaddr_t)15;
	vaddr_t cursor = sp;
#define PUSH_WORD(value)                                       \
	do {                                                   \
		stack_word(cursor, (uintptr_t)(value), width); \
		cursor += width;                               \
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

static int execve_common(const char *f, char **argv, char **envp,
			 int owned_vectors)
{
	vaddr_t eip = 0;
	int i = 0;
	vaddr_t esp_top;
	char *file_name;
	unsigned argc = 0, envc = 0;
	char **s_argv = 0;
	char **s_envp = 0;
	char *tmp;
	mos_binfmt fmt = { 0 };
	task_struct *cur = CURRENT_TASK();
	struct stat s;
	file *fp;
	file *exec_fp = NULL;
	elf_image *image = NULL;
	unsigned exec_euid = cur->user->euid;
	unsigned exec_egid = cur->user->egid;
	int len = 64; /* max bytes to read for the first line of a script */
	char *firstline = NULL;
	if (!f) {
		return -ENOENT;
	}

	/* resolve path into full path */
	file_name = name_get();
	resolve_path(f, file_name);

	/* make script interp as first argument if file starts with #! */

	/* open file for state checking and first line reading */
	fp = fs_open_file(file_name, O_RDONLY, 0);
	if (!fp) {
		name_put(file_name);
		return -ENOENT;
	}

	/* stat via VFS inode ops */
	memset(&s, 0, sizeof(s));
	if (!fp->f_fop || !fp->f_fop->getattr ||
	    fp->f_fop->getattr(fp, &s) != 0) {
		fs_put_file(fp);
		name_put(file_name);
		return -ENOENT;
	}

	/* if file not executable, just return */
	if (!S_ISREG(s.st_mode) || fs_check_perm(&s, 1) || (s.st_size < 4)) {
		fs_put_file(fp);
		name_put(file_name);
		return -EPERM;
	}

	/* read first line via VFS file ops */
	len = len > (int)s.st_size ? (int)s.st_size : len;
	firstline = malloc(64);
	if (!fp->f_fop || !fp->f_fop->read) {
		free(firstline);
		fs_put_file(fp);
		name_put(file_name);
		return -ENOENT;
	} else {
		loff_t pos = 0;
		ssize_t n = fp->f_fop->read(fp, firstline, len, &pos);
		if (n < 0) {
			free(firstline);
			fs_put_file(fp);
			name_put(file_name);
			return -ENOENT;
		}
	}
	/*
	 * Determine binary type and build the final argc/argv/envp.
	 *
	 * ELF: file_name unchanged, argv/envp deep-copied as-is.
	 *
	 * #!: parse "#!/path/to/interp[ optional_arg]", update file_name
	 *     to the interpreter. Linux passes the script pathname, not the
	 *     caller's argv[0], as the first script argument:
	 *       [interp, optional_arg?, script_path, argv[1], ...]
	 *
	 * After this block: file_name is the executable to load; argc/s_argv
	 * and envc/s_envp are fully set and ready for setup_user_stack().
	 */
	if (firstline[0] == 0x7f && firstline[1] == 'E' &&
	    firstline[2] == 'L' && firstline[3] == 'F') {
		free(firstline);
		firstline = NULL;
		argc = count_strv(argv);
		envc = count_strv(envp);
		s_argv = dup_strv(argv, argc);
		s_envp = dup_strv(envp, envc);
		exec_fp = fp;
		if (!(fp->f_mount_flags & MS_NOSUID) &&
		    !cur->user->ptrace_tracer) {
			if (s.st_mode & S_ISUID)
				exec_euid = s.st_uid;
			if ((s.st_mode & (S_ISGID | S_IXGRP)) ==
			    (S_ISGID | S_IXGRP))
				exec_egid = s.st_gid;
		}
	} else if (firstline[0] == '#' && firstline[1] == '!') {
		const char *interp, *interp_arg;
		unsigned shebang_argc, user_argc, j, dst;

		parse_shebang(firstline, &interp, &interp_arg);

		user_argc = count_strv(argv);
		envc = count_strv(envp);

		shebang_argc = 1 + (interp_arg ? 1 : 0);
		argc = shebang_argc + 1 + (user_argc ? user_argc - 1 : 0);

		/* Build the interpreter argv in one arena allocation. */
		{
			char **src_argv = kmalloc(argc * sizeof(char *));
			if (!src_argv) {
				free(firstline);
				fs_put_file(fp);
				name_put(file_name);
				return -ENOMEM;
			}
			src_argv[0] = (char *)interp;
			dst = 1;
			if (interp_arg)
				src_argv[dst++] = (char *)interp_arg;
			src_argv[dst++] = file_name;
			for (j = 1; j < user_argc; j++)
				src_argv[dst++] = argv[j];
			s_argv = dup_strv(src_argv, argc);
			kfree(src_argv);
		}
		if (!s_argv) {
			free(firstline);
			fs_put_file(fp);
			name_put(file_name);
			return -ENOMEM;
		}

		strcpy(file_name, interp);

		free(firstline);
		firstline = NULL;
		fs_put_file(fp);
		s_envp = dup_strv(envp, envc);
	} else {
		free(firstline);
		fs_put_file(fp);
		name_put(file_name);
		return -ENOEXEC;
	}

	/* Reject invalid images before closing descriptors or replacing memory. */
	if (!exec_fp)
		exec_fp = fs_open_file(file_name, O_RDONLY, 0);
	{
		int ret = elf_prepare(exec_fp, &image);
		if (ret) {
			if (exec_fp)
				fs_put_file(exec_fp);
			free_v(s_argv, argc);
			free_v(s_envp, envc);
			name_put(file_name);
			return ret;
		}
	}

	/* exec owns a private table before applying FD_CLOEXEC. */
	if (ps_unshare_fds(cur) != 0) {
		elf_release(image);
		if (exec_fp)
			fs_put_file(exec_fp);
		free_v(s_argv, argc);
		free_v(s_envp, envc);
		name_put(file_name);
		return -ENOMEM;
	}

	/* save command line into task struct (bounded to one page) */
	{
		char *end = cur->user->command + PAGE_SIZE - 1;
		unsigned len;

		tmp = cur->user->command;
		len = strlen(file_name);
		if (len > (unsigned)(end - tmp))
			len = (unsigned)(end - tmp);
		memcpy(tmp, file_name, len);
		tmp[len] = '\0';
		tmp += len + 1;

		for (i = 1; i < (int)argc && tmp < end; i++) {
			len = strlen(s_argv[i]);
			if (len > (unsigned)(end - tmp))
				len = (unsigned)(end - tmp);
			memcpy(tmp, s_argv[i], len);
			tmp[len] = '\0';
			tmp += len + 1;
		}
		cur->user->cmd_len = tmp - cur->user->command;
	}

	/* save environment into task struct (bounded to one page) */
	{
		char *end = cur->user->environment + PAGE_SIZE - 1;
		unsigned len;

		tmp = cur->user->environment;
		for (i = 0; i < (int)envc && tmp < end; i++) {
			len = strlen(s_envp[i]);
			if (len > (unsigned)(end - tmp))
				len = (unsigned)(end - tmp);
			memcpy(tmp, s_envp[i], len);
			tmp[len] = '\0';
			tmp += len + 1;
		}
		cur->user->env_len = tmp - cur->user->environment;
	}

	/* log if needed */
	if (TEST_LOG(TEST_LOG_INFO))
		klog("execve(%s)\n", cur->user->command);

	/*
	 * Linux execve semantics for signal state:
	 * caught handlers reset to SIG_DFL, ignored handlers stay ignored,
	 * pending signals and the signal mask are preserved, and the alt signal
	 * stack is disabled.
	 */
	{
		int sig;

		for (sig = 1; sig < NSIG; sig++) {
			struct sigaction *sa = &cur->signal->sig_handlers[sig];

			if (sa->sa_handler != SIG_IGN)
				sa->sa_handler = SIG_DFL;
			sa->sa_flags = 0;
			sa->sa_restorer = NULL;
			sa->sa_mask = 0;
		}
		cur->signal->restore_sigmask = 0;
		cur->signal->saved_sigmask = 0;
		cur->signal->altstack.ss_sp = NULL;
		cur->signal->altstack.ss_size = 0;
		cur->signal->altstack.ss_flags = SS_DISABLE;
	}

	/*
	 * unmap all user vm, and close all fds if O_CLOEXEC set
	 * note that we will trigger vfork event if this task is
	 * created by vfork syscall
	 */
	if (owned_vectors) {
		kfree(argv);
		kfree(envp);
	}
	int abi = elf_image_abi(image);
	cleanup();
	cur->user->abi = abi;
	esp_top = abi == MOS_ABI_I386 ? MOS_COMPAT_TASK_SIZE :
					MOS_NATIVE_TASK_SIZE;
	cur->user->vm->task_size = esp_top;
	cur->user->vm->mmap_base = abi == MOS_ABI_I386 ? USER_HEAP_END :
							 0x100000000ULL;

	/*
	 * now we parse and load elf file.
	 * A typical executable file in linux  will have a section
	 * "interp" (usually ld-linux.so), we read that interp and
	 * load PT_LOAD sections into memory, e_entry of interp is
	 * the address we are going to jump in, setup the stack in
	 * proper format and jump to interp, that's all we have to
	 * do in execv syscall. All staffs like dynamic library
	 * loading / symbol resolve / etc will be handled by interp
	 * pretty easy ha?
	 */
	elf_map_prepared(image, &fmt);
	elf_release(image);
	if (exec_fp)
		fs_put_file(exec_fp);
	eip = fmt.interp_load_addr;
	cur->user->vm->start_brk = fmt.start_brk;
	cur->user->vm->brk = fmt.start_brk;
	vm_set_brk(cur->user->vm, fmt.start_brk, fmt.start_brk);
	if (!eip) {
		klog("exec: unable to load ELF image %s\n", file_name);
		free_v(s_argv, argc);
		free_v(s_envp, envc);
		name_put(file_name);
		/* The previous address space has already been released. */
		do_exit(SIGSEGV);
		__builtin_unreachable();
	}

	if (cur->user->vm->start_brk > 0 &&
	    cur->user->vm->start_brk < USER_HEAP_END) {
		do_mmap(cur->user->vm->start_brk, PAGE_SIZE,
			PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED, -1, 0);
		cur->user->vm->brk =
			cur->user->vm->start_brk + KERNEL_TASK_BYTES;
		vm_set_brk(cur->user->vm, cur->user->vm->start_brk,
			   cur->user->vm->brk);
	}

	/*
	 * map a kernel code region into user land, usually used in
	 * signal deliver and signal return.
	 */
	if (abi == MOS_ABI_I386)
		mm_vdso_map();

	/* Map only the top USER_STACK_INIT_PAGES pages initially.
	 * The stack grows downward automatically via the page fault handler.
	 */
	{
		vaddr_t stack_init_bottom =
			esp_top - USER_STACK_INIT_PAGES * PAGE_SIZE;
		do_mmap(stack_init_bottom, USER_STACK_INIT_PAGES * PAGE_SIZE,
			PROT_READ | PROT_WRITE, MAP_FIXED, -1, 0);
		cur->user->vm->start_stack = stack_init_bottom;
		vm_set_stack(cur->user->vm, stack_init_bottom);
	}

	/* Privileged executable images clear the parent-death signal. */
	if ((s.st_mode & S_ISUID) ||
	    (s.st_mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP))
		cur->pdeath_signal = 0;

	/* Commit executable credentials before constructing the auxiliary vector. */
	cur->user->euid = cur->user->suid = cur->user->fsuid = exec_euid;
	cur->user->egid = cur->user->sgid = cur->user->fsgid = exec_egid;
	cur->user->keep_capabilities = 0;
	memset(cur->user->cap_effective, 0, sizeof(cur->user->cap_effective));
	memset(cur->user->cap_permitted, 0, sizeof(cur->user->cap_permitted));
	cur->user->cap_initialized = 0;

	/* setup arguments and enviroments in proper way for interp */
	esp_top = setup_user_stack(file_name, argc, s_argv, envc, s_envp,
				   esp_top, &fmt);

	/*
	 * A traced task that reaches execve without a prior startup SIGSTOP
	 * still needs a visible stop so the tracer can take control before
	 * first user instruction in the new image.
	 */
	ps_ptrace_stop_exec(eip, esp_top);

	/* that's all */
	free_v(s_argv, argc);
	free_v(s_envp, envc);
	name_put(file_name);
	cur->type = ps_user;
	extern void switch_to_user_mode(vaddr_t eip, vaddr_t esp);
	switch_to_user_mode(eip, esp_top);
	// never return here
	return 0;
}

/*
 * run_if_exist - exec @path if the file exists, otherwise panic
 *
 * Used during early boot to launch the first user-space program.  If the
 * file is missing (e.g. the root filesystem is not populated), the kernel
 * prints a diagnostic and calls DIE() to halt.
 */
static void run_if_exist(const char *path, const char *argv[],
			 const char *envp[])
{
	struct stat s;
	if (fs_stat(path, &s) != -1) {
		sys_execve(path, (char **)argv, (char **)envp);
	}

	printk("%s fails!\n", path);
	DIE();
}

static void prepare_interactive_userspace(task_struct *cur)
{
	strcpy(cur->user->cwd, "/root");

	printk("rtc: Sync local time\n");
	time_sync_rtc();

	printk("mnt: Re-mount rootfs (rw)\n");
	{
		blockdev_info rootdev;
		if (blockdev_first_mountable(&rootdev))
			fs_do_mount(rootdev.name, "/", "ext4", MS_REMOUNT,
				    NULL);
	}

	printk("mnt: Mounting proc on /proc\n");
	fs_do_mount("proc", "/proc", "proc", 0, NULL);

	printk("mnt: Mounting devpts on /dev/pts\n");
	fs_do_mount("devpts", "/dev/pts", "devpts", 0, NULL);

	printk("mnt: Mounting tmpfs on /dev/shm\n");
	fs_do_mount("tmpfs", "/dev/shm", "tmpfs", 0, NULL);
}

/*
 * run_first_user_process - set up and exec the first user-space process
 *
 * Called once from kinit_userspace() in the context of the first kernel
 * thread that will become PID 1.  Responsibilities:
 *   1. Update TSS.esp0 so the CPU knows where to switch to kernel stack
 *      when this process takes an interrupt or syscall.
 *   2. Open the three standard file descriptors (stdin/stdout/stderr):
 *        fd 0 — /dev/tty1 O_RDONLY (keyboard input)
 *        fd 1 — /dev/tty1 O_WRONLY (terminal output)
 *        fd 2 — /dev/tty1 O_WRONLY (terminal error output)
 *   3. exec /bin/bash as the init process.
 */
static void kinit_userspace()
{
	const char *devault_argv[] = { "/sbin/init", NULL, NULL };
	const char *default_envp[] = { "TERM=linux", NULL };
	const char *user_argv[] = { "/bin/bash", "-l", NULL };
	const char *test_bash_argv[] = {
		"/bin/bash",
		"-c",
		"/proc/tests/all && /proc/tests/all_script",
		NULL,
	};
	const char *user_envp[] = { "PATH=/bin:/usr/bin:/sbin", "TERM=linux",
				    "HOME=/root", "LANG=en_US", NULL };
	task_struct *cur = CURRENT_TASK();
	const char **argv = devault_argv;
	const char **envp = default_envp;
	vaddr_t esp0 = (vaddr_t)(uintptr_t)cur + KERNEL_TASK_BYTES;
	const char *arg = g_cmdline;

	/* The initial userspace process has no userspace parent. */
	cur->ppid = 0;

	/* Pass an explicit supported runlevel to SysV init. */
	while (*arg) {
		const char *end;
		while (*arg == ' ' || *arg == '\t')
			arg++;
		end = arg;
		while (*end && *end != ' ' && *end != '\t')
			end++;
		if (end - arg == 1 && (*arg == '3' || *arg == '5'))
			devault_argv[1] = *arg == '3' ? "3" : "5";
		arg = end;
	}

	if (TestControl.bash) {
		argv = user_argv;
		envp = user_envp;
		prepare_interactive_userspace(cur);
	}

	if (TestControl.test) {
		argv = test_bash_argv;
		envp = user_envp;
		prepare_interactive_userspace(cur);
	}

	printk("Now bringup first user process %s\n", argv[0]);

	ps_update_tss(esp0);

	/* Open stdin, stdout, stderr (fds 0, 1, 2) — all on /dev/tty1. */
	fs_open("/dev/tty1", O_RDONLY, 0);
	fs_open("/dev/tty1", O_WRONLY, 0);
	fs_open("/dev/tty1", O_RDWR, 0);

	run_if_exist(argv[0], argv, envp);
}

KERNEL_INIT(8, kinit_userspace);

int sys_execve(const char *file, char **argv, char **envp)
{
	return execve_common(file, argv, envp, 0);
}
int sys_execve_owned(const char *file, char **argv, char **envp)
{
	return execve_common(file, argv, envp, 1);
}
