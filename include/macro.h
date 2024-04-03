#ifndef _MACRO_H_
#define _MACRO_H_

#define LOAD_CR3(val) asm volatile("movl %%cr3, %0" : "=r"(val))

#define SET_DS(val)                                 \
	asm volatile("movw %0, %%ax" : : "I"(val)); \
	asm volatile("movw %ax, %ds");              \
	asm volatile("movw %ax, %es");              \
	asm volatile("movw %ax, %fs");              \
	asm volatile("movw %ax, %gs");              \
	asm volatile("movw %ax, %ss");

#define SET_CS(val) asm volatile("ljmp %0, $1f \n1:\n\tnop" : : "I"(val));

#define SET_ESP(val) asm volatile("movl %0, %%esp" : : "m"(val))

#define SET_EBP(val) asm volatile("movl %0, %%ebp" : : "q"(val))

#define SET_EIP(val) asm volatile("jmp %0" : : "q"(val))

#define SET_TSS(val) asm volatile("ltr %w0" : : "q"(val));

#define SET_GDT(val) asm volatile("lgdt %0" : : "m"(val));

#define SET_IDT(val) asm volatile("lidt %0\nsti" : : "m"(val));

#define SET_CR3(val) asm volatile("movl %0, %%cr3" : : "q"(val));

#define ENABLE_PAGING()                       \
	asm volatile("movl %cr0,%eax");       \
	asm volatile("orl $0x80000000,%eax"); \
	asm volatile("movl %eax,%cr0");

#define RELOAD_CR3()                                             \
	do {                                                     \
		unsigned __cr3__;                                \
		asm volatile("movl %%cr3, %0" : "=q"(__cr3__));  \
		asm volatile("movl %0, %%cr3" : : "q"(__cr3__)); \
	} while (0)

#define RELOAD_EIP() \
	asm volatile("jmp 1f \n1:\n\tmovl $1f,%eax\n\tjmp *%eax \n1:\n\tnop");

#define RELOAD_ESP()                                           \
	asm volatile("movl %esp, %ecx");                       \
	asm volatile("addl %0, %%ecx" : : "i"(KERNEL_OFFSET)); \
	asm volatile("movl %ecx, %esp");

#define ROUND_UP(X, STEP) (((X) + (STEP)-1) / (STEP) * (STEP))

// clang-format off
#define MAKE_SEG_DESC(base,  limit,  class,  type, dpl,  granularity)               \
    (unsigned long long)                                                            \
    (((unsigned)(((unsigned int)limit & 0xffff) | ((unsigned int )base << 16))) |   \
    (((unsigned long long)((((unsigned int )base >> 16) & 0xff)                     \
          | ((unsigned int)type << 8)                                               \ 
          | ((unsigned int)class << 12)                                             \     
          | ((unsigned int)dpl << 13)                                               \     
          | (1 << 15)                                                               \
          | ((unsigned int)limit & 0xf0000)                                         \
          | (1 << 22)                                                               \
          | ((unsigned int)granularity << 23)                                       \
          | ((unsigned int )base & 0xff000000))) << 32))


#define MAKE_GATE(function, dpl, type)                                                   \
    (unsigned long long)                                                                 \
    (((unsigned int)(((unsigned int)function & 0xffff) | (KERNEL_CODE_SELECTOR << 16))) |\
    (((unsigned long long)(((unsigned int)function & 0xffff0000) | (1 << 15)             \     
          | ((unsigned int)dpl << 13)                                                    \     
          | (0 << 12)                                                                    \ 
          | ((unsigned int)type << 8)))                                                  \
             << 32))



#define MAKE_GDTR_OPERAND(limit, base)                                          \
     (unsigned long long)                                                       \
     ((unsigned short)limit | ((unsigned long long)(unsigned int)base << 16))


#define MAKE_INTR_GATE(function, dpl) MAKE_GATE(function, dpl, 14)


#define MAKE_TRAP_GATE(function, dpl) MAKE_GATE(function, dpl, 15)


#define  MAKE_IDTR_OPERAND(limit, base)                                         \
    (unsigned long long)                                                        \
     ((unsigned short)limit | ((unsigned long long)(unsigned int)base << 16))

#endif