#include <ps.h>
#include <lock.h>
#include <mm.h>
#include <klib.h>
#include <config.h>
#include <int.h>
#include <list.h>
#include <fs.h>
#include <dsr.h>


typedef struct _ps_control
{
    LIST_ENTRY ready_queue[MAX_PRIORITY];
    LIST_ENTRY dying_queue;
    LIST_ENTRY  wait_queue;
    LIST_ENTRY  mgr_queue;
    task_struct* ps_dsr;
}ps_control;

static unsigned cur_cr3, cr3, next_cr3;
static unsigned next_eip;

static ps_control control;
static spinlock ps_lock;
static spinlock psid_lock;
static spinlock dying_queue_lock;
static spinlock wait_queue_lock;
static spinlock mgr_queue_lock;
static spinlock map_lock;
static unsigned int ps_id;
static void ps_restore_user_map();
static void lock_ps()
{
    spinlock_lock(&ps_lock);
}

static void unlock_ps()
{
    spinlock_unlock(&ps_lock);
}

static void lock_dying()
{
    spinlock_lock(&dying_queue_lock);
}

static void unlock_dying()
{
    spinlock_unlock(&dying_queue_lock);
}

static void lock_waiting()
{
    spinlock_lock(&wait_queue_lock);
}

static void unlock_waiting()
{
    spinlock_unlock(&wait_queue_lock);
}

static void lock_mgr()
{
    spinlock_lock(&mgr_queue_lock);
}

static void unlock_mgr()
{
    spinlock_unlock(&mgr_queue_lock);
}

static void ps_add_mgr(task_struct* task)
{
    lock_mgr();
    InsertTailList(&control.mgr_queue, &task->ps_mgr);
    unlock_mgr();
}

static void ps_remove_mgr(task_struct* task)
{
    lock_mgr();
    RemoveEntryList(&task->ps_mgr);
    unlock_mgr();
}

static void ps_notify_process(unsigned pid)
{
    task_struct* task = ps_find_process(pid);
    if (task)
    {
        ps_put_to_ready_queue(task);
    }
}

task_struct* ps_find_process(unsigned psid)
{
    LIST_ENTRY* head = &control.mgr_queue;
    LIST_ENTRY* entry;
    task_struct* task = 0;

    lock_mgr();

    entry = head->Flink;
    while (entry != head)
    {
        task = CONTAINER_OF(entry, task_struct, ps_mgr);
        if (task->psid == psid)
        {
            break;
        }
        else
        {
            task = 0;
        }
        entry = entry->Flink;
    }
    unlock_mgr();

    return task;
}

void ps_put_to_dying_queue(task_struct* task)
{
    // Remove from whatever queue it is, then put into dying queue
    lock_dying();
    if (task->ps_list.Flink && task->ps_list.Blink)
    {
        RemoveEntryList(&task->ps_list);
    }
    if (task->psid != 0xffffffff)
        InsertTailList(&control.dying_queue, &task->ps_list);
    
    // insert into list before put the status into ps_dying
    // because a ps_dying task will not be picked up by 
    // scheduler. If this task is interrupted between set
    // status and put into list, it will not scheduled anymore
    // which means it will never go into dying_queue
    task->status = ps_dying;

    ps_notify_process(task->parent);
    
    unlock_dying();
}

void ps_put_to_wait_queue(task_struct* task)
{
    // Remove from whatever queue it is, then put into wait queue
    lock_waiting();
    if (task->ps_list.Flink && task->ps_list.Blink)
    {
        RemoveEntryList(&task->ps_list);
    }
    if (task->psid != 0xffffffff)
        InsertTailList(&control.wait_queue, &task->ps_list);

    unlock_waiting();
}

void ps_put_to_ready_queue(task_struct* task)
{
    lock_ps();
    if (task->type == ps_dsr)
    {
        control.ps_dsr = task;
    }
    else
    {
        // Remove from whatever queue it is, then put into ready queue
        if (task->ps_list.Flink && task->ps_list.Blink)
        {
            RemoveEntryList(&task->ps_list);
        }

        if (task->psid != 0xffffffff)
            InsertTailList(&control.ready_queue[task->priority], &task->ps_list);
    }
    unlock_ps();
}

