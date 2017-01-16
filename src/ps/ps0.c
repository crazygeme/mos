#include <ps0.h>
#include <ps.h>
#include <int.h>
#include <klib.h>
#include <elf.h>
#include <lock.h>
#include <mm.h>
#include <config.h>
#include <unistd.h>
#include <mmap.h>
#include <fcntl.h>
#include <fs.h>
#include <errno.h>

static void cleanup()
{
    task_struct* cur = CURRENT_TASK();
    int i = 0;

    if (cur->fork_flag & FORK_FLAG_VFORK){
        sema_trigger(&cur->vfork_event);
    }

    for (i = 0; i < MAX_FD; i++)
    {
        if ( cur->fds[i].used && (cur->fds[i].flag & O_CLOEXEC))
        {
            fs_close(i);
        }
    }
    ps_cleanup_all_user_map(cur);

    cur->user.heap_top = USER_HEAP_BEGIN;
    vm_destroy(cur->user.vm);
    cur->user.vm = vm_create();
}

static void ps_get_argc_envc(const char* file,
    char** argv, char** envp,
    unsigned* argv_len, unsigned* envp_len)
{
    char* tmp = 0;
    int i = 0;
    if (!argv_len || !envp_len)
    {
        return;
    }

    *argv_len = *envp_len = 0;
    //*argv_len = *argv_len + 1;
    if (argv)
    {
        tmp = argv[i];
        while (argv[i] && *argv[i])
        {
            *argv_len = *argv_len + 1;
            tmp = argv[++i];
        }
    }

    i = 0;
    if (envp)
    {
        tmp = envp[i];
        while (envp[i] && *envp[i])
        {
            *envp_len = *envp_len + 1;
            tmp = envp[++i];
        }
    }
}

static char** ps_save_argv(const char* file, char** argv, unsigned argc, char* script)
{
    char** ret = 0;
    int i = 0;
    if (!argc)
    {
        return 0;
    }

    if (script){
        ret = kmalloc((argc + 1) * sizeof(char*));
        ret[0] = strdup(script);
        for (i = 0; i < (argc); i++)
            ret[i+1] = strdup(argv[i]);
    }else{
        ret = kmalloc(argc * sizeof(char*));
        for (i = 0; i < (argc); i++)
            ret[i] = strdup(argv[i]);
    }


    return ret;
}

static char** ps_save_envp(char** envp, unsigned envc)
{
    int i = 0;
    char** ret = 0;
    if (!envc)
    {
        return 0;
    }

    ret = kmalloc(envc * sizeof(char*));
    for (i = 0; i < envc; i++)
    {
        ret[i] = strdup(envp[i]);
    }

    return ret;

}

static void ps_free_v(char** v, unsigned size)
{
    int i = 0;
    if (!v)
    {
        return;
    }

    for (i = 0; i < size; i++)
    {
        kfree(v[i]);
    }
    kfree(v);
}

/* TODO; fix this */
#define ELF_HWCAP   0x0183FBFF  //(boot_cpu_data.x86_capability)

/* This yields a string that ld.so will use to load implementation
specific libraries for optimization.  This is more specific in
intent than poking at uname or /proc/cpuinfo.

For the moment, we have only optimizations for the Intel generations,
but that could change... */

#define ELF_PLATFORM "i686"
/* Symbolic values for the entries in the auxiliary table
   put on the initial stack */
#define AT_NULL   0	/* end of vector */
#define AT_IGNORE 1	/* entry should be ignored */
#define AT_EXECFD 2	/* file descriptor of program */
#define AT_PHDR   3	/* program headers for program */
#define AT_PHENT  4	/* size of program header entry */
#define AT_PHNUM  5	/* number of program headers */
#define AT_PAGESZ 6	/* system page size */
#define AT_BASE   7	/* base address of interpreter */
#define AT_FLAGS  8	/* flags */
#define AT_ENTRY  9	/* entry point of program */
#define AT_NOTELF 10	/* program is not ELF */
#define AT_UID    11	/* real uid */
#define AT_EUID   12	/* effective uid */
#define AT_GID    13	/* real gid */
#define AT_EGID   14	/* effective gid */
#define AT_PLATFORM 15  /* string identifying CPU for optimizations */
#define AT_HWCAP  16    /* arch dependent hints at CPU capabilities */
#define AT_CLKTCK   17      /* Frequency of times() */

