#include <syscall/syscall.h>
#include <int/int.h>
#include <syscall/unistd.h>

static int test_call(unsigned arg0, unsigned arg1, unsigned arg2);
static int sys_write(unsigned fd, char* buf, unsigned len);

static unsigned call_table[NR_syscalls] = {
	test_call, 	// 0
	0,
	0,
	0,
	sys_write,
	0			// 5
};

static void unhandled_syscall(unsigned callno)
{
	printk("unhandled syscall %d\n", callno);
}

static void syscall_process(intr_frame* frame)
{
	unsigned fn = call_table[frame->eax];
	if ( !fn )
	{
		return unhandled_syscall(frame->eax);
	}

	__asm__("movl %0, %%eax" : : "m"(fn));
	__asm__("pushl %edx");
	__asm__("pushl %ecx");
	__asm__("pushl %ebx");
	__asm__("call *%eax");
	__asm__("popl %ebx");
	__asm__("popl %ecx");
	__asm__("popl %edx");
}

void syscall_init()
{
	int_register(0x80, syscall_process, 1, 3);
}

static int test_call(unsigned arg0, unsigned arg1, unsigned arg2)
{
	printk("test call: arg0 %x, arg1 %x, arg2 %x\n", arg0, arg1, arg2);
	return 0;
}

static int sys_write(unsigned fd, char* buf, unsigned len)
{
	// FIXME, fd 0 not always refer to console
	if (fd == 0)
	{
		printf(buf);
	}

	return len;
}
