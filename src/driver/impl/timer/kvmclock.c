#include <device/time_internal.h>
#include <mm/mm.h>
#include <ps/smp.h>
#include <macro.h>

/* KVM's versioned 32-byte ABI. Each vCPU owns a permanently mapped slot. */
struct kvm_time_info {
	volatile unsigned version;
	unsigned pad0;
	volatile unsigned long long tsc_timestamp;
	volatile unsigned long long system_time;
	volatile unsigned multiplier;
	volatile signed char shift;
	volatile unsigned char flags;
	unsigned char pad[2];
} __attribute__((packed));

static struct kvm_time_info clocks[SMP_MAX_CPUS] __attribute__((aligned(64)));

static unsigned long long read_tsc(void)
{
	unsigned low, high;
	/* SSE2 is required below. LFENCE orders the preceding ABI field loads. */
	asm volatile("lfence; rdtsc; lfence"
		     : "=a"(low), "=d"(high)
		     :
		     : "memory");
	return ((unsigned long long)high << 32) | low;
}

void time_kvm_cpu_init(void)
{
	paddr_t physical = VIRT_TO_PHY(&clocks[smp_cpu_id()]);
	arch_cpu_write_msr(0x4b564d01, (unsigned)physical | 1,
			   (unsigned)((unsigned long long)physical >> 32));
}

int time_kvm_init(void)
{
	unsigned a, b, c, d;
	arch_cpu_cpuid(1, 0, &a, &b, &c, &d);
	if (!(c & (1U << 31)) || !(d & (1U << 4)) || !(d & (1U << 5)) ||
	    !(d & (1U << 26)))
		return 0;
	arch_cpu_cpuid(0x40000000, 0, &a, &b, &c, &d);
	if (b != 0x4b4d564b || c != 0x564b4d56 || d != 0x4d ||
	    (a && a < 0x40000001))
		return 0;
	arch_cpu_cpuid(0x40000001, 0, &a, &b, &c, &d);
	if (!(a & (1U << 3)))
		return 0;
	time_kvm_cpu_init();
	return 1;
}

unsigned long long time_kvm_read_us(void)
{
	struct kvm_time_info *clock = &clocks[smp_cpu_id()];
	unsigned version, multiplier;
	int shift;
	unsigned long long timestamp, base, delta;
	for (;;) {
		version = clock->version;
		if (version & 1)
			continue;
		BARRIER();
		timestamp = clock->tsc_timestamp;
		base = clock->system_time;
		multiplier = clock->multiplier;
		shift = clock->shift;
		delta = read_tsc() - timestamp;
		BARRIER();
		if (version == clock->version)
			break;
	}
	if (shift < 0)
		delta >>= -shift;
	else
		delta <<= shift;
	/* Preserve the high 32 bits of a 64-by-32 multiply on both architectures. */
	delta = (delta >> 32) * multiplier +
		(((delta & 0xffffffffULL) * multiplier) >> 32);
	return (base + delta) / 1000;
}
