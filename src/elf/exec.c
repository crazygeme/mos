#include <elf/exec.h>
#include <elf/elf.h>
#include <ps/ps.h>
#include <int/int.h>
#include <hw/tty.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <mm/vdso.h>
#include <fs/fcntl.h>
#include <fs/fs.h>
#include <lib/klib.h>
#include <lib/lock.h>
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
 *   2. Close all file descriptors that have O_CLOEXEC set — POSIX requires
 *      these to be closed across exec.
 *   3. Unmap all user virtual-memory regions (code, data, stack, heap).
 *   4. Reset the heap pointer and replace the VM descriptor with a fresh one
 *      so the new image starts with a clean address space.
 */
static void cleanup()
{
	task_struct *cur = CURRENT_TASK();
	int i = 0;

	/* Wake the vfork()-ing parent now that we are replacing the address space. */
	if (cur->fork_flag & FORK_FLAG_VFORK) {
		cond_notify(&cur->vfork_event);
	}

	/* Close all O_CLOEXEC file descriptors. */
	for (i = 0; i < MAX_FD; i++) {
		if (cur->fds[i].used && (cur->fds[i].flag & O_CLOEXEC)) {
			fs_close(i);
		}
	}

	vm_destroy(cur->user->vm);

	/* Unmap every user-space page table entry. */
	ps_cleanup_all_user_map(cur);

	/* Reset heap; start_brk/brk will be set after elf_map(). */
	cur->user->start_brk = 0;
	cur->user->brk = 0;

	cur->user->vm = vm_create();
}

/*
 * get_argc_envc - count argument and environment strings
 *
 * Walks the NULL-terminated @argv and @envp arrays (same convention as the
 * POSIX execve ABI) and writes the counts into *argv_len and *envp_len.
 * Both pointers must be non-NULL; either array pointer may be NULL (treated
 * as empty).
 */
static void get_argc_envc(const char *file, char **argv, char **envp,
			  unsigned *argv_len, unsigned *envp_len)
{
	int i = 0;
	if (!argv_len || !envp_len) {
		return;
	}

	*argv_len = *envp_len = 0;
	if (argv) {
		while (argv[i] && *argv[i]) {
			*argv_len = *argv_len + 1;
			i++;
		}
	}

	i = 0;
	if (envp) {
		while (envp[i] && *envp[i]) {
			*envp_len = *envp_len + 1;
			i++;
		}
	}
}

/*
 * save_argv - deep-copy the argv array into kernel heap memory
 *
 * The user-supplied @argv pointers will become invalid after the address
 * space is torn down in cleanup(), so we strdup() every string here.
 *
 * For shell scripts (identified by a non-NULL @script, which holds the
 * interpreter path extracted from the "#!" line), the interpreter path is
 * prepended as argv[0] and the original argv is shifted by one, matching
 * the POSIX execve behaviour for script execution.
 *
 * Returns a newly allocated array of @argc (or @argc+1 for scripts) heap
 * strings, or NULL if @argc is zero.
 */
static char **save_argv(const char *file, char **argv, unsigned argc,
			char *script)
{
	char **ret = 0;
	int i = 0;
	if (!argc) {
		return 0;
	}

	if (script) {
		/* Prepend interpreter path; original argv follows. */
		ret = kmalloc((argc + 1) * sizeof(char *));
		ret[0] = strdup(script);
		for (i = 0; i < (argc); i++)
			ret[i + 1] = strdup(argv[i]);
	} else {
		ret = kmalloc(argc * sizeof(char *));
		for (i = 0; i < (argc); i++)
			ret[i] = strdup(argv[i]);
	}

	return ret;
}

/*
 * save_envp - deep-copy the envp array into kernel heap memory
 *
 * Same rationale as save_argv(): environment strings must be copied
 * before the user address space is destroyed.  Returns a newly allocated
 * array of @envc heap strings, or NULL if @envc is zero.
 */
