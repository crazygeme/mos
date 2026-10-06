#include <ps/ps.h>
#include <ps/signal.h>
#include "signal_frame.h"
#include <int/int.h>
#include <lib/klib.h>
#include <errno.h>
#include <macro.h>
/*
 * Linux/i386 rt_sigaction uses the kernel layout:
 *   handler, flags, restorer, mask
 *
 * glibc 2.3.2 passes sigsetsize = 8 on this ABI, so the kernel-visible mask
 * payload is just two 32-bit words even though libc's userspace sigset_t is
 * larger. We only support signals 1..31, so we preserve the low word and
 * clear the high word on writeback.
 */
struct rt_sigaction_user {
	uint32_t sa_handler;
	uint32_t sa_flags;
	uint32_t sa_restorer;
	uint32_t sa_mask[2];
};

typedef struct _rt_siginfo_user {
	int si_signo;
	int si_errno;
	int si_code;
	int _pad[(128 / sizeof(int)) - 3];
} rt_siginfo_user;

typedef struct _rt_sigcontext_user {
	unsigned short gs, __gsh;
	unsigned short fs, __fsh;
	unsigned short es, __esh;
	unsigned short ds, __dsh;
	uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
	uint32_t trapno;
	uint32_t err;
	uint32_t eip;
	unsigned short cs, __csh;
	uint32_t eflags;
	uint32_t esp_at_signal;
	unsigned short ss, __ssh;
	uint32_t fpstate;
	uint32_t oldmask;
	uint32_t cr2;
} rt_sigcontext_user;

typedef struct _rt_ucontext_user {
	uint32_t uc_flags;
	uint32_t uc_link;
	struct {
		uint32_t ss_sp;
		int32_t ss_flags;
		uint32_t ss_size;
	} uc_stack;
	rt_sigcontext_user uc_mcontext;
	sigset_t uc_sigmask;
} rt_ucontext_user;

typedef struct _rt_fpreg_user {
	unsigned short significand[4];
	unsigned short exponent;
} rt_fpreg_user;

typedef struct _rt_fpstate_user {
	uint32_t cw;
	uint32_t sw;
	uint32_t tag;
	uint32_t ipoff;
	uint32_t cssel;
	uint32_t dataoff;
	uint32_t datasel;
	rt_fpreg_user _st[8];
	uint32_t status;
} rt_fpstate_user;

typedef struct _rt_signal_frame {
	uint32_t pretcode;
	int sig;
	uint32_t pinfo;
	uint32_t puc;
	rt_siginfo_user info;
	rt_ucontext_user uc;
	rt_fpstate_user fpstate;
	unsigned char retcode[8];
} rt_signal_frame;

int sys_rt_sigaction(int sig, void *act, void *oact, unsigned sigsetsize)
{
	task_struct *cur = CURRENT_TASK();
	struct sigaction *sa;

	if (sig <= 0 || sig >= NSIG)
		return -EINVAL;
	if (sig == SIGKILL || sig == SIGSTOP)
		return -EINVAL;
	if (sigsetsize != 8)
		return -EINVAL;

	sa = &cur->signal->sig_handlers[sig];

	if (oact) {
		struct rt_sigaction_user *u = (struct rt_sigaction_user *)oact;
		u->sa_handler = (uint32_t)(uintptr_t)sa->sa_handler;
		u->sa_flags = sa->sa_flags;
		u->sa_restorer = (uint32_t)(uintptr_t)sa->sa_restorer;
		u->sa_mask[0] = sa->sa_mask;
		u->sa_mask[1] = 0;
	}
	if (act) {
		struct rt_sigaction_user *u = (struct rt_sigaction_user *)act;
		sa->sa_handler = (void *)(uintptr_t)u->sa_handler;
		sa->sa_flags = u->sa_flags;
		sa->sa_restorer = (void *)(uintptr_t)u->sa_restorer;
		sa->sa_mask = (unsigned long)u->sa_mask[0];
	}

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("rt_sigaction(%d, %x, %x)\n", sig, act, oact);

	return 0;
}

