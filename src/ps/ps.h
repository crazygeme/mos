#ifndef _PS_PS_H
#define _PS_PS_H

#include <lib/klib.h>
#include <fs/vfs.h>
#include <fs/fs.h>
#include <lib/list.h>
#include <lib/ref_count.h>
#include <lib/lock.h>
#include <ps/signal.h>
#include <stddef.h>
#include <config.h>
#include <arch/types.h>
#include <int/interrupt.h>
#include <ps/task.h>

#define FORK_FLAG_VFORK 1
#define FORK_FLAG_THREAD 4
typedef struct _vm_region vm_region;

typedef struct _task_memory task_memory;
typedef task_memory mm_struct;
typedef task_memory *vm_struct_t;

/* Native-width sampled ticks: modulo 2^32 on i386, 2^64 on AMD64. */
typedef unsigned long ps_tick_t;

typedef struct _task_stats {
	unsigned niv_switches; /* involuntary context switches */
	unsigned total_switches; /* total context switches       */
	unsigned long long start_tickets; /* start time (jiffies)        */
	ps_tick_t user_tickets; /* sampled user CPU ticks */
	ps_tick_t kernel_tickets; /* sampled system CPU ticks */
	unsigned pf_major; /* major page faults            */
	unsigned pf_minor; /* minor page faults            */
} task_stats_t;

#define RLIM_NLIMITS 16
#define RLIM_INFINITY UINT64_MAX

typedef struct {
	uint64_t rlim_cur;
	uint64_t rlim_max;
} rlimit_t;

/* Filesystem state is shared only by CLONE_FS. */
typedef struct _task_fs {
	ref_count_t ref;
	rmutex_t lock;
	super_block *root;
	unsigned umask;
	char *cwd;
	char *root_path;
} task_fs;

typedef struct _signal_queue_entry {
	list_entry list;
	int signo, timer_id;
	uintptr_t value;
	struct mos_signal_info info;
} signal_queue_entry;

/* Protected by ps_lock; shared by all members of a thread group. */
typedef struct _task_thread {
	ref_count_t ref;
	unsigned tgid, group_id, session_id;
	unsigned live_tasks;
	unsigned teardown_owner;
	unsigned child_subreaper;
	int oom_score_adj;
	unsigned posix_locks_used; /* Sticky hint: this group has requested range locks. */
	list_entry members, children;
	ps_tick_t user_tickets, kernel_tickets, child_utime, child_stime;
	rlimit_t rlimits[RLIM_NLIMITS];
	sigset_t sig_pending;
	list_entry pending_queue;
	struct mos_signal_info pending_info[NSIG];
	uint64_t signal_sequence[NSIG];
	unsigned long long alarm_expire_ms, alarm_interval_ms;
	struct rb_node alarm_rb;
} task_thread;

/* Raw kernel credentials are per task, inherited by value on clone. */
typedef struct _task_credentials {
	unsigned uid, euid, suid;
	unsigned gid, egid, sgid;
	unsigned fsuid, fsgid;
	unsigned cap_effective[2], cap_permitted[2], cap_inheritable[2];
	unsigned cap_initialized, keep_capabilities, securebits;
	unsigned cap_ambient[2];
	unsigned cap_bounding[2], no_new_privs;
} task_credentials;

/* Private execution and ABI state: never shared by clone. */
typedef struct _task_execution {
	task_arch_context arch;
	/* Textual final-symlink resolution; file metadata is never cached. */
	int fs_open_error;
	super_block *link_cache_root;
	uint64_t link_cache_generation;
	char link_cache_path[256], link_cache_target[256];
	void *ext4_file_cache; /* One closed, small description; freed with the task. */
	task_fs *open_cache_fs;
	uint64_t open_cache_generation;
	char open_cache_input[256], open_cache_path[256];
	struct _vm_region *mmap_cache;
	task_memory *mmap_cache_vm;
	unsigned mmap_cache_generation;
	unsigned char fpu_storage[512 + 15];
	unsigned long long tls_desc[GDT_ENTRY_TLS_COUNT];
	unsigned ptrace_tracer, ptrace_mode, ptrace_options;
	unsigned long ptrace_eventmsg;
	unsigned ptrace_orig_eax, ptrace_frame_valid;
	ptrace_saved_frame ptrace_frame;
	unsigned char io_priv_level, io_allow_all;
	unsigned char *io_bitmap;
	int *clear_child_tid;
	void *robust_list_head;
	size_t robust_list_size;
	int (*robust_list_reader)(struct _task_struct *, uintptr_t, uintptr_t *,
				  intptr_t *, uintptr_t *);
} task_execution;

