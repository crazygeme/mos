#include <syscall/syscall.h>
#include <int/int.h>
#include <syscall/unistd.h>
#include <ps/ps.h>
#include <user/ps0.h>
#include <config.h>




static int test_call(unsigned arg0, unsigned arg1, unsigned arg2);
static int sys_read(unsigned fd, char* buf, unsigned len);
static int sys_write(unsigned fd, char* buf, unsigned len);
static int sys_getpid();
static int sys_uname(struct utsname* utname);
static int sys_sched_yield();

static unsigned call_table[NR_syscalls] = {
	test_call, 
    sys_exit, sys_fork, sys_read, sys_write, 0,   // 1  ~ 5
    0, 0, 0, 0, 0,           // 6  ~ 10  
    sys_execve, 0, 0, 0, 0,           // 11 ~ 15
    0, 0, 0, 0, sys_getpid,  // 16 ~ 20   
    0, 0, 0, 0, 0,          // 21 ~ 25 
    0, 0, 0, 0, 0,          // 26 ~ 30 
    0, 0, 0, 0, 0,          // 31 ~ 35 
    0, 0, 0, 0, 0,          // 36 ~ 40 
    0, 0, 0, 0, 0,          // 41 ~ 45 
    0, 0, 0, 0, 0,          // 46 ~ 50 
    0, 0, 0, 0, 0,          // 51 ~ 55 
    0, 0, 0, 0, 0,          // 56 ~ 60 
    0, 0, 0, 0, 0,          // 61 ~ 65 
    0, 0, 0, 0, 0,          // 66 ~ 70 
    0, 0, 0, 0, 0,          // 71 ~ 75 
    0, 0, 0, 0, 0,          // 76 ~ 80 
    0, 0, 0, 0, 0,          // 81 ~ 85 
    0, 0, 0, 0, 0,          // 86 ~ 90 
    0, 0, 0, 0, 0,          // 91 ~ 95 
    0, 0, 0, 0, 0,          // 96 ~ 100 
    0, 0, 0, 0, 0,          // 101 ~ 105
    0, 0, 0, 0, 0,          // 106 ~ 110 
    0, 0, 0, 0, 0,          // 111 ~ 115
    0, 0, 0, 0, 0,          // 116 ~ 120
    0, sys_uname, 0, 0, 0,          // 121 ~ 125
    0, 0, 0, 0, 0,          // 126 ~ 130
    0, 0, 0, 0, 0,          // 131 ~ 135
    0, 0, 0, 0, 0,          // 136 ~ 140
    0, 0, 0, 0, 0,          // 141 ~ 145
    0, 0, 0, 0, 0,          // 145 ~ 150
    0, 0, 0, 0, 0,          // 151 ~ 155
    0, 0, sys_sched_yield, 0, 0,          // 156 ~ 160
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

static int sys_read(unsigned fd, char* buf, unsigned len)
{
	task_struct* cur = CURRENT_TASK();
	unsigned ino = 0;

	if (fd > MAX_FD)
		return -1;

	ino = cur->fds[fd];
	if ( ino == INODE_STD_OUT || ino == INODE_STD_ERR )
	{
		return -1;
	}
	else if ( ino == INODE_STD_IN )
	{
		if ( len < 1 )
		   return -1;

		*buf = kb_buf_get();
		return 1;
	}
	else
	{
		unsigned offset = cur->file_off[fd];
		fs_read(fd, offset, buf, len);
		offset += len;
		cur->file_off[fd] = offset;
	}

	return len;
}

static int sys_write(unsigned fd, char* buf, unsigned len)
{
	task_struct* cur = CURRENT_TASK();
	unsigned ino = 0;

	if (fd > MAX_FD)
		return -1;

	ino = cur->fds[fd];
	if ( ino == INODE_STD_OUT || ino == INODE_STD_ERR )
	{
		tty_write(buf, len);
	}
	else if ( ino == INODE_STD_IN )
	{
		return -1;
	}
	else
	{
		unsigned offset = cur->file_off[fd];
		fs_write(fd, offset, buf, len);
		offset += len;
		cur->file_off[fd] = offset;
	}

	return len;
}

static int sys_getpid()
{
    task_struct* cur = CURRENT_TASK();
    return cur->psid;
}

static int sys_uname(struct utsname* utname)
{
	strcpy(utname->machine, "qemu");
	strcpy(utname->nodename, "qemu-enum");
	strcpy(utname->release, "ender");
	strcpy(utname->sysname, "Mos");
	strcpy(utname->version, "0.1");
	return 1;
}

static int sys_sched_yield()
{
    task_sched();
    return 0;
}



