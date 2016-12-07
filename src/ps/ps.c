#include <ps.h>
#include <lock.h>
#include <mm.h>
#include <klib.h>
#include <config.h>
#include <int.h>
#include <list.h>


typedef struct _ps_control
{
    LIST_ENTRY ready_queue[MAX_PRIORITY];
    LIST_ENTRY dying_queue;
    LIST_ENTRY  wait_queue;
    LIST_ENTRY  mgr_queue;
    task_struct* ps_dsr;
}ps_control;

static unsigned cur_cr3, cr3, next_cr3;

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

unsigned int ps_id_gen()
{
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
unsigned ps_create(process_fn fn, void *param, int priority, ps_type type)
{
    unsigned int stack_buttom;
    int i = 0;
    int size = 0;
    task_struct *task = (task_struct *)vm_alloc(KERNEL_TASK_SIZE);
    stack_frame *frame;

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
    task->esp0 = task->ebp = stack_buttom;
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
    task->fds = vm_alloc(1);//kmalloc(MAX_FD*sizeof(fd_type));

    for (i = 0; i < MAX_FD; i++)
    {
        task->fds[i].file = 0;
        task->fds[i].file_off = 0;
        task->fds[i].flag = 0;
    }
    task->user.heap_top = USER_HEAP_BEGIN;
    task->user.vm = vm_create();
    memset(task->cwd, 0, 64);
    //strcpy(task->cwd, "\0");

    sema_init(&task->fd_lock, "fd_lock", 0);
    task->magic = 0xdeadbeef;

    frame = (stack_frame *)(unsigned int)(stack_buttom - sizeof(*frame));
    frame->ebp = stack_buttom;
    frame->eip = (unsigned int)ps_run;
    frame->fs = frame->gs = frame->es = frame->ss = frame->ds =
        KERNEL_DATA_SELECTOR;

    task->esp0 = task->esp = (unsigned int)frame;
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
    for (i = 0; i < MAX_FD; i++)
    {
        if (!cur->fds[i].flag)
        {
            continue;
        }

        task->fds[i] = cur->fds[i];
        task->fds[i].path = strdup(cur->fds[i].path);
        vfs_refrence(cur->fds[i].file);
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

    // FIXME
    // change it into COW
    new_ps = (unsigned int*)task->user.page_dir;
    for (i = 0; i < 1024; i++)
    {
        new_ps[i] = 0;
    }


    ps_enum_user_map(cur, ps_enum_for_dup, task);

}

static void ps_resolve_ebp_to_new(unsigned ebp)
{
    unsigned offset;
    unsigned bottom = ebp & PAGE_SIZE_MASK;
    unsigned top = bottom + PAGE_SIZE - 4;
    while (ebp != 0 && ebp > bottom && ebp < top)
    {
        unsigned *saved = (unsigned *)ebp;
        unsigned saved_ebp = *saved;
        if (saved_ebp == 0 || saved_ebp < KERNEL_OFFSET)
        {
            return;
        }
        if (saved_ebp > top || saved_ebp < bottom)
        {
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

#ifdef __VERBOS_SYSCALL__
    klog("fork()\n");
#endif
    memcpy(task, cur, PAGE_SIZE);
    task->remain_ticks = DEFAULT_TASK_TIME_SLICE;
    task->psid = ps_id_gen();
    sema_init(&task->fd_lock, "fd_lock", 0);
    __asm__("movl %%esp, %0" : "=m"(esp));
    esp_off = esp - (unsigned)cur;
    frame = (stack_frame*)((unsigned)task + esp_off - sizeof(*frame));
    __asm__("movl %%ebp, %0" : "=m"(ebp));
    __asm__("movl $CHILD, %0" : "=m"(frame->eip));
    frame->fs = frame->gs = frame->es = frame->ss = frame->ds =
        KERNEL_DATA_SELECTOR;
    frame->ebp = (ebp - (unsigned)cur) + (unsigned)task;
    ps_resolve_ebp_to_new(frame->ebp);
    task->esp = frame;
    task->esp0 = (unsigned int)task + PAGE_SIZE;
    task->parent = cur->psid;
    task->fds = vm_alloc(1);//kmalloc(MAX_FD*sizeof(fd_type));
    task->ps_list.Blink = task->ps_list.Flink = 0;
    task->user.vm = vm_create();
    task->user.reserve = (unsigned)vm_alloc(1);
    memset(task->cwd, 0, 64);
    strcpy(task->cwd, cur->cwd);
    ps_dup_fds(cur, task);
    ps_dup_user_maps(cur, task);
    ps_put_to_ready_queue(task);
    ps_add_mgr(task);

    task->remain_ticks = cur->remain_ticks =
        cur->remain_ticks / 2;
    task_sched();
    is_parent = 1;
    __asm__("CHILD: nop");

    if (is_parent)
    {
        return task->psid;
    }
    else
    {
        cur = CURRENT_TASK();
        ps_update_tss(cur->esp0);
        next_cr3 = cur->user.page_dir - KERNEL_OFFSET;
        RELOAD_CR3(next_cr3);
        cur->is_switching = 0;
        int_intr_enable();
        sched_cal_end();
        return 0;
    }


}

int sys_exit(unsigned status)
{
    task_struct *cur = CURRENT_TASK();
    int i = 0;
    cur->exit_status = status;
#ifdef __VERBOS_SYSCALL__
    klog("exit(%d)\n", status);
#endif

    if (cur->user.vm)
    {
        vm_destroy(cur->user.vm);
        cur->user.vm = 0;
    }

    // close all fds
    for (i = 0; i < MAX_FD; i++)
    {
        if (cur->fds[i].flag)
        {
            fs_close(i);
        }
    }

    vm_free(cur->fds, 1);

    // free all physical memory
    ps_cleanup_all_user_map(cur);

    if (cur->psid == 0)
    {
        printk("fatal error! process 0 exit\n");
        __asm__("hlt");
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
    cur->ps_list.Flink = cur->ps_list.Blink = 0;
    _ps_enabled = 1;
    memset(cur->cwd, 0, 64);
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
    if (current->status != ps_dying)
    {
        // save page dir entry for kernel space
        ps_save_kernel_map(task);
    }

    SAVE_ALL();
    __asm__("movl %%esp, %0" : "=q"(current->esp));
    __asm__("movl %0, %%eax" : : "q"(task->esp));
    __asm__("movl %eax, %esp");

    RESTORE_ALL();

    __asm__("NEXT: nop");

    RELOAD_CR3(next_cr3);
    current = CURRENT_TASK();
    reset_tss(current);
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
