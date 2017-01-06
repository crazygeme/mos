#ifndef _PS_H_
#define _PS_H_

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
    unsigned short   link;
    unsigned short   link_h;

    unsigned long   esp0;
    unsigned short   ss0;
    unsigned short   ss0_h;

    unsigned long   esp1;
    unsigned short   ss1;
    unsigned short   ss1_h;

    unsigned long   esp2;
    unsigned short   ss2;
    unsigned short   ss2_h;

    unsigned long   cr3;
    unsigned long   eip;
    unsigned long   eflags;

    unsigned long   eax;
    unsigned long   ecx;
    unsigned long   edx;
    unsigned long    ebx;

    unsigned long   esp;
    unsigned long   ebp;

    unsigned long   esi;
    unsigned long   edi;

    unsigned short   es;
    unsigned short   es_h;

    unsigned short   cs;
    unsigned short   cs_h;

    unsigned short   ss;
    unsigned short   ss_h;

    unsigned short   ds;
    unsigned short   ds_h;

    unsigned short   fs;
    unsigned short   fs_h;

    unsigned short   gs;
    unsigned short   gs_h;

    unsigned short   ldt;
    unsigned short   ldt_h;

    unsigned short   trap;
    unsigned short   iomap;

} tss_struct;

typedef struct _page_table_list_entry
{
	unsigned int addr;
	LIST_ENTRY list;
}page_table_list_entry;

struct _region_elem
{
	int fd;
	int file_off;
	unsigned start;
	unsigned end;
	struct _region_elem* next;
};


typedef void* vm_struct_t; 

typedef struct _user_enviroment
{
	unsigned int page_dir; // every process needs it's own clone of page dir
    unsigned reserve;
    unsigned heap_top;
	//unsigned zone_top;
    vm_struct_t vm;
	//region_elem_t region_head;
}user_enviroment;

typedef enum _ps_status
{
	ps_running,
	ps_ready,
	ps_waiting,
	ps_dying
}ps_status;

typedef enum _ps_type
{
    ps_kernel,
    ps_user,
    ps_dsr
}ps_type;

#define MAX_PRIORITY 5


typedef void (*process_fn)(void* param);

typedef struct _task_frame
{
    unsigned short ds;
    unsigned short ss;
    unsigned short es;
    unsigned short gs;
    unsigned short fs;
    unsigned short cs;
    unsigned long edx;
    unsigned long ecx;
    unsigned long ebx;
    unsigned long eax;
    unsigned long ebp;
    unsigned long eip;
    unsigned long esp0; // kernel esp
	unsigned long esp;  // kernel or user esp
}task_frame;


typedef struct _task_struct
{
	task_frame tss;
    unsigned long cr3;
    unsigned int psid;
    process_fn fn;
	void* param;
	user_enviroment user;
	int priority;
    // in schedule list
	LIST_ENTRY ps_list;
    // in wait list if waiting a lock
    LIST_ENTRY lock_list;
    // in all process list
    LIST_ENTRY ps_mgr;
	ps_status status;
    ps_type type;
    int remain_ticks;
    int is_switching;
    file_descriptor* fds;
    semaphore fd_lock;
	unsigned exit_status;
	unsigned parent;
	unsigned group_id;
	char *cwd;
    unsigned fork_flag;
    semaphore vfork_event;
	unsigned int magic; // to avoid stack overflow

}task_struct;

#define KERNEL_TASK_SIZE 1 // 1 pages
#define DEFAULT_TASK_TIME_SLICE 5

task_struct* CURRENT_TASK();


void ps_init();


unsigned ps_create(process_fn, void* param, int priority, ps_type type);

void ps_kickoff();

int ps_enabled();

void ps_update_tss(unsigned int esp0);


// task functions
void _task_sched(const char* func);
#define task_sched() _task_sched(__func__)


typedef void (*fpuser_map_callback)(void* aux, unsigned vir, unsigned phy);

void ps_enum_user_map(task_struct* task, fpuser_map_callback fn, void* aux);

void ps_cleanup_all_user_map(task_struct* task);

void ps_put_to_ready_queue(task_struct* task);

void ps_put_to_dying_queue(task_struct* task);

void ps_put_to_wait_queue(task_struct* task);

task_struct* ps_find_process(unsigned psid);

// if process that >= priority exist
int ps_has_ready(int priority);
typedef void (*ps_enum_callback)(task_struct* task);
void ps_enum_all(ps_enum_callback callback);
// syscall handler
int sys_fork();
int sys_vfork();
int sys_exit(unsigned status);
int sys_waitpid(unsigned pid, int* status, int options);
char *sys_getcwd(char *buf, unsigned size);
#endif
