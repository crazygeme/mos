#include <ptrace.h>
#include <ps.h>
#include <int.h>
#include <mm.h>
#include <klib.h>
#include <errno.h>
#include <config.h>

/*
 * ptrace_peek_mem - read one aligned word from the tracee's virtual address space.
 *
 * Walks the tracee's two-level page table.  task->user.page_dir is the
 * kernel-virtual address of the page directory (PGD).  Each PDE/PTE stores
 * the physical address of the next-level table in its upper 20 bits; adding
 * KERNEL_OFFSET converts it to a kernel-virtual address (valid as long as
 * physical RAM < 1 GB, which is true for this QEMU target).
 *
 * Returns 0 on success, -EFAULT if any level is not present or addr is
 * in kernel space.
 */
static int ptrace_peek_mem(task_struct *tracee, unsigned long addr,
			   unsigned long *out)
{
	unsigned *pgd, *pte;
	unsigned pde, entry, phys;

	if (!tracee->user.page_dir || addr >= KERNEL_OFFSET)
		return -EFAULT;

	pgd = (unsigned *)tracee->user.page_dir;
	pde = pgd[addr >> 22];
	if (!(pde & PAGE_ENTRY_PRESENT))
		return -EFAULT;

	pte = (unsigned *)((pde & PAGE_SIZE_MASK) + KERNEL_OFFSET);
	entry = pte[(addr >> 12) & 0x3FF];
	if (!(entry & PAGE_ENTRY_PRESENT))
		return -EFAULT;

	phys = entry & PAGE_SIZE_MASK;
	*out = *(unsigned long *)(phys + KERNEL_OFFSET +
				  (addr & ~PAGE_SIZE_MASK));
	return 0;
}

/*
 * ptrace_poke_mem - write one aligned word into the tracee's virtual address
 * space.  Same page-table walk as ptrace_peek_mem.
 *
 * Note: COW pages shared with a parent are written-through to the shared
 * physical frame.  The caller (tracer) should ensure the tracee is the sole
 * owner of the page (e.g. after exec) before poking text/data.
 */
static int ptrace_poke_mem(task_struct *tracee, unsigned long addr,
			   unsigned long data)
{
	unsigned *pgd, *pte;
	unsigned pde, entry, phys;

	if (!tracee->user.page_dir || addr >= KERNEL_OFFSET)
		return -EFAULT;

	pgd = (unsigned *)tracee->user.page_dir;
	pde = pgd[addr >> 22];
	if (!(pde & PAGE_ENTRY_PRESENT))
		return -EFAULT;

	pte = (unsigned *)((pde & PAGE_SIZE_MASK) + KERNEL_OFFSET);
	entry = pte[(addr >> 12) & 0x3FF];
	if (!(entry & PAGE_ENTRY_PRESENT))
		return -EFAULT;

	phys = entry & PAGE_SIZE_MASK;
	*(unsigned long *)(phys + KERNEL_OFFSET + (addr & ~PAGE_SIZE_MASK)) =
		data;
	return 0;
}

/*
 * ptrace_get_reg - read one register from a stopped tracee's saved intr_frame.
 * offset is a PT_OFF_* byte offset matching the Linux i386 user_regs_struct.
 */
static int ptrace_get_reg(task_struct *tracee, unsigned long offset,
			  unsigned long *out)
{
	intr_frame *f = TASK_INTR_FRAME(tracee);

	switch (offset) {
	case PT_OFF_EBX:
		*out = f->ebx;
		break;
	case PT_OFF_ECX:
		*out = f->ecx;
		break;
	case PT_OFF_EDX:
		*out = f->edx;
		break;
	case PT_OFF_ESI:
		*out = f->esi;
		break;
	case PT_OFF_EDI:
		*out = f->edi;
		break;
	case PT_OFF_EBP:
		*out = f->ebp;
		break;
	case PT_OFF_EAX:
	case PT_OFF_ORIG_EAX:
		*out = f->eax;
		break;
	case PT_OFF_DS:
		*out = f->ds;
		break;
	case PT_OFF_ES:
		*out = f->es;
		break;
	case PT_OFF_FS:
		*out = f->fs;
		break;
	case PT_OFF_GS:
		*out = f->gs;
		break;
	case PT_OFF_EIP:
		*out = (unsigned long)f->eip;
		break;
	case PT_OFF_CS:
		*out = f->cs;
		break;
	case PT_OFF_EFLAGS:
		*out = f->eflags;
		break;
	case PT_OFF_ESP:
		*out = (unsigned long)f->esp;
		break;
	case PT_OFF_SS:
		*out = f->ss;
		break;
	default:
		return -EIO;
	}
	return 0;
}