static task_struct* ps_get_available_ready_task(LIST_ENTRY* ready_queue)
{
    LIST_ENTRY* entry;
    task_struct* task = 0;
    entry = ready_queue->Flink;

    while (entry != ready_queue)
    {
        task = CONTAINER_OF(entry, task_struct, ps_list);
        if (task->status == ps_dying)
        {
            task = 0;
            entry = entry->Flink;
            continue;
        }
        else
        {
            break;
        }
    }

    // put it to last, so it will not be picked immediately next time
    if (task)
    {
        RemoveEntryList(&task->ps_list);
        InsertTailList(&control.ready_queue[task->priority], &task->ps_list);
    }
    return task;
}

static task_struct* ps_get_next_task()
{
    task_struct* task = 0;
    int i = MAX_PRIORITY - 1;
    LIST_ENTRY* ready_queue;
    if (dsr_has_task())
    {
        return control.ps_dsr;
    }
        
    lock_ps();

    for (; i >= 0; i--)
    {
        ready_queue = &control.ready_queue[i];
        if (IsListEmpty(ready_queue))
        {
            continue;
        }

        task = ps_get_available_ready_task(ready_queue);
        if (!task)
        {
            continue;
        }
        else
        {
            break;
        }
    }
    unlock_ps();
    return task;

}

static int _ps_enabled = 0;
unsigned task_schedule_time = 0;
static unsigned sched_begin = 0;
static unsigned sched_end = 0;

static void sched_cal_begin()
{
    sched_begin = time_now();
}

static void sched_cal_end()
{
    sched_end = time_now();
    if (sched_begin)
    {
        task_schedule_time += (sched_end - sched_begin);
    }
    sched_begin = sched_end = 0;
}

void report_sched_time()
{
    unsigned total = time_now();
    unsigned sched = task_schedule_time;

    printk("total time: %d.%d, sched used %d.%d\n", (total / 1000), (total % 1000),
        (sched / 1000), (sched % 1000));
}

static void ps_run()
{
    task_struct *task;
    process_fn fn;
    void *param;

    int_intr_enable();
    _ps_enabled = 1;
    task = CURRENT_TASK();
    task->status = ps_running;
    task->is_switching = 0;
    fn = task->fn;
    if (fn)
    {
        fn(task->param);
    }


    task->status = ps_dying;
    // put to dying_queue
    ps_put_to_dying_queue(task);
    ps_remove_mgr(task);
    ps_notify_process(task->parent);
    task_sched();
    //vm_free(task, KERNEL_TASK_SIZE);
}


static int debug = 0;
static tss_struct* tss_address = 0;

void ps_init()
{
    int i = 0;
    InitializeListHead(&control.dying_queue);
    InitializeListHead(&control.wait_queue);
    InitializeListHead(&control.mgr_queue);
    for (i = 0; i < MAX_PRIORITY; i++)
    {
        InitializeListHead(&control.ready_queue[i]);
    }
    spinlock_init(&ps_lock);
    spinlock_init(&psid_lock);
    spinlock_init(&dying_queue_lock);
    spinlock_init(&wait_queue_lock);
    spinlock_init(&mgr_queue_lock);
    spinlock_init(&map_lock);
    ps_id = 0;
    _ps_enabled = 0;
    task_schedule_time = 0;
    sched_begin = sched_end = 0;
    debug = 0;
    tss_address = kmalloc(sizeof(tss_struct));
    int_update_tss((unsigned int)tss_address);

}



int ps_enabled()
{
    return _ps_enabled;
}


static void* psid_map[PSID_MAP_SIZE] = {0};
static inline unsigned int __bsr(unsigned int value)
{
    int i;
    for (i = 0; i < 32; i++){
        if ((value % 2) == 0)
            return i;
        value >>= 1;
    }
}

