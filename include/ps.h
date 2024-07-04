#ifndef _PS_H_
#define _PS_H_

#include <time.h>
#include <mount.h>
#include <list.h>
#include <lock.h>
#include <fs.h>

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
	unsigned heap_top;
	// unsigned zone_top;
	vm_struct_t vm;
	// region_elem_t region_head;
} user_enviroment;

typedef enum _ps_status {
	ps_running,
	ps_ready,
	ps_waiting,
	ps_dying
} ps_status;

typedef enum _ps_type { ps_kernel, ps_user, ps_dsr } ps_type;

#define MAX_PRIORITY 5

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

typedef volatile struct _task_struct {
	task_frame tss;
	unsigned long cr3;
	unsigned int psid;
	process_fn fn;
	void *command;
	user_enviroment user;
	int priority;
	// in schedule list
	list_entry ps_list;
	// in wait list if waiting a lock
	list_entry lock_list;
	// in all process list
	list_entry ps_mgr;
	ps_status status;
	ps_type type;
	int remain_ticks;
	int is_switching;
	unsigned timeout;
	file_descriptor *fds;
	mutex_t fd_lock;
	unsigned exit_status;
	unsigned parent;
	unsigned group_id;
	char *cwd;
	unsigned fork_flag;
	cond_t vfork_event;
	mount_point *root;
	unsigned umask;
	unsigned niv_switches;
	unsigned user_tickets;
	unsigned kernel_tickets;
	unsigned pf_major;
	unsigned pf_minor;
	unsigned int magic; // to avoid stack overflow

} task_struct;

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

unsigned ps_create(process_fn, int priority, ps_type type);

void ps_kickoff();

int ps_enabled();

void ps_update_tss(unsigned int esp0);

// task functions
void _task_sched(const char *func);
#define task_sched() _task_sched(__func__)

typedef void (*fpuser_map_callback)(void *aux, unsigned vir, unsigned phy);

void ps_enum_user_map(task_struct *task, fpuser_map_callback fn, void *aux);

void ps_cleanup_all_user_map(task_struct *task);

void ps_put_to_ready_queue(task_struct *task);

void ps_put_to_dying_queue(task_struct *task);

void ps_put_to_wait_queue(task_struct *task);

task_struct *ps_find_process(unsigned psid);

// if process that >= priority exist
int ps_has_ready(int priority);
typedef void (*ps_enum_callback)(task_struct *task);
void ps_enum_all(ps_enum_callback callback);
// syscall handler
int sys_fork();
int sys_vfork();
int sys_exit(unsigned status);
int sys_waitpid(unsigned pid, int *status, int options);
int do_waitpid(unsigned pid, int *status, int options, rusage *rusage);
char *sys_getcwd(char *buf, unsigned size);
void reboot();
void shutdown();

#endif
