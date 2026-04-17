#ifndef _PS_PS_H
#define _PS_PS_H

#include <hw/time.h>
#include <fs/vfs.h>
#include <fs/fs.h>
#include <lib/list.h>
#include <lib/lock.h>
#include <ps/signal.h>
#include <stddef.h>
#include <config.h>

#define FORK_FLAG_VFORK 1
#define FORK_FLAG_SHARE_VM 2
#define FORK_FLAG_THREAD 4
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

#define TSS_IO_BITMAP_BYTES 8192

typedef struct _tss_io_struct {
	tss_struct tss;
	unsigned char io_bitmap[TSS_IO_BITMAP_BYTES + 1];
} tss_io_struct;

#define TSS_SEG_LIMIT ((unsigned)(sizeof(tss_io_struct) - 1))

typedef struct _vm_region vm_region;

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

typedef struct _task_stats {
	unsigned niv_switches; /* involuntary context switches */
	unsigned total_switches; /* total context switches       */
	unsigned long long start_tickets; /* start time (jiffies)        */
	unsigned kernel_tickets; /* jiffies in kernel (stime)    */
	unsigned long long idle; /* temp: tick when sched began  */
	unsigned idle_tickets; /* jiffies off-CPU              */
	unsigned pf_major; /* major page faults            */
	unsigned pf_minor; /* minor page faults            */
	unsigned long long child_utime; /* reaped children user ticks   */
	unsigned child_stime; /* reaped children system ticks */
} task_stats_t;

#define RLIM_NLIMITS 16
#define RLIM_INFINITY 0xFFFFFFFFu

typedef struct {
	unsigned long rlim_cur;
	unsigned long rlim_max;
} rlimit_t;

typedef struct _user_enviroment {
	unsigned int page_dir; // every process needs it's own clone of page dir
	unsigned start_brk; /* base of heap, set from ELF BSS end at exec time */
	unsigned brk; /* current program break (Linux: mm->brk) */
	unsigned stack_bottom; /* lowest mapped stack page (grows down on fault) */
	vm_struct_t vm;
	vm_region *mmap_cache; /* Linux-style last find_vma() result cache */
	char *command;
	size_t cmd_len;
	char *environment;
	size_t env_len;
	unsigned group_id; /* process group id (pgid) */
	unsigned session_id; /* session id (sid) */
	char *cwd;
	char *root_path; /* absolute chroot prefix within cur->root */
	/* process credentials */
	unsigned uid, euid, suid; /* real, effective, saved-set user id */
	unsigned gid, egid, sgid; /* real, effective, saved-set group id */
	unsigned fsuid, fsgid; /* filesystem uid/gid */
	/* Per-process TLS GDT descriptors (GDT_ENTRY_TLS_MIN .. GDT_ENTRY_TLS_MAX) */
	unsigned long long tls_desc[GDT_ENTRY_TLS_COUNT];
	unsigned long long ldt_desc[LDT_ENTRY_COUNT];
	rlimit_t rlimits[RLIM_NLIMITS];
} user_enviroment;

