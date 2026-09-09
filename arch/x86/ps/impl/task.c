#include <ps/task.h>
#include <config.h>
#include <int/int.h>
#include <lib/klib.h>
#include <macro.h>
#include <ps/ps.h>
#include <ps/smp.h>

void arch_task_init(task_struct *task)
{
	task->tss.fs = CPU_LOCAL_SELECTOR;
	task->tss.gs = task->tss.ds = task->tss.es = task->tss.ss =
		KERNEL_DATA_SELECTOR;
	task->tss.cs = KERNEL_CODE_SELECTOR;
}

static void load_ldt(task_struct *task, unsigned long long *gdt)
{
	if (!task || !task->user || !task->user->ldt_present) {
		SET_LDT(0);
		return;
	}

	unsigned limit = LDT_ENTRY_COUNT * sizeof(unsigned long long) - 1;
	gdt[LDT_SELECTOR / 8] =
		MAKE_SEG_DESC((unsigned)task->user->ldt_desc, limit,
			      SEG_CLASS_SYSTEM, 2, KERNEL_PRIVILEGE, SEG_BASE_1);
	SET_LDT(LDT_SELECTOR);
}

void ps_update_ldt(task_struct *task)
{
	load_ldt(task, smp_gdt());
}

void ps_load_task_segments(task_struct *task)
{
	unsigned long long *gdt = smp_gdt();

	if (!task || !task->user)
		return;
	gdt[GDT_ENTRY_TLS_MIN + 0] = task->user->tls_desc[0];
	gdt[GDT_ENTRY_TLS_MIN + 1] = task->user->tls_desc[1];
	gdt[GDT_ENTRY_TLS_MIN + 2] = task->user->tls_desc[2];
	load_ldt(task, gdt);
}

void reset_tss(task_struct *task)
{
	tss_struct *tss = smp_tss();
	tss_io_struct *io_tss = (tss_io_struct *)tss;

	tss->cr3 = task->address_space;
	tss->esp0 = task->tss.esp0;
	if (task->io_allow_all) {
		tss->iomap = (unsigned short)offsetof(tss_io_struct, io_bitmap);
		memset(io_tss->io_bitmap, 0x00, TSS_IO_BITMAP_BYTES);
	} else if (task->io_bitmap) {
		tss->iomap = (unsigned short)offsetof(tss_io_struct, io_bitmap);
		memcpy(io_tss->io_bitmap, task->io_bitmap, TSS_IO_BITMAP_BYTES);
	} else {
		/* No bitmap means all port I/O is denied; omit the 8 KiB copy. */
		tss->iomap = sizeof(tss_io_struct);
	}
	io_tss->io_bitmap[TSS_IO_BITMAP_BYTES] = 0xff;
	tss->ss0 = KERNEL_DATA_SELECTOR;
	tss->ss = tss->gs = tss->fs = tss->ds = tss->es =
		KERNEL_DATA_SELECTOR | 0x3;
	tss->cs = KERNEL_CODE_SELECTOR | 0x3;
	int_update_tss((void *)tss);
}

void arch_task_activate(task_struct *task)
{
	reset_tss(task);
	ps_load_task_segments(task);
}

void arch_task_reset_tls(task_struct *task, intr_frame *frame)
{
	memset(task->user->tls_desc, 0, sizeof(task->user->tls_desc));
	memset(task->user->ldt_desc, 0, sizeof(task->user->ldt_desc));
	task->user->ldt_present = 0;
	frame->gs = 0;
	task->tss.gs = 0;
	SET_GS(0);
	load_ldt(task, smp_gdt());
}

void arch_task_init_user_frame(intr_frame *frame, vaddr_t ip, vaddr_t sp)
{
	memset(frame, 0, sizeof(*frame));
	frame->eip = (void *)ip;
	frame->esp = (void *)sp;
	frame->cs = USER_CODE_SELECTOR;
	frame->ss = frame->ds = frame->es = frame->fs = frame->gs =
		USER_DATA_SELECTOR;
	frame->eflags = 0x202;
}