static int ps_id_find(int index, unsigned* page)
{
    int i = 0;
    int begin = index * PAGE_SIZE * 8;
    int off;
    for (i = 0; i < (PAGE_SIZE/sizeof(*page)); i++){
        if (page[i] == (unsigned)-1) 
            continue;
        off = __bsr(page[i]);
        return begin + i*32 + off;
    }
}

static void ps_id_mark(int psid, int used)
{
    int cnt_per_pg = PAGE_SIZE * 8;
    int pg_idx = psid / cnt_per_pg;
    int pg_off = psid % cnt_per_pg;
    int bit_idx = pg_off / 32;
    int bit_off = pg_off % 32;
    unsigned mask = (unsigned)1 << (bit_off);
    unsigned* page = (unsigned*)psid_map[pg_idx];
    if (used)
        page[bit_idx] |= mask;
    else
        page[bit_idx] &= ~mask;
}

static unsigned  do_ps_id_gen()
{
    int ret = -1;
    int i = 0;
    void* page = NULL;
    
    spinlock_lock(&psid_lock);

    for (i = 0; i < PSID_MAP_SIZE; i++){
        if (psid_map[i] != NULL){
            ret = ps_id_find(i, psid_map[i]);
            if (ret >= 0)
                break;
        }else{
            break;
        }
    }

    if (ret < 0 && i < PSID_MAP_SIZE){
        psid_map[i] = vm_alloc(1);
        ret = ps_id_find(i, psid_map[i]);
    }

    if (ret < 0){
        // too many processes!!
        printk("Too many processes");
        for(;;) __asm__("hlt");
    }

    ps_id_mark(ret, 1);

    spinlock_unlock(&psid_lock);
    return (unsigned)ret;
}
#ifdef PSID_ALLOC
static unsigned last_gen_pid = (unsigned)-1;
#else
static unsigned last_gen_pid = (unsigned)0;
#endif

unsigned ps_id_gen()
{
#ifdef PSID_ALLOC
    unsigned ret = do_ps_id_gen();
    unsigned ret2;
    unsigned id;
    if ( __sync_add_and_fetch(&last_gen_pid, 0) == ret){
        ret2 = do_ps_id_gen();
        ps_id_free(ret);
        __sync_lock_test_and_set(&last_gen_pid, ret2);
        id = ret2;
    }else{
        __sync_lock_test_and_set(&last_gen_pid, ret);
        id = ret;
    }
    return id;
#else
    return __sync_fetch_and_add(&last_gen_pid, 1); 
#endif
}

void ps_id_free(unsigned psid)
{
#ifdef PSID_ALLOC
    spinlock_lock(&psid_lock);
    ps_id_mark(psid, 0);
    spinlock_unlock(&psid_lock);
#endif
}

static void ps_setup_task_frame(task_struct* task, unsigned data_seg,
                                unsigned eax, unsigned ebx,
                                unsigned ecx, unsigned edx,
                                unsigned ebp, unsigned esp,
                                unsigned esp0, unsigned eip)
{
    task->tss.fs = 
    task->tss.gs = 
    task->tss.es = 
    task->tss.ss = 
    task->tss.ds = data_seg;
    task->tss.cs = KERNEL_CODE_SELECTOR;
    task->tss.eax = eax;
    task->tss.ebx = ebx;
    task->tss.ecx = ecx;
    task->tss.edx = edx;
    task->tss.ebp = ebp;
    task->tss.esp = esp;
    task->tss.esp0 = esp0;
    task->tss.eip = eip;
}

