#include <ps/ps.h>
#include <ps/lock.h>
#include <mm/mm.h>
#include <lib/klib.h>
#include <config.h>
#include <int/int.h>

typedef struct _ps_control
{
    LIST_ENTRY ready_queue;
    LIST_ENTRY dying_queue;
}ps_control;


static ps_control control;
static spinlock ps_lock;
static spinlock psid_lock;
static unsigned int ps_id;

static void lock_ps() {
    spinlock_lock(&ps_lock);
}

static void unlock_ps() {
    spinlock_unlock(&ps_lock);
}

static task_struct* ps_get_task(PLIST_ENTRY list) {
    PLIST_ENTRY entry;
    task_struct *ret;
    lock_ps();
    if (IsListEmpty(list)) {
        unlock_ps();
        return 0;
    }

    entry = RemoveHeadList(list);

    //printf("get entry %x\n", entry);
    ret = CONTAINER_OF(entry, task_struct, ps_list);
    unlock_ps();
    return ret;
}

static void ps_put_task(PLIST_ENTRY list, task_struct *task) {
    lock_ps();
    //printf("put entry %x\n", &(task->ps_list));

    InsertTailList(list, &(task->ps_list));
    unlock_ps();
}


static int _ps_enabled = 0;

static void ps_run() {
    task_struct *task;
    process_fn fn;
    void *param;

    _ps_enabled = 1;
    task = CURRENT_TASK();
    task->status = ps_running;
    fn = task->fn;
    if (fn) {
        fn(task->param);
    }


    task->status = ps_dying;

//  lock_ps();
//  RemoveEntryList(&task->ps_list);
//  unlock_ps();

    // put to dying_queue
    ps_put_task(&control.dying_queue,task);
    task_sched();
    //vm_free(task, KERNEL_TASK_SIZE);
}


static int debug = 0;
static tss_struct* tss_address = 0;

void ps_init() {
    InitializeListHead(&control.dying_queue);
    InitializeListHead(&control.ready_queue);

    spinlock_init(&ps_lock);
    spinlock_init(&psid_lock);
    ps_id = 0;
    _ps_enabled = 0;
	debug = 0;
    tss_address = kmalloc(sizeof(tss_struct));
    int_update_tss((unsigned int)tss_address);

}



int ps_enabled()
{ 
    return _ps_enabled;
}

unsigned int ps_id_gen() {
    unsigned int ret;
    spinlock_lock(&psid_lock);
    ret = ps_id;
    ps_id++;
    spinlock_unlock(&psid_lock);
    return ret;
}

typedef struct _stack_frame
{
    unsigned long ds;
    unsigned long ss;
    unsigned long es;
    unsigned long gs;
    unsigned long fs;
    unsigned long edx;
    unsigned long ecx;
    unsigned long ebx;
    unsigned long eax;
    unsigned long ebp;
    unsigned long eip;
}stack_frame;


// FIXME
// now we ignore priority
unsigned ps_create(process_fn fn, void *param, int priority, ps_type type) {
    unsigned int stack_buttom;
    int i = 0;
    //unsigned int* val;
    task_struct *task = (task_struct *)vm_alloc(KERNEL_TASK_SIZE);
    stack_frame *frame;
    memset(task, 0, KERNEL_TASK_SIZE * PAGE_SIZE);

    task->user.page_dir = (unsigned int)vm_alloc(1);
    // 0 ~ KERNEL_PAGE_DIR_OFFSET-1 are dynamic, KERNEL_PAGE_DIR_OFFSET ~ 1023 are shared
    memset(task->user.page_dir, 0, PAGE_SIZE);
    InitializeListHead(&(task->user.page_table_list));

	stack_buttom = (unsigned int)task + KERNEL_TASK_SIZE * PAGE_SIZE - 4;
    task->esp0 = task->ebp = stack_buttom;
    LOAD_CR3(task->cr3);

    task->fn = fn;
    task->param = param;
    task->priority = priority;
    task->type = type;
    task->status = ps_ready;
    task->remain_ticks = DEFAULT_TASK_TIME_SLICE;
    task->psid = ps_id_gen();
    task->is_switching = 0;
    for (i = 0; i < MAX_FD; i++) {
        task->fds[i] = 0;
    }

    sema_init(&task->fd_lock, 0, "fd_lock");
    task->magic = 0xdeadbeef; 

    frame = (stack_frame *)(unsigned int)(stack_buttom - sizeof(*frame));
    frame->ebp = stack_buttom;
    frame->eip = (unsigned int)ps_run;
    frame->fs = frame->gs = frame->es = frame->ss = frame->ds = 
        KERNEL_DATA_SELECTOR;

    task->esp0 = task->esp = (unsigned int)frame;
    //val = (unsigned int*)(stack_buttom-4);
    //printf("create ps %x, frame %x, thing in stack button %x\n", task, frame, val[0]);
    ps_put_task(&control.ready_queue, task);
	
	return task->psid;
}


