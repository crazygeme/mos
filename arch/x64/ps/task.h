#ifndef MOS_X64_ARCH_TASK_H
#define MOS_X64_ARCH_TASK_H
#include <arch/types.h>
struct _task_struct;
struct _arch_intr_frame;
typedef volatile struct {
	uint32_t reserved0;
	uint64_t esp0, rsp1, rsp2, reserved1;
	uint64_t ist[7];
	uint64_t reserved2;
	uint16_t reserved3, iomap;
} __attribute__((packed)) tss_struct;
#define TSS_IO_BITMAP_BYTES 8192
typedef struct {
	tss_struct tss;
	unsigned char io_bitmap[TSS_IO_BITMAP_BYTES + 1];
} __attribute__((packed)) tss_io_struct;
#define TSS_SEG_LIMIT (sizeof(tss_io_struct) - 1)
_Static_assert(sizeof(tss_struct) == 104, "IA-32e TSS size");
typedef volatile struct {
	uint16_t ds, ss, es, gs, fs, cs;
	uintptr_t edi, esi, edx, ecx, ebx, eax, ebp;
	uintptr_t eip, esp0, esp;
	uintptr_t r12, r13, r14, r15;
	uintptr_t fs_base, gs_base;
} task_frame;

/* Stopped register image used by IA-32 and AMD64 ptrace layouts. */
typedef struct _ptrace_saved_frame {
	uintptr_t edi, esi, ebp, ebx, edx, ecx, eax;
	unsigned short gs, fs, es, ds;
	unsigned error_code;
	uintptr_t eip;
	unsigned short cs;
	unsigned eflags;
	uintptr_t esp;
	unsigned short ss;
	uintptr_t r8, r9, r10, r11, r12, r13, r14, r15;
	uintptr_t fs_base, gs_base;
} ptrace_saved_frame;

/* Keep architecture task pointers width-neutral at common call sites. */

void arch_task_init_switch_frame(struct _task_struct *task);
void arch_task_init(struct _task_struct *task);
void arch_task_activate(struct _task_struct *task);
void arch_task_reset_tls(struct _task_struct *task,
			 struct _arch_intr_frame *frame);
void arch_task_init_user_frame(struct _arch_intr_frame *frame, vaddr_t ip,
			       vaddr_t sp);

#endif
