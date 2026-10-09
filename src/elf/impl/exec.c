#include <elf/exec.h>
#include <elf/format.h>
#include <device/time.h>
#include <elf/elf.h>
#include <dev/blockdev.h>
#include <ps/ps.h>
#include <ps/smp.h>
#include <int/int.h>
#include <dev/tty.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <mm/vdso.h>
#include <fs/fcntl.h>
#include <fs/fs.h>
#include <fs/inotify.h>
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
static void cleanup(mm_struct *old_mm)
{
	task_struct *cur = CURRENT_TASK();
	intr_frame *frame = (intr_frame *)((char *)cur + KERNEL_TASK_BYTES -
					   sizeof(*frame));
	unsigned word;

	if (cur->life->fork_flag & FORK_FLAG_VFORK) {
		cond_notify(&cur->life->vfork_event);
		cur->life->fork_flag &= ~FORK_FLAG_VFORK;
	}

	smp_mm_activate(VIRT_TO_PHY(cur->memory->page_dir));
	ref_count_put(old_mm);
	vm_invalidate_task_cache(cur);

	/* Close all O_CLOEXEC file descriptors. */
	/* ps_unshare_fds() makes this bitmap private before cleanup. */
	for (word = 0; cur->files && word < FD_BITMAP_WORDS; word++) {
		unsigned long pending = cur->files->cloexec[word];
		while (pending) {
			unsigned fd =
				word * FD_BITMAP_BITS + __builtin_ctzl(pending);
			pending &= pending - 1;
			if (fd < MAX_FD && cur->files->fds[fd])
				fs_close(fd);
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
 * Empty strings are arguments; only a NULL pointer ends the vector.
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
 * address space, so every string is copied here. Returns a
 * heap-allocated array of @n strings, or NULL if @n is zero.
 * Free with free_v(). The pointer array and all strings share one allocation.
 */
static char **dup_strv(char **v, unsigned n)
{
	unsigned i, bytes, lengths[32];
	char **ret;
	char *strings;
	if (!n || n > UINT32_MAX / sizeof(char *))
		return NULL;
	bytes = n * sizeof(char *);
	for (i = 0; i < n; i++) {
		size_t len = strlen(v[i]) + 1;
		if (len > UINT32_MAX - bytes)
			return NULL;
		if (n <= (sizeof(lengths) / sizeof(lengths[0])))
			lengths[i] = len;
		bytes += len;
	}
	ret = kmalloc(bytes);
	if (!ret)
		return NULL;
	strings = (char *)(ret + n);
	for (i = 0; i < n; i++) {
		size_t len = n <= (sizeof(lengths) / sizeof(lengths[0])) ?
				     lengths[i] :
				     strlen(v[i]) + 1;
		if (len > bytes - (unsigned)(strings - (char *)ret)) {
			kfree(ret);
			return NULL;
		}
		ret[i] = strings;
		memcpy(strings, v[i], len);
		/* Keep copied strings terminated even if another thread changed them. */
		strings[len - 1] = 0;
		strings += len;
	}
	return ret;
}

/* Copy the interpreter prefix and argv[1..] directly into one arena, avoiding
 * a temporary pointer vector. Prefix lengths are reused when copying. */
static char **dup_script_argv(const char *interp, const char *interp_arg,
			      const char *script, char **argv,
			      unsigned user_argc, unsigned *argc)
{
	const char *prefix[3];
	unsigned lengths[3], prefixes = 0, bytes = 0, i;
	unsigned tail = user_argc ? user_argc - 1 : 0;
	char **ret;
	char *strings;

	prefix[prefixes++] = interp;
	if (interp_arg)
		prefix[prefixes++] = interp_arg;
	prefix[prefixes++] = script;
	*argc = prefixes + tail;
	if (*argc < tail || *argc > UINT32_MAX / sizeof(char *))
		return NULL;
	bytes = *argc * sizeof(char *);
	for (i = 0; i < prefixes; i++) {
		size_t length = strlen(prefix[i]) + 1;
		if (length > UINT32_MAX - bytes)
			return NULL;
		lengths[i] = length;
		bytes += length;
	}
	for (i = 1; i < user_argc; i++) {
		size_t length = strlen(argv[i]) + 1;
		if (length > UINT32_MAX - bytes)
			return NULL;
		bytes += length;
	}
	ret = kmalloc(bytes);
	if (!ret)
		return NULL;
	strings = (char *)(ret + *argc);
	for (i = 0; i < prefixes; i++) {
		ret[i] = strings;
		memcpy(strings, prefix[i], lengths[i]);
		strings += lengths[i];
	}
	for (i = 1; i < user_argc; i++) {
		size_t length = strlen(argv[i]) + 1;
		/* User strings can change between sizing and copying. */
		if (length > bytes - (unsigned)(strings - (char *)ret)) {
			kfree(ret);
			return NULL;
		}
		ret[prefixes + i - 1] = strings;
		memcpy(strings, argv[i], length);
		strings += length;
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
 * @length is the number of bytes actually read; line[length] is writable.
 * @truncated means more file data follows the read window. Reject a directive
 * without a terminator in that window rather than use a cut-off path or arg.
 * The returned pointers remain valid while the caller's buffer is alive.
 */
static int parse_shebang(char *line, unsigned length, int truncated,
			 const char **interp, const char **interp_arg)
{
	char *p = line, *end = line + length;
	while (p < end && *p && *p != '\n' && *p != '\r')
		p++;
	if (p == end && truncated)
		return -ENOEXEC;
	*p = '\0';

	p = line + 2;
	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p)
		return -ENOEXEC;
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
	return 0;
}

/*
 * free_v - free the single arena containing an argv/envp array and its strings.
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
	unsigned exec_euid = cur->credentials->euid;
	unsigned exec_egid = cur->credentials->egid;
	enum { HEADER_BYTES = 64 };
	char firstline[HEADER_BYTES + 1];
	unsigned header_length;
	int header_is_elf = 0;
	const char *interp = NULL, *interp_arg = NULL;
	if (!f) {
		return -ENOENT;
	}

	/* resolve path into full path */
	file_name = name_get();
	if (!file_name)
		return -ENOMEM;
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

	inotify_file_open(fp, cur->fs->root);
	/* read first line via VFS file ops */
	if (!fp->f_fop || !fp->f_fop->read) {
		fs_put_file(fp);
		name_put(file_name);
		return -ENOENT;
	} else {
		loff_t pos = 0;
		size_t length = s.st_size < HEADER_BYTES ? s.st_size :
							   HEADER_BYTES;
		ssize_t n = fp->f_fop->read(fp, firstline, length, &pos);
		if (n > 0)
			inotify_file_event(fp, IN_ACCESS);
		if (n < 0 || (size_t)n > length) {
			fs_put_file(fp);
			name_put(file_name);
			return -ENOENT;
		}
		header_length = n;
		firstline[header_length] = '\0';
	}
	/* Identify the image before copying any user vectors. For scripts keep
	 * file_name as the script pathname until its interpreter is validated. */
	if (header_length >= 4 && firstline[0] == 0x7f && firstline[1] == 'E' &&
	    firstline[2] == 'L' && firstline[3] == 'F') {
		exec_fp = fp;
		header_is_elf = 1;
		if (!(fp->f_mount_flags & MS_NOSUID) &&
		    !cur->execution->ptrace_tracer) {
			if (s.st_mode & S_ISUID)
				exec_euid = s.st_uid;
			if ((s.st_mode & (S_ISGID | S_IXGRP)) ==
			    (S_ISGID | S_IXGRP))
				exec_egid = s.st_gid;
		}
	} else if (header_length >= 2 && firstline[0] == '#' &&
		   firstline[1] == '!') {
		int ret = parse_shebang(firstline, header_length,
					s.st_size > header_length, &interp,
					&interp_arg);
		if (ret) {
			fs_put_file(fp);
			name_put(file_name);
			return ret;
		}

		exec_fp = fs_open_file(interp, O_RDONLY, 0);
		fs_put_file(fp);
	} else {
		fs_put_file(fp);
		name_put(file_name);
		return -ENOEXEC;
	}
	/* Reject invalid images before allocating vectors, closing descriptors,
	 * or replacing memory. */
	{
		int ret = header_is_elf ?
				  elf_prepare_header(exec_fp, &image, firstline,
						     header_length, &s) :
				  elf_prepare(exec_fp, &image);
		if (ret) {
			if (exec_fp)
				fs_put_file(exec_fp);
			free_v(s_argv, argc);
			free_v(s_envp, envc);
			name_put(file_name);
			return ret;
		}
	}

	argc = count_strv(argv);
	envc = count_strv(envp);
	if (header_is_elf) {
		s_argv = dup_strv(argv, argc);
	} else {
		unsigned user_argc = argc;
		s_argv = dup_script_argv(interp, interp_arg, file_name, argv,
					 user_argc, &argc);
		strcpy(file_name, interp);
	}
	s_envp = dup_strv(envp, envc);
	if (((argc || !header_is_elf) && !s_argv) || (envc && !s_envp)) {
		elf_release(image);
		fs_put_file(exec_fp);
		free_v(s_argv, argc);
		free_v(s_envp, envc);
		name_put(file_name);
		return -ENOMEM;
	}

	/* exec owns a private table before applying FD_CLOEXEC. */
	if (ps_unshare_fds(cur) != 0 || ps_unshare_exec_context(cur) != 0) {
		elf_release(image);
		if (exec_fp)
			fs_put_file(exec_fp);
		free_v(s_argv, argc);
		free_v(s_envp, envc);
		name_put(file_name);
		return -ENOMEM;
	}

	/* Prepare the whole CLONE_VM resource before replacing the old image. */
	mm_struct *old_mm = cur->memory;
	mm_struct *new_mm = vm_create();
	if (new_mm)
		new_mm->page_dir = vm_alloc(1);
	if (!new_mm || !new_mm->page_dir) {
		ref_count_put(new_mm);
		elf_release(image);
		fs_put_file(exec_fp);
		free_v(s_argv, argc);
		free_v(s_envp, envc);
		name_put(file_name);
		return -ENOMEM;
	}
	mm_init_process_page_dir(new_mm->page_dir);
	cur->memory = new_mm;

	/* save command line into task struct (bounded to one page) */
	{
		char *end = cur->memory->command + PAGE_SIZE - 1;
		unsigned len;

		tmp = cur->memory->command;
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
		cur->memory->cmd_len = tmp - cur->memory->command;
	}

	/* save environment into task struct (bounded to one page) */
	{
		char *end = cur->memory->environment + PAGE_SIZE - 1;
		unsigned len;

		tmp = cur->memory->environment;
		for (i = 0; i < (int)envc && tmp < end; i++) {
			len = strlen(s_envp[i]);
			if (len > (unsigned)(end - tmp))
				len = (unsigned)(end - tmp);
			memcpy(tmp, s_envp[i], len);
			tmp[len] = '\0';
			tmp += len + 1;
		}
		cur->memory->env_len = tmp - cur->memory->environment;
	}

	/* log if needed */
	if (TEST_LOG(TEST_LOG_INFO))
		klog("execve(%s)\n", cur->memory->command);

	/*
	 * Linux execve semantics for signal state:
	 * caught handlers reset to SIG_DFL, ignored handlers stay ignored,
	 * pending signals and the signal mask are preserved, and the alt signal
	 * stack is disabled.
	 */
	{
		int sig;

		for (sig = 1; sig < NSIG; sig++) {
			struct sigaction *sa = &cur->sighand->actions[sig];

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
	const struct elf_format *format = elf_image_format(image);
	cleanup(old_mm);
	esp_top = format->task_size;
	cur->memory->task_size = esp_top;
	cur->memory->mmap_base = format->mmap_base;
	cur->memory->brk_limit = format->brk_limit;

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
	/* Retain the main image for /proc/PID/exe, including its resolved path. */
	if (cur->memory->executable)
		fs_put_file(cur->memory->executable);
	cur->memory->executable = exec_fp;
	exec_fp = NULL;
	eip = fmt.interp_load_addr;
	cur->memory->start_brk = fmt.start_brk;
	cur->memory->brk = fmt.start_brk;
	vm_set_brk(cur->memory, fmt.start_brk, fmt.start_brk);
	if (!eip) {
		klog("exec: unable to load ELF image %s\n", file_name);
		free_v(s_argv, argc);
		free_v(s_envp, envc);
		name_put(file_name);
		/* The previous address space has already been released. */
		do_exit(SIGSEGV);
		__builtin_unreachable();
	}

	if (cur->memory->start_brk > 0 &&
	    cur->memory->start_brk < USER_HEAP_END) {
		do_mmap(cur->memory->start_brk, PAGE_SIZE,
			PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED, -1, 0);
		cur->memory->brk = cur->memory->start_brk + KERNEL_TASK_BYTES;
		vm_set_brk(cur->memory, cur->memory->start_brk,
			   cur->memory->brk);
	}

	/* Install the executable's user register context and helper mappings. */
	format->activate(cur);

	/* Map only the top USER_STACK_INIT_PAGES pages initially.
	 * The stack grows downward automatically via the page fault handler.
	 */
	{
		vaddr_t stack_init_bottom =
			esp_top - USER_STACK_INIT_PAGES * PAGE_SIZE;
		do_mmap(stack_init_bottom, USER_STACK_INIT_PAGES * PAGE_SIZE,
			PROT_READ | PROT_WRITE, MAP_FIXED, -1, 0);
		cur->memory->start_stack = stack_init_bottom;
		vm_set_stack(cur->memory, stack_init_bottom);
	}

	/* Privileged executable images clear the parent-death signal. */
	if ((s.st_mode & S_ISUID) ||
	    (s.st_mode & (S_ISGID | S_IXGRP)) == (S_ISGID | S_IXGRP))
		cur->life->pdeath_signal = 0;

	/* Commit executable credentials before constructing the auxiliary vector. */
	cur->credentials->euid = cur->credentials->suid =
		cur->credentials->fsuid = exec_euid;
	cur->credentials->egid = cur->credentials->sgid =
		cur->credentials->fsgid = exec_egid;
	cur->credentials->keep_capabilities = 0;
	memset(cur->credentials->cap_effective, 0,
	       sizeof(cur->credentials->cap_effective));
	memset(cur->credentials->cap_permitted, 0,
	       sizeof(cur->credentials->cap_permitted));
	cur->credentials->cap_initialized = 0;

	/* setup arguments and enviroments in proper way for interp */
	esp_top = format->setup_stack(file_name, argc, s_argv, envc, s_envp,
				      esp_top, &fmt);

	/*
	 * A traced task that reaches execve without a prior startup SIGSTOP
	 * still needs a visible stop so the tracer can take control before
	 * first user instruction in the new image.
	 */
	ps_ptrace_stop_exec(eip, esp_top, format->exec_syscall);

	/* that's all */
	free_v(s_argv, argc);
	free_v(s_envp, envc);
	name_put(file_name);
	cur->life->type = ps_user;
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
	strcpy(cur->fs->cwd, "/root");

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
 *   3. exec /sbin/init, or the test runner in test mode.
 */
static void kinit_userspace()
{
	const char *default_argv[] = { "/sbin/init",
				       TestControl.text ? "3" : NULL, NULL };
	const char *default_envp[] = { "TERM=linux", NULL };
	const char *test_bash_argv[] = {
		"/bin/bash",
		"-c",
		"/proc/tests/all && /proc/tests/all_script",
		NULL,
	};
	const char *user_envp[] = { "PATH=/bin:/usr/bin:/sbin", "TERM=linux",
				    "HOME=/root", "LANG=en_US", NULL };
	task_struct *cur = CURRENT_TASK();
	const char **argv = default_argv;
	const char **envp = default_envp;

	/* The initial userspace process has no userspace parent. */
	cur->life->ppid = 0;

	if (TestControl.test) {
		argv = test_bash_argv;
		envp = user_envp;
		prepare_interactive_userspace(cur);
	}

	printk("Now bringup first user process %s\n", argv[0]);

	reset_tss(cur);

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
