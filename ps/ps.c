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
static spinlock dying_queue_lock;
static unsigned int ps_id;
static void ps_restore_user_map();
static void lock_ps() {
    spinlock_lock(&ps_lock);
}

static void unlock_ps() {
    spinlock_unlock(&ps_lock);
}

static void lock_dying() {
    spinlock_lock(&dying_queue_lock);
}

static void unlock_dying() {
    spinlock_unlock(&dying_queue_lock);
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

    int_intr_enable();
    _ps_enabled = 1;
    task = CURRENT_TASK();
    task->status = ps_running;
    task->is_switching = 0;
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
    spinlock_init(&dying_queue_lock);
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
    int size = 0;
    task_struct *task = (task_struct *)vm_alloc(KERNEL_TASK_SIZE);
    stack_frame *frame;
    memset(task, 0, KERNEL_TASK_SIZE * PAGE_SIZE);

    size = sizeof(*task);

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
    task->fds = kmalloc(MAX_FD*sizeof(fd_type));

    for (i = 0; i < MAX_FD; i++) {
        task->fds[i].file = 0;
        task->fds[i].file_off = 0;
        task->fds[i].flag = 0;
    }
    task->fds[STDIN_FILENO].file = INODE_STD_IN;
    task->fds[STDOUT_FILENO].file = INODE_STD_OUT;
    task->fds[STDERR_FILENO].file = INODE_STD_ERR;
    task->fds[STDIN_FILENO].flag |= fd_flag_used;
    task->fds[STDOUT_FILENO].flag |= fd_flag_used;
    task->fds[STDERR_FILENO].flag |= fd_flag_used;
    task->user.heap_top = USER_HEAP_BEGIN;
    task->user.zone_top = USER_ZONE_BEGIN;
    memset(task->cwd, 0, 256);
    //strcpy(task->cwd, "\0");

    sema_init(&task->fd_lock, "fd_lock", 0);
    task->magic = 0xdeadbeef; 

    frame = (stack_frame *)(unsigned int)(stack_buttom - sizeof(*frame));
    frame->ebp = stack_buttom;
    frame->eip = (unsigned int)ps_run;
    frame->fs = frame->gs = frame->es = frame->ss = frame->ds = 
        KERNEL_DATA_SELECTOR;

    task->esp0 = task->esp = (unsigned int)frame;
    ps_put_task(&control.ready_queue, task);
	
	return task->psid;
}

static void ps_dup_fds(task_struct* cur, task_struct* task)
{
    // FIXME
    memset(task->fds, 0, MAX_FD*sizeof(fd_type));
    task->fds[STDIN_FILENO].file = INODE_STD_IN;
    task->fds[STDOUT_FILENO].file = INODE_STD_OUT;
    task->fds[STDERR_FILENO].file = INODE_STD_ERR;
    task->fds[STDIN_FILENO].flag |= fd_flag_used;
    task->fds[STDOUT_FILENO].flag |= fd_flag_used;
    task->fds[STDERR_FILENO].flag |= fd_flag_used;
}

static void ps_dup_user_page(unsigned vir, unsigned int* new_ps, unsigned flag)
{
    unsigned int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
    unsigned int page_table_offset = ADDR_TO_PET_OFFSET(vir);
    unsigned int* page_table;
    unsigned int tmp;
    int i = 0;

    if (!new_ps[page_dir_offset]) {
        new_ps[page_dir_offset] = mm_alloc_page_table() - KERNEL_OFFSET;
        new_ps[page_dir_offset] |= flag;

    }

    page_table = (new_ps[page_dir_offset]&PAGE_SIZE_MASK) + KERNEL_OFFSET;
    tmp = vm_alloc(1);
    page_table[page_table_offset] = tmp - KERNEL_OFFSET;
    page_table[page_table_offset] |= flag;
    memcpy(tmp, vir, PAGE_SIZE);
    vm_free(tmp, 1);
    mm_set_phy_page_mask( (tmp-KERNEL_OFFSET) / PAGE_SIZE, 1);
}

