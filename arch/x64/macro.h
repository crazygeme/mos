#ifndef MOS_X64_MACRO_H
#define MOS_X64_MACRO_H

#define ROUND_UP(x) (((x) + sizeof(long) - 1) & ~(sizeof(long) - 1))

#define LOAD_CR2(val) asm volatile("movq %%cr2, %0" : "=r"(val))

#define LOAD_CR3(val) asm volatile("movq %%cr3, %0" : "=r"(val))

#define LOAD_ESP(val) asm volatile("movq %%rsp, %0" : "=r"(val) : : "memory")

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

#define SET_GS(val) \
	asm volatile("movw %0, %%gs" : : "rm"((unsigned short)val) : "memory")

#define SET_ESP(val) asm volatile("movq %0, %%rsp" : : "rm"(val) : "memory")

#define SET_EBP(val) asm volatile("movq %0, %%rbp" : : "rm"(val) : "memory")

#define SET_EIP(val) asm volatile("jmp *%0" : : "rm"(val) : "memory")

#define SET_TSS(val) asm volatile("ltr %w0" : : "r"(val) : "memory")

#define SET_LDT(val) asm volatile("lldt %w0" : : "r"(val) : "memory")

#define SET_GDT(val) asm volatile("lgdt %0" : : "m"(val) : "memory")

#define SET_IDT(val) asm volatile("lidt %0\nsti" : : "m"(val) : "memory")

#define SET_CR3(val) asm volatile("movq %0, %%cr3" : : "r"(val) : "memory")

#ifndef __ASSEMBLER__
void smp_tlb_flush(void);
#endif
#define LOCAL_RELOAD_CR3()                                  \
	do {                                                \
		unsigned long __cr3;                        \
		asm volatile("mov %%cr3, %0; mov %0, %%cr3" \
			     : "=&r"(__cr3)                 \
			     :                              \
			     : "memory");                   \
	} while (0)
#define RELOAD_CR3() LOCAL_RELOAD_CR3()
#define INVLPG(addr) asm volatile("invlpg (%0)" : : "r"(addr) : "memory")

#define GET_INTR_FLAG(flag)                                                   \
	({                                                                    \
		unsigned long __flags;                                        \
		asm volatile("pushfq; popq %0" : "=r"(__flags) : : "memory"); \
		(flag) = __flags;                                             \
	})

#define ENABLE_INTR() asm volatile("sti" : : : "memory")

#define DISABLE_INTR() asm volatile("cli" : : : "memory")

#define HLT() asm volatile("hlt" : : : "memory")

#define PAUSE() asm volatile("pause")

#define NOP() asm volatile("nop")

#ifndef __ASSEMBLER__
/* Flush every TLB entry, including global translations.  A CR3 reload alone
 * deliberately preserves global entries once CR4.PGE is enabled. */
static inline void arch_cpu_reload_tlb(void)
{
	unsigned long cr4;

	asm volatile("movq %%cr4, %0" : "=r"(cr4));
	if (!(cr4 & (1U << 7))) {
		LOCAL_RELOAD_CR3();
		return;
	}
	asm volatile("movq %0, %%cr4\n\t"
		     "movq %1, %%cr4"
		     :
		     : "r"(cr4 & ~(1U << 7)), "r"(cr4)
		     : "memory");
}
static inline void arch_cpu_idle_wait(void)
{
	asm volatile("sti; hlt; cli" : : : "memory");
}
static inline void arch_cpu_cpuid(unsigned leaf, unsigned subleaf, unsigned *a,
				  unsigned *b, unsigned *c, unsigned *d)
{
	asm volatile("cpuid"
		     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
		     : "a"(leaf), "c"(subleaf)
		     : "memory");
}
static inline void arch_cpu_read_msr(unsigned msr, unsigned *low,
				     unsigned *high)
{
	asm volatile("rdmsr" : "=a"(*low), "=d"(*high) : "c"(msr));
}
static inline void arch_cpu_write_msr(unsigned msr, unsigned low, unsigned high)
{
	asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr));
}
static inline void arch_cpu_fpu_init(void)
{
	unsigned long cr0, cr4;
	asm volatile("mov %%cr0, %0" : "=r"(cr0));
	cr0 = (cr0 & ~12U) | 0x10022U;
	asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
	asm volatile("mov %%cr4, %0" : "=r"(cr4));
	/* Keep CR4.PGE intact while enabling OSFXSR and OSXMMEXCPT. */
	cr4 |= 3U << 9;
	asm volatile("mov %0, %%cr4; fninit" : : "r"(cr4) : "memory");
}
static inline void arch_cpu_fpu_save(void *state)
{
	asm volatile("fxsave64 (%0)" : : "r"(state) : "memory");
}
static inline void arch_cpu_fpu_restore(const void *state)
{
	asm volatile("fxrstor64 (%0)" : : "r"(state) : "memory");
}
static inline void arch_cpu_load_idt(const void *operand)
{
	asm volatile("lidt (%0)" : : "r"(operand) : "memory");
}
#endif

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