#define SAVE_ALL(task)\
({\
    __asm__("movl $NEXT, %0" : "=m"(current->tss.eip));\
    __asm__("movl %%ebp, %0" : "=m"(current->tss.ebp));\
    __asm__("movl %%eax, %0" : "=m"(current->tss.eax));\
    __asm__("movl %%ebx, %0" : "=m"(current->tss.ebx));\
    __asm__("movl %%ecx, %0" : "=m"(current->tss.ecx));\
    __asm__("movl %%edx, %0" : "=m"(current->tss.edx));\
    __asm__("movl %%esp, %0" : "=m"(current->tss.esp));\
    __asm__("mov %%fs, %0" : "=m"(current->tss.fs));\
    __asm__("mov %%gs, %0" : "=m"(current->tss.gs));\
    __asm__("mov %%es, %0" : "=m"(current->tss.es));\
    __asm__("mov %%ss, %0" : "=m"(current->tss.ss));\
    __asm__("mov %%ds, %0" : "=m"(current->tss.ds));\
})

#define RESTORE_ALL(task)\
({\
    __asm__("mov %0, %%ds" : : "m"(task->tss.ds));\
    __asm__("mov %0, %%ss" : : "m"(task->tss.ss));\
    __asm__("mov %0, %%es" : : "m"(task->tss.es));\
    __asm__("mov %0, %%gs" : : "m"(task->tss.gs));\
    __asm__("mov %0, %%fs" : : "m"(task->tss.fs));\
    __asm__("movl %0, %%edx" : : "m"(task->tss.edx));\
    __asm__("movl %0, %%ecx" : : "m"(task->tss.ecx));\
    __asm__("movl %0, %%ebx" : : "m"(task->tss.ebx));\
    __asm__("movl %0, %%eax" : : "m"(task->tss.eax));\
    /* after we change ebp, "task" variable will be changed \
     so ebp should be the last one that restored \
     that's why we have to save eip into edx first \
     FIXME: we assume edx not used */ \
    next_eip = task->tss.eip;\
    __asm__("movl %0, %%esp" : : "m"(task->tss.esp));\
    __asm__("movl %0, %%ebp" : : "m"(task->tss.ebp));\
})

#define JUMP_TO_NEXT_TASK_EIP()\
({\
    __asm__("movl %0, %%edx" : : "m"(next_eip));\
    __asm__("jmp *%edx");\
})

// FIXME
// now we ignore priority
unsigned ps_create(process_fn fn, void *param, int priority, ps_type type)
{
    unsigned int stack_buttom;
    int i = 0;
    int size = 0;
    task_struct *task = (task_struct *)vm_alloc(KERNEL_TASK_SIZE);

    if (priority >= MAX_PRIORITY)
    {
        vm_free(task, 1);
        return 0xffffffff;
    }

    memset(task, 0, KERNEL_TASK_SIZE * PAGE_SIZE);

    size = sizeof(*task);

    task->user.reserve = (unsigned)vm_alloc(1);
    task->user.page_dir = (unsigned int)vm_alloc(1);
    // 0 ~ KERNEL_PAGE_DIR_OFFSET-1 are dynamic, KERNEL_PAGE_DIR_OFFSET ~ 1023 are shared
    memset(task->user.page_dir, 0, PAGE_SIZE);

    stack_buttom = (unsigned int)task + KERNEL_TASK_SIZE * PAGE_SIZE - 4;
    LOAD_CR3(task->cr3);
    task->ps_list.Flink = task->ps_list.Blink = 0;
    task->fn = fn;
    task->param = param;
    task->priority = priority;
    task->type = type;
    task->status = ps_ready;
    task->remain_ticks = DEFAULT_TASK_TIME_SLICE;
    task->psid = ps_id_gen();
    task->is_switching = 0;
    task->fds = vm_alloc(1);
    task->cwd = name_get();
    memset(task->fds, 0, PAGE_SIZE);
    task->user.heap_top = USER_HEAP_BEGIN;
    task->user.vm = vm_create();
    memset(task->cwd, 0, MAX_PATH);
    //strcpy(task->cwd, "\0");

    sema_init(&task->fd_lock, "fd_lock", 0);
    task->magic = 0xdeadbeef;

    ps_setup_task_frame(task,
                        KERNEL_DATA_SELECTOR,
                        0, 0, 0, 0,
                        (unsigned)stack_buttom,
                        (unsigned)stack_buttom,
                        (unsigned)stack_buttom,
                        (unsigned)ps_run);
    ps_put_to_ready_queue(task);
    ps_add_mgr(task);
    return task->psid;
}

