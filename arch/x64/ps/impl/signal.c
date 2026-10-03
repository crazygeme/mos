#include <ps/ps.h>
#include <ps/impl/ps_internal.h>
#include <ps/smp.h>
#include <int/int.h>
#include <lib/klib.h>
#include <errno.h>
#include <syscall/impl/syscall_internal.h>

struct native_stack {
	uint64_t sp;
	int32_t flags;
	uint32_t pad;
	uint64_t size;
};
struct native_action {
	uint64_t handler, flags, restorer, mask;
};
struct native_context {
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, flags;
	uint16_t cs, gs, fs, ss;
	uint64_t error, trap, oldmask, cr2, fpstate, reserved[8];
};
struct native_ucontext {
	uint64_t flags, link;
	struct native_stack stack;
	struct native_context context;
	uint64_t mask;
};
struct native_signal_frame {
	uint64_t restorer;
	struct native_ucontext uc;
	unsigned char info[128];
};
_Static_assert(sizeof(struct native_context) == 256, "AMD64 sigcontext layout");
_Static_assert(sizeof(struct native_ucontext) == 304, "AMD64 ucontext layout");
int native_sigaction(int sig, const void *input, void *output, unsigned size)
{
	if (size != 8 || sig <= 0 || sig >= NSIG || sig == SIGKILL ||
	    sig == SIGSTOP)
		return -EINVAL;
	struct sigaction *action = &current->signal->sig_handlers[sig];
	if (output) {
		struct native_action *out = output;
		*out = (struct native_action){ (uintptr_t)action->sa_handler,
					       action->sa_flags,
					       (uintptr_t)action->sa_restorer,
					       action->sa_mask };
	}
	if (input) {
		const struct native_action *in = input;
		if (in->handler > 1 &&
		    (!(in->flags & SA_RESTORER) || !in->restorer))
			return -EINVAL;
		if (in->handler >= MOS_NATIVE_TASK_SIZE ||
		    in->restorer >= MOS_NATIVE_TASK_SIZE)
			return -EFAULT;
		action->sa_handler = (void *)(uintptr_t)in->handler;
		action->sa_flags = in->flags;
		action->sa_restorer = (void *)(uintptr_t)in->restorer;
		action->sa_mask = in->mask;
	}
	return 0;
}
int native_sigaltstack(const void *input, void *output)
{
	stack_t stack, old;
	if (input) {
		const struct native_stack *in = input;
		if (in->size > 0xffffffffULL ||
		    in->sp >= MOS_NATIVE_TASK_SIZE ||
		    in->size > MOS_NATIVE_TASK_SIZE - in->sp)
			return -ENOMEM;
		stack.ss_sp = (void *)(uintptr_t)in->sp;
		stack.ss_flags = in->flags;
		stack.ss_size = in->size;
	}
	int ret = sys_sigaltstack(input ? &stack : NULL, output ? &old : NULL);
	if (!ret && output)
		*(struct native_stack *)output =
			(struct native_stack){ (uintptr_t)old.ss_sp,
					       old.ss_flags, 0, old.ss_size };
	return ret;
}
void arch_signal_deliver_native(task_struct *task, intr_frame *frame,
				struct sigaction *action, int sig)
{
	struct native_signal_frame saved;
	memset(&saved, 0, sizeof(saved));
	uintptr_t sp = (uintptr_t)frame->esp;
	stack_t *alt = &task->signal->altstack;
	if ((action->sa_flags & SA_ONSTACK) &&
	    !(alt->ss_flags & (SS_DISABLE | SS_ONSTACK)))
		sp = (uintptr_t)alt->ss_sp + alt->ss_size;
	else
		sp -= 128; /* Preserve the AMD64 red zone. */
	uintptr_t fp = (sp - 512) & ~(uintptr_t)15;
	sp = ((fp - sizeof(saved)) & ~(uintptr_t)15) - 8;
	if (sp >= MOS_NATIVE_TASK_SIZE || !action->sa_restorer) {
		do_exit(SIGSEGV);
		return;
	}
	saved.restorer = (uintptr_t)action->sa_restorer;
	saved.uc.stack = (struct native_stack){ (uintptr_t)alt->ss_sp,
						alt->ss_flags, 0,
						alt->ss_size };
	saved.uc.mask = task->signal->restore_sigmask ?
				task->signal->saved_sigmask :
				task->signal->sig_mask;
	struct native_context *sc = &saved.uc.context;
#define SAVE(dst, src) sc->dst = frame->src
	SAVE(r8, r8);
	SAVE(r9, r9);
	SAVE(r10, r10);
	SAVE(r11, r11);
	SAVE(r12, r12);
	SAVE(r13, r13);
	SAVE(r14, r14);
	SAVE(r15, r15);
	SAVE(rdi, edi);
	SAVE(rsi, esi);
	SAVE(rbp, ebp);
	SAVE(rbx, ebx);
	SAVE(rdx, edx);
	SAVE(rax, eax);
	SAVE(rcx, ecx);
	SAVE(flags, eflags);
#undef SAVE
	sc->rsp = (uintptr_t)frame->esp;
	sc->rip = (uintptr_t)frame->eip;
	sc->cs = USER64_CODE_SELECTOR;
	sc->ss = USER_DATA_SELECTOR;
	sc->fpstate = fp;
	sc->oldmask = saved.uc.mask;
	sc->error = frame->error_code;
	*(int *)saved.info = sig;
	smp_fpu_save(task);
	if (ps_write_process_memory(task, (void *)fp, task->user->fpu, 512) <
		    0 ||
	    ps_write_process_memory(task, (void *)sp, &saved, sizeof(saved)) <
		    0) {
		do_exit(SIGSEGV);
		return;
	}
	task->signal->restore_sigmask = 0;
	if (sp >= (uintptr_t)alt->ss_sp &&
	    sp < (uintptr_t)alt->ss_sp + alt->ss_size)
		alt->ss_flags |= SS_ONSTACK;
	if (!(action->sa_flags & SA_NODEFER))
		task->signal->sig_mask |= 1U << (sig - 1);
	task->signal->sig_mask |= action->sa_mask;
	frame->edi = sig;
	frame->esi = sp + __builtin_offsetof(struct native_signal_frame, info);
	frame->edx = sp + __builtin_offsetof(struct native_signal_frame, uc);
	frame->eax = 0;
	frame->eip = (void *)action->sa_handler;
	frame->esp = (void *)sp;
	if (action->sa_flags & SA_RESETHAND)
		action->sa_handler = SIG_DFL;
}
intptr_t native_sigreturn(intr_frame *frame)
{
	struct native_ucontext saved;
	if (ps_read_process_memory(current, frame->esp, &saved, sizeof(saved)) <
	    0)
		goto bad;
	struct native_context *sc = &saved.context;
	if (sc->rip >= MOS_NATIVE_TASK_SIZE ||
	    sc->rsp >= MOS_NATIVE_TASK_SIZE || sc->cs != USER64_CODE_SELECTOR ||
	    sc->ss != USER_DATA_SELECTOR)
		goto bad;
#define RESTORE(dst, src) frame->dst = sc->src
	RESTORE(r8, r8);
	RESTORE(r9, r9);
	RESTORE(r10, r10);
	RESTORE(r11, r11);
	RESTORE(r12, r12);
	RESTORE(r13, r13);
	RESTORE(r14, r14);
	RESTORE(r15, r15);
	RESTORE(edi, rdi);
	RESTORE(esi, rsi);
	RESTORE(ebp, rbp);
	RESTORE(ebx, rbx);
	RESTORE(edx, rdx);
	RESTORE(eax, rax);
	RESTORE(ecx, rcx);
#undef RESTORE
	frame->eip = (void *)(uintptr_t)sc->rip;
	frame->esp = (void *)(uintptr_t)sc->rsp;
	frame->eflags = (sc->flags & 0x250dd5) | 0x202;
	current->signal->sig_mask =
		saved.mask & ~((1U << (SIGKILL - 1)) | (1U << (SIGSTOP - 1)));
	if (sc->fpstate) {
		if (ps_read_process_memory(current,
					   (void *)(uintptr_t)sc->fpstate,
					   current->user->fpu, 512) < 0)
			goto bad;
		/* Unsupported MXCSR bits cause #GP in FXRSTOR. */
		*(uint32_t *)(current->user->fpu + 24) &= 0xffbf;
		smp_fpu_restore(current);
	}
	stack_t *alt = &current->signal->altstack;
	if (sc->rsp < (uintptr_t)alt->ss_sp ||
	    sc->rsp >= (uintptr_t)alt->ss_sp + alt->ss_size)
		alt->ss_flags &= ~SS_ONSTACK;
	return sc->rax;
bad:
	do_exit(SIGSEGV);
	return -EFAULT;
}
