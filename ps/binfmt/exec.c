#include <errno.h> 
#include <ps/binfmt/binfmts.h>
#include <mm/mm.h>
#include <config.h>
#include <ps/ps.h>


static struct linux_binfmt *formats = (struct linux_binfmt *) NULL;

#ifdef __VERBOS_SYSCALL__
#define my_print printk
#else
#define my_print(fmt, ...) {}
#endif

void binfmt_setup(void)
{
  init_exe_binfmt(); 
  init_elf_binfmt();
  init_script_binfmt();
}


int register_binfmt(struct linux_binfmt * fmt)
{
  struct linux_binfmt ** tmp = &formats;

  if (!fmt)
    return -EINVAL;
  if (fmt->next)
    return -EBUSY;
  while (*tmp) {
    if (fmt == *tmp)
      return -EBUSY;
    tmp = &(*tmp)->next;
  }
  fmt->next = formats;
  formats = fmt;
  return 0;
}



/*
 * cycle the list of binary formats handler, until one recognizes the image
 */
int search_binary_handler(struct linux_binprm *bprm)
{
  int retval=0;
  struct linux_binfmt *fmt;

  for (fmt = formats ; fmt ; fmt = fmt->next) {
    int (*fn)(struct linux_binprm *) = fmt->load_binary;
    
    if (!fn) 
      continue;
      
    retval = fn(bprm);
    if (retval != -ENOEXEC) 
      break;
  }
  
  return retval;
}


int read_exec(int fd, unsigned long offset,  char *addr, 
              unsigned long count, int to_kmem)
{
  int ret;
	
  ret = fs_read(fd, offset, addr, count);
    
  
  return ret;  
}             

    
    
static int count(char ** argv, int max)
{
  int i = 0;

  if (argv != NULL) {
    for (;;) {
      if (!*argv) 
        break;
      
      argv++;
      if (++i > max) return -E2BIG;
    }
  }
  return i;
}


unsigned long copy_strings(int argc, char ** argv,unsigned long *page,
                           unsigned long p)
{
  char *str;
  char *pag;
  long offset_i, bytes_to_copy;
  if ((long)p <= 0)
    return p; /* bullet-proofing */
  while( argc-- > 0){
	  int len;
	  unsigned long pos;
	  str = argv[argc];
	  if( !str )
		  return -EFAULT;
	  len = strlen(str)+1;
	  if (!len || len > p) {  /* EFAULT or E2BIG */
		  return len ? -E2BIG : -EFAULT;
	  }
	  p -= len;
	  memcpy((char*)page+p, str, len);
  }
  return p;
}


unsigned long setup_arg_pages(unsigned long p, struct linux_binprm * bprm)
{
  unsigned long stack_base;
  int i;

#define STACK_SIZE (1024*1024)-1
#define STACK_BASE (USER_STACK_TOP - STACK_SIZE)

  stack_base = USER_STACK_TOP - MAX_ARG_PAGES*PAGE_SIZE;


  i = do_mmap(-1, STACK_BASE, STACK_SIZE, 0,
              0, 0);
  if (i != STACK_BASE) {
    printf("error mmaping stack\n");
    exit(1);
  }


  p += stack_base;
  if (bprm->loader)
    bprm->loader += stack_base;
  bprm->exec += stack_base;
  
	memcpy((void*)stack_base, (char*)bprm->page, PAGE_SIZE*MAX_ARG_PAGES);
	stack_base += (PAGE_SIZE*MAX_ARG_PAGES);

  
  return p;
}


void remove_arg_zero(struct linux_binprm *bprm)
{
	if (bprm->argc) {
		unsigned long offset;
		char * page;
		offset = bprm->p;
		page = ((char*)bprm->page)[bprm->p/PAGE_SIZE];
		while(bprm->p++,*(page+offset++))
			;
		bprm->argc--;
	}
}

static char* bin_path[] = {
	"",
	"/bin/",
	"/usr/bin/",
	"/usr/local/bin/"
};
 
int do_exec(char *filename, char *argv[], char *envp[])
{
  struct linux_binprm bprm;
  int retval = 0;
  int fd;
  int i;
  char *fullfile;
  char* slash;
  bprm.p = PAGE_SIZE*MAX_ARG_PAGES-sizeof(void *)*PAGE_SIZE;
  bprm.page = vm_alloc(MAX_ARG_PAGES);
  memset( bprm.page, 0, PAGE_SIZE*MAX_ARG_PAGES);

	fullfile = kmalloc(256);

	for (i = 0; i < (sizeof(bin_path) / sizeof(bin_path[0])); i++) {
		strcpy(fullfile, bin_path[i]);
		strcat(fullfile, filename);
		fd = fs_open(fullfile);
		if (fd != MAX_FD)
			break;
	}
	if (fd == MAX_FD) {
		kfree(fullfile);
		return -1;
	}

	filename = fullfile;

  my_print("[exec] filename = %s", filename);

  bprm.filename = filename;
  bprm.fd = fd;
  
  bprm.sh_bang = 0;
  
  bprm.loader = 0;
  bprm.exec = 0;
  
  /* these fields aren't used right now, but init them anyways */
  bprm.e_uid = geteuid();
  bprm.e_gid = getegid();
  
  if ((bprm.argc = count(argv, bprm.p / sizeof(void *))) < 0) {
    close(fd);
		kfree(fullfile);
    return bprm.argc;
  }

  if ((bprm.envc = count(envp, bprm.p / sizeof(void *))) < 0) {
    close(fd);
		kfree(fullfile);
    return bprm.envc;
  }
  
  bzero(bprm.buf, sizeof(bprm.buf));  
  retval = fs_read(bprm.fd, 0, bprm.buf, sizeof(bprm.buf));
  
  if (retval < 0) retval = -1;
  if (retval >= 0) {
	bprm.p = copy_strings(1, &bprm.filename, bprm.page, bprm.p);
    bprm.exec = bprm.p;
    bprm.p = copy_strings(bprm.envc,envp,bprm.page,bprm.p);
    bprm.p = copy_strings(bprm.argc,argv,bprm.page,bprm.p);
    if ((long)bprm.p < 0) {
      retval = (long)bprm.p;
    }
  }


  if (retval >= 0) {
	  my_print("[debug] copy arv, env done\n");
    retval = search_binary_handler(&bprm);
    /* only returns on error */
  }
  
  /*for (i=0 ; i<MAX_ARG_PAGES ; i++) {
    if (bprm.page[i]) {
      free((void*)bprm.page[i]);
    }
  }*/
  if( bprm.page )
	  vm_free(bprm.page, MAX_ARG_PAGES);
  fs_close(bprm.fd);
  return retval;
}