typedef struct _signal_handlers {
	ref_count_t ref;
	struct sigaction actions[NSIG];
} signal_handlers;

typedef struct _task_signal {
	sigset_t sig_pending;
	sigset_t sig_mask;
	list_entry pending_queue;
	struct mos_signal_info pending_info[NSIG];
	uint64_t signal_sequence[NSIG];
	sigset_t saved_sigmask;
	int restore_sigmask;
	stack_t altstack;
} task_signal;

typedef enum _ps_status {
	ps_running,
	ps_ready,
	ps_waiting,
	ps_stopped,
	ps_dying
} ps_status;

typedef enum _ps_type { ps_kernel, ps_user } ps_type;

/* Higher levels take precedence over lower levels. */
typedef enum _ps_priority {
	ps_idle = 0,
	ps_normal,
	ps_deferred, /* deferred device callbacks */
	PS_PRIORITY_MAX
} ps_priority;

typedef void (*process_fn)(void *param);

typedef struct {
	ref_count_t ref;
	file **fds;
	unsigned long cloexec[FD_BITMAP_WORDS];
	mutex_t lock;
} task_files;

struct mos_namespace;
struct mos_cgroup;
typedef struct _task_struct task_struct;
/* Scheduler and blocking state are private even when all resources are shared. */
typedef struct _task_schedule {
	task_struct *task;
	uintptr_t switch_sp;
	unsigned on_cpu, terminate_requested, enumeration_refs;
	unsigned affinity_mask; /* Zero permits every online CPU. */
	unsigned live_member;
	unsigned net_core_depth, vm_lock_depth;
	int sched_level, priority, remain_ticks;
	ps_status status;
	list_entry ps_list;
	list_entry thread_list, child_list, creator_list;
	struct rb_node mgr_rb;
} task_schedule;

typedef struct _task_wait {
	task_struct *task;
	int wait_interruptible;
	unsigned long signal_wait_mask;
	void (*cancel_io_wait)(void *);
	void *io_wait;
	list_entry io_files;
	struct rb_node timer_rb;
	unsigned long long timer_due_ms;
} task_wait;

typedef struct _task_lifecycle {
	ps_type type;
	unsigned psid, ppid;
	unsigned exit_status, exit_signal, pdeath_signal;
	unsigned nchildren, fork_flag;
	unsigned exit_ready;
	struct _task_struct *creator;
	task_thread *parent_group;
	list_entry children;
	unsigned stop_signal, stop_report_pending;
	process_fn fn;
	void *param;
	cond_t vfork_event;
	list_entry dying_queue;
} task_lifecycle;

struct _task_struct {
	struct mos_namespace *namespaces[7], *pid_children;
	struct mos_cgroup *cgroups[4];
	unsigned cgroup_exited;
	/* Private execution, scheduling, identity, and waits. */
	task_schedule *sched;
	task_wait *wait;
	task_lifecycle *life;
	task_execution *execution;
	task_credentials *credentials;
	task_signal *signal;
	task_stats_t *stats;

	/* Independent shared owners, selected by the corresponding clone flag. */
	task_memory *memory; /* CLONE_VM: mappings, image metadata and LDT. */
	task_files *files; /* CLONE_FILES */
	task_fs *fs; /* CLONE_FS */
	signal_handlers *sighand; /* CLONE_SIGHAND */
	task_thread *thread; /* CLONE_THREAD */
	unsigned magic; /* Kernel-stack overflow sentinel. */
};

#define KERNEL_TASK_SIZE MOS_KERNEL_TASK_PAGES
#define KERNEL_TASK_BYTES (KERNEL_TASK_SIZE * PAGE_SIZE)

static inline uintptr_t task_stack_top(const task_struct *task)
{
	return (uintptr_t)task + KERNEL_TASK_BYTES;
}

