#include <ps/task.h>
#include <mm/mmu.h>
#include <mm/mmap.h>
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
static uintptr_t read_base(unsigned msr)
{
	unsigned low, high;
	arch_cpu_read_msr(msr, &low, &high);
	return low | ((uintptr_t)high << 32);
}
void arch_task_copy_user_context(task_struct *child, const task_struct *parent)
{
	unsigned irq = int_intr_disable();
	child->execution->arch.cs = parent->execution->arch.cs;
	if (parent == current &&
	    parent->execution->arch.cs == USER64_CODE_SELECTOR) {
		/* User selector loads can change bases without arch_prctl. */
		child->execution->arch.fs_base = read_base(0xc0000100);
		child->execution->arch.gs_base = read_base(0xc0000102);
	}
	int_intr_setlevel(irq);
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
		unsigned limit = sizeof(task->memory->ldt_desc) - 1;
		cpu->gdt[LDT_SELECTOR / 8] =
			(limit & 0xffff) | ((base & 0xffffff) << 16) |
			(0x82ULL << 40) | ((base & 0xff000000ULL) << 32);
		cpu->gdt[LDT_SELECTOR / 8 + 1] = base >> 32;
		SET_LDT(LDT_SELECTOR);
	} else
		SET_LDT(0);
	/* LLDT caches the table base/limit, not individual entries. */
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
	if (task->execution->arch.cs == USER64_CODE_SELECTOR) {
		int valid = cpu->native_bases_valid &&
			    cpu->native_base_owner == task;
		if (!valid ||
		    cpu->loaded_fs_base != task->execution->arch.fs_base)
			set_base(0xc0000100, task->execution->arch.fs_base);
		if (!valid ||
		    cpu->loaded_gs_base != task->execution->arch.gs_base)
			set_base(0xc0000102, task->execution->arch.gs_base);
		cpu->loaded_fs_base = task->execution->arch.fs_base;
		cpu->loaded_gs_base = task->execution->arch.gs_base;
		cpu->native_base_owner = task;
		cpu->native_bases_valid = 1;
	} else {
		/* Compatibility returns load selectors and change FS/GS bases. */
		cpu->native_bases_valid = 0;
	}
	int_intr_setlevel(irq);
}

void arch_task_save_user_segments(task_struct *task)
{
	if (!task || !task->execution ||
	    task->execution->arch.cs != USER64_CODE_SELECTOR)
		return;
	unsigned irq = int_intr_disable();
	struct smp_cpu *cpu = arch_cpu_local();
	task->execution->arch.fs_base = read_base(0xc0000100);
	task->execution->arch.gs_base = read_base(0xc0000102);
	cpu->loaded_fs_base = task->execution->arch.fs_base;
	cpu->loaded_gs_base = task->execution->arch.gs_base;
	cpu->native_base_owner = task;
	cpu->native_bases_valid = 1;
	int_intr_setlevel(irq);
}

void arch_task_set_user_bases(task_struct *task)
{
	/* An explicit request must win even after a user selector load changed
	 * a live base without changing the last kernel-managed cache value. */
	unsigned irq = int_intr_disable();
	arch_cpu_local()->native_bases_valid = 0;
	ps_load_task_segments(task);
	int_intr_setlevel(irq);
}

void reset_tss(task_struct *task)
{
	tss_io_struct *io = (tss_io_struct *)smp_tss();
	io->tss.esp0 = task_stack_top(task);
	arch_cpu_local()->syscall_sp = task_stack_top(task);
	if (task->execution->io_allow_all) {
		memset(io->io_bitmap, 0, TSS_IO_BITMAP_BYTES);
		io->tss.iomap = offsetof(tss_io_struct, io_bitmap);
	} else if (task->execution->io_bitmap) {
		memcpy(io->io_bitmap, task->execution->io_bitmap,
		       TSS_IO_BITMAP_BYTES);
		io->tss.iomap = offsetof(tss_io_struct, io_bitmap);
	} else
		io->tss.iomap = sizeof(*io);
	io->io_bitmap[TSS_IO_BITMAP_BYTES] = 0xff;
	int_update_tss((void *)&io->tss);
}
void arch_task_activate(task_struct *task)
{
	/* A task/ABI boundary requires a fresh native base installation. */
	arch_cpu_local()->native_bases_valid = 0;
	reset_tss(task);
	ps_load_task_segments(task);
}
void arch_task_reset_tls(task_struct *task, intr_frame *frame)
{
	unsigned irq = int_intr_disable();
	arch_cpu_local()->native_bases_valid = 0;
	memset(task->execution->tls_desc, 0, sizeof(task->execution->tls_desc));
	memset(task->memory->ldt_desc, 0, sizeof(task->memory->ldt_desc));
	task->memory->ldt_present = 0;
	task->execution->arch.fs_base = task->execution->arch.gs_base = 0;
	task->execution->arch.gs = 0;
	frame->fs = frame->gs = 0;
	asm volatile("mov %0, %%fs" : : "r"((uint16_t)0) : "memory");
	set_base(0xc0000100, 0);
	set_base(0xc0000102, 0);
	struct smp_cpu *cpu = arch_cpu_local();
	cpu->loaded_fs_base = cpu->loaded_gs_base = 0;
	cpu->native_base_owner = task;
	cpu->native_bases_valid = task->execution->arch.cs ==
				  USER64_CODE_SELECTOR;
	ps_load_task_segments(task);
	int_intr_setlevel(irq);
}
void arch_task_init_user_frame(intr_frame *frame, vaddr_t ip, vaddr_t sp)
{
	memset(frame, 0, sizeof(*frame));
	frame->eip = (void *)ip;
	frame->esp = (void *)sp;
	frame->cs = current->execution->arch.cs;
	frame->ss = frame->ds = frame->es = USER_DATA_SELECTOR;
	frame->fs = frame->gs =
		frame->cs == USER64_CODE_SELECTOR ? 0 : USER_DATA_SELECTOR;
	frame->eflags = 0x202;
}
