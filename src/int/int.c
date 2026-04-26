#include <int/int.h>
#include <ps/ps.h>
#include <mm/mm.h>
#include <lib/port.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <macro.h>
#include <errno.h>
extern void do_signal(intr_frame *frame);
extern unsigned long long gdt[];

/* Sends an end-of-interrupt signal to the PIC for the given IRQ.
If we don't acknowledge the IRQ, it will never be delivered to
us again, so this is important. */
static void pic_end_of_interrupt(int irq)
{
	/* Acknowledge master PIC. */
	port_write_byte(0x20, 0x20);

	/* Acknowledge slave PIC if this is a slave interrupt. */
	if (irq >= 0x28)
		port_write_byte(0xa0, 0x20);
}

static int_callback in_callbacks[IDT_SIZE];

void int_register(int vec_no, int_callback fn, int is_trap, int dpl)
{
	unsigned long f = 0;
	if (vec_no < 0 || vec_no >= IDT_SIZE)
		return;

	f = intr_stubs[vec_no];
	if (is_trap) {
		idt[vec_no] = MAKE_TRAP_GATE(f, dpl);
	} else {
		idt[vec_no] = MAKE_INTR_GATE(f, dpl);
	}

	in_callbacks[vec_no] = fn;
}

void int_unregister(int vec_no)
{
	if (vec_no < 0 || vec_no >= IDT_SIZE) {
		return;
	}

	in_callbacks[vec_no] = 0;
	idt[vec_no] = 0;
}
static void intr_maybe_preempt(void)
{
	task_struct *cur = CURRENT_TASK();

	if (cur->psid == 0xffffffff || !ps_enabled())
		return;

	if (sched_is_enabled() && cur->remain_ticks <= 0) {
		cur->stats->niv_switches++;
		cur->remain_ticks = DEFAULT_TASK_TIME_SLICE;
		int_intr_enable();
		task_sched();
		int_intr_disable();
	}
}

static void intr_sanitize_user_return(intr_frame *frame)
{
	task_struct *cur = CURRENT_TASK();
	const unsigned user_eflags_clear = 0x00054000U;

	if (cur->psid == 0xffffffff || !ps_enabled())
		return;

	/*
	 * TLS slots 6..8 live in the shared CPU GDT, but their contents are
	 * per-task. If kernel execution returns to user mode without taking the
	 * normal scheduler path that reloads them, "pop %gs" in intr_exit can
	 * resolve selector 0x33 against stale or zeroed descriptors and fault.
	 * This can happen even on a nested kernel interrupt, because MOS keeps
	 * the current task's user %gs live while executing in ring 0. Refresh
	 * them on every interrupt/syscall exit so the live GDT always matches
	 * the current task before any saved %gs is restored.
	 */
	ps_load_task_segments(cur);

	if (frame->cs != USER_CODE_SELECTOR)
		return;

	/*
	 * Never reflect privileged/special control bits back into ring 3.
	 * RF/NT/AC/VM are not part of normal user execution for MOS and can
	 * produce hard-to-explain traps in freshly exec'd helper processes.
	 */
	frame->eflags &= ~user_eflags_clear;

	/*
	 * Restore the task's requested IOPL on every return to user mode.
	 * XFree86 uses iopl(3) on i386 to reach PCI config ports such as
	 * 0xcf8/0xcfc. We still recompute the flags here so preemption,
	 * signals, and nested syscalls do not silently drop that request.
	 */
	frame->eflags &= ~0x3000U;
	frame->eflags |= ((unsigned)(cur->io_priv_level & 0x3) << 12);
}

static void intr_prepare_user_return(intr_frame *frame)
{
	intr_sanitize_user_return(frame);

	/* Deliver pending signals when returning to user space.
	 * This ensures alarm() and kill() work even in tight user-space loops. */
	do_signal(frame);
}

void intr_handler(intr_frame *frame)
{
	int external = frame->vec_no >= 0x20 && frame->vec_no < 0x30;
	int_callback fn = 0;

	if (frame->vec_no < 0 || frame->vec_no >= IDT_SIZE) {
		return;
	}

	fn = in_callbacks[frame->vec_no];
	if (fn)
		fn(frame);

	if (external) {
		pic_end_of_interrupt(frame->vec_no);
	}

	/*
	 * Page faults are part of normal demand paging and COW, so avoid the
	 * heavier preemption/signal checks on every #PF return.  We still scrub
	 * the user-visible EFLAGS image and restore IOPL before iret.
	 */
	if (frame->vec_no == 0x0e) {
		intr_sanitize_user_return(frame);
		return;
	}

	intr_maybe_preempt();
	intr_prepare_user_return(frame);
}