static void ps_dup_fds(task_struct* cur, task_struct* task)
{
    unsigned fd;
    int i = 0;
    memset(task->fds, 0, PAGE_SIZE);
    sema_wait(&cur->fd_lock);
    for (i = 0; i < MAX_FD; i++){
        if (!cur->fds[i].used)
            continue;

        task->fds[i] = cur->fds[i];
        fs_refrence(cur->fds[i].fp);
    }
    sema_trigger(&cur->fd_lock);
}

extern short pgc_entry_count[1024];
static void ps_dup_user_page(unsigned vir, task_struct* task, unsigned flag)
{
    unsigned int* new_ps = (unsigned int*)task->user.page_dir;
    unsigned target_page_idx = mm_get_attached_page_index(vir);
    unsigned target_page_addr = target_page_idx * PAGE_SIZE;
    unsigned int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
    unsigned int page_table_offset = ADDR_TO_PET_OFFSET(vir);
    unsigned int* cur_page_table;
    unsigned int* cur_page_dir = mm_get_pagedir();
    task_struct* cur = CURRENT_TASK();
    unsigned int* page_table;
    unsigned int tmp, phy;
    int i = 0, idx;

    if (!(new_ps[page_dir_offset] & PAGE_SIZE_MASK))
    {
        new_ps[page_dir_offset] = mm_alloc_page_table() - KERNEL_OFFSET;
        new_ps[page_dir_offset] |= flag;

    }

    page_table = (new_ps[page_dir_offset] & PAGE_SIZE_MASK) + KERNEL_OFFSET;
    cur_page_table = (cur_page_dir[page_dir_offset] & PAGE_SIZE_MASK) + KERNEL_OFFSET;
    cur_page_table[page_table_offset] &= ~PAGE_ENTRY_WRITABLE;

    phy = page_table[page_table_offset] & PAGE_SIZE_MASK;
    if (!phy)
    {
        idx = (PAGE_TABLE_CACHE_END - (unsigned)page_table) / PAGE_SIZE - 1;
        pgc_entry_count[idx]++;
    }

    page_table[page_table_offset] = cur_page_table[page_table_offset];
    phymm_reference_page(target_page_idx);
}


static void ps_enum_for_dup(void* aux, unsigned vir, unsigned phy)
{
    task_struct* task = aux;
    ps_dup_user_page(vir, task, PAGE_ENTRY_USER_DATA);

}

static void ps_dup_user_maps(task_struct* cur, task_struct* task)
{
    unsigned int* per_ps = 0;
    unsigned int* new_ps = 0;
    LIST_ENTRY* entry = 0;
    int i = 0;
    unsigned int cr3;

    __asm__("movl %%cr3, %0" : "=q"(cr3));
    per_ps = (unsigned int*)(cr3 + KERNEL_OFFSET);
    task->user.page_dir = vm_alloc(1);

    vm_dup(cur->user.vm, task->user.vm);

    new_ps = (unsigned int*)task->user.page_dir;
    memset((char*)new_ps, 0, PAGE_SIZE);
    ps_enum_user_map(cur, ps_enum_for_dup, task);
}