/*
 * ptrace_set_reg - write one register in a stopped tracee's saved intr_frame.
 * Segment registers are left at user-mode values; EIP/ESP are rejected if
 * they point into kernel space; EFLAGS is masked to user-visible bits only
 * (preserving the current IF so interrupts stay enabled on resume).
 */
static int ptrace_set_reg(task_struct *tracee, unsigned long offset,
			  unsigned long val)
{
	intr_frame *f = TASK_INTR_FRAME(tracee);

	switch (offset) {
	case PT_OFF_EBX:
		f->ebx = val;
		break;
	case PT_OFF_ECX:
		f->ecx = val;
		break;
	case PT_OFF_EDX:
		f->edx = val;
		break;
	case PT_OFF_ESI:
		f->esi = val;
		break;
	case PT_OFF_EDI:
		f->edi = val;
		break;
	case PT_OFF_EBP:
		f->ebp = val;
		break;
	case PT_OFF_EAX:
		f->eax = val;
		break;
	case PT_OFF_EIP:
		if (val >= KERNEL_OFFSET)
			return -EIO;
		f->eip = (void (*)(void))val;
		break;
	case PT_OFF_EFLAGS:
		/* Allow only user-visible EFLAGS bits; keep the current IF */
		f->eflags = (f->eflags & ~0x4DD5U) | (val & 0x4DD5U);
		break;
	case PT_OFF_ESP:
		if (val >= KERNEL_OFFSET)
			return -EIO;
		f->esp = (void *)val;
		break;
	/* Silently ignore segment-register writes to avoid SIGSEGV on resume */
	case PT_OFF_DS:
	case PT_OFF_ES:
	case PT_OFF_FS:
	case PT_OFF_GS:
	case PT_OFF_CS:
	case PT_OFF_SS:
		break;
	default:
		return -EIO;
	}
	return 0;
}

/*
 * ptrace_stop - called from syscall_process when a traced task needs to pause
 * so that its tracer can inspect or modify it.
 *
 * 1. Sets PT_STOPPED and clears PT_STOP_REPORTED (fresh stop).
 * 2. Wakes the tracer (moves it to the ready queue) so its waitpid() loop
 *    sees the stopped child.
 * 3. Moves the current task to the wait queue and yields via task_sched().
 * 4. Returns when the tracer resumes us with PTRACE_CONT or PTRACE_SINGLESTEP
 *    (both clear PT_STOPPED and put us back on the ready queue).
 */
void ptrace_stop(intr_frame *frame)
{
	task_struct *cur = CURRENT_TASK();
	task_struct *tracer;

	cur->ptrace |= PT_STOPPED;
	cur->ptrace &= ~PT_STOP_REPORTED;

	/* Wake the tracer's waitpid() loop */
	tracer = cur->ptrace_tracer;
	if (tracer)
		ps_put_to_ready_queue(tracer);

	/* Block until the tracer calls PTRACE_CONT or PTRACE_SINGLESTEP */
	ps_put_to_wait_queue(cur, NULL, __func__);
	task_sched();
	/* PT_STOPPED and PT_STOP_REPORTED are cleared by the tracer */
}

/*
 * sys_ptrace - the ptrace(2) syscall implementation.
 *
 * Arguments come through the i386 syscall ABI: ebx=request, ecx=pid,
 * edx=addr, esi=data.  The syscall dispatcher casts them to unsigned long.
 */
