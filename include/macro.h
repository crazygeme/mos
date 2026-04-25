#ifndef _MACRO_H
#define _MACRO_H

#define ROUND_UP(x) (((x) + sizeof(long) - 1) & ~(sizeof(long) - 1))

#define LOAD_CR2(val) asm volatile("movl %%cr2, %0" : "=r"(val))

#define LOAD_CR3(val) asm volatile("movl %%cr3, %0" : "=r"(val))

#define LOAD_ESP(val) asm volatile("movl %%esp, %0" : "=r"(val) : : "memory")

#define SET_DS(val)                                           \
	({                                                    \
		unsigned short __seg = (unsigned short)(val); \
		asm volatile("movw %0, %%ds\n\t"              \
			     "movw %0, %%es\n\t"              \
			     "movw %0, %%fs\n\t"              \
			     "movw %0, %%gs\n\t"              \
			     "movw %0, %%ss"                  \
			     :                                \
			     : "rm"(__seg)                    \
			     : "memory");                     \
	})

#define SET_CS(val) \
	asm volatile("ljmp %0, $1f \n1:\n\tnop" : : "I"(val) : "memory")

#define SET_ESP(val) asm volatile("movl %0, %%esp" : : "rm"(val) : "memory")

#define SET_EBP(val) asm volatile("movl %0, %%ebp" : : "rm"(val) : "memory")

#define SET_EIP(val) asm volatile("jmp *%0" : : "rm"(val) : "memory")

#define SET_TSS(val) asm volatile("ltr %w0" : : "r"(val) : "memory")

#define SET_LDT(val) asm volatile("lldt %w0" : : "r"(val) : "memory")

#define SET_GDT(val) asm volatile("lgdt %0" : : "m"(val) : "memory")

#define SET_IDT(val) asm volatile("lidt %0\nsti" : : "m"(val) : "memory")

#define SET_CR3(val) asm volatile("movl %0, %%cr3" : : "r"(val) : "memory")

#define ENABLE_PAGING()                                   \
	({                                                \
		asm volatile("movl %%cr0, %%eax\n\t"      \
			     "orl $0x80000000, %%eax\n\t" \
			     "movl %%eax, %%cr0"          \
			     :                            \
			     :                            \
			     : "eax", "cc", "memory");    \
	})

#define RELOAD_CR3()                                 \
	({                                           \
		asm volatile("movl %%cr3, %%eax\n\t" \
			     "movl %%eax, %%cr3"     \
			     :                       \
			     :                       \
			     : "eax", "memory");     \
	})

#define INVLPG(addr) asm volatile("invlpg (%0)" : : "r"(addr) : "memory")

#define RELOAD_EIP()                               \
	({                                         \
		asm volatile("jmp 1f\n\t"          \
			     "1:\n\t"              \
			     "movl $1f, %%eax\n\t" \
			     "jmp *%%eax\n\t"      \
			     "1:\n\t"              \
			     "nop"                 \
			     :                     \
			     :                     \
			     : "eax", "memory");   \
	})

#define RELOAD_ESP()                                   \
	({                                             \
		asm volatile("movl %%esp, %%ecx\n\t"   \
			     "addl %0, %%ecx\n\t"      \
			     "movl %%ecx, %%esp"       \
			     :                         \
			     : "ir"(KERNEL_OFFSET)     \
			     : "ecx", "cc", "memory"); \
	})

#define GET_INTR_FLAG(flag) \
	({ asm volatile("pushfl\n\tpopl %0" : "=r"(flag) : : "memory"); })

#define ENABLE_INTR() asm volatile("sti" : : : "memory")

#define DISABLE_INTR() asm volatile("cli" : : : "memory")

#define HLT() asm volatile("hlt" : : : "memory")

#define PAUSE() asm volatile("pause")

#define NOP() asm volatile("nop")

/* Per-CPU TSS selector: CPU 0 → TSS_SELECTOR, CPU n → TSS_SELECTOR + n*8 */
#define TSS_SELECTOR_FOR(n) (TSS_SELECTOR + (n) * 8)

#define DIE()                                      \
	({                                         \
		printf("\nDIE at %s\n", __func__); \
		OUT_PORT(0xf4, 0x7f);              \
		for (;;) {                         \
			HLT();                     \
		}                                  \
	})

/* In .multiboot section we can not call port_write_xxx functions */
#define OUT_PORT(port, data)                                \
	({                                                  \
		asm volatile("outb %b0, %w1"                \
			     :                              \
			     : "a"((unsigned char)(data)),  \
			       "Nd"((unsigned short)(port)) \
			     : "memory");                   \
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

#define JUMP_TO_NEXT_TASK_EIP(next) \
	({ asm volatile("jmp *%0" : : "r"(next) : "memory"); })

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

#define LIKELY(x)      __builtin_expect(!!(x), 1)
#define UNLIKELY(x)    __builtin_expect(!!(x), 0)
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