extern void ret_from_syscall();
int do_fork(unsigned flag)
{
    task_struct* cur = CURRENT_TASK();
    task_struct* task = vm_alloc(KERNEL_TASK_SIZE);
    intr_frame* cur_intr_frame = (intr_frame*)((char*)cur + PAGE_SIZE - sizeof(intr_frame));
    intr_frame* task_intr_frame = (intr_frame*)((char*)task + PAGE_SIZE - sizeof(intr_frame));

    //memcpy(task, cur, PAGE_SIZE);
    *task = *cur;
    *task_intr_frame = *cur_intr_frame;
    if (cur->remain_ticks > DEFAULT_TASK_TIME_SLICE / 2)
        cur->remain_ticks = task->remain_ticks = cur->remain_ticks / 2;
    else
        task->remain_ticks = cur->remain_ticks;
    task->psid = ps_id_gen();
    sema_init(&task->fd_lock, "fd_lock", 0);

    // for kernel part
    // when return from intr, those will restored by user mode value
    task->tss.fs = task->tss.gs = task->tss.ds = task->tss.es = task->tss.ss = KERNEL_DATA_SELECTOR;
    task->tss.cs = KERNEL_CODE_SELECTOR;
    // those are for new task
    task->tss.eax = 0;
    task->tss.ebp = (char*)task_intr_frame;
    task->tss.esp = (char*)task_intr_frame;
    task->tss.esp0 = (unsigned)task + PAGE_SIZE;
    task->tss.eip = (unsigned)ret_from_syscall;
    task_intr_frame->eax = 0;
    task->parent = cur->psid;
    task->fds = vm_alloc(1);//kmalloc(MAX_FD*sizeof(fd_type));
    task->ps_list.Blink = task->ps_list.Flink = 0;
    task->user.vm = vm_create();
    task->user.reserve = (unsigned)vm_alloc(1);
    task->cwd = name_get();
    task->fork_flag = flag;
    memset(task->cwd, 0, MAX_PATH);
    strcpy(task->cwd, cur->cwd);
    ps_dup_fds(cur, task);
    ps_dup_user_maps(cur, task);
    ps_put_to_ready_queue(task);
    ps_add_mgr(task);
    if (flag & FORK_FLAG_VFORK){
        sema_init(&task->vfork_event, "vfork_event", 1);
        sema_wait(&task->vfork_event);
    }
    cur_intr_frame->eax = task->psid;
    return task->psid;
}

int sys_fork()
{
    if (TestControl.verbos)
        klog("%d: fork()\n", CURRENT_TASK()->psid);
    
    return do_fork(0);
}

/*
 * currently we still duplication address space
 * it's fast enough because of COW
 */
int sys_vfork()
{
        if (TestControl.verbos)
        klog("%d: vfork()\n", CURRENT_TASK()->psid);
    
    return do_fork(FORK_FLAG_VFORK);
}