static char **save_envp(char **envp, unsigned envc)
{
	int i = 0;
	char **ret = 0;
	if (!envc) {
		return 0;
	}

	ret = kmalloc(envc * sizeof(char *));
	for (i = 0; i < envc; i++) {
		ret[i] = strdup(envp[i]);
	}

	return ret;
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
#define ELF_HWCAP 0x0183FBFF //(boot_cpu_data.x86_capability)

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
	char *esp = (char *)(top);
	unsigned *sp, *platform = 0;
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
	*((unsigned *)esp) = 0;

	/* Push the executable filename as a string. */
	len = strlen(file) + 1;
	esp -= len;
	strcpy(esp, file);

	/* Push environment strings in reverse order, recording pointers. */
	for (i = envc - 1; i >= 0; i--) {
		int len = strlen(envp[i]) + 1;
		esp -= len;
		strcpy(esp, envp[i]);
		tmp_array_env[i] = esp;
		esp[len - 1] = '\0';
	}
	tmp_array_env[envc] = 0; /* NULL terminator for envp array */

	/* Push argument strings in reverse order, recording pointers. */
	for (i = argc - 1; i >= 0; i--) {
		int len = strlen(argv[i]) + 1;
		esp -= len;
		strcpy(esp, argv[i]);
		tmp_array_argv[i] = esp;
		esp[len - 1] = '\0';
	}
	tmp_array_argv[argc] = 0; /* NULL terminator for argv array */

	/* Push the ELF platform string (used by AT_PLATFORM). */
	len = sizeof(ELF_PLATFORM);
	esp -= len;
	strcpy(esp, ELF_PLATFORM);
	platform = (unsigned *)esp;

	/* Align stack to 16 bytes (ABI requirement) then step back one slot. */
	esp = (char *)((~15UL & (unsigned long)(esp)) - 16UL);
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

	/* Hardware capability, page size, and clock-tick frequency. */
	sp -= 3 * 2;
	NEW_AUX_ENT(0, AT_HWCAP, ELF_HWCAP);
	NEW_AUX_ENT(1, AT_PAGESZ, 4096);
	NEW_AUX_ENT(2, AT_CLKTCK, 100);

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
	esp = (char *)sp;
	esp -= (env_buf_len + 4);
	envpp = esp;
	memcpy(envpp, tmp_array_env, env_buf_len + 4);
	esp -= (argv_buf_len + 4);
	argvp = esp;
	memcpy(argvp, tmp_array_argv, argv_buf_len + 4);

	kfree(tmp_array_argv);
	kfree(tmp_array_env);

	/* Push argc — this is what the entry point (or crt0) reads first. */
	esp -= 4;
	*((unsigned *)esp) = argc;
	return esp;
}

