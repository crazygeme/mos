#include <ps/smp.h>
#include <int/int.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <mm/mm.h>
#include <mm/mmu.h>
#include <ps/cpu_local.h>

struct smp_cpu smp_cpus[SMP_MAX_CPUS];
static unsigned ncpu = 1;
volatile unsigned smp_online_count = 1;
static volatile unsigned *lapic;
static volatile unsigned kernel_owner;
static volatile unsigned kernel_ticket;
static volatile unsigned kernel_serving;
static volatile unsigned tlb_generation;
static addr_space_t boot_pd;
static void *firmware_copies[128];
static unsigned firmware_copy_count;
static unsigned char ap_stacks[SMP_MAX_CPUS][PAGE_SIZE]
	__attribute__((aligned(PAGE_SIZE)));
static unsigned char clean_fpu[512] __attribute__((aligned(16)));
extern unsigned char smp_trampoline[], smp_trampoline_end[];

static unsigned apic_read(unsigned reg) { return lapic[reg / 4]; }
static void apic_write(unsigned reg, unsigned value)
{
	lapic[reg / 4] = value;
	(void)apic_read(0x20);
}

unsigned smp_cpu_id(void)
{
	return arch_cpu_local()->index;
}

static unsigned smp_lapic_cpu_id(void)
{
	unsigned id, i;
	if (!lapic)
		return 0;
	id = apic_read(0x20) >> 24;
	for (i = 0; i < ncpu; i++)
		if (smp_cpus[i].apic_id == id)
			return i;
	return 0;
}

unsigned smp_cpu_count(void)
{
	return __atomic_load_n(&smp_online_count, __ATOMIC_ACQUIRE);
}

unsigned long long *smp_gdt(void)
{
	return arch_cpu_local()->gdt;
}

tss_struct *smp_tss(void) { return &arch_cpu_local()->tss.tss; }

static void ipi(unsigned id, unsigned value)
{
	while (apic_read(0x300) & (1 << 12))
		PAUSE();
	apic_write(0x310, id << 24);
	apic_write(0x300, value);
	while (apic_read(0x300) & (1 << 12))
		PAUSE();
}

void smp_tlb_poll(void)
{
	struct smp_cpu *cpu = arch_cpu_local();
	unsigned gen = __atomic_load_n(&tlb_generation, __ATOMIC_ACQUIRE);
	if (cpu->tlb_ack != gen) {
		arch_cpu_reload_tlb();
		__atomic_store_n(&cpu->tlb_ack, gen, __ATOMIC_RELEASE);
	}
}

/* Only the BKL owner publishes requests. Waiters poll with IF clear, so
 * neither interrupt masking nor BKL contention can block acknowledgment. */
void smp_tlb_flush(void)
{
	unsigned i, me = arch_cpu_local()->index;
	unsigned irq = int_intr_disable();
	arch_cpu_reload_tlb();
	if (ncpu > 1) {
		unsigned gen = __atomic_add_fetch(&tlb_generation, 1, __ATOMIC_RELEASE);
		smp_cpus[me].tlb_ack = gen;
		for (i = 0; i < ncpu; i++)
			if (i != me && smp_cpus[i].online)
				ipi(smp_cpus[i].apic_id, SMP_TLB_VECTOR);
		for (i = 0; i < ncpu; i++)
			if (i != me && smp_cpus[i].online)
				while (__atomic_load_n(&smp_cpus[i].tlb_ack,
						      __ATOMIC_ACQUIRE) != gen)
					PAUSE();
	}
	int_intr_setlevel(irq);
}

int smp_kernel_enter(void)
{
	struct smp_cpu *cpu;
	unsigned irq;
	unsigned owner;
	if (ncpu == 1) return 1;
	cpu = arch_cpu_local();
	owner = cpu->index + 1;
	if (kernel_owner == owner)
		return 1;
	irq = int_intr_disable();
	if (kernel_owner == owner) {
		int_intr_setlevel(irq);
		return 1;
	}
	{
		unsigned ticket = __sync_fetch_and_add(&kernel_ticket, 1);
		while (__atomic_load_n(&kernel_serving, __ATOMIC_ACQUIRE) != ticket) {
			smp_tlb_poll();
			PAUSE();
		}
		kernel_owner = owner;
		smp_tlb_poll();
	}
	int_intr_setlevel(irq);
	return 0;
}

void smp_kernel_leave(void)
{
	if (ncpu == 1) return;
	__atomic_store_n(&kernel_owner, 0, __ATOMIC_RELEASE);
	__atomic_add_fetch(&kernel_serving, 1, __ATOMIC_RELEASE);
}

void smp_return(intr_frame *frame)
{
	struct smp_cpu *cpu;
	if (ncpu == 1) return;
	cpu = arch_cpu_local();
	DISABLE_INTR();
	if ((frame->cs & 3) == 3 && kernel_owner == cpu->index + 1)
		smp_kernel_leave();
}