static inline unsigned char *task_fpu(const task_struct *task)
{
	return (unsigned char *)(((uintptr_t)task->execution->fpu_storage +
				  15) &
				 ~(uintptr_t)15);
}

typedef struct _rusage {
	struct timeval ru_utime; /* user CPU time used */
	struct timeval ru_stime; /* system CPU time used */
	int32_t ru_maxrss; /* maximum resident set size */
	int32_t ru_ixrss; /* integral shared memory size */
	int32_t ru_idrss; /* integral unshared data size */
	int32_t ru_isrss; /* integral unshared stack size */
	int32_t ru_minflt; /* page reclaims (soft page faults) */
	int32_t ru_majflt; /* page faults (hard page faults) */
	int32_t ru_nswap; /* swaps */
	int32_t ru_inblock; /* block input operations */
	int32_t ru_oublock; /* block output operations */
	int32_t ru_msgsnd; /* IPC messages sent */
	int32_t ru_msgrcv; /* IPC messages received */
	int32_t ru_nsignals; /* signals received */
	int32_t ru_nvcsw; /* voluntary context switches */
	int32_t ru_nivcsw; /* involuntary context switches */
} rusage;

/* Sampled CPU time does not advance while a task is off CPU. */
static inline unsigned long long task_utime(task_struct *task)
{
	return __atomic_load_n(&task->stats->user_tickets, __ATOMIC_RELAXED);
}

static __attribute__((always_inline)) inline task_struct *CURRENT_TASK(void)
{
	uintptr_t stack;
#if __SIZEOF_POINTER__ == 8
	asm volatile("mov %%rsp, %0" : "=r"(stack) : : "memory");
#else
	asm volatile("mov %%esp, %0" : "=r"(stack) : : "memory");
#endif
	return (task_struct *)(stack & ~(uintptr_t)(KERNEL_TASK_BYTES - 1));
}

#define current CURRENT_TASK()

int ps_has_capability(task_struct *task, unsigned capability);
int ps_unshare_fds(task_struct *task);
int ps_unshare_exec_context(task_struct *task);
int ps_init_private(task_struct *task);
void ps_free_task(task_struct *task);
int ps_init_resources(task_struct *task);
void ps_put_resources(task_struct *task);
unsigned long ps_pending_signals(task_struct *task);
void ps_queue_group_signal_unsafe(task_struct *task, int sig);
int ps_send_thread_signal(unsigned tid, int sig);
int ps_take_signal_info(task_struct *task, sigset_t mask,
			struct mos_signal_info *info);
sigset_t ps_signalfd_snapshot(unsigned tid, task_thread *group, sigset_t mask,
			      uint64_t *sequence);
void ps_child_signal_unsafe(task_struct *parent, task_struct *child, int sig);
int ps_take_signal(task_struct *task, sigset_t mask, int *timer_id,
		   uintptr_t *value);
void ps_signal_action(task_struct *task, int sig, const struct sigaction *in,
		      struct sigaction *out);
void ps_claim_signal_action(task_struct *task, int sig, struct sigaction *out);

void ps_put_fds(task_struct *task);
void ps_init();
void ps_reaper_start(void);

