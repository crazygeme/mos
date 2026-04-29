#include <elf/exec.h>
#include <hw/time.h>
#include <elf/elf.h>
#include <dev/blockdev.h>
#include <ps/ps.h>
#include <int/int.h>
#include <hw/tty.h>
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
	intr_frame *frame =
		(intr_frame *)((char *)cur + PAGE_SIZE - sizeof(*frame));
	heap_state *heap;
	int i = 0;

	if (cur->fork_flag & FORK_FLAG_VFORK) {
		unsigned int *new_pd;

		/* Wake the blocked parent. */
		cond_notify(&cur->vfork_event);
		cur->fork_flag &= ~FORK_FLAG_VFORK;

		/*
		 * The child borrowed the parent's page_dir and vm.  Give the
		 * child its own empty page directory now, copying only the
		 * kernel half so kernel code remains reachable after SET_CR3.
		 * The parent's page directory and vm are untouched.
		 */
		new_pd = vm_alloc(1);
		mm_init_process_page_dir((unsigned int)new_pd);
		cur->user->page_dir = (unsigned int)new_pd;
		SET_CR3(VIRT_TO_PHY(new_pd));
		cur->user->vm = vm_create();
		cur->user->mmap_cache = NULL;
	} else {
		vm_destroy(cur->user->vm);
		/* Unmap every user-space page table entry. */
		ps_cleanup_all_user_map(cur);
		cur->user->vm = vm_create();
		cur->user->mmap_cache = NULL;
	}

	/* Close all O_CLOEXEC file descriptors. */
	for (i = 0; i < MAX_FD; i++) {
		if (cur->fds[i] && fd_bitmap_test(cur->fd_cloexec, i)) {
			fs_close(i);
		}
	}

	/*
	 * Signal state is handled by execve() itself using Linux semantics:
	 * caught handlers reset to SIG_DFL, ignored handlers stay SIG_IGN,
	 * pending signals are cleared, and the altstack is disabled.
	 * Do not wipe the whole signal context here or we'd lose SIG_IGN.
	 */

	/*
	 * execve gets a fresh user image, so detach from any shared heap state
	 * before resetting the program break.
	 */
	heap = ps_heap_state_new();
	if (!heap)
		DIE();
	ps_heap_state_put(cur->user->heap);
	cur->user->heap = heap;

	/*
	 * execve replaces the user image, so any thread-local descriptor state
	 * from the old program must not leak into the new one. Otherwise the new
	 * image can hit set_thread_area(entry=-1) with all three slots already
	 * appearing occupied before it has installed its own TLS.
	 */
	memset(cur->user->tls_desc, 0, sizeof(cur->user->tls_desc));
	memset(cur->user->ldt_desc, 0, sizeof(cur->user->ldt_desc));
	frame->gs = 0;
	cur->tss.gs = 0;
	asm volatile("movw %0, %%gs" : : "rm"((unsigned short)0));
	ps_update_ldt(cur);
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
 * Free with free_v().
 */
