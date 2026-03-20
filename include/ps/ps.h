#ifndef _PS_PS_H
#define _PS_PS_H

#include <hw/time.h>
#include <fs/vfs.h>
#include <fs/fs.h>
#include <lib/list.h>
#include <lib/lock.h>
#include <ps/signal.h>
#include <stddef.h>

#define FORK_FLAG_VFORK 1
/*
 * ----------------------------
 * offset	|31-16		15-0  |
 * ----------------------------
 * 0x00		|reserved	|LINK |
 * ----------------------------
 * 0x04		|ESP0			  |
 * ----------------------------
 * 0x08		|reserved	|SS0  |
 * ----------------------------
 * 0x0C		|ESP1			  |
 * ----------------------------
 * 0x10		|reserved	|SS1  |
 * ----------------------------
 * 0x14		|ESP2			  |
 * ----------------------------
 * 0x18		|reserved	|SS2  |
 * ----------------------------
 * 0x1C		|CR3			  |
 * ----------------------------
 * 0x20		|EIP			  |
 * ----------------------------
 * 0x24		|EFLAGS			  |
 * ----------------------------
 * 0x28		|EAX			  |
 * ----------------------------
 * 0x2C		|ECX			  |
 * ----------------------------
 * 0x30		|EDX			  |
 * ----------------------------
 * 0x34		|EBX			  |
 * ----------------------------
 * 0x38		|ESP			  |
 * ----------------------------
 * 0x3C		|EBP			  |
 * ----------------------------
 * 0x40		|ESI			  |
 * ----------------------------
 * 0x44		|EDI			  |
 * ----------------------------
 * 0x48		|reserved	|ES	  |
 * ----------------------------
 * 0x4C		|reserved	|CS	  |
 * ----------------------------
 * 0x50		|reserved	|SS	  |
 * ----------------------------
 * 0x54		|reserved	|DS	  |
 * ----------------------------
 * 0x58		|reserved	|FS	  |
 * ----------------------------
 * 0x5C		|reserved	|GS	  |
 * ----------------------------
 * 0x60		|reserved	|LDTR |
 * ----------------------------
 * 0x64		|IOPB		|reser|
 * ----------------------------
 */
typedef volatile struct __tss_struct {
	unsigned short link;
	unsigned short link_h;

	unsigned long esp0;
	unsigned short ss0;
	unsigned short ss0_h;

	unsigned long esp1;
	unsigned short ss1;
	unsigned short ss1_h;

	unsigned long esp2;
	unsigned short ss2;
	unsigned short ss2_h;

	unsigned long cr3;
	unsigned long eip;
	unsigned long eflags;

	unsigned long eax;
	unsigned long ecx;
	unsigned long edx;
	unsigned long ebx;

	unsigned long esp;
	unsigned long ebp;

	unsigned long esi;
	unsigned long edi;

	unsigned short es;
	unsigned short es_h;

	unsigned short cs;
	unsigned short cs_h;

	unsigned short ss;
	unsigned short ss_h;

	unsigned short ds;
	unsigned short ds_h;

	unsigned short fs;
	unsigned short fs_h;

	unsigned short gs;
	unsigned short gs_h;

	unsigned short ldt;
	unsigned short ldt_h;

	unsigned short trap;
	unsigned short iomap;

} tss_struct;

typedef struct _page_table_list_entry {
	unsigned int addr;
	list_entry list;
} page_table_list_entry;

struct _region_elem {
	int fd;
	int file_off;
	unsigned start;
	unsigned end;
	struct _region_elem *next;
};

typedef void *vm_struct_t;

typedef struct _user_enviroment {
	unsigned int page_dir; // every process needs it's own clone of page dir
	unsigned start_brk; /* base of heap, set from ELF BSS end at exec time */
	unsigned brk; /* current program break (Linux: mm->brk) */
	vm_struct_t vm;
	char *command;
	size_t cmd_len;
	char *environment;
	size_t env_len;
	unsigned group_id;
	unsigned session_id;
	char *cwd;
} user_enviroment;

typedef struct _signal_context {
	struct sigaction
		sig_handlers[NSIG]; /* indexed by signal number 1..NSIG-1 */
	sigset_t sig_pending; /* bitmask: bit (sig-1) set = pending  */
	sigset_t sig_mask; /* bitmask: blocked signals             */
} signal_context;

typedef enum _ps_status {
	ps_running,
	ps_ready,
	ps_waiting,
	ps_dying
} ps_status;

typedef enum _ps_type { ps_kernel, ps_user } ps_type;

// normal and idle
typedef enum _ps_priority {
	ps_idle = 0,
	ps_normal,
	PS_PRIORITY_MAX
} ps_priority;