static void reset_tss(task_struct* task)
{
    tss_address->cr3 = task->cr3;
    tss_address->esp0 = task->esp0;
    tss_address->iomap = (unsigned short) sizeof(tss_struct);
    tss_address->ss0 = KERNEL_DATA_SELECTOR;
    tss_address->ss = tss_address->gs = tss_address->fs = 
        tss_address->ds = tss_address->es = KERNEL_DATA_SELECTOR | 0x3;
    tss_address->cs = KERNEL_CODE_SELECTOR | 0x3;
    int_update_tss((unsigned int)tss_address);
}

void ps_update_tss(unsigned int esp0)
{
    task_struct* task = CURRENT_TASK();
    task->esp0 = esp0;
    reset_tss(task);

}

void ps_kickoff() {
    task_struct *task = ps_get_task(&control.ready_queue);

    if (!task) {
        return;
    }


    // when kickoff, it's always in kernel mode
    // FIXME: change it to run in user mode, then we dont have to warry about
    // code segment anymore, tss always do it for us

    //printf("kikoff task %x, type %x, fn %x\n", task, task->type, task->fn);
    // a first process should never return, if it does, we just halt
    if (task->type == ps_kernel) {
        _RELOAD_KERNEL_CS();
        RELOAD_KERNEL_DS();
    } else {
        _RELOAD_USER_CS();
        RELOAD_USER_DS();
    }


    reset_tss(task);
    _ps_enabled = 1;
    __asm__("movl %0, %%eax" : : "m"(task->fn));
    __asm__("pushl %eax");
    __asm__("movl %0, %%ebx" : : "m"(task->param));
    __asm__("pushl %ebx");
    __asm__("movl %0, %%ecx" : : "m"(task->ebp));
    __asm__("pushl %ecx");
    __asm__("movl %0, %%edx" : : "m"(task->esp0));
    __asm__("pushl %edx");
    __asm__("popl %edx");
    __asm__("popl %ecx");
    __asm__("popl %ebx");
    __asm__("popl %eax");
    __asm__("movl %ecx, %ebp");
    __asm__("movl %edx, %esp");
    __asm__("pushl %ebx");
    __asm__("call *%eax");


    __asm__("hlt");


}

task_struct* CURRENT_TASK() {
    unsigned int esp;
    task_struct *ret;
    __asm__("movl %%esp, %0" : "=m"(esp));
    ret = (task_struct *)((unsigned int)esp & PAGE_SIZE_MASK);
    return ret;
}

#define SAVE_ALL() \
    __asm__("push $NEXT");\
    __asm__("pushl %ebp");\
    __asm__("pushl %eax");\
    __asm__("pushl %ebx");\
    __asm__("pushl %ecx");\
    __asm__("pushl %edx");\
    __asm__("push %fs");\
    __asm__("push %gs");\
    __asm__("push %es");\
    __asm__("push %ss");\
    __asm__("push %ds");

#define RESTORE_ALL()\
    __asm__("pop %ds");\
    __asm__("pop %ss");\
    __asm__("pop %es");\
    __asm__("pop %gs");\
    __asm__("pop %fs");\
    __asm__("popl %edx");\
    __asm__("popl %ecx");\
    __asm__("popl %ebx");\
    __asm__("popl %eax");\
    __asm__("popl %ebp");\
    __asm__("popl %eax");\
    __asm__("jmp *%eax");


void ps_cleanup_dying_task()
{
    task_struct *task = 0;

    task = ps_get_task(&control.dying_queue);
    while (task) {
        PLIST_ENTRY entry;
        page_table_list_entry* node;

        //#ifdef DEBUG
        printf("cleanup %d\n", task->psid);
        //#endif

        if (task->user.page_dir) {
            vm_free(task->user.page_dir, 1);
        }

        while (!IsListEmpty(&(task->user.page_table_list))) {
            entry = RemoveHeadList(&(task->user.page_table_list));
            node = CONTAINER_OF(entry, page_table_list_entry, list);
            mm_del_dynamic_map(node->addr);
            kfree(node);
        }

        vm_free((unsigned int)task, KERNEL_TASK_SIZE);
        task = ps_get_task(&control.dying_queue);
    }

}