typedef struct _signal_context {
	struct sigaction
		sig_handlers[NSIG]; /* indexed by signal number 1..NSIG-1 */
	sigset_t sig_pending; /* bitmask: bit (sig-1) set = pending  */
	sigset_t sig_mask; /* bitmask: blocked signals             */
	sigset_t saved_sigmask; /* mask to restore after sigsuspend     */
	int restore_sigmask; /* if set, restore saved_sigmask after signal delivery */
	stack_t altstack; /* alternate signal stack (sigaltstack)  */
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
	unsigned int tgid;
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
	file **fds;
	unsigned long *fd_cloexec;
	mutex_t fd_lock;
	unsigned exit_status;
	unsigned exit_signal;
	unsigned ppid;
	unsigned nchildren; /* count of children not yet reaped (living + zombie) */
	unsigned fork_flag;
	cond_t vfork_event;
	super_block *root;
	unsigned umask;
	task_stats_t *stats;
	/* alarm: absolute expiry time in ms (0 = no pending alarm) */
	unsigned long long alarm_expire_ms;
	/* interval for ITIMER_REAL in ms (0 = one-shot) */
	unsigned long long alarm_interval_ms;
	struct rb_node timer_rb; /* node in control.timer_queue when sleeping */
	unsigned timer_due_ms; /* expiry time in ms; 0 = not in timer queue */
	unsigned char
		io_priv_level; /* requested iopl(2) level for compatibility */
	unsigned char
		io_allow_all; /* allow all port I/O via the TSS I/O bitmap */
	unsigned char *io_bitmap; /* per-task I/O-permission bitmap */
	int *clear_child_tid; /* Linux set_tid_address / CLONE_CHILD_CLEARTID */
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

/*
 * task_utime — corrected user-mode CPU time for a task (in jiffies).
 *
 * idle_tickets is only flushed at the SELF label in _task_sched(), i.e. when
 * the task is rescheduled back in.  For a sleeping or ready task the current
 * off-CPU period has NOT yet been added to idle_tickets.  We correct for that
 * by adding (now - stats->idle), where stats->idle was stamped at the top of
 * _task_sched() when the task last yielded.
 *
 * Guard stats->idle > 0: a newly created task that has never called
 * _task_sched() has stats->idle == 0 (from zalloc); adding (now - 0) would
 * wildly overcount.
 */
static inline unsigned long long task_utime(task_struct *task)
{
	task_stats_t *s = task->stats;
	unsigned long long eff_idle = s->idle_tickets;

	if (task->status != ps_running && s->idle > 0)
		eff_idle += time_now_tickets() - s->idle;

	unsigned long long elapsed = time_now_tickets() - s->start_tickets;
	unsigned long long busy = s->kernel_tickets + eff_idle;
	return elapsed > busy ? elapsed - busy : 0;
}

task_struct *__attribute__((noinline)) CURRENT_TASK(void);

#define current CURRENT_TASK()

void ps_init();

#define ps_create(fn, param, priority, type) \
	_ps_create(fn, #fn, param, priority, type)
unsigned _ps_create(process_fn fn, const char *name, void *param,
		    ps_priority priority, ps_type type);
void ps_start_system_services(void);

void ps_kickoff();

/* Called by each AP after per-CPU setup to enter the scheduler loop. */
void ps_kickoff_ap(void);

int ps_enabled();

void ps_update_tss(unsigned int esp0);
void reset_tss(task_struct *task);
int ps_set_ioperm(task_struct *task, unsigned long from, unsigned long num,
		  int turn_on);

// task functions
void _task_sched(const char *func);
#define task_sched() _task_sched(__func__)

int sched_enable();

int sched_disable();

int sched_is_enabled();

int sched_set_level(int level);

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
int ps_total_count();
int ps_send_signal(unsigned pid, int sig);
void ps_send_signal_pgrp(unsigned pgrp, int sig);
void ps_send_signal_owner(int owner, int sig);

typedef void (*ps_enum_callback)(task_struct *task, void *ctx);
void ps_enum_all(ps_enum_callback callback, void *ctx);
// syscall handler
int sys_fork();
int sys_vfork();
void ps_update_ldt(task_struct *task);
void do_exit(unsigned encoded_status);
int sys_exit(unsigned status);
int sys_waitpid(unsigned pid, int *status, int options);
int do_waitpid(unsigned pid, int *status, int options, rusage *rusage);
int do_waitpid_pgrp(unsigned pgrp, int *status, int options, rusage *rusage);
void qemu_exit(unsigned char code);
char *sys_getcwd(char *buf, unsigned size);
int sys_getrusage(int who, rusage *usage);
void reboot();
void shutdown();

void time_wait(unsigned ms);
void ps_signal_wait(void);

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)
#define WNOHANG 1
#define WUNTRACED 2
#define WSTOPPED WUNTRACED
#define WEXITED 4
#define WCONTINUED 8

#endif