#define ps_create(fn, param, priority, type) \
	_ps_create(fn, #fn, param, priority, type)
unsigned _ps_create(process_fn fn, const char *name, void *param,
		    ps_priority priority, ps_type type);
void ps_start_system_services(void);

void ps_init_bootstrap_task(void);
void ps_kickoff();

int ps_enabled();

void reset_tss(task_struct *task);
int ps_set_ioperm(task_struct *task, unsigned long from, unsigned long num,
		  int turn_on);

// task functions
void task_sched(void);
int ps_has_ready_task(void);

int sched_enable();

int sched_disable();

int sched_is_enabled();

int sched_set_level(int level);

typedef void (*fpuser_map_callback)(void *aux, vaddr_t vir, paddr_t phy);

void ps_enum_user_map(task_struct *task, fpuser_map_callback fn, void *aux);

void ps_cleanup_all_user_map(task_struct *task);

int ps_may_access_process(task_struct *target);
task_memory *ps_get_process_memory(task_struct *task);
void ps_replace_process_memory(task_struct *task, task_memory *memory);

/* Copy between kernel buffers and a task's user address space.
 * Return 0 on completion or -EFAULT; earlier pages may have been copied.
 * Missing pages and write faults are resolved in the target task's mappings.
 */
int ps_read_process_memory(task_struct *task, const void *addr, void *dst,
			   unsigned len);
int ps_write_process_memory(task_struct *task, void *addr, const void *src,
			    unsigned len);

void ps_put_to_ready_queue_unsafe(task_struct *task);
void ps_put_to_ready_queue(task_struct *task);

void ps_put_to_dying_queue_unsafe(task_struct *task);
void ps_wake_parent_waiters_unsafe(task_struct *parent);
void ps_put_to_dying_queue(task_struct *task);

void ps_put_to_wait_queue_unsafe(task_struct *task, list_entry *which_list);
void ps_put_to_wait_queue(task_struct *task, list_entry *which_list);
task_struct *ps_find_process_unsafe(unsigned psid);
task_struct *ps_find_process(unsigned psid);
task_struct *ps_find_process_ref(unsigned psid);
void ps_put_process_ref(task_struct *task);
int ps_total_count();
int ps_send_signal(unsigned pid, int sig);
/* Actionable, unmasked signals; ignored signals do not interrupt I/O. */
unsigned long ps_interrupting_signals(task_struct *task);
/* Publish an interruptible wait atomically with the pending-signal check.
 * Return -EINTR without sleeping, or 0; pair success with finish_timed_wait.
 */
int ps_prepare_interruptible_wait(task_struct *task, list_entry *queue,
				  unsigned ms);
/* Relative ms in/out: snapshot old values and optionally replace the alarm. */
void ps_alarm_update(task_struct *task, int set, unsigned long long *value,
		     unsigned long long *interval);
void ps_alarm_tick(void);
void ps_stop_terminated_task(void);
int ps_task_ready(task_struct *task);

/* Requires ps_lock; signal delivery is authorized by the kernel caller. */
void ps_queue_signal_unsafe(task_struct *target, int sig);
void ps_timer_notify(unsigned tid, int signo, int timer_id, uintptr_t value,
		     int thread, unsigned overrun);
void ps_timer_poll(void);
void ps_timer_discard_group(unsigned tgid);
void ps_send_signal_pgrp(unsigned pgrp, int sig);
void ps_send_signal_owner(int owner, int sig);

typedef void (*ps_enum_callback)(task_struct *task, void *ctx);
void ps_enum_all(ps_enum_callback callback, void *ctx);
unsigned ps_thread_count(unsigned tgid);
// syscall handler
int sys_fork();
int sys_vfork();
void ps_update_ldt(task_struct *task);
void ps_load_task_segments(task_struct *task);
void do_exit(unsigned encoded_status);
void do_group_exit(unsigned encoded_status);
void ps_kill_thread_group(task_struct *caller, unsigned encoded_status);
void ps_fault_signal(intr_frame *frame, int sig,
		     const struct signal_fault *fault);
int sys_exit(unsigned status);
int sys_waitpid(unsigned pid, int *status, int options);
int do_waitpid(unsigned pid, int *status, int options, rusage *rusage);
int do_waitpid_pgrp(unsigned pgrp, int *status, int options, rusage *rusage);
void ps_stop_current(intr_frame *frame, int sig);
int ps_ptrace_maybe_stop_syscall(intr_frame *frame, int entering);
void ps_ptrace_stop_exec(vaddr_t eip, vaddr_t esp, unsigned syscall_number);
void ps_ptrace_stop_exit(unsigned status);
void qemu_exit(unsigned char code);
intptr_t sys_getcwd(char *buf, size_t size);
int sys_getrusage(int who, rusage *usage);
void reboot();
void shutdown();

void time_wait(unsigned ms);
void ps_prepare_timed_wait(task_struct *task, unsigned ms);
void ps_finish_timed_wait(task_struct *task);
void ps_signal_wait(void);

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)
#define WNOHANG 1
#define WUNTRACED 2
#define WSTOPPED WUNTRACED
#define WEXITED 4
#define WCONTINUED 8
#define WNOWAIT 0x01000000
int do_waitid(int, unsigned, struct mos_signal_info *, int, rusage *);

#endif
