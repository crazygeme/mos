#ifndef _CONFIG_H_
#define _CONFIG_H_
#define  KERNEL_PRIVILEGE  0  
#define  USER_PRIVILEGE    3  
  
#define  NULL_SELECTOR           0x0
#define  KERNEL_DATA_SELECTOR    0x8
#define  KERNEL_CODE_SELECTOR    0x10
#define  USER_DATA_SELECTOR      0x1b
#define  USER_CODE_SELECTOR      0x23
#define  TSS_SELECTOR            0x28 // we are not going to use TSS when task switch, 
                                      //but we have to make tr register valid or x86 process
#define  SELECTOR_COUNT          6 // kernel 2 + user 2 + tss 1 + empty

#define  ADDRESS_LIMIT          0xfffff //  always 4k bytes algined, so last 0xfffff means 4G space

#define  SEG_CLASS_DATA         1
#define  SEG_CLASS_SYSTEM       0       // this is for TSS

#define  SEG_BASE_4K            1       // address count with 4k
#define  SEG_BASE_1             0       // address count with 1 byte
#define  TSS_SEG_BASE SEG_BASE_1

#define IDT_SIZE 256

#define KHEAP_BEGIN             0xC0700000 // increase by 4M if kernel img is too large for this reserve
#define KHEAP_END               (KHEAP_BEGIN + 0x004FF000)

#define PAGE_TABLE_CACHE_BEGIN  (KHEAP_END + 0x00001000)
#define PAGE_TABLE_CACHE_END    (PAGE_TABLE_CACHE_BEGIN + 0x0400000)

#define RESERVED_PAGES ((PAGE_TABLE_CACHE_END - KERNEL_OFFSET)/PAGE_SIZE)


#define UNIMPL printk("unimpl function %s\n", __func__)
 
#define DSR_CACHE_DEPTH 100

#define HDD_CACHE_SIZE 2048 /* pages */

#define HDD_CACHE_OPEN 1

#define HDD_CACHE_WRITE_THOUGH 0
#define HDD_CACHE_WRITE_BACK 1

#define HDD_CACHE_WRITE_POLICY HDD_CACHE_WRITE_BACK

#define USER_STACK_PAGES 16

#define SYSCALL_INT_NO 0x80


#define STDIN_FILENO    0   /* Standard input.  */
#define STDOUT_FILENO   1   /* Standard output.  */
#define STDERR_FILENO   2   /* Standard error output.  */

#define PIPE_BUF_LEN (4096*4)

#define USER_HEAP_BEGIN  0x30000000
// left one page (a hole) to protect stack overflow and heap overflow
#define USER_HEAP_END	(KERNEL_OFFSET - (USER_STACK_PAGES-1)*PAGE_SIZE)
//#define USER_HEAP_END	 0x600f0000

#define USER_ZONE_BEGIN 0x10000000
#define USER_ZONE_END	(USER_HEAP_BEGIN - PAGE_SIZE)

// supported resolution
#define VGA_RESOLUTION_X 1024
#define VGA_RESOLUTION_Y 512
#define VGA_COLOR_DEPTH  32


#endif
