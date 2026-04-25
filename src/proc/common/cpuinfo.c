#include <config.h>
#include "common.h"

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

static void fill(proc_buf_t *pb)
{
	char brand[48];
	char vendor[13];
	unsigned eax, ebx, ecx, edx;
	unsigned family, model, stepping;
	unsigned mhz;

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
	proc_buf_printf(pb,
			"processor\t: 0\n"
			"vendor_id\t: %s\n"
			"cpu family\t: %d\n"
			"model\t\t: %d\n"
			"model name\t: %s\n"
			"stepping\t: %d\n"
			"cpu MHz\t\t: %d\n"
			"cache size\t: unknown\n"
			"bogomips\t: %d\n"
			"flags\t\t:",
			vendor, family, model, brand, stepping, mhz, mhz * 2);

	/* Feature flags from CPUID leaf 1 EDX */
	if (edx & (1u << 0))
		proc_buf_printf(pb, " fpu");
	if (edx & (1u << 1))
		proc_buf_printf(pb, " vme");
	if (edx & (1u << 4))
		proc_buf_printf(pb, " tsc");
	if (edx & (1u << 5))
		proc_buf_printf(pb, " msr");
	if (edx & (1u << 8))
		proc_buf_printf(pb, " cx8");
	if (edx & (1u << 11))
		proc_buf_printf(pb, " sep");
	if (edx & (1u << 15))
		proc_buf_printf(pb, " cmov");
	if (edx & (1u << 19))
		proc_buf_printf(pb, " clflush");
	if (edx & (1u << 23))
		proc_buf_printf(pb, " mmx");
	if (edx & (1u << 24))
		proc_buf_printf(pb, " fxsr");
	if (edx & (1u << 25))
		proc_buf_printf(pb, " sse");
	if (edx & (1u << 26))
		proc_buf_printf(pb, " sse2");
	if (edx & (1u << 28))
		proc_buf_printf(pb, " ht");

	/* Feature flags from CPUID leaf 1 ECX */
	if (ecx & (1u << 0))
		proc_buf_printf(pb, " sse3");
	if (ecx & (1u << 9))
		proc_buf_printf(pb, " ssse3");
	if (ecx & (1u << 19))
		proc_buf_printf(pb, " sse4_1");
	if (ecx & (1u << 20))
		proc_buf_printf(pb, " sse4_2");
	if (ecx & (1u << 23))
		proc_buf_printf(pb, " popcnt");
	if (ecx & (1u << 30))
		proc_buf_printf(pb, " rdrand");

	proc_buf_printf(pb, "\n\n");
}

DEFINE_PROC_FILE(cpuinfo, fill);
