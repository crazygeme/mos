#include <lib/klib.h>
#include <lib/lock.h>
#include <device/time.h>

static spinlock_t random_lock;
static uint64_t random_state;
static int random_initialized;
static int random_hardware;

static uint64_t random_fallback(void)
{
	uint64_t value = (random_state += 0x9e3779b97f4a7c15ULL);
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

static void random_initialize(void)
{
	unsigned eax = 1, ebx, ecx = 0, edx;
	asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
	random_hardware = (ecx & (1U << 30)) != 0;
	random_state = time_now_us() ^ 0x6a09e667f3bcc909ULL;
	random_initialized = 1;
}

void kernel_random_bytes(void *buffer, unsigned size)
{
	unsigned char *out = buffer;
	int irq;
	spinlock_lock(&random_lock, &irq);
	if (!random_initialized)
		random_initialize();
	while (size) {
		unsigned value = 0;
		unsigned char ready = 0;
		if (random_hardware) {
			for (unsigned retry = 0; retry < 10 && !ready; retry++)
				asm volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ready));
		}
		if (!ready)
			value = (unsigned)random_fallback();
		unsigned count = size < sizeof(value) ? size : sizeof(value);
		memcpy(out, &value, count);
		out += count;
		size -= count;
	}
	spinlock_unlock(&random_lock, irq);
}

void kernel_random_mix(uint64_t value)
{
	int irq;
	spinlock_lock(&random_lock, &irq);
	if (!random_initialized)
		random_initialize();
	random_state ^= value;
	(void)random_fallback();
	spinlock_unlock(&random_lock, irq);
}