static unsigned ps_setup_v(char* file,
    int argc, char** argv,
    int envc, char** envp,
    unsigned top,
    mos_binfmt* exec)
{
    int i = 0;
    char *esp = (char *)(top);
    unsigned *sp, *platform = 0;
    int argv_buf_len = argc * sizeof(char *);
    int env_buf_len = envc * sizeof(char *);
    char **tmp_array_argv = 0;
    char **tmp_array_env = 0;
    unsigned argvp, envpp;
    int len;
    tmp_array_argv = kmalloc(argv_buf_len + 4);
    tmp_array_env = kmalloc(env_buf_len + 4);
    // end marker
    esp -= 4;
    *((unsigned *)esp) = 0;
    // file name
    len = strlen(file) + 1;
    esp -= len;
    strcpy(esp, file);
    // env strings
    for (i = envc - 1; i >= 0; i--)
    {
        int len = strlen(envp[i]) + 1;
        esp -= len;
        strcpy(esp, envp[i]);
        tmp_array_env[i] = esp;
        esp[len - 1] = '\0';
    }
    tmp_array_env[envc] = 0;
    // argv strings
    for (i = argc - 1; i >= 0; i--)
    {
        int len = strlen(argv[i]) + 1;
        esp -= len;
        strcpy(esp, argv[i]);
        tmp_array_argv[i] = esp;
        esp[len - 1] = '\0';
    }
    tmp_array_argv[argc] = 0;
    // platform
    len = sizeof(ELF_PLATFORM);
    esp -= len;
    strcpy(esp, ELF_PLATFORM);
    platform = esp;
    // 16 byte padding
    esp = (char *)((~15UL & (unsigned long)(esp)) - 16UL);
    sp = esp;

#define __put_user(val, addr) (*(unsigned long *)(addr) = (unsigned long)(val))

#define NEW_AUX_ENT(nr, id, val)     \
    __put_user((id), sp + (nr * 2)); \
    __put_user((val), sp + (nr * 2 + 1));

    sp -= 2;
    NEW_AUX_ENT(0, AT_NULL, 0); //end of vector
    if (platform)
    {
        sp -= 2;
        NEW_AUX_ENT(0, AT_PLATFORM, (unsigned long)platform);
    }
    sp -= 3 * 2;
    NEW_AUX_ENT(0, AT_HWCAP, ELF_HWCAP);
    NEW_AUX_ENT(1, AT_PAGESZ, 4096); // 4096
    NEW_AUX_ENT(2, AT_CLKTCK, 100);  // 100

    sp -= 10 * 2;

    NEW_AUX_ENT(0, AT_PHDR, exec->elf_load_addr + exec->e_phoff);
    NEW_AUX_ENT(1, AT_PHENT, sizeof(Elf32_Phdr));
    NEW_AUX_ENT(2, AT_PHNUM, exec->e_phnum);
    NEW_AUX_ENT(3, AT_BASE, exec->interp_bias);
    NEW_AUX_ENT(4, AT_FLAGS, 0);
    NEW_AUX_ENT(5, AT_ENTRY, exec->e_entry);
    NEW_AUX_ENT(6, AT_UID, 0);
    NEW_AUX_ENT(7, AT_EUID, 0);
    NEW_AUX_ENT(8, AT_GID, 0);
    NEW_AUX_ENT(9, AT_EGID, 0);

#undef NEW_AUX_ENT

    esp = sp;
    esp -= (env_buf_len + 4);
    envpp = esp;
    memcpy(envpp, tmp_array_env, env_buf_len + 4);
    esp -= (argv_buf_len + 4);
    argvp = esp;
    memcpy(argvp, tmp_array_argv, argv_buf_len + 4);
    kfree(tmp_array_argv);
    kfree(tmp_array_env);
    esp -= 4;
    *((unsigned *)esp) = argc;
    return esp;
}