int sys_ptrace(unsigned long request, unsigned long pid, unsigned long addr,
	       unsigned long data)
{
	task_struct *cur = CURRENT_TASK();
	task_struct *tracee;
	int ret = -1;

	switch (request) {
	/*
	 * PTRACE_TRACEME: child asks to be traced by its parent.
	 * Sets PT_TRACE_SYSCALL so the very next syscall entry triggers a stop,
	 * letting the parent see the process before it does any real work.
	 */
	case PTRACE_TRACEME:
		if (cur->ptrace & PT_TRACED) {
			ret = -EPERM;
			break;
		}

		cur->ptrace = PT_TRACED | PT_TRACE_SYSCALL;
		cur->ptrace_tracer = cur->parent;
		ret = 0;
		break;

	/*
	 * PTRACE_ATTACH: tracer attaches to an already-running process.
	 * Forces the tracee into the wait queue (stops it) immediately.
	 * The tracer should call waitpid() afterwards to synchronise.
	 */
	case PTRACE_ATTACH:
		tracee = ps_find_process(pid);
		if (!tracee || tracee->psid == cur->psid) {
			ret = -ESRCH;
			break;
		}

		if (tracee->ptrace & PT_TRACED) {
			ret = -EPERM;
			break;
		}

		tracee->ptrace = PT_TRACED | PT_STOPPED;
		tracee->ptrace_tracer = cur;
		ps_put_to_wait_queue(tracee, NULL, __func__);
		ret = 0;
		break;

	/*
	 * PTRACE_DETACH: tracer releases the tracee and resumes it.
	 */
	case PTRACE_DETACH:
		tracee = ps_find_process(pid);
		if (!tracee || tracee->ptrace_tracer != cur) {
			ret = -ESRCH;
			break;
		}

		tracee->ptrace = 0;
		tracee->ptrace_tracer = 0;
		ps_put_to_ready_queue(tracee);
		ret = 0;
		break;

	/*
	 * PTRACE_PEEKTEXT / PTRACE_PEEKDATA: read one word from the tracee's
	 * address space and return it directly as the syscall return value,
	 * as specified by ptrace(2): "PTRACE_PEEK* requests return the
	 * requested data".
	 */
	case PTRACE_PEEKTEXT:
	case PTRACE_PEEKDATA: {
		unsigned long word = 0;
		tracee = ps_find_process(pid);
		if (!tracee || tracee->ptrace_tracer != cur) {
			ret = -ESRCH;
			break;
		}

		if (!(tracee->ptrace & PT_STOPPED)) {
			ret = -EIO;
			break;
		}

		ret = ptrace_peek_mem(tracee, addr, &word);
		if (ret)
			break;

		*(unsigned long *)data = word;
		ret = (int)word;
		break;
	}

	/*
	 * PTRACE_POKETEXT / PTRACE_POKEDATA: write one word into the tracee's
	 * address space (e.g. to plant a breakpoint via INT3 = 0xCC).
	 */
	case PTRACE_POKETEXT:
	case PTRACE_POKEDATA:
		tracee = ps_find_process(pid);
		if (!tracee || tracee->ptrace_tracer != cur) {
			ret = -ESRCH;
			break;
		}

		if (!(tracee->ptrace & PT_STOPPED)) {
			ret = -EIO;
			break;
		}

		ret = ptrace_poke_mem(tracee, addr, data);
		break;

	/*
	 * PTRACE_PEEKUSER: read one register by its byte offset in the
	 * user_regs_struct layout (PT_OFF_* constants).
	 */
	case PTRACE_PEEKUSER: {
		unsigned long word = 0;
		tracee = ps_find_process(pid);
		if (!tracee || tracee->ptrace_tracer != cur) {
			ret = -ESRCH;
			break;
		}

		if (!(tracee->ptrace & PT_STOPPED)) {
			ret = -EIO;
			break;
		}

		if (addr >= PT_OFF_MAX) {
			ret = -EIO;
			break;
		}

		ret = ptrace_get_reg(tracee, addr, &word);
		if (ret)
			break;

		*(unsigned long *)data = word;
		ret = (int)word;
		break;
	}

	/*
	 * PTRACE_POKEUSER: write one register by byte offset.
	 */
	case PTRACE_POKEUSER:
		tracee = ps_find_process(pid);
		if (!tracee || tracee->ptrace_tracer != cur) {
			ret = -ESRCH;
			break;
		}

		if (!(tracee->ptrace & PT_STOPPED)) {
			ret = -EIO;
			break;
		}

		if (addr >= PT_OFF_MAX) {
			ret = -EIO;
			break;
		}

		ret = ptrace_set_reg(tracee, addr, data);
		break;

	/*
	 * PTRACE_GETREGS: copy the full user_regs_struct to *data.
	 */
	case PTRACE_GETREGS: {
		struct user_regs_struct regs;
		intr_frame *f;
		tracee = ps_find_process(pid);
		if (!tracee || tracee->ptrace_tracer != cur) {
			ret = -ESRCH;
			break;
		}

		if (!(tracee->ptrace & PT_STOPPED)) {
			ret = -EIO;
			break;
		}

		f = TASK_INTR_FRAME(tracee);
		regs.ebx = f->ebx;
		regs.ecx = f->ecx;
		regs.edx = f->edx;
		regs.esi = f->esi;
		regs.edi = f->edi;
		regs.ebp = f->ebp;
		regs.eax = f->eax;
		regs.xds = f->ds;
		regs.xes = f->es;
		regs.xfs = f->fs;
		regs.xgs = f->gs;
		regs.orig_eax = f->eax;
		regs.eip = (unsigned long)f->eip;
		regs.xcs = f->cs;
		regs.eflags = f->eflags;
		regs.esp = (unsigned long)f->esp;
		regs.xss = f->ss;
		memcpy((void *)data, &regs, sizeof(regs));
		ret = 0;
		break;
	}

	/*
	 * PTRACE_SETREGS: load the full user_regs_struct from *data.
	 * EIP and ESP are rejected if they point into kernel space.
	 * Only user-visible EFLAGS bits are updated.
	 */
	case PTRACE_SETREGS: {
		struct user_regs_struct regs;
		intr_frame *f;
		tracee = ps_find_process(pid);
		if (!tracee || tracee->ptrace_tracer != cur) {
			ret = -ESRCH;
			break;
		}

		if (!(tracee->ptrace & PT_STOPPED)) {
			ret = -EIO;
			break;
		}

		memcpy(&regs, (const void *)data, sizeof(regs));
		f = TASK_INTR_FRAME(tracee);
		f->ebx = regs.ebx;
		f->ecx = regs.ecx;
		f->edx = regs.edx;
		f->esi = regs.esi;
		f->edi = regs.edi;
		f->ebp = regs.ebp;
		f->eax = regs.eax;
		f->ds = regs.xds;
		f->es = regs.xes;
		f->fs = regs.xfs;
		f->gs = regs.xgs;
		if (regs.eip < KERNEL_OFFSET)
			f->eip = (void (*)(void))regs.eip;
		f->eflags = (f->eflags & ~0x4DD5U) | (regs.eflags & 0x4DD5U);
		if (regs.esp < KERNEL_OFFSET)
			f->esp = (void *)regs.esp;
		ret = 0;
		break;
	}

	/*
	 * PTRACE_CONT: resume the tracee normally (no syscall stops).
	 * If a signal number is passed in data it is silently ignored since
	 * this kernel has no signal delivery yet.
	 */
	case PTRACE_CONT:
		tracee = ps_find_process(pid);
		if (!tracee || tracee->ptrace_tracer != cur) {
			ret = -ESRCH;
			break;
		}

		if (!(tracee->ptrace & PT_STOPPED)) {
			ret = -EIO;
			break;
		}

		/* Clear single-step TF if it was set from a prior SINGLESTEP */
		if (tracee->ptrace & PT_SINGLESTEP) {
			TASK_INTR_FRAME(tracee)->eflags &= ~EFLAGS_TF;
			tracee->ptrace &= ~PT_SINGLESTEP;
		}
		tracee->ptrace &=
			~(PT_STOPPED | PT_STOP_REPORTED | PT_TRACE_SYSCALL);
		ps_put_to_ready_queue(tracee);
		ret = 0;
		break;

	/*
	 * PTRACE_SINGLESTEP: resume the tracee with the CPU Trap Flag set.
	 * The hardware raises a debug exception (INT 1) after exactly one
	 * instruction; a debug-exception handler would call ptrace_stop()
	 * again to let the tracer see the new state.
	 */
	case PTRACE_SINGLESTEP:
		tracee = ps_find_process(pid);
		if (!tracee || tracee->ptrace_tracer != cur) {
			ret = -ESRCH;
			break;
		}

		if (!(tracee->ptrace & PT_STOPPED)) {
			ret = -EIO;
			break;
		}

		TASK_INTR_FRAME(tracee)->eflags |= EFLAGS_TF;
		tracee->ptrace |= PT_SINGLESTEP;
		tracee->ptrace &=
			~(PT_STOPPED | PT_STOP_REPORTED | PT_TRACE_SYSCALL);
		ps_put_to_ready_queue(tracee);
		ret = 0;
		break;

	/*
	 * PTRACE_SYSCALL: resume the tracee and stop it at the next syscall
	 * entry or exit.  Identical to PTRACE_CONT except PT_TRACE_SYSCALL is
	 * kept set so that syscall_process() calls ptrace_stop() on the next
	 * syscall boundary.
	 */
	case PTRACE_SYSCALL:
		tracee = ps_find_process(pid);
		if (!tracee || tracee->ptrace_tracer != cur) {
			ret = -ESRCH;
			break;
		}

		if (!(tracee->ptrace & PT_STOPPED)) {
			ret = -EIO;
			break;
		}

		/* Clear single-step TF if it was set from a prior SINGLESTEP */
		if (tracee->ptrace & PT_SINGLESTEP) {
			TASK_INTR_FRAME(tracee)->eflags &= ~EFLAGS_TF;
			tracee->ptrace &= ~PT_SINGLESTEP;
		}
		tracee->ptrace &= ~(PT_STOPPED | PT_STOP_REPORTED);
		tracee->ptrace |= PT_TRACE_SYSCALL;
		ps_put_to_ready_queue(tracee);
		ret = 0;
		break;

	/*
	 * PTRACE_KILL: terminate the tracee by moving it directly to the
	 * dying queue, as if it had called exit().
	 */
	case PTRACE_KILL:
		tracee = ps_find_process(pid);
		if (!tracee || tracee->ptrace_tracer != cur) {
			ret = -ESRCH;
			break;
		}

		tracee->ptrace = 0;
		tracee->ptrace_tracer = 0;
		ps_put_to_dying_queue(tracee);
		ret = 0;
		break;

	default:
		ret = -EIO;
		break;
	}

	if (TestControl.verbos)
		klog("%d: ptrace(%d, %d, %d, %x) = %d\n", cur->psid, request,
		     pid, addr, data, ret);

	return ret;
}