/*
 * sys_sigreturn — restore user context after a signal handler returns.
 *
 * The signal handler's trampoline called "int $0x80" with eax=119 after
 * the handler's "ret" popped return_addr from the stack.  At that point:
 *   user ESP = signal_frame_base + 4   (return_addr has been popped)
 * so the saved frame is at (frame->esp - 4).
 */
int sys_sigreturn()
{
	task_struct *cur = CURRENT_TASK();
	intr_frame *frame = (intr_frame *)((char *)cur + KERNEL_TASK_BYTES -
					   sizeof(intr_frame));
	signal_frame *sf = (signal_frame *)((unsigned char *)frame->esp - 8);

	/* Restore original user registers and EIP into the interrupt frame. */
	frame->eip = (void *)(uintptr_t)sf->saved_eip;
	frame->eflags = sf->saved_eflags;
	frame->esp = (void *)(uintptr_t)sf->saved_esp;
	frame->eax = sf->saved_eax;
	frame->ebx = sf->saved_ebx;
	frame->ecx = sf->saved_ecx;
	frame->edx = sf->saved_edx;
	frame->esi = sf->saved_esi;
	frame->edi = sf->saved_edi;
	frame->ebp = sf->saved_ebp;
	frame->ds = sf->saved_ds;
	frame->es = sf->saved_es;
	frame->fs = sf->saved_fs;
	frame->gs = sf->saved_gs;

	/* Restore the signal mask saved before delivery. */
	cur->signal->sig_mask = sf->saved_mask;

	/*
	 * If we delivered on the altstack, check whether the restored esp
	 * falls outside the altstack range; if so we're leaving it.
	 */

	stack_t *alt = &cur->signal->altstack;
	if (alt->ss_flags & SS_ONSTACK) {
		uintptr_t alt_base = (uintptr_t)alt->ss_sp;
		uintptr_t alt_top = alt_base + alt->ss_size;
		uintptr_t restored = (uintptr_t)sf->saved_esp;
		if (restored < alt_base || restored >= alt_top)
			alt->ss_flags &= ~SS_ONSTACK;
	}

	/*
	 * Return the original eax so that the interrupted syscall's result
	 * is preserved when execution resumes.  syscall_process will write
	 * this into frame->eax, but do_signal (called afterwards) will see
	 * the restored frame and may deliver the next queued signal.
	 */
	return (int)sf->saved_eax;
}

int sys_rt_sigreturn()
{
	task_struct *cur = CURRENT_TASK();
	intr_frame *frame = (intr_frame *)((char *)cur + KERNEL_TASK_BYTES -
					   sizeof(intr_frame));
	rt_signal_frame *sf =
		(rt_signal_frame *)((unsigned char *)frame->esp - 4);
	rt_sigcontext_user *sc = &sf->uc.uc_mcontext;

	frame->eip = (void *)(uintptr_t)sc->eip;
	frame->eflags = sc->eflags;
	frame->esp = (void *)(uintptr_t)sc->esp_at_signal;
	frame->eax = sc->eax;
	frame->ebx = sc->ebx;
	frame->ecx = sc->ecx;
	frame->edx = sc->edx;
	frame->esi = sc->esi;
	frame->edi = sc->edi;
	frame->ebp = sc->ebp;
	frame->ds = sc->ds;
	frame->es = sc->es;
	frame->fs = sc->fs;
	frame->gs = sc->gs;
	cur->signal->sig_mask = sf->uc.uc_sigmask;

	stack_t *alt = &cur->signal->altstack;
	if (alt->ss_flags & SS_ONSTACK) {
		unsigned alt_base = (unsigned)(uintptr_t)alt->ss_sp;
		unsigned alt_top = alt_base + alt->ss_size;
		unsigned restored = sc->esp_at_signal;
		if (restored < alt_base || restored >= alt_top)
			alt->ss_flags &= ~SS_ONSTACK;
	}

	return (int)sc->eax;
}