void smp_check_stop(void)
{
	if (ps_enabled() && current->terminate_requested) {
		list_remove_entry(&current->ps_list);
		current->status = ps_stopped;
		task_sched();
		DIE();
	}
}

void smp_idle(void)
{
	DISABLE_INTR();
	smp_kernel_leave();
	/* PIT broadcasts guarantee a wake even if a ready transition races hlt. */
	arch_cpu_idle_wait();
	smp_kernel_enter();
	ENABLE_INTR();
}

void smp_tick(void)
{
	unsigned i;
	if (!lapic)
		return;
	for (i = 1; i < ncpu; i++)
		if (smp_cpus[i].online)
			ipi(smp_cpus[i].apic_id, SMP_TICK_VECTOR);
}

int smp_interrupt(intr_frame *frame)
{
	if (frame->vec_no == SMP_SPURIOUS_VECTOR)
		return 1;
	if (frame->vec_no == SMP_TLB_VECTOR) {
		smp_tlb_poll();
		apic_write(0xb0, 0);
		return 1;
	}
	if (frame->vec_no == SMP_TICK_VECTOR) {
		apic_write(0xb0, 0);
		return 2;
	}
	return 0;
}

void smp_fpu_init(void)
{
	arch_cpu_fpu_init();
}

void smp_fpu_save(task_struct *task)
{
	arch_cpu_fpu_save((void *)task->fpu);
}

void smp_fpu_restore(task_struct *task)
{
	arch_cpu_fpu_restore((const void *)task->fpu);
}

void smp_fpu_new(task_struct *task)
{
	memcpy(task->fpu, clean_fpu, sizeof(clean_fpu));
}

static int checksum(const void *ptr, unsigned len)
{
	const unsigned char *p = ptr;
	unsigned char sum = 0;
	while (len--) sum += *p++;
	return sum == 0;
}

static void *firmware(unsigned addr, unsigned len)
{
	unsigned p;
	if (!len || addr + len < addr)
		return NULL;
	if (addr + len > KERNEL_DIRECT_MAP_LIMIT) {
		unsigned off = 0;
		char *copy;
		if (len > 65536 || firmware_copy_count == 128) return NULL;
		copy = kmalloc(len);
		if (!copy) return NULL;
		firmware_copies[firmware_copy_count++] = copy;
		while (off < len) {
			unsigned phys = addr + off;
			unsigned count = PAGE_SIZE - (phys & (PAGE_SIZE - 1));
			if (count > len - off) count = len - off;
			if (mm_kmap_phys(phys) != 1) return NULL;
			memcpy(copy + off, (void *)PHY_TO_VIRT(phys), count);
			mm_kunmap_phys(phys);
			off += count;
		}
		return copy;
	}
	for (p = addr & PAGE_SIZE_MASK; p < addr + len; p += PAGE_SIZE)
		if (mm_kmap_phys(p) != 1) return NULL;
	return (void *)(addr + KERNEL_OFFSET);
}

static void add_cpu(unsigned id)
{
	unsigned i;
	for (i = 0; i < ncpu; i++)
		if (smp_cpus[i].apic_id == id) return;
	if (ncpu == SMP_MAX_CPUS) {
		klog("SMP: CPU limit exceeded\n");
		DIE();
	}
	smp_cpus[ncpu].index = ncpu;
	smp_cpus[ncpu++].apic_id = id;
}

/* ACPI RSDT/MADT supplies actual APIC IDs, including non-contiguous IDs. */
static int acpi_scan(unsigned begin, unsigned end)
{
	unsigned addr;
	for (addr = begin; addr + 20 <= end; addr += 16) {
		unsigned char *r = firmware(addr, 20);
		unsigned *rsdt;
		unsigned len, i;
		if (!r || memcmp(r, "RSD PTR ", 8) || !checksum(r, 20)) continue;
		rsdt = firmware(*(unsigned *)(r + 16), 36);
		if (!rsdt || memcmp(rsdt, "RSDT", 4)) continue;
		len = rsdt[1];
		if (len < 36 || len > 65536) continue;
		rsdt = firmware(*(unsigned *)(r + 16), len);
		if (!rsdt || !checksum(rsdt, len)) continue;
		for (i = 9; i < len / 4; i++) {
			unsigned char *m = firmware(rsdt[i], 44);
			unsigned off, size;
			if (!m || memcmp(m, "APIC", 4)) continue;
			size = *(unsigned *)(m + 4);
			if (size < 44 || size > 65536) continue;
			m = firmware(rsdt[i], size);
			if (!m || !checksum(m, size)) continue;
			for (off = 44; off + 2 <= size;) {
				unsigned char *e = m + off;
				if (e[1] < 2 || off + e[1] > size) break;
				if (e[0] == 0 && e[1] >= 8 && (*(unsigned *)(e + 4) & 1))
					add_cpu(e[3]);
				off += e[1];
			}
			return 1;
		}
	}
	return 0;
}