int sys_execve(const char *f, char **argv, char **envp)
{
	unsigned eip = 0;
	int i = 0;
	unsigned esp_buttom = KERNEL_OFFSET - USER_STACK_PAGES * PAGE_SIZE;
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
	int len = 256; /* max bytes to read for the first line of a script */
	char *firstline = NULL;
	if (!f) {
		return -ENOENT;
	}

	/* resolve path into full path */
	file_name = name_get();
	resolve_path(f, file_name);

	/* make script interp as first argument if file starts with #! */

	/* open file for state checking and first line reading */
	fp = fs_open_file(file_name, O_RDONLY, 0, 1);
	if (!fp) {
		name_put(file_name);
		return -ENOENT;
	}

	/* stat via VFS inode ops */
	memset(&s, 0, sizeof(s));
	if (!fp->f_inode->i_op || !fp->f_inode->i_op->getattr ||
	    fp->f_inode->i_op->getattr(fp->f_inode, &s) != 0) {
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
	firstline = malloc(256);
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
	 * check whether starts with #!,
	 * if it is, assign script interp to file_name
	 */
	if (firstline[0] == 0x7f && firstline[1] == 'E' &&
	    firstline[2] == 'L') {
		free(firstline);
		firstline = NULL;
	} else if (firstline[0] == '#' && firstline[1] == '!') {
		char *lf = strchr(firstline, '\n');
		if (lf)
			*lf = '\0';
		strcpy(file_name, firstline + 2);
	} else {
		free(firstline);
		name_put(file_name);
		return -ENOEXEC;
	}

	/* 
	 * parse arguments and enviroments 
	 * note that a #! file needs additional slot 
	 */
	get_argc_envc(file_name, argv, envp, &argc, &envc);
	s_argv = save_argv(file_name, argv, argc, firstline);
	if (firstline)
		argc++;
	s_envp = save_envp(envp, envc);

	if (firstline) {
		free(firstline);
		firstline = NULL;
	}

	/* save command line into task struct */
	tmp = cur->user->command;
	strcpy(tmp, file_name);
	tmp = tmp + strlen(tmp) + 1;
	for (i = 1; i < argc; i++) {
		strcpy(tmp, s_argv[i]);
		tmp = tmp + strlen(tmp) + 1;
	}
	cur->user->cmd_len = tmp - cur->user->command;

	tmp = cur->user->environment;
	if (envc > 0) {
		strcpy(tmp, s_envp[0]);
		tmp = tmp + strlen(tmp);
	}

	for (i = 1; i < envc; i++) {
		strcpy(tmp, s_envp[i]);
		tmp = tmp + strlen(tmp);
	}
	cur->user->env_len = tmp - cur->user->environment;

	/* log if needed */
	if (TestControl.verbos)
		klog("execve(%s)\n", cur->user->command);

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
	cur->user->start_brk = fmt.start_brk;
	cur->user->brk = fmt.start_brk;
	if (!eip) {
		printk("fatal error: file %s not found!\n", file_name);
		asm("hlt");
	}

	/*
	 * map a kernel code region into user land, usually used in
	 * signal deliver and signal return.
	 */
	mm_vdso_map();

	/* don't forget to setup a stack for our program... */
	do_mmap(esp_buttom, USER_STACK_PAGES * PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, -1, 0);

	/* setup arguments and enviroments in proper way for interp */
	esp_top = setup_user_stack(file_name, argc, s_argv, envc, s_envp,
				   esp_top, &fmt);

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

/*
 * run_first_user_process - set up and exec the first user-space process
 *
 * Called once from kinit_userspace() in the context of the first kernel
 * thread that will become PID 1.  Responsibilities:
 *   1. Update TSS.esp0 so the CPU knows where to switch to kernel stack
 *      when this process takes an interrupt or syscall.
 *   2. Open the three standard file descriptors (stdin/stdout/stderr):
 *        fd 0 — /dev/tty O_RDONLY (keyboard input)
 *        fd 1 — /dev/tty O_WRONLY (terminal output)
 *        fd 2 — /dev/tty O_WRONLY (terminal error output)
 *   3. exec /bin/bash as the init process.
 */
static void kinit_userspace()
{
	const char *devault_argv[] = { "/sbin/init", "1", "fastboot", NULL };
	const char *default_envp[] = { NULL };
	const char *user_argv[] = { "placeholder", NULL };
	const char *user_envp[] = { "PATH=/bin:/usr/bin:/sbin", "TERM=linux",
				    "HOME=/root", "LANG=en_US.UTF-8", NULL };
	task_struct *cur = CURRENT_TASK();

	const char **argv = devault_argv;
	const char **envp = default_envp;

	if (TestControl.init_binary && *TestControl.init_binary) {
		user_argv[0] = TestControl.init_binary;
		argv = user_argv;
		envp = user_envp;
		strcpy(cur->user->cwd, "/root");
	}

	unsigned esp0 = (unsigned)cur + (unsigned)+PAGE_SIZE;

	ps_update_tss(esp0);

	/* Open stdin, stdout, stderr (fds 0, 1, 2) — all on /dev/tty. */
	fs_open("/dev/tty0", O_RDONLY, NULL);
	fs_open("/dev/tty0", O_WRONLY, NULL);
	fs_open("/dev/tty0", O_WRONLY, NULL);

	run_if_exist(argv[0], argv, envp);
}

KERNEL_INIT(8, kinit_userspace);