void intr_syscall_handler(intr_frame *frame)
{
	int_callback fn = 0;
	task_struct *cur = CURRENT_TASK();
	unsigned long long start = 0;
	unsigned long long idle_start = cur->stats->idle_tickets;
	fn = in_callbacks[frame->vec_no];
	if (fn) {
		start = time_now_tickets();
		fn(frame);
		cur->stats->kernel_tickets +=
			time_now_tickets() - start -
			(cur->stats->idle_tickets - idle_start);
	}

	intr_prepare_user_return(frame);
}

static void handle_invalid_opcode(intr_frame *frame)
{
	sys_exit(-EFAULT);
}

static void handle_general_protection(intr_frame *frame)
{
	task_struct *cur = CURRENT_TASK();
	unsigned long long tls0 = 0;
	unsigned long long tls1 = 0;
	unsigned long long tls2 = 0;

	if (cur->user) {
		tls0 = cur->user->tls_desc[0];
		tls1 = cur->user->tls_desc[1];
		tls2 = cur->user->tls_desc[2];
	}

	klog("#GP happens for pid %d, command %s, eip %x, esp %x, ebp %x, eax %x, ebx %x, ecx %x, edx %x, ds %x, cs %x, gs %x, fs %x, error_code %x\n",
	     cur->psid, cur->user->command, frame->eip, frame->esp, frame->ebp,
	     frame->eax, frame->ebx, frame->ecx, frame->edx, frame->ds,
	     frame->cs, frame->gs, frame->fs, frame->error_code);
	klog("#GP state: status %d, ptrace_mode %u, stop_signal %u, ptrace_frame_valid %u, tss.gs %x, ptrace.gs %x, ptrace.eip %x, ptrace.esp %x\n",
	     cur->status, cur->ptrace_mode, cur->stop_signal,
	     cur->ptrace_frame_valid, cur->tss.gs, cur->ptrace_frame.gs,
	     cur->ptrace_frame.eip, cur->ptrace_frame.esp);
	klog("#GP tls: slot6 %x:%x slot7 %x:%x slot8 %x:%x\n",
	     (unsigned)(tls0 >> 32), (unsigned)tls0, (unsigned)(tls1 >> 32),
	     (unsigned)tls1, (unsigned)(tls2 >> 32), (unsigned)tls2);
	klog("#GP gdt: slot6 %x:%x slot7 %x:%x slot8 %x:%x\n",
	     (unsigned)(gdt[GDT_ENTRY_TLS_MIN + 0] >> 32),
	     (unsigned)gdt[GDT_ENTRY_TLS_MIN + 0],
	     (unsigned)(gdt[GDT_ENTRY_TLS_MIN + 1] >> 32),
	     (unsigned)gdt[GDT_ENTRY_TLS_MIN + 1],
	     (unsigned)(gdt[GDT_ENTRY_TLS_MIN + 2] >> 32),
	     (unsigned)gdt[GDT_ENTRY_TLS_MIN + 2]);
	/* Kill user tasks cleanly so userspace faults surface in krn.log. */
	sys_exit(-EFAULT);
	return;
}

void int_enable_all(void)
{
	int i = 0;
	unsigned long long idtr = 0;
	unsigned long long gdtr = 0;

	idtr = MAKE_IDTR_OPERAND(idt_size - 1, idt);
	gdtr = MAKE_GDTR_OPERAND(gdt_size - 1, gdt);
	SET_IDT(idtr);
	SET_GDT(gdtr);
	SET_CS(KERNEL_CODE_SELECTOR);
	SET_DS(KERNEL_DATA_SELECTOR);

	port_write_byte(0x21, 0x0);
	port_write_byte(0xA1, 0x0);

	for (i = 0; i < IDT_SIZE; i++) {
		in_callbacks[i] = 0;
	}
	int_register(6, handle_invalid_opcode, 0, 3);
	int_register(INT_VECTOR_PROTECTION, handle_general_protection, 0, 0);
}

/*
 * address should be a physical address
 */
void int_update_tss(void *address)
{
	unsigned int base = (unsigned int)address;
	gdt[TSS_SELECTOR / 8] = MAKE_SEG_DESC(base, TSS_SEG_LIMIT,
					      SEG_CLASS_SYSTEM, 9,
					      KERNEL_PRIVILEGE, SEG_BASE_1);
	SET_TSS(TSS_SELECTOR);
}
