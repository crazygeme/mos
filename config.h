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

#define IDT_SIZE 256

#define KHEAP_END	 0xC07FF000
#define KHEAP_BEGIN	 0xC0300000

#define UNIMPL printk("unimpl function %s\n", __func__)
 

#endif