typedef void (*process_fn)(void *param);

typedef volatile struct _task_frame {
	unsigned short ds;
	unsigned short ss;
	unsigned short es;
	unsigned short gs;
	unsigned short fs;
	unsigned short cs;
	unsigned long edi;
	unsigned long esi;
	unsigned long edx;
	unsigned long ecx;
	unsigned long ebx;
	unsigned long eax;
	unsigned long ebp;
	unsigned long eip;
	unsigned long esp0; // kernel esp
	unsigned long esp; // kernel or user esp
} task_frame;

typedef struct _task_struct task_struct;
struct _task_struct {
	task_frame tss;
	unsigned long cr3;
	unsigned int psid;
	process_fn fn;
	void *param;
	user_enviroment *user;
	signal_context *signal;
	int priority;
	int type;
	list_entry ps_list; /* dying-queue or wait-queue list node */
	struct rb_node mgr_rb; /* management-queue RB-tree node */
	ps_status status;
	const char *wait_func;
	int remain_ticks;
	unsigned timeout;
	file_descriptor *fds;
	mutex_t fd_lock;
	unsigned exit_status;
	task_struct *parent;
	unsigned fork_flag;
	cond_t vfork_event;
	super_block *root;
	unsigned umask;
	unsigned niv_switches;
	unsigned user_tickets;
	unsigned kernel_tickets;
	unsigned pf_major;
	unsigned pf_minor;
	/* alarm: absolute expiry time in ms (0 = no pending alarm) */
	unsigned long long alarm_expire_ms;
	/* signal handling */

	unsigned int magic; // to avoid stack overflow
};

typedef struct _rusage {
	struct timeval ru_utime; /* user CPU time used */
	struct timeval ru_stime; /* system CPU time used */
	long ru_maxrss; /* maximum resident set size */
	long ru_ixrss; /* integral shared memory size */
	long ru_idrss; /* integral unshared data size */
	long ru_isrss; /* integral unshared stack size */
	long ru_minflt; /* page reclaims (soft page faults) */
	long ru_majflt; /* page faults (hard page faults) */
	long ru_nswap; /* swaps */
	long ru_inblock; /* block input operations */
	long ru_oublock; /* block output operations */
	long ru_msgsnd; /* IPC messages sent */
	long ru_msgrcv; /* IPC messages received */
	long ru_nsignals; /* signals received */
	long ru_nvcsw; /* voluntary context switches */
	long ru_nivcsw; /* involuntary context switches */
} rusage;

#define KERNEL_TASK_SIZE 1 // 1 pages
#define DEFAULT_TASK_TIME_SLICE 10

task_struct *CURRENT_TASK();

void ps_init();

unsigned ps_create(process_fn fn, void *param, ps_priority priority,
		   ps_type type);

void ps_kickoff();

/* Called by each AP after per-CPU setup to enter the scheduler loop. */
void ps_kickoff_ap(void);

int ps_enabled();

void ps_update_tss(unsigned int esp0);

// task functions
void _task_sched(const char *func);
#define task_sched() _task_sched(__func__)

int sched_enable();

int sched_disable();

int sched_set_level(int level);

int sched_is_enabled();

typedef void (*fpuser_map_callback)(void *aux, unsigned vir, unsigned phy);

void ps_enum_user_map(task_struct *task, fpuser_map_callback fn, void *aux);

void ps_cleanup_all_user_map(task_struct *task);

void ps_put_to_ready_queue_unsafe(task_struct *task);
void ps_put_to_ready_queue(task_struct *task);

void ps_put_to_dying_queue_unsafe(task_struct *task);
void ps_put_to_dying_queue(task_struct *task);

void ps_put_to_wait_queue_unsafe(task_struct *task, list_entry *which_list,
				 const char *func);
void ps_put_to_wait_queue(task_struct *task, list_entry *which_list,
			  const char *func);
task_struct *ps_find_process_unsafe(unsigned psid);
task_struct *ps_find_process(unsigned psid);
int ps_send_signal(unsigned pid, int sig);
void ps_send_signal_pgrp(unsigned pgrp, int sig);

typedef void (*ps_enum_callback)(task_struct *task, void *ctx);
void ps_enum_all(ps_enum_callback callback, void *ctx);
// syscall handler
int sys_fork();
int sys_vfork();
void do_exit(unsigned encoded_status);
int sys_exit(unsigned status);
int sys_waitpid(unsigned pid, int *status, int options);
int do_waitpid(unsigned pid, int *status, int options, rusage *rusage);
char *sys_getcwd(char *buf, unsigned size);
int sys_getrusage(int who, rusage *usage);
void reboot();
void shutdown();

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)

#endif