static char **dup_strv(char **v, unsigned n)
{
	unsigned i;
	char **ret;
	if (!n)
		return NULL;
	ret = kmalloc(n * sizeof(char *));
	for (i = 0; i < n; i++)
		ret[i] = strdup(v[i]);
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
	int i = 0;
	if (!v) {
		return;
	}

	for (i = 0; i < size; i++) {
		kfree(v[i]);
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
static unsigned setup_user_stack(char *file, int argc, char **argv, int envc,
				 char **envp, unsigned top, mos_binfmt *exec)
{
	int i = 0;
	unsigned long esp = top;
	unsigned *sp, *platform = 0;
	unsigned vdso_entry = mm_vdso_fastcall_entry();
	int argv_buf_len = argc * sizeof(char *);
	int env_buf_len = envc * sizeof(char *);
	char **tmp_array_argv = 0;
	char **tmp_array_env = 0;
	unsigned argvp, envpp;
	int len;

	/* Temporary kernel-side pointer arrays; filled while copying strings. */
	tmp_array_argv = kmalloc(argv_buf_len + 4);
	tmp_array_env = kmalloc(env_buf_len + 4);

	/* 4-byte sentinel at the very top of the stack region. */
	esp -= 4;
	memcpy((void *)esp, &(unsigned){ 0 }, sizeof(unsigned));

	/* Push the executable filename as a string. */
	len = strlen(file) + 1;
	esp -= len;
	strcpy((char *)esp, file);

	/* Push environment strings in reverse order, recording pointers. */
	for (i = envc - 1; i >= 0; i--) {
		int len = strlen(envp[i]) + 1;
		esp -= len;
		strcpy((char *)esp, envp[i]);
		tmp_array_env[i] = (char *)esp;
		((char *)esp)[len - 1] = '\0';
	}
	tmp_array_env[envc] = 0; /* NULL terminator for envp array */

	/* Push argument strings in reverse order, recording pointers. */
	for (i = argc - 1; i >= 0; i--) {
		int len = strlen(argv[i]) + 1;
		esp -= len;
		strcpy((char *)esp, argv[i]);
		tmp_array_argv[i] = (char *)esp;
		((char *)esp)[len - 1] = '\0';
	}
	tmp_array_argv[argc] = 0; /* NULL terminator for argv array */

	/* Push the ELF platform string (used by AT_PLATFORM). */
	len = sizeof(ELF_PLATFORM);
	esp -= len;
	strcpy((char *)esp, ELF_PLATFORM);
	platform = (unsigned *)esp;

	/* Align stack to 16 bytes (ABI requirement) then step back one slot. */
	esp = (~15UL & esp) - 16UL;
	sp = (unsigned *)esp;

#define __put_user(val, addr) (*(unsigned long *)(addr) = (unsigned long)(val))

/* Write one auxiliary vector entry (id, val) at slot @nr relative to sp. */
#define NEW_AUX_ENT(nr, id, val)         \
	__put_user((id), sp + (nr * 2)); \
	__put_user((val), sp + (nr * 2 + 1));

	/* AT_NULL terminates the auxiliary vector. */
	sp -= 2;
	NEW_AUX_ENT(0, AT_NULL, 0);

	if (platform) {
		sp -= 2;
		NEW_AUX_ENT(0, AT_PLATFORM, (unsigned long)platform);
	}

	/* Hardware capability, page size, clock-tick frequency, optional vsyscall entry. */
	sp -= (vdso_entry ? 4 : 3) * 2;
	NEW_AUX_ENT(0, AT_HWCAP, ELF_HWCAP);
	NEW_AUX_ENT(1, AT_PAGESZ, 4096);
	NEW_AUX_ENT(2, AT_CLKTCK, 100);
	if (vdso_entry)
		NEW_AUX_ENT(3, AT_SYSINFO, vdso_entry);

	/*
	 * Core auxiliary entries consumed by the dynamic linker:
	 *   AT_PHDR  — virtual address of the program header table
	 *   AT_PHENT — size of one program header entry
	 *   AT_PHNUM — number of program headers
	 *   AT_BASE  — load bias of the interpreter (ld.so)
	 *   AT_FLAGS — always 0
	 *   AT_ENTRY — original entry point of the executable
	 *   AT_UID/EUID/GID/EGID — all 0 (root) for now
	 */
	sp -= 10 * 2;
	NEW_AUX_ENT(0, AT_PHDR, exec->elf_load_addr + exec->e_phoff);
	NEW_AUX_ENT(1, AT_PHENT, sizeof(Elf32_Phdr));
	NEW_AUX_ENT(2, AT_PHNUM, exec->e_phnum);
	NEW_AUX_ENT(3, AT_BASE, exec->interp_bias);
	NEW_AUX_ENT(4, AT_FLAGS, 0);
	NEW_AUX_ENT(5, AT_ENTRY, exec->e_entry);
	NEW_AUX_ENT(6, AT_UID, 0);
	NEW_AUX_ENT(7, AT_EUID, 0);
	NEW_AUX_ENT(8, AT_GID, 0);
	NEW_AUX_ENT(9, AT_EGID, 0);

#undef NEW_AUX_ENT

	/* Copy the envp and argv pointer arrays onto the stack. */
	esp = (unsigned long)sp;
	esp -= (env_buf_len + 4);
	envpp = esp;
	memcpy((void *)envpp, tmp_array_env, env_buf_len + 4);
	esp -= (argv_buf_len + 4);
	argvp = esp;
	memcpy((void *)argvp, tmp_array_argv, argv_buf_len + 4);

	kfree(tmp_array_argv);
	kfree(tmp_array_env);

	/* Push argc — this is what the entry point (or crt0) reads first. */
	esp -= 4;
	memcpy((void *)esp, &argc, sizeof(argc));
	return (unsigned)esp;
}

int sys_execve(const char *f, char **argv, char **envp)
{
	unsigned eip = 0;
	int i = 0;
	unsigned esp_top = KERNEL_OFFSET;
	char *file_name;
	unsigned argc = 0, envc = 0;
	char **s_argv = 0;
	char **s_envp = 0;
	char *tmp;
	mos_binfmt fmt = { 0 };
	task_struct *cur = CURRENT_TASK();
	struct stat s;
	file *fp;
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
	if (!(s.st_mode & S_IXUSR) || (s.st_size < 4)) {
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
	fs_put_file(fp);

	/*
	 * Determine binary type and build the final argc/argv/envp.
	 *
	 * ELF: file_name unchanged, argv/envp deep-copied as-is.
	 *
	 * #!: parse "#!/path/to/interp[ optional_arg]", update file_name
	 *     to the interpreter, and prepend [interp, optional_arg?] ahead
	 *     of the original argv so the final layout is:
	 *       [interp, optional_arg?, argv[0], argv[1], ...]
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
	} else if (firstline[0] == '#' && firstline[1] == '!') {
		const char *interp, *interp_arg;
		unsigned shebang_argc, user_argc, j;

		parse_shebang(firstline, &interp, &interp_arg);

		user_argc = count_strv(argv);
		envc = count_strv(envp);

		shebang_argc = 1 + (interp_arg ? 1 : 0);
		argc = shebang_argc + user_argc;

		s_argv = kmalloc(argc * sizeof(char *));
		s_argv[0] = strdup(interp);
		if (interp_arg)
			s_argv[1] = strdup(interp_arg);
		for (j = 0; j < user_argc; j++)
			s_argv[shebang_argc + j] = strdup(argv[j]);

		strcpy(file_name, interp);

		free(firstline);
		firstline = NULL;
		s_envp = dup_strv(envp, envc);
	} else {
		free(firstline);
		name_put(file_name);
		return -ENOEXEC;
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
	 * pending signals are cleared, and the alt signal stack is disabled.
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
		cur->signal->sig_pending = 0;
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
	cleanup();

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
	elf_map(file_name, &fmt);
	eip = fmt.interp_load_addr;
	cur->user->heap->start_brk = fmt.start_brk;
	cur->user->heap->brk = fmt.start_brk;
	if (!eip) {
		printk("fatal error: file %s not found!\n", file_name);
		asm("hlt");
	}

	if (cur->user->heap->start_brk > 0 &&
	    cur->user->heap->start_brk < USER_HEAP_END) {
		do_mmap(cur->user->heap->start_brk, PAGE_SIZE,
			PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED, -1, 0);
		cur->user->heap->brk = cur->user->heap->start_brk + PAGE_SIZE;
	}

	/*
	 * map a kernel code region into user land, usually used in
	 * signal deliver and signal return.
	 */
	mm_vdso_map();

	/* Map only the top USER_STACK_INIT_PAGES pages initially.
	 * The stack grows downward automatically via the page fault handler.
	 */
	{
		unsigned stack_init_bottom =
			KERNEL_OFFSET - USER_STACK_INIT_PAGES * PAGE_SIZE;
		do_mmap(stack_init_bottom, USER_STACK_INIT_PAGES * PAGE_SIZE,
			PROT_READ | PROT_WRITE, MAP_FIXED, -1, 0);
		cur->user->stack_bottom = stack_init_bottom;
	}

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
	extern void switch_to_user_mode(unsigned eip, unsigned esp);
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
	const char *devault_argv[] = { "/sbin/init", NULL };
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
	unsigned esp0 = (unsigned)cur + (unsigned)PAGE_SIZE;

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