static void build_rt_sigreturn_code(unsigned char retcode[8])
{
	retcode[0] = 0xb8; /* mov imm32,%eax */
	*(unsigned int *)&retcode[1] = 173;
	retcode[5] = 0xcd; /* int $0x80 */
	retcode[6] = 0x80;
	retcode[7] = 0x90; /* nop */
}

static void build_sigreturn_code(unsigned char retcode[8])
{
	/*
	 * Match Linux/glibc i386 __restore:
	 *   popl %eax         ; discard signo argument
	 *   mov $__NR_sigreturn,%eax
	 *   int $0x80
	 */
	retcode[0] = 0x58; /* popl %eax */
	retcode[1] = 0xb8; /* mov imm32,%eax */
	*(unsigned int *)&retcode[2] = 119;
	retcode[6] = 0xcd; /* int $0x80 */
	retcode[7] = 0x80;
}

/* Resolve the stack pointer to use for signal frame delivery. */
static unsigned char *resolve_sigstack(task_struct *cur, intr_frame *frame,
				       struct sigaction *sa)
{
	if ((sa->sa_flags & SA_ONSTACK) && cur->signal->altstack.ss_sp &&
	    !(cur->signal->altstack.ss_flags & SS_DISABLE) &&
	    !(cur->signal->altstack.ss_flags & SS_ONSTACK)) {
		cur->signal->altstack.ss_flags |= SS_ONSTACK;
		return (unsigned char *)cur->signal->altstack.ss_sp +
		       cur->signal->altstack.ss_size;
	}
	return (unsigned char *)frame->esp;
}

/* Build the rt (SA_SIGINFO) signal frame on the user stack. */
static void build_rt_frame(task_struct *cur, intr_frame *frame,
			   rt_signal_frame *rt_sf, struct sigaction *sa,
			   int sig, const struct signal_fault *fault)
{
	rt_sigcontext_user *sc = &rt_sf->uc.uc_mcontext;
	unsigned long saved_mask = cur->signal->restore_sigmask ?
					   cur->signal->saved_sigmask :
					   cur->signal->sig_mask;

	if ((sa->sa_flags & SA_RESTORER) && sa->sa_restorer) {
		rt_sf->pretcode = (arch_reg_t)(uintptr_t)sa->sa_restorer;
	} else {
		build_rt_sigreturn_code(rt_sf->retcode);
		rt_sf->pretcode = (unsigned int)(uintptr_t)&rt_sf->retcode[0];
	}
	rt_sf->sig = sig;
	rt_sf->pinfo = (arch_reg_t)(uintptr_t)&rt_sf->info;
	rt_sf->puc = (arch_reg_t)(uintptr_t)&rt_sf->uc;
	memset(&rt_sf->info, 0, sizeof(rt_sf->info));
	rt_sf->info.si_signo = sig;
	if (fault) {
		rt_sf->info.si_code = fault->code;
		rt_sf->info._pad[0] = (uint32_t)fault->address;
	}
	rt_sf->uc.uc_flags = 0;
	rt_sf->uc.uc_link = NULL;
	rt_sf->uc.uc_stack.ss_sp =
		(uint32_t)(uintptr_t)cur->signal->altstack.ss_sp;
	rt_sf->uc.uc_stack.ss_flags = cur->signal->altstack.ss_flags;
	rt_sf->uc.uc_stack.ss_size = cur->signal->altstack.ss_size;
	rt_sf->uc.uc_sigmask = saved_mask;
	memset(&rt_sf->fpstate, 0, sizeof(rt_sf->fpstate));
	memset(sc, 0, sizeof(*sc));
	sc->gs = frame->gs;
	sc->fs = frame->fs;
	sc->es = frame->es;
	sc->ds = frame->ds;
	sc->edi = frame->edi;
	sc->esi = frame->esi;
	sc->ebp = frame->ebp;
	sc->esp = (arch_reg_t)(uintptr_t)frame->esp;
	sc->ebx = frame->ebx;
	sc->edx = frame->edx;
	sc->ecx = frame->ecx;
	sc->eax = frame->eax;
	sc->trapno = fault ? fault->trap : 0;
	sc->err = frame->error_code;
	sc->eip = (arch_reg_t)(uintptr_t)frame->eip;
	sc->cs = frame->cs;
	sc->eflags = frame->eflags;
	sc->esp_at_signal = (arch_reg_t)(uintptr_t)frame->esp;
	sc->ss = frame->ss;
	sc->fpstate = (uint32_t)(uintptr_t)&rt_sf->fpstate;
	sc->oldmask = saved_mask;
	sc->cr2 = fault ? (uint32_t)fault->address : 0;
	cur->signal->restore_sigmask = 0;
}