static void cpu_setup(void)
{
	struct smp_cpu *cpu = &smp_cpus[smp_lapic_cpu_id()];
	arch_cpu_local_init(cpu);
	smp_fpu_init();
	if (lapic) {
		apic_write(0xf0, 0x100 | SMP_SPURIOUS_VECTOR);
		apic_write(0x80, 0);
		apic_write(0x320, 1 << 16);
		apic_write(0x350, cpu->index ? (1 << 16) : (7 << 8));
		apic_write(0x360, 1 << 16);
	}
}

void smp_bootstrap(void)
{
	smp_cpus[0].index = 0;
	arch_cpu_local_init(&smp_cpus[0]);
}

static void ap_main(void)
{
	struct smp_cpu *cpu;
	cpu_setup();
	cpu = arch_cpu_local();
	cpu->tlb_ack = tlb_generation;
	__atomic_store_n(&cpu->online, 1, __ATOMIC_RELEASE);
	__atomic_add_fetch(&smp_online_count, 1, __ATOMIC_RELEASE);
	smp_kernel_enter();
	ps_kickoff();
	for (;;) PAUSE();
}

void smp_init(void)
{
	unsigned a, b, c, d, low, high;
	unsigned ebda;
	DISABLE_INTR();
	arch_cpu_cpuid(1, 0, &a, &b, &c, &d);
	if (!(d & (1U << 24))) { klog("SMP: FXSR required\n"); DIE(); }
	if (d & (1U << 9)) {
		arch_cpu_read_msr(0x1b, &low, &high);
		low = (low | (1U << 11)) & ~(1U << 10);
		arch_cpu_write_msr(0x1b, low, high);
		if (mm_map_io(low & PAGE_SIZE_MASK) != 1) DIE();
		lapic = (void *)(low & PAGE_SIZE_MASK);
		/* APIC registers must never be cacheable. */
		mm_set_map_flag((unsigned)lapic, PAGE_ENTRY_KERNEL_DATA | PAGE_ENTRY_CD | PAGE_ENTRY_WT);
		smp_cpus[0].apic_id = apic_read(0x20) >> 24;
		ebda = *(unsigned short *)(KERNEL_OFFSET + 0x40e) << 4;
		if (!ebda || !acpi_scan(ebda, ebda + 1024))
			acpi_scan(0xe0000, 0x100000);
		while (firmware_copy_count)
			kfree(firmware_copies[--firmware_copy_count]);
	}
	cpu_setup();
	smp_cpus[0].online = 1;
	smp_online_count = 1;
	kernel_owner = 1;
	kernel_ticket = kernel_serving + 1;
	arch_cpu_fpu_save(clean_fpu);
	boot_pd = arch_mm_current_address_space();
	ENABLE_INTR();
}

void smp_start(void)
{
	unsigned i, irq = int_intr_disable();
	pte_t *pd = (void *)(boot_pd + KERNEL_OFFSET);
	pte_t old = pd[0];
	pte_t *pt = (void *)mm_alloc_page_table();
	if (!pt) DIE();
	pt[7] = 0x7000 | PAGE_ENTRY_KERNEL_DATA;
	pd[0] = VIRT_TO_PHY(pt) | PAGE_ENTRY_KERNEL_DATA;
	memcpy((void *)(KERNEL_OFFSET + 0x7000), smp_trampoline,
	       smp_trampoline_end - smp_trampoline);
	for (i = 1; i < ncpu; i++) {
		volatile unsigned *args = (void *)(KERNEL_OFFSET + 0x7ff0);
		unsigned timeout;
		args[0] = boot_pd;
		args[1] = (unsigned)&ap_stacks[i][PAGE_SIZE];
		args[2] = (unsigned)ap_main;
		__sync_synchronize();
		ipi(smp_cpus[i].apic_id, 0xc500);
		delay(10000);
		ipi(smp_cpus[i].apic_id, 0x8500);
		delay(200);
		ipi(smp_cpus[i].apic_id, 0x607);
		delay(200);
		ipi(smp_cpus[i].apic_id, 0x607);
		for (timeout = 0; timeout < 10000 && !smp_cpus[i].online; timeout++)
			delay(100);
		if (!smp_cpus[i].online) {
			klog("SMP: APIC %u failed to start\n", smp_cpus[i].apic_id);
			DIE();
		}
	}
	pd[0] = old;
	smp_tlb_flush();
	mm_free_page_table((unsigned)pt);
	klog("SMP: %u CPUs online\n", smp_cpu_count());
	int_intr_setlevel(irq);
}
