#include <int/int.h>
#include <config.h>
#include <macro.h>
#include <ps/ps.h>
#include <mm/vdso.h>
extern void i386_syscall_process(intr_frame *frame);
extern unsigned mm_vdso_sysenter_return(void);
extern void intr_syscall_handler(intr_frame *);

void arch_sysenter_handler(intr_frame *frame)
{
	unsigned arg6;
	frame->error_code = 0;
	frame->eip = (void *)mm_vdso_sysenter_return();
	int_intr_enable();
	/* Read the sixth argument only after a complete frame and kernel
	 * segments exist, so a bad/demand-paged user stack is handled safely. */
	arg6 = *(unsigned *)frame->esp;
	frame->ebp = arg6;
	frame->frame_pointer = (void *)arg6;
	intr_syscall_handler(frame);
}

static void syscall_init(void)
{
	int_register(SYSCALL_INT_NO, i386_syscall_process, 0, 3);
}
KERNEL_INIT(7, syscall_init);