static void ps_dup_user_maps(task_struct* cur, task_struct* task)
{
    unsigned int* per_ps = 0;
    unsigned int* new_ps = 0;
    LIST_ENTRY* entry = 0;
    int i = 0;
    unsigned int cr3;

    __asm__("movl %%cr3, %0" : "=q"(cr3));
    per_ps = (unsigned int*)(cr3+KERNEL_OFFSET);
    task->user.page_dir = vm_alloc(1);
    new_ps = (unsigned int*)task->user.page_dir;
    for (i = 0; i < 1024; i++) {
        new_ps[i] = 0;
    }

    InitializeListHead(&(task->user.page_table_list));
    entry = cur->user.page_table_list.Flink;
    while (entry != (&cur->user.page_table_list)) {
        page_table_list_entry* pt = CONTAINER_OF(entry, page_table_list_entry, list);
        page_table_list_entry* new_entry = kmalloc(sizeof(*new_entry));

        ps_dup_user_page(pt->addr, new_ps, PAGE_ENTRY_USER_DATA);

        new_entry->addr = pt->addr;
        InsertTailList(&task->user.page_table_list, &new_entry->list);

        entry = entry->Flink;
    }
}

static void ps_resolve_ebp_to_new(unsigned ebp)
{
    unsigned offset;
    unsigned bottom = ebp & PAGE_SIZE_MASK;
    unsigned top = bottom + PAGE_SIZE - 4;
    while (ebp != 0 && ebp > bottom && ebp < top) {
        unsigned *saved = (unsigned *)ebp; 
        unsigned saved_ebp = *saved;
        if (saved_ebp == 0 || saved_ebp < KERNEL_OFFSET) {
            return;
        }
        if (saved_ebp > top || saved_ebp < bottom) {
            offset = saved_ebp & (~PAGE_SIZE_MASK);
            *saved = bottom + offset;
        }

        ebp = *saved;
    }
}

int sys_fork()
{
    // todo:
    // 1. create new task struct
    // 2. copy flags, eip, esp from current to new
    //      * actually whole page should be copied
    //      * then init new task, set values if possible
    //      * or else kernel stack will be missed
    // 3. copy file descriptors
    //      * open INODE in new task
    // 4. copy user memory maps
    //      * page tables should be newly allocated
    //      * target physical pages newly allocated too
    //      * then copy content from current to new
    task_struct* cur = CURRENT_TASK();
    task_struct* task = vm_alloc(KERNEL_TASK_SIZE);
    unsigned esp = 0, ebp = 0;
    stack_frame* frame = 0;
    unsigned esp_off = 0;
    int is_parent = 0;

    memcpy(task, cur, PAGE_SIZE);
    task->remain_ticks = DEFAULT_TASK_TIME_SLICE;
    task->psid = ps_id_gen();
    sema_init(&task->fd_lock, "fd_lock", 0);
    __asm__ ("movl %%esp, %0" : "=m"(esp));
    esp_off = esp - (unsigned)cur;
    frame = (stack_frame*)((unsigned)task + esp_off - sizeof(*frame));
    __asm__ ("movl %%ebp, %0" : "=m"(ebp));
    __asm__ ("movl $CHILD, %0" : "=m"(frame->eip) );
    frame->fs = frame->gs = frame->es = frame->ss = frame->ds = 
        KERNEL_DATA_SELECTOR;
    frame->ebp = (ebp - (unsigned)cur) + (unsigned)task;
    ps_resolve_ebp_to_new(frame->ebp);
    task->esp = frame; 
    task->esp0 = (unsigned int)task + PAGE_SIZE;
    task->parent = cur->psid;
    task->fds = kmalloc(MAX_FD*sizeof(fd_type));
    memset(task->cwd, 0, 256);
    strcpy(task->cwd, cur->cwd);
    ps_dup_fds(cur, task);
    ps_dup_user_maps(cur,task);
    ps_put_task(&control.ready_queue,task);


    task_sched();
    is_parent = 1;
    __asm__("CHILD: nop");

    if (is_parent) {
        return task->psid;
    }else{
        cur = CURRENT_TASK();
        ps_update_tss(cur->esp0);
        ps_restore_user_map();
        cur->is_switching = 0;
        int_intr_enable();
        return 0; 
    }


}

int sys_exit(unsigned status)
{
    task_struct *cur = CURRENT_TASK();
    cur->exit_status = status;
    ps_cleanup_all_user_map(cur);
    if (cur->user.page_dir) {
        vm_free(cur->user.page_dir, 1);
    }

    if (cur->psid == 0) {
        printk("fatal error! process 0 exit\n");
        __asm__ ("hlt");
    }

    // FIXME
    // modify all child->parent into cur->parent
    cur->status = ps_dying;
    ps_put_task(&control.dying_queue,cur);
    task_sched();
    return 0;
}

