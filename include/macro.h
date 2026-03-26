#ifndef _MACRO_H
#define _MACRO_H

#define ROUND_UP(x) (((x) + sizeof(long) - 1) & ~(sizeof(long) - 1))

#define LOAD_CR2(val) asm volatile("movl %%cr2, %0" : "=r"(val))

#define LOAD_CR3(val) asm volatile("movl %%cr3, %0" : "=r"(val))

#define SET_DS(val)                                         \
	({                                                  \
		asm volatile("movw %0, %%ax" : : "I"(val)); \
		asm volatile("movw %ax, %ds");              \
		asm volatile("movw %ax, %es");              \
		asm volatile("movw %ax, %fs");              \
		asm volatile("movw %ax, %gs");              \
		asm volatile("movw %ax, %ss");              \
	})

#define SET_CS(val) asm volatile("ljmp %0, $1f \n1:\n\tnop" : : "I"(val))

#define SET_ESP(val) asm volatile("movl %0, %%esp" : : "m"(val))

#define SET_EBP(val) asm volatile("movl %0, %%ebp" : : "q"(val))

#define SET_EIP(val) asm volatile("jmp %0" : : "q"(val))

#define SET_TSS(val) asm volatile("ltr %w0" : : "q"(val))

#define SET_GDT(val) asm volatile("lgdt %0" : : "m"(val))

#define SET_IDT(val) asm volatile("lidt %0\nsti" : : "m"(val))

#define SET_CR3(val) asm volatile("movl %0, %%cr3" : : "q"(val))

#define ENABLE_PAGING()                               \
	({                                            \
		asm volatile("movl %cr0,%eax");       \
		asm volatile("orl $0x80000000,%eax"); \
		asm volatile("movl %eax,%cr0");       \
	})

#define RELOAD_CR3()                                             \
	({                                                       \
		unsigned __cr3__;                                \
		asm volatile("movl %%cr3, %0" : "=q"(__cr3__));  \
		asm volatile("movl %0, %%cr3" : : "q"(__cr3__)); \
	})

#define INVLPG(addr) asm volatile("invlpg (%0)" : : "r"(addr) : "memory")

#define RELOAD_EIP()                           \
	({                                     \
		asm volatile("jmp 1f");        \
		asm volatile("1:");            \
		asm volatile("movl $1f,%eax"); \
		asm volatile("jmp *%eax");     \
		asm volatile("1:");            \
		asm volatile("nop");           \
	})

#define RELOAD_ESP()                                                   \
	({                                                             \
		asm volatile("movl %esp, %ecx");                       \
		asm volatile("addl %0, %%ecx" : : "i"(KERNEL_OFFSET)); \
		asm volatile("movl %ecx, %esp");                       \
	})

#define GET_INTR_FLAG(flag)                           \
	({                                            \
		asm volatile("pushfl");               \
		asm volatile("popl %0" : "=q"(flag)); \
	})

#define ENABLE_INTR() asm volatile("sti")

#define DISABLE_INTR() asm volatile("cli")

#define HLT() asm volatile("hlt")

#define PAUSE() asm volatile("pause")

#define NOP() asm volatile("nop")

/* Per-CPU TSS selector: CPU 0 → TSS_SELECTOR, CPU n → TSS_SELECTOR + n*8 */
#define TSS_SELECTOR_FOR(n) (TSS_SELECTOR + (n) * 8)

#define DIE()                                      \
	({                                         \
		printf("\nDIE at %s\n", __func__); \
		for (;;) {                         \
			HLT();                     \
		}                                  \
	})

