#include <syscall/syscall.h>
#include <int/int.h>
#include <syscall/unistd.h>
#include <ps/ps.h>
#include <config.h>

static int test_call(unsigned arg0, unsigned arg1, unsigned arg2);
static int sys_write(unsigned fd, char* buf, unsigned len);
static int sys_getpid();

static unsigned call_table[NR_syscalls] = {
	test_call, 
    0, sys_fork, 0, sys_write, 0,   // 0  ~ 5
    0, 0, 0, 0, 0,           // 6  ~ 10  
    0, 0, 0, 0, 0,           // 11 ~ 15
    0, 0, 0, 0, sys_getpid,  // 16 ~ 20                   
    0
};

static int unhandled_syscall(unsigned callno)
{
	printk("unhandled syscall %d\n", callno);
    return -1;
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
	int_register(SYSCALL_INT_NO, syscall_process, 1, 3);
}

static int test_call(unsigned arg0, unsigned arg1, unsigned arg2)
{
	printk("test call: arg0 %x, arg1 %x, arg2 %x\n", arg0, arg1, arg2);
	return 0;
}

static int sys_write(unsigned fd, char* buf, unsigned len)
{
	// FIXME, fd 1 not always refer to console
	if (fd == 1)
	{
        task_struct* task = CURRENT_TASK();
		printk("ps[%d]: %s", task->psid, buf);
	}

	return len;
}

static int sys_getpid()
{
    task_struct* cur = CURRENT_TASK();
    return cur->psid;
}


