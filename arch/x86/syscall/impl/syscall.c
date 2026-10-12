#include <int/int.h>
#include <config.h>
#include <macro.h>
#include <ps/ps.h>
#include <mm/vdso.h>
#include <mm/mmap.h>
extern void i386_syscall_process(intr_frame *frame);
extern unsigned mm_vdso_sysenter_return(void);
extern void intr_syscall_handler(intr_frame *);

void arch_sysenter_handler(intr_frame *frame)
{
	unsigned arg6;
	void *stack = frame->esp;
	unsigned ecx = frame->ecx, edx = frame->edx;
	unsigned landing = mm_vdso_sysenter_return();
	frame->error_code = 0;
	frame->eip = (void *)landing;
	frame->cs = SYSEXIT_CODE_SELECTOR;
	frame->ss = SYSEXIT_DATA_SELECTOR;
	int_intr_enable();
	/* Read the sixth argument only after a complete frame and kernel
	 * segments exist, so a bad/demand-paged user stack is handled safely. */
	arg6 = *(unsigned *)frame->esp;
	frame->ebp = arg6;
	frame->frame_pointer = (void *)arg6;
	intr_syscall_handler(frame);
	/* The wrapper restores ECX/EDX from its save area. Changed debugger or
	 * signal frames must instead use the complete IRET register image. */
	if ((unsigned)frame->eip == landing && frame->esp == stack &&
	    frame->cs == SYSEXIT_CODE_SELECTOR &&
	    frame->ss == SYSEXIT_DATA_SELECTOR && frame->ecx == ecx &&
	    frame->edx == edx && !(frame->eflags & 0x30100U) &&
	    (frame->eflags & 0x200U) &&
	    (unsigned)stack < current->memory->task_size)
		frame->error_code = 1;
}

static void syscall_init(void)
{
	int_register(SYSCALL_INT_NO, i386_syscall_process, 0, 3);
}
KERNEL_INIT(7, syscall_init);
