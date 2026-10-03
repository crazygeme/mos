#include <int/int.h>
#include <config.h>
#include <macro.h>
extern void i386_syscall_process(intr_frame *frame);
static void syscall_init(void)
{
	int_register(SYSCALL_INT_NO, i386_syscall_process, 0, 3);
}
KERNEL_INIT(7, syscall_init);
