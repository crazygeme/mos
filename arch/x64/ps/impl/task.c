#include <ps/task.h>
#include <ps/ps.h>
#include <ps/smp.h>
#include <ps/cpu_local.h>
#include <int/int.h>
#include <lib/klib.h>
#include <macro.h>

static void set_base(unsigned msr, uintptr_t base)
{
	arch_cpu_write_msr(msr, (uint32_t)base, (uint32_t)(base >> 32));
}
void arch_task_copy_user_context(task_struct *child, const task_struct *parent)
{
	child->tss.cs = parent->tss.cs;
}

void arch_task_init(task_struct *task)
{
	task->tss.ds = task->tss.es = task->tss.ss = KERNEL_DATA_SELECTOR;
	task->tss.cs = KERNEL_CODE_SELECTOR;
}
void ps_update_ldt(task_struct *task)
{
	uint64_t *gdt = (uint64_t *)smp_gdt();
	if (!task || !task->user || !task->user->ldt_present) {
		SET_LDT(0);
		return;
	}
	uintptr_t base = (uintptr_t)task->user->ldt_desc;
	unsigned limit = sizeof(task->user->ldt_desc) - 1;
	gdt[LDT_SELECTOR / 8] = (limit & 0xffff) | ((base & 0xffffff) << 16) |
				(0x82ULL << 40) |
				((base & 0xff000000ULL) << 32);
	gdt[LDT_SELECTOR / 8 + 1] = base >> 32;
	SET_LDT(LDT_SELECTOR);
}
void ps_load_task_segments(task_struct *task)
{
	if (!task || !task->user)
		return;
	uint64_t *gdt = (uint64_t *)smp_gdt();
	for (unsigned i = 0; i < GDT_ENTRY_TLS_COUNT; i++)
		gdt[GDT_ENTRY_TLS_MIN + i] = task->user->tls_desc[i];
	ps_update_ldt(task);
	if (task->tss.cs == USER64_CODE_SELECTOR) {
		set_base(0xc0000100, task->tss.fs_base);
		set_base(0xc0000102, task->tss.gs_base);
	}
}
void reset_tss(task_struct *task)
{
	tss_io_struct *io = (tss_io_struct *)smp_tss();
	io->tss.esp0 = task->tss.esp0;
	arch_cpu_local()->syscall_sp = task->tss.esp0;
	if (task->io_allow_all) {
		memset(io->io_bitmap, 0, TSS_IO_BITMAP_BYTES);
		io->tss.iomap = offsetof(tss_io_struct, io_bitmap);
	} else if (task->io_bitmap) {
		memcpy(io->io_bitmap, task->io_bitmap, TSS_IO_BITMAP_BYTES);
		io->tss.iomap = offsetof(tss_io_struct, io_bitmap);
	} else
		io->tss.iomap = sizeof(*io);
	io->io_bitmap[TSS_IO_BITMAP_BYTES] = 0xff;
	int_update_tss((void *)&io->tss);
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
	task->tss.fs_base = task->tss.gs_base = 0;
	task->tss.fs = task->tss.gs = 0;
	frame->fs = frame->gs = 0;
	asm volatile("mov %0, %%fs" : : "r"((uint16_t)0) : "memory");
	set_base(0xc0000100, 0);
	set_base(0xc0000102, 0);
	ps_update_ldt(task);
}
void arch_task_init_user_frame(intr_frame *frame, vaddr_t ip, vaddr_t sp)
{
	memset(frame, 0, sizeof(*frame));
	frame->eip = (void *)ip;
	frame->esp = (void *)sp;
	frame->cs = current->tss.cs;
	frame->ss = frame->ds = frame->es = USER_DATA_SELECTOR;
	frame->fs = frame->gs =
		frame->cs == USER64_CODE_SELECTOR ? 0 : USER_DATA_SELECTOR;
	frame->eflags = 0x202;
}