/* In .multiboot section we can not call port_write_xxx functions */
#define OUT_PORT(port, data)                         \
	({                                           \
		asm volatile("mov $" #port ", %dx"); \
		asm volatile("mov $" #data ", %al"); \
		asm volatile("outb %al, %dx");       \
	})

/* Optimization barrier.

   The compiler will not reorder operations across an
   optimization barrier.  See "Optimization Barriers" in the
   reference guide for more information.*/
#define BARRIER() asm volatile("" : : : "memory")

#define SAVE_ALL(task, label)                                               \
	({                                                                  \
		asm volatile("movl $" #label ", %0" : "=m"(task->tss.eip)); \
		asm volatile("movl %%ebp, %0" : "=m"(task->tss.ebp));       \
		asm volatile("movl %%eax, %0" : "=m"(task->tss.eax));       \
		asm volatile("movl %%ebx, %0" : "=m"(task->tss.ebx));       \
		asm volatile("movl %%ecx, %0" : "=m"(task->tss.ecx));       \
		asm volatile("movl %%edx, %0" : "=m"(task->tss.edx));       \
		asm volatile("movl %%edi, %0" : "=m"(task->tss.edi));       \
		asm volatile("movl %%esi, %0" : "=m"(task->tss.esi));       \
		asm volatile("movl %%esp, %0" : "=m"(task->tss.esp));       \
		asm volatile("mov %%fs, %0" : "=m"(task->tss.fs));          \
		asm volatile("mov %%gs, %0" : "=m"(task->tss.gs));          \
		asm volatile("mov %%es, %0" : "=m"(task->tss.es));          \
		asm volatile("mov %%ss, %0" : "=m"(task->tss.ss));          \
		asm volatile("mov %%ds, %0" : "=m"(task->tss.ds));          \
	})

#define RESTORE_ALL(task, next)                                        \
	({                                                             \
		asm volatile("mov %0, %%ds" : : "m"(task->tss.ds));    \
		asm volatile("mov %0, %%ss" : : "m"(task->tss.ss));    \
		asm volatile("mov %0, %%es" : : "m"(task->tss.es));    \
		asm volatile("mov %0, %%gs" : : "m"(task->tss.gs));    \
		asm volatile("mov %0, %%fs" : : "m"(task->tss.fs));    \
		asm volatile("movl %0, %%edi" : : "m"(task->tss.edi)); \
		asm volatile("movl %0, %%esi" : : "m"(task->tss.esi)); \
		asm volatile("movl %0, %%edx" : : "m"(task->tss.edx)); \
		asm volatile("movl %0, %%ecx" : : "m"(task->tss.ecx)); \
		asm volatile("movl %0, %%ebx" : : "m"(task->tss.ebx)); \
		asm volatile("movl %0, %%eax" : : "m"(task->tss.eax)); \
		next = task->tss.eip;                                  \
		asm volatile("movl %0, %%esp" : : "m"(task->tss.esp)); \
		asm volatile("movl %0, %%ebp" : : "m"(task->tss.ebp)); \
	})

#define JUMP_TO_NEXT_TASK_EIP(next)                           \
	({                                                    \
		asm volatile("movl %0, %%edx" : : "m"(next)); \
		asm volatile("jmp *%edx");                    \
	})

// clang-format off
#define MAKE_SEG_DESC(base, limit, class, type, dpl, granularity)                    \
    (unsigned long long)                                                             \
    (((unsigned)(((unsigned int)limit & 0xffff) | ((unsigned int)base << 16))) |    \
    (((unsigned long long)((((unsigned int)base >> 16) & 0xff)                      \
          | ((unsigned int)type << 8)                                               \
          | ((unsigned int)class << 12)                                             \
          | ((unsigned int)dpl << 13)                                               \
          | (1 << 15)                                                               \
          | ((unsigned int)limit & 0xf0000)                                         \
          | (1 << 22)                                                               \
          | ((unsigned int)granularity << 23)                                       \
          | ((unsigned int)base & 0xff000000))) << 32))


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
     (((unsigned short)(limit)) | ((unsigned long long)(unsigned int)(base) << 16))


#define MAKE_INTR_GATE(function, dpl) MAKE_GATE(function, dpl, 14)


#define MAKE_TRAP_GATE(function, dpl) MAKE_GATE(function, dpl, 15)


#define  MAKE_IDTR_OPERAND(limit, base)                                         \
    (unsigned long long)                                                        \
     (((unsigned short)(limit)) | ((unsigned long long)(unsigned int)(base) << 16))

#define NAKED __attribute__((naked))
/*
 * Kernel init call mechanism.
 *
 * KERNEL_INIT(index, fn) registers a void (*)(void) function to be called
 * during kernel startup.  The function pointer is placed in the ELF section
 * ".kinit.<index>"; the linker script collects all such sections (sorted by
 * name) between __kinit_start and __kinit_end, and run_kinit() iterates over
 * them in order.
 *
 * Usage:
 *   static void my_init(void) { ... }
 *   KERNEL_INIT(3, my_init);
 */
typedef void (*kinit_fn_t)(void);

#define KERNEL_INIT(index, fn)                                          \
	static kinit_fn_t __kinit_##index##_##fn                        \
		__attribute__((used, section(".kinit." #index))) = (fn)

#endif