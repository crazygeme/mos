#ifndef MOS_PS_PTRACE_H
#define MOS_PS_PTRACE_H
#include <ps/ps.h>
#define PTRACE_TRACEME 0
#define PTRACE_PEEKTEXT 1
#define PTRACE_PEEKDATA 2
#define PTRACE_PEEKUSER 3
#define PTRACE_CONT 7
#define PTRACE_KILL 8
#define PTRACE_GETREGS 12
#define PTRACE_ATTACH 16
#define PTRACE_DETACH 17
#define PTRACE_SYSCALL 24
#define PTRACE_SETOPTIONS 0x4200
#define PTRACE_GETEVENTMSG 0x4201
#define PTRACE_SEIZE 0x4206
#define PTRACE_O_TRACESYSGOOD 0x01
#define PTRACE_O_TRACEEXEC 0x10
#define PTRACE_O_TRACEEXIT 0x40
#define PTRACE_EVENT_EXEC 4
#define PTRACE_EVENT_EXIT 6

#define PTRACE_MODE_NONE 0
#define PTRACE_MODE_CONT 1
#define PTRACE_MODE_SYSCALL 2

int ps_ptrace_target(int, task_struct **);
int ps_ptrace_control(int, int, void *, void *);
void arch_ptrace_save(task_struct *, intr_frame *);
#endif