/* Build the legacy signal frame on the user stack. */
static void build_legacy_frame(task_struct *cur, intr_frame *frame,
			       signal_frame *sf, struct sigaction *sa, int sig)
{
	if ((sa->sa_flags & SA_RESTORER) && sa->sa_restorer) {
		sf->return_addr = (arch_reg_t)(uintptr_t)sa->sa_restorer;
		memset(sf->trampoline, 0, sizeof(sf->trampoline));
	} else {
		build_sigreturn_code(sf->trampoline);
		sf->return_addr = (arch_reg_t)(uintptr_t)&sf->trampoline[0];
	}
	sf->signo = sig;
	sf->saved_eip = (arch_reg_t)(uintptr_t)frame->eip;
	sf->saved_eflags = frame->eflags;
	sf->saved_esp = (arch_reg_t)(uintptr_t)frame->esp;
	sf->saved_eax = frame->eax;
	sf->saved_ebx = frame->ebx;
	sf->saved_ecx = frame->ecx;
	sf->saved_edx = frame->edx;
	sf->saved_esi = frame->esi;
	sf->saved_edi = frame->edi;
	sf->saved_ebp = frame->ebp;
	sf->saved_ds = frame->ds;
	sf->saved_es = frame->es;
	sf->saved_fs = frame->fs;
	sf->saved_gs = frame->gs;
	/*
	 * If inside sigsuspend, save the pre-sigsuspend mask so sigreturn
	 * restores it (not the temporary sigsuspend mask).
	 */
	sf->saved_mask = cur->signal->restore_sigmask ?
				 cur->signal->saved_sigmask :
				 cur->signal->sig_mask;
	cur->signal->restore_sigmask = 0;
}

void i386_signal_deliver(task_struct *cur, intr_frame *frame,
			 struct sigaction *sa, int sig,
			 const struct signal_fault *fault)
{
	unsigned char *new_esp;
	void (*handler)(int);
	int is_rt;
	/*
	 * Linux/i386 chooses the rt signal frame based on SA_SIGINFO, not on
	 * whether the handler was installed via rt_sigaction().
	 */
	is_rt = (sa->sa_flags & SA_SIGINFO) != 0;

	new_esp = resolve_sigstack(cur, frame, sa);
	new_esp -= is_rt ? sizeof(rt_signal_frame) : sizeof(signal_frame);
	new_esp =
		(unsigned char *)(uintptr_t)((uintptr_t)new_esp &
					     ~(uintptr_t)0xf); /* 16-byte align */

	if (is_rt)
		build_rt_frame(cur, frame, (rt_signal_frame *)new_esp, sa, sig,
			       fault);
	else
		build_legacy_frame(cur, frame, (signal_frame *)new_esp, sa,
				   sig);

	/* Block this signal while handler runs (unless SA_NODEFER). */
	if (!(sa->sa_flags & SA_NODEFER))
		cur->signal->sig_mask |= (1UL << (sig - 1));
	cur->signal->sig_mask |= sa->sa_mask;

	handler = sa->sa_handler;
	if (sa->sa_flags & SA_RESETHAND)
		sa->sa_handler = SIG_DFL;

	frame->eip = (void *)handler;
	frame->esp = (void *)new_esp;

	if (TEST_LOG(TEST_LOG_TRACE))
		klog("sig_deliver(%d)\n", sig);
}
