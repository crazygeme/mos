#ifndef MOS_X86_ARCH_TASK_H
#define MOS_X86_ARCH_TASK_H

#include <arch/types.h>

struct _task_struct;
struct _intr_frame;

/* Hardware task-state layout consumed by the x86 CPU and GDT. */
typedef volatile struct __tss_struct {
	unsigned short link, link_h;
	unsigned long esp0;
	unsigned short ss0, ss0_h;
	unsigned long esp1;
	unsigned short ss1, ss1_h;
	unsigned long esp2;
	unsigned short ss2, ss2_h;
	unsigned long cr3, eip, eflags;
	unsigned long eax, ecx, edx, ebx;
	unsigned long esp, ebp, esi, edi;
	unsigned short es, es_h;
	unsigned short cs, cs_h;
	unsigned short ss, ss_h;
	unsigned short ds, ds_h;
	unsigned short fs, fs_h;
	unsigned short gs, gs_h;
	unsigned short ldt, ldt_h;
	unsigned short trap, iomap;
} tss_struct;

#define TSS_IO_BITMAP_BYTES 8192

typedef struct _tss_io_struct {
	tss_struct tss;
	unsigned char io_bitmap[TSS_IO_BITMAP_BYTES + 1];
} tss_io_struct;

#define TSS_SEG_LIMIT ((unsigned)(sizeof(tss_io_struct) - 1))

/* Stack layout consumed by arch/x86/ps/sched/switch.S. */
typedef volatile struct _task_frame {
	unsigned short ds, ss, es, gs, fs, cs;
	unsigned long edi, esi, edx, ecx, ebx, eax, ebp;
	unsigned long eip;
	unsigned long esp0;
	unsigned long esp;
} task_frame;

/* Register image exported by the x86 ptrace implementation. */
typedef struct _ptrace_saved_frame {
	uintptr_t edi, esi, ebp, ebx, edx, ecx, eax;
	unsigned short gs, fs, es, ds;
	unsigned error_code;
	uintptr_t eip;
	unsigned short cs;
	unsigned eflags;
	uintptr_t esp;
	unsigned short ss;
} ptrace_saved_frame;

/* Keep architecture task pointers width-neutral at common call sites. */

void arch_task_init(struct _task_struct *task);
void arch_task_activate(struct _task_struct *task);
void arch_task_reset_tls(struct _task_struct *task,
			 struct _intr_frame *frame);
void arch_task_init_user_frame(struct _intr_frame *frame, vaddr_t ip,
			       vaddr_t sp);

#endif