int sys_execve(const char* file, char** argv, char** envp)
{
    unsigned eip = 0;
    int i = 0;
    unsigned esp_buttom = KERNEL_OFFSET - USER_STACK_PAGES*PAGE_SIZE;
    unsigned esp_top = KERNEL_OFFSET;
    char *file_name;
    unsigned argc = 0, envc = 0;
    char** s_argv = 0;
    char** s_envp = 0;
    mos_binfmt fmt = {0};
    task_struct* cur = CURRENT_TASK();
    struct stat s;
    filep fp;
    int len = PAGE_SIZE;
    char* firstline = NULL;
    size_t wcnt;
    if (!file) {
        return -ENOENT;
    }

    /* resolve path into full path */
    file_name = name_get();
    resolve_path(file, file_name);

    /* make script interp as first argument if file starts with #! */

    /* open file for state checking and first line reading */
    fp = fs_open_file(file_name, O_RDONLY, 0, 1);
    if (!fp){
        name_put(file_name);
        return -ENOENT;
    }

      /* if file not exist, just return */
    if (ext4_fstat(fp->inode, &s) != 0) {
        fs_destroy(fp);
        name_put(file_name);
        return -ENOENT;
    }

    /* if file not executable, just return */
    if (!(s.st_mode & S_IXUSR) || (s.st_size < 4)) {
        fs_destroy(fp);
        name_put(file_name);
        return -EPERM;
    }

    /* read first line */
    len = len > s.st_size ? s.st_size : len;
    firstline = vm_alloc(1);
    if (ext4_fread(fp->inode, firstline, len, &wcnt) != EOK){
        fs_destroy(fp);
        name_put(file_name);
        return -ENOENT;
    }
    fs_destroy(fp);

    /* 
     * check whether starts with #!, 
     * if it is, assign script interp to file_name 
     */
    if (firstline[0] == 0x7f && 
        firstline[1] == 'E' && 
        firstline[2] == 'L' &&
        firstline[1] != 'F'){
        vm_free(firstline, 1);
        firstline = NULL;
    }else if (firstline[0] == '#' && firstline[1] == '!'){
        char* lf = strchr(firstline, '\n');
        if (lf)
            *lf = '\0';
        strcpy(file_name, firstline+2);
    }else{
        vm_free(firstline, 1);
        name_put(file_name);
        return -ENOEXEC;
    }

    /* parse arguments and enviroments */
    /* note that a #! file needs additional slot */
    ps_get_argc_envc(file_name, argv, envp, &argc, &envc);
    s_argv = ps_save_argv(file_name, argv, argc, firstline);
    if (firstline) argc++;
    s_envp = ps_save_envp(envp, envc);

    if (firstline){
        vm_free(firstline, 1);
        firstline = NULL;
    }

    /* save command line into task struct */
    strcpy(cur->command, file_name);
    for (i = 1; i < argc; i++){
        strcat(cur->command, " ");
        strcat(cur->command, s_argv[i]);
    }

    /* log if needed */
    if (TestControl.verbos) {
        klog("%d: execve(%s, [", CURRENT_TASK()->psid, file_name);
        for (i = 0; i < argc; i++) {
            klog_printf("%s ", s_argv[i]);
        }
        klog_printf("]\n");
    }

    /* 
     * unmap all user vm, and close all fds if O_CLOEXEC set
     * note that we will trigger vfork event if this task is 
     * created by vfork syscall
     */
    cleanup();
    
    /*
     * now we parse and load elf file. 
     * A typical executable file in linux  will have a section 
     * "interp" (usually ld-linux.so), we read that interp and 
     * load PT_LOAD sections into memory, e_entry of interp is 
     * the address we are going to jump in, setup the stack in 
     * proper format and jump to interp, that's all we have to
     * do in execv syscall. All staffs like dynamic library 
     * loading / symbol resolve / etc will be handled by interp
     * pretty easy ha?
     */
    elf_map(file_name, &fmt);
    eip = fmt.interp_load_addr;
    if (!eip)
    {
        printk("fatal error: file %s not found!\n", file);
        asm("hlt");
    }

    /* don't forget to setup a stack for our program... */
    do_mmap(esp_buttom, USER_STACK_PAGES*PAGE_SIZE, 0, 0, -1, 0);

    /* setup arguments and enviroments in proper way for interp */
    esp_top = ps_setup_v(file_name, argc, s_argv, envc, s_envp, esp_top, &fmt);

    /* that's all */
    ps_free_v(s_argv, argc);
    ps_free_v(s_envp, envc);
    name_put(file_name);
    extern void switch_to_user_mode(unsigned eip, unsigned esp);
    switch_to_user_mode(eip, esp_top);
    // never return here
    return 0;
}

static void run_if_exist(char* path)
{
    struct stat s;
    char *argv[2];
    argv[0] = path;
    argv[1] = 0;
    if (fs_stat(path, &s) != -1)
    {
        sys_execve(path, argv, 0);
    }
}
static void user_setup_enviroment()
{
    unsigned esp0 = (unsigned)CURRENT_TASK() + PAGE_SIZE;
    ps_update_tss(esp0);
    // fd 0, 1, 2
    fs_open("k", 0, "r");
    fs_open("t", 0, "r");
    fs_open("t", 0, "r");

    run_if_exist("/bin/bash");
}



void user_first_process_run()
{
    user_setup_enviroment();
}
