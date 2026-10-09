#include <ps/task.h>
#include <mm/mmu.h>
#include <mm/mmap.h>
#include <config.h>
#include <int/int.h>
#include <lib/klib.h>
#include <macro.h>
#include <ps/ps.h>
#include <ps/smp.h>
#include <ps/cpu_local.h>

void arch_task_copy_user_context(task_struct *child, const task_struct *parent)
{
	child->execution->arch.cs = parent->execution->arch.cs;
}

void arch_task_init(task_struct *task)
{
	task->execution->arch.cs = KERNEL_CODE_SELECTOR;
	task->execution->arch.gs = 0;
}

static void load_ldt(struct smp_cpu *cpu, task_struct *task)
{
	uintptr_t base = task && task->execution && task->memory->ldt_present ?
				 (uintptr_t)task->memory->ldt_desc :
				 0;
	if (cpu->loaded_ldt_valid && cpu->loaded_ldt_base == base)
		return;
	if (base) {
		unsigned limit =
			LDT_ENTRY_COUNT * sizeof(unsigned long long) - 1;
		cpu->gdt[LDT_SELECTOR / 8] =
			MAKE_SEG_DESC(base, limit, SEG_CLASS_SYSTEM, 2,
				      KERNEL_PRIVILEGE, SEG_BASE_1);
		SET_LDT(LDT_SELECTOR);
	} else
		SET_LDT(0);
	cpu->loaded_ldt_base = base;
	cpu->loaded_ldt_valid = 1;
}

void ps_update_ldt(task_struct *task)
{
	unsigned irq = int_intr_disable();
	load_ldt(arch_cpu_local(), task);
	int_intr_setlevel(irq);
}

void ps_load_task_segments(task_struct *task)
{
	if (!task || !task->execution)
		return;
	unsigned irq = int_intr_disable();
	struct smp_cpu *cpu = arch_cpu_local();
	for (unsigned i = 0; i < GDT_ENTRY_TLS_COUNT; i++)
		if (cpu->gdt[GDT_ENTRY_TLS_MIN + i] !=
		    task->execution->tls_desc[i])
			cpu->gdt[GDT_ENTRY_TLS_MIN + i] =
				task->execution->tls_desc[i];
	load_ldt(cpu, task);
	int_intr_setlevel(irq);
}

void arch_task_save_user_segments(task_struct *task)
{
	/* i386 selectors are saved by interrupt/context-switch assembly. */
	(void)task;
}

void reset_tss(task_struct *task)
{
	tss_struct *tss = smp_tss();
	tss_io_struct *io_tss = (tss_io_struct *)tss;

	tss->cr3 = VIRT_TO_PHY(task->memory->page_dir);
	tss->esp0 = task_stack_top(task);
	if (arch_cpu_local()->sysenter_enabled)
		arch_cpu_write_msr(0x175, task_stack_top(task), 0);
	if (task->execution->io_allow_all) {
		tss->iomap = (unsigned short)offsetof(tss_io_struct, io_bitmap);
		memset(io_tss->io_bitmap, 0x00, TSS_IO_BITMAP_BYTES);
	} else if (task->execution->io_bitmap) {
		tss->iomap = (unsigned short)offsetof(tss_io_struct, io_bitmap);
		memcpy(io_tss->io_bitmap, task->execution->io_bitmap,
		       TSS_IO_BITMAP_BYTES);
	} else {
		/* No bitmap means all port I/O is denied; omit the 8 KiB copy. */
		tss->iomap = sizeof(tss_io_struct);
	}
	io_tss->io_bitmap[TSS_IO_BITMAP_BYTES] = 0xff;
	tss->ss0 = KERNEL_DATA_SELECTOR;
	tss->ss = tss->gs = tss->fs = tss->ds = tss->es = KERNEL_DATA_SELECTOR |
							  0x3;
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
	unsigned irq = int_intr_disable();
	memset(task->execution->tls_desc, 0, sizeof(task->execution->tls_desc));
	memset(task->memory->ldt_desc, 0, sizeof(task->memory->ldt_desc));
	task->memory->ldt_present = 0;
	frame->gs = 0;
	task->execution->arch.gs = 0;
	SET_GS(0);
	ps_load_task_segments(task);
	int_intr_setlevel(irq);
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