char *sys_getcwd(char *buf, unsigned size)
{
	task_struct* cur = CURRENT_TASK();

	if (!cur->cwd[0])
	    strcpy(buf, "/");
	else
		strcpy(buf, cur->cwd);

	return buf;
}


int sys_waitpid(unsigned pid, int* status, int options)
{
    LIST_ENTRY* entry = 0;
    task_struct* cur = CURRENT_TASK();
    int ret = 0;
    int can_return = 0;
    do {
        lock_dying();
        entry = control.dying_queue.Flink;
        while (entry != &control.dying_queue) {
            task_struct* task = CONTAINER_OF(entry, task_struct, ps_list);
            int match = 0;
            if (task->parent == cur->psid) {
                if ( pid && (pid == task->psid)) {
                    match = 1;
                }else if(!pid){
                    match = 1;
                }
            }

            if (match) {
                ret = task->psid;
                if (status) {
                    *status = task->exit_status;
                }
                RemoveEntryList(entry);
                kfree(task->fds);
                vm_free(task, 1);
                can_return = 1;
                break;
            }

            entry = entry->Flink;
        }
        unlock_dying();
        task_sched();
    }
    while (can_return == 0); 
    return ret;
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

void ps_kickoff() 
{
    task_struct* cur = CURRENT_TASK();
    cur->psid = 0xffffffff;
    _ps_enabled = 1;
    memset(cur->cwd, 0, 256);
    task_sched();
    return;
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

void ps_cleanup_all_user_map(task_struct* task)
{
    PLIST_ENTRY entry;
    page_table_list_entry* node;

    while (!IsListEmpty(&(task->user.page_table_list))) {
        entry = RemoveHeadList(&(task->user.page_table_list));
        node = CONTAINER_OF(entry, page_table_list_entry, list);
        mm_del_dynamic_map(node->addr);
        kfree(node);
    }
}

void ps_cleanup_dying_task()
{
}

static void ps_save_user_map()
{
    task_struct *current = 0;
    int i = 0;
    current = CURRENT_TASK();

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
}

static void ps_restore_user_map()
{
    task_struct *current = 0;
    int i = 0;
    current = CURRENT_TASK();

    if (current->user.page_dir) {
        unsigned int cr3;
        unsigned int* in_use;
        unsigned int* per_ps = (unsigned int*)current->user.page_dir;

        __asm__("movl %%cr3, %0" : "=q"(cr3));
        in_use = (unsigned int*)(cr3+KERNEL_OFFSET);
        for (i = 0; i < KERNEL_PAGE_DIR_OFFSET; i++) {
            in_use[i] = per_ps[i];
        }

        RELOAD_CR3(cr3);
    }
}

static task_struct* ps_get_next_task(PLIST_ENTRY wait_list)
{
    task_struct* task = 0;
    do {
         task = ps_get_task(wait_list); // next avaliable
         if (!task) {
             break;
         }
         if (task->status == ps_dying) {
            task = ps_get_task(wait_list);
        }else{
            break;
        }
    }while (1); 

    return task;

}

static void _task_sched(PLIST_ENTRY wait_list) {
    task_struct *task = 0;
    task_struct *current = 0;
    int i = 0;
    int intr_enabled = int_is_intr_enabled();

    int_intr_disable();
    current = CURRENT_TASK();
    current->is_switching = 1;
    // put self to ready queue, and choose next
    task = ps_get_next_task(wait_list);
    if (!task) {
        task = current;
        goto SELF;
    }
    task->status = ps_running; 


    if (current->status != ps_dying &&
        current->psid != 0xffffffff) {
        current->status = ps_ready; 
        ps_put_task(wait_list, current);
        // save page dir entry for user space
        ps_save_user_map();
    }
	


    SAVE_ALL();
    __asm__("movl %%esp, %0" : "=q"(current->esp));
    __asm__("movl %0, %%eax" : : "q"(task->esp));
    __asm__("movl %eax, %esp");



    RESTORE_ALL();

    __asm__("NEXT: nop");

    current = CURRENT_TASK();
    reset_tss(current);
    
    ps_restore_user_map();
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