static void _task_sched(PLIST_ENTRY wait_list) {
    task_struct *task = 0;
    task_struct *current = 0;
    int i = 0;
    int intr_enabled = int_is_intr_enabled();

    int_intr_disable();
    current = CURRENT_TASK();
    current->is_switching = 1;
    do {
         task = ps_get_task(&control.ready_queue); // next avaliable
         if (!task) {
             break;
         }
         if (task->status == ps_dying) {
            task = ps_get_task(&control.ready_queue);
        }else{
            break;
        }
    }while (1); 

    if (!task) {
        task = current;
        goto SELF;
    }
    task->status = ps_running; 


    if (current->status != ps_dying) {
        current->status = ps_ready; 
        ps_put_task(wait_list, current);
    }
	
    // save page dir entry for user space
    if (current->user.page_dir) {
        unsigned int cr3;
        unsigned int* in_use;
        unsigned int* per_ps = (unsigned int*)current->user.page_dir;

        __asm__("movl %%cr3, %0" : "=q"(cr3));
        in_use = (unsigned int*)(cr3+KERNEL_OFFSET);
        for (i = 0; i < KERNEL_PAGE_DIR_OFFSET; i++) {
            per_ps[i] = in_use[i];
        }
    }

    // put self to ready queue, and choose next
    SAVE_ALL();
    __asm__("movl %%esp, %0" : "=q"(current->esp));
    __asm__("movl %0, %%eax" : : "q"(task->esp));
    __asm__("movl %eax, %esp");
    RESTORE_ALL();
    __asm__("NEXT: nop");

    current = CURRENT_TASK();
    reset_tss(current);
    if (current->user.page_dir) {
        unsigned int cr3;
        unsigned int* in_use;
        unsigned int* per_ps = (unsigned int*)current->user.page_dir;

        __asm__("movl %%cr3, %0" : "=q"(cr3));
        in_use = (unsigned int*)(cr3+KERNEL_OFFSET);
        for (i = 0; i < KERNEL_PAGE_DIR_OFFSET; i++) {
            in_use[i] = per_ps[i];
        }
    }
 SELF:
     if (1) {
         int_intr_enable();
     }
     
    current ->is_switching = 0;
    return;
}

void task_sched()
{
	_task_sched(&control.ready_queue);
}

void task_sched_wait(PLIST_ENTRY wait_list)
{
	_task_sched(wait_list);
}

void task_sched_wakeup(PLIST_ENTRY wait_list, int wakeup_all)
{
	PLIST_ENTRY node;
	if(debug){
		printk("task_sched_wakeup, list %x, wakeup all %d\n", wait_list, wakeup_all);
	}
	lock_ps();

	if (wakeup_all){
		while(!IsListEmpty(wait_list)){
			node = RemoveHeadList(wait_list);
			InsertTailList(&control.ready_queue, node);
		}
	}else{
		if (!IsListEmpty(wait_list)){
			node = RemoveHeadList(wait_list);
			InsertTailList(&control.ready_queue, node);
		}
	}
	
	unlock_ps();

	// _task_sched(&control.ready_queue);
}

void ps_record_dynamic_map(unsigned int vir)
{
    task_struct* cur = CURRENT_TASK();
    page_table_list_entry* entry = kmalloc(sizeof(*entry));

    entry->addr = vir;
    InsertTailList(&cur->user.page_table_list, &entry->list);
}

#ifdef TEST_PS
void ps_mmm() {
    task_struct *task1 = (task_struct *)vm_alloc(KERNEL_TASK_SIZE);
    task_struct *task2 = (task_struct *)vm_alloc(KERNEL_TASK_SIZE);

    printf("1: create task1 %x, task2 %x\n", task1, task2);

    ps_put_task(&control.ready_queue, task1);
    ps_put_task(&control.ready_queue, task2);

    task1 = ps_get_task(&control.ready_queue);
    task2 = ps_get_task(&control.ready_queue);

    printf("2: create task1 %x, task2 %x\n", task1, task2);


    ps_put_task(&control.ready_queue, task1);
    ps_put_task(&control.ready_queue, task2);

    task1 = ps_get_task(&control.ready_queue);

    ps_put_task(&control.ready_queue, task1);

    task2 = ps_get_task(&control.ready_queue);

    printf("2: create task1 %x, task2 %x\n", task1, task2);


}
#endif

#ifdef TEST_PS
static void ps_test(void* param)
{
	task_struct *task;
	task = CURRENT_TASK();

    while (1) {
        printk("I'm a process %d\n", task->psid);
		msleep(200);
    }
}

static void ps_test2(void* param)
{
	task_struct *task;
	int i = 0;
	task = CURRENT_TASK();

    while (i++<100) {
        printk("I'm a process %d\n", task->psid);
		msleep(400);
    }
}

static void ps_test3(void* param)
{
	task_struct *task;
	int i = 0;
	task = CURRENT_TASK();

    while (i++<100) {
        printk("I'm a process %d\n", task->psid);
		msleep(600);
    }
}

static void ps_test4(void* param)
{
	task_struct *task;
	unsigned int i = (unsigned int)(param);
	task = CURRENT_TASK();

    while (1) {
        printk("I'm a process %d\n", task->psid);
        msleep(i);
    }
}

void test_ps_process()
{
#define MIN_SLEEP 1000
#define MAX_SLEEP 2000
	ps_create(ps_test4, 1200, 1, ps_kernel);
    {
        int i = 0;
        for (i = 0; i < TEST_PS; i++) {
            unsigned sleep = klib_rand() % (MAX_SLEEP-MIN_SLEEP);
            sleep += MIN_SLEEP;
            ps_create(ps_test4, sleep, 1, ps_kernel);
        }
    }
    ps_kickoff();
}
#endif