int sys_exit(unsigned status)
{
    task_struct *cur = CURRENT_TASK();
    int i = 0;
    cur->exit_status = status;
    if (TestControl.verbos)
        klog("%d: exit(%d)\n", cur->psid, status);

    if (cur->fork_flag & FORK_FLAG_VFORK){
        sema_trigger(&cur->vfork_event);
    }

    if (cur->user.vm)
    {
        vm_destroy(cur->user.vm);
        cur->user.vm = 0;
    }

    // close all fds
    for (i = 0; i < MAX_FD; i++)
    {
        if (cur->fds[i].used)
        {
            fs_close(i);
        }
    }

    vm_free(cur->fds, 1);
    cur->fds = NULL;

    // free all physical memory
    ps_cleanup_all_user_map(cur);

    if (cur->psid == 0)
    {
        printk("fatal error! process 0 exit\n");
        __asm__("hlt");
    }

    if (cur->psid == 1)
    {
        reboot();
    }
    // things like cur->user.page_dir have to to freed in
    // cleanup stage (polled by waitpid) because this sys
    // call can be interrupted

    // FIXME
    // modify all child->parent into cur->parent
    ps_put_to_dying_queue(cur);
    
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

#ifdef TRACE_SCHED_CALL
static struct _sched_call_map
{
    char* func;
    int times
} sched_call_map[100] = {0};


static void sched_call_map_add(const char* func, int times)
{
    int i;
    int found = 0;
    spinlock_lock(&map_lock);
    for (i = 0; i < 100; i++)
    {
        if (sched_call_map[i].func != NULL)
            if (!strcmp(sched_call_map[i].func, func)) {
                sched_call_map[i].times += times;
                found = 1;
                break;
            }
    }

    if (!found)
    {
        for(i = 0; i < 100; i++)
            if (sched_call_map[i].func == NULL)
            {
                sched_call_map[i].func = strdup(func);
                sched_call_map[i].times = times;
                break;
            }
    }
    spinlock_unlock(&map_lock);
}

static void sched_call_map_clear()
{
    int i;
    spinlock_lock(&map_lock);
    for (i = 0; i < 100; i++)
    {
        if (sched_call_map[i].func != NULL)
        {
            kfree(sched_call_map[i].func);
            sched_call_map[i].func = NULL;
            sched_call_map[i].times = 0;
        }
    }
    spinlock_unlock(&map_lock);
}

static void sched_call_map_print()
{
    int i;
    spinlock_lock(&map_lock);
    for (i = 0; i < 100; i++)
    {
        if (sched_call_map[i].func != NULL)
        {
            printk("[sched] %s: %d times\n", sched_call_map[i].func, sched_call_map[i].times);
        }
    }
    spinlock_unlock(&map_lock);
}
#endif

int sys_waitpid(unsigned pid, int* status, int options)
{
    LIST_ENTRY* entry = 0;
    task_struct* cur = CURRENT_TASK();
    int ret = 0;
    int can_return = 0;
    if (TestControl.verbos) {
        klog("%d: wait(%d)\n", CURRENT_TASK()->psid, pid);
    }
    
    do
    {
        lock_dying();
        entry = control.dying_queue.Flink;
        while (entry != &control.dying_queue)
        {
            task_struct* task = CONTAINER_OF(entry, task_struct, ps_list);
            int match = 0;
            if (task->parent == cur->psid)
            {
                if (pid && (pid == task->psid))
                {
                    match = 1;
                }
                else if (!pid)
                {
                    match = 1;
                }
            }

            if (match)
            {
                ret = task->psid;
                if (status)
                {
                    *status = task->exit_status;
                }
                RemoveEntryList(entry);
                if (task->user.reserve)
                {
                    vm_free(task->user.reserve, 1);
                    task->user.reserve = 0;
                }

                if (task->cwd)
                {
                    name_put(task->cwd);
                    task->cwd = 0;
                }

                if (task->user.page_dir)
                {
                    vm_free(task->user.page_dir, 1);
                    task->user.page_dir = 0;
                }
                ps_remove_mgr(task);
                vm_free(task, 1);
                can_return = 1;
#ifdef TRACE_SCHED_CALL
                sched_call_map_print();
                sched_call_map_clear();
#endif
                break;
            }

            entry = entry->Flink;
        }

        if (!can_return)
        {
            ps_put_to_wait_queue(cur);
        }
        unlock_dying();
        task_sched();
    }
    while (can_return == 0);
    ps_id_free(ret);
    return ret;
}


static void reset_tss(task_struct* task)
{
    tss_address->cr3 = task->cr3;
    tss_address->esp0 = task->tss.esp0;
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
    task->tss.esp0 = esp0;
    reset_tss(task);

}

void ps_kickoff()
{
    task_struct* cur = CURRENT_TASK();
    cur->psid = 0xffffffff;
    cur->ps_list.Flink = cur->ps_list.Blink = 0;
    _ps_enabled = 1;
    task_sched();
    return;
}

task_struct* CURRENT_TASK()
{
    unsigned int esp;
    task_struct *ret;
    __asm__("movl %%esp, %0" : "=m"(esp));
    ret = (task_struct *)((unsigned int)esp & PAGE_SIZE_MASK);
    return ret;
}





static void ps_cleanup_enum_callback(void* aux, unsigned vir, unsigned phy)
{
    mm_del_dynamic_map(vir);
}

void ps_cleanup_all_user_map(task_struct* task)
{
    ps_enum_user_map(task, ps_cleanup_enum_callback, 0);
    REFRESH_CACHE();
}

void ps_enum_user_map(task_struct* task, fpuser_map_callback fn, void* aux)
{
    unsigned i = 0, j = 0;

    if (!fn)
    {
        return;
    }

    if (task->user.page_dir)
    {
        unsigned int* page_dir = (unsigned int*)task->user.page_dir;
        for (i = 0; i < KERNEL_PAGE_DIR_OFFSET; i++)
        {
            unsigned *page_table = (unsigned*)(page_dir[i] & PAGE_SIZE_MASK);
            if (!page_table)
            {
                continue;
            }

            page_table = (unsigned*)((unsigned)page_table + KERNEL_OFFSET);
            for (j = 0; j < 1024; j++)
            {
                if (page_table[j] != 0)
                {
                    unsigned vir = (i << 22) + (j << 12);
                    unsigned phy = page_table[j] & PAGE_SIZE_MASK;
                    fn(aux, vir, phy);
                }
            }
        }
    }
}

static void ps_save_kernel_map(task_struct* task)
{
    int i = 0;

    if (task->user.page_dir)
    {
        unsigned int cr3;
        unsigned int* in_use;
        unsigned int* per_ps = (unsigned int*)task->user.page_dir;

        __asm__("movl %%cr3, %0" : "=q"(cr3));
        in_use = (unsigned int*)(cr3 + KERNEL_OFFSET);
        for (i = KERNEL_PAGE_DIR_OFFSET; i < 1024; i++)
        {
            per_ps[i] = in_use[i];
        }
    }
}

static void ps_restore_user_map()
{
    task_struct *current = 0;
    int i = 0;
    current = CURRENT_TASK();

    if (current->user.page_dir)
    {
        unsigned int cr3;
        unsigned int* in_use;
        unsigned int* per_ps = (unsigned int*)current->user.page_dir;

        __asm__("movl %%cr3, %0" : "=q"(cr3));
        in_use = (unsigned int*)(cr3 + KERNEL_OFFSET);
        for (i = 0; i < KERNEL_PAGE_DIR_OFFSET; i++)
        {
            in_use[i] = per_ps[i];
        }

        RELOAD_CR3(cr3);
    }
}


void _task_sched(const char* func)
{
    task_struct *task = 0;
    task_struct *current = 0;
    unsigned pdr;
    int i = 0;
    int intr_enabled = int_is_intr_enabled();


    sched_cal_begin();

    int_intr_disable();

    current = CURRENT_TASK();
    current->is_switching = 1;
    task = ps_get_next_task();

    cur_cr3 = current->user.page_dir - KERNEL_OFFSET;
    __asm__("movl %%cr3, %0" : "=q"(cr3));
    if (task->psid == current->psid)
    {
        goto SELF;
    }

#ifdef TRACE_SCHED_CALL
    sched_call_map_add(func, 1);
#endif

    task->status = ps_running;
    next_cr3 = task->user.page_dir - KERNEL_OFFSET;
    // save page dir entry for kernel space
    ps_save_kernel_map(task);

    SAVE_ALL(current);
    
    RESTORE_ALL(task);
    RELOAD_CR3(next_cr3);
    current = CURRENT_TASK();
    reset_tss(current);
    JUMP_TO_NEXT_TASK_EIP();
    __asm__("NEXT: nop");
SELF:
    int_intr_enable();
    current->is_switching = 0;
    sched_cal_end();
    return;
}


int ps_has_ready(int priority)
{
    int i = 0;

    if (priority >= MAX_PRIORITY || priority < 0)
    {
        return 0;
    }

    for (i = priority; i < MAX_PRIORITY; i++)
    {
        LIST_ENTRY* head = &(control.ready_queue[i]);
        if (!IsListEmpty(head))
            return 1;
    }

    return 0;
}

void ps_enum_all(ps_enum_callback callback)
{
    LIST_ENTRY* head = &control.mgr_queue;
    LIST_ENTRY* node = head->Flink;
    if (!callback)
        return;

    while (node != head){
        task_struct* task = (task_struct*)CONTAINER_OF(node, task_struct, ps_mgr);
        callback(task);
        node = node->Flink;
    }
}
