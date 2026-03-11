#include <fs/mount.h>
#include <fs/super/debugfs.h>
#include <lib/klib.h>
#include <hw/cpu.h>
#include <hw/time.h>

/* Execute the CPUID instruction. */
static void do_cpuid(unsigned leaf, unsigned *eax, unsigned *ebx, unsigned *ecx,
		     unsigned *edx)
{
	asm volatile("cpuid"
		     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
		     : "a"(leaf)
		     : "memory");
}

/*
 * Fetch the CPU brand string via CPUID leaves 0x80000002–0x80000004.
 * buf must be at least 48 bytes.
 */
static void cpu_brand_string(char *buf)
{
	unsigned *p = (unsigned *)buf;
	unsigned max_ext, dummy;

	do_cpuid(0x80000000, &max_ext, &dummy, &dummy, &dummy);
	if (max_ext < 0x80000004) {
		strcpy(buf, "Unknown");
		return;
	}
	do_cpuid(0x80000002, p, p + 1, p + 2, p + 3);
	do_cpuid(0x80000003, p + 4, p + 5, p + 6, p + 7);
	do_cpuid(0x80000004, p + 8, p + 9, p + 10, p + 11);
	buf[47] = '\0';
}

static void fill(void *buf, size_t size)
{
	char *p = buf;
	char brand[48];
	char vendor[13];
	unsigned eax, ebx, ecx, edx;
	unsigned family, model, stepping;
	unsigned mhz;
	int i, ncpu;

	memset(buf, 0, size);

	/* --- CPUID leaf 0: vendor string (EBX=chars 0-3, EDX=4-7, ECX=8-11) --- */
	do_cpuid(0, &eax, &ebx, &ecx, &edx);
	memcpy(vendor, &ebx, 4);
	memcpy(vendor + 4, &edx, 4);
	memcpy(vendor + 8, &ecx, 4);
	vendor[12] = '\0';

	/* --- CPUID leaf 1: family / model / stepping / feature flags --- */
	do_cpuid(1, &eax, &ebx, &ecx, &edx);
	stepping = eax & 0xf;
	model = (eax >> 4) & 0xf;
	family = (eax >> 8) & 0xf;
	if (family == 0xf)
		family += (eax >> 20) & 0xff;
	if (family == 0x6 || family == 0xf)
		model |= ((eax >> 16) & 0xf) << 4;

	cpu_brand_string(brand);
	mhz = time_get_cpu_mhz();
	ncpu = ncpus > 0 ? ncpus : 1;

	for (i = 0; i < ncpu; i++) {
		/* Per-processor block — mirrors Linux /proc/cpuinfo layout. */
		sprintf(p,
			"processor\t: %d\n"
			"vendor_id\t: %s\n"
			"cpu family\t: %d\n"
			"model\t\t: %d\n"
			"model name\t: %s\n"
			"stepping\t: %d\n"
			"cpu MHz\t\t: %d\n"
			"cache size\t: unknown\n"
			"physical id\t: %d\n"
			"apic id\t\t: %d\n"
			"online\t\t: %s\n"
			"bogomips\t: %d\n"
			"flags\t\t:",
			i, vendor, family, model, brand, stepping, mhz, i,
			(i < MAX_CPUS) ? (int)cpus[i].apic_id : i,
			(i < MAX_CPUS && cpus[i].online) ? "yes" : "no",
			mhz * 2);
		p += strlen(p);

		/* Feature flags from CPUID leaf 1 EDX */
		if (edx & (1u << 0)) {
			sprintf(p, " fpu");
			p += strlen(p);
		}
		if (edx & (1u << 1)) {
			sprintf(p, " vme");
			p += strlen(p);
		}
		if (edx & (1u << 4)) {
			sprintf(p, " tsc");
			p += strlen(p);
		}
		if (edx & (1u << 5)) {
			sprintf(p, " msr");
			p += strlen(p);
		}
		if (edx & (1u << 8)) {
			sprintf(p, " cx8");
			p += strlen(p);
		}
		if (edx & (1u << 9)) {
			sprintf(p, " apic");
			p += strlen(p);
		}
		if (edx & (1u << 11)) {
			sprintf(p, " sep");
			p += strlen(p);
		}
		if (edx & (1u << 15)) {
			sprintf(p, " cmov");
			p += strlen(p);
		}
		if (edx & (1u << 19)) {
			sprintf(p, " clflush");
			p += strlen(p);
		}
		if (edx & (1u << 23)) {
			sprintf(p, " mmx");
			p += strlen(p);
		}
		if (edx & (1u << 24)) {
			sprintf(p, " fxsr");
			p += strlen(p);
		}
		if (edx & (1u << 25)) {
			sprintf(p, " sse");
			p += strlen(p);
		}
		if (edx & (1u << 26)) {
			sprintf(p, " sse2");
			p += strlen(p);
		}
		if (edx & (1u << 28)) {
			sprintf(p, " ht");
			p += strlen(p);
		}

		/* Feature flags from CPUID leaf 1 ECX */
		if (ecx & (1u << 0)) {
			sprintf(p, " sse3");
			p += strlen(p);
		}
		if (ecx & (1u << 9)) {
			sprintf(p, " ssse3");
			p += strlen(p);
		}
		if (ecx & (1u << 19)) {
			sprintf(p, " sse4_1");
			p += strlen(p);
		}
		if (ecx & (1u << 20)) {
			sprintf(p, " sse4_2");
			p += strlen(p);
		}
		if (ecx & (1u << 23)) {
			sprintf(p, " popcnt");
			p += strlen(p);
		}
		if (ecx & (1u << 30)) {
			sprintf(p, " rdrand");
			p += strlen(p);
		}

		sprintf(p, "\n\n");
		p += strlen(p);
	}
}

static void debugfs_cpu_init(super_block *mp)
{
	vfs_create_file(mp, "/proc/cpuinfo", fill);
}

DEBUGFS_INIT(debugfs_cpu_init);
