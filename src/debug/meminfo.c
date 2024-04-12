#include <mount.h>
#include <debugfs.h>
#include <mm.h>
#include <klib.h>
#include <timer.h>

extern unsigned page_fault_count;
extern unsigned long long page_falut_total_time;
extern unsigned int heap_quota;
extern unsigned int heap_quota_high;
extern unsigned int cur_block_top;
extern unsigned phymm_begin;
extern unsigned phymm_end;
extern unsigned pgc_count;
extern unsigned pgc_top;
extern unsigned phymm_kernel_used;
extern unsigned phymm_user_used;

static void fill(void *buf, size_t size)
{
	memset(buf, 0, size);
	sprintf(buf,
		"Physical memory Begin:    %h\n"
		"Physical memory End:      %h\n"
		"Physical memory Total:    %h\n"
		"Physical memory Used(K):  %h\n"
		"Physical memory Used(U):  %h\n"
		"Page table cache Highest: %h\n"
		"Page table cache Current: %h\n"
		"Page fault count:         %d\n"
		"Page fault spent:         %d.%d ms\n"
		"kernel heap quota         %h\n"
		"kernel heap Highest       %h\n"
		"\n\n",
		phymm_begin * PAGE_SIZE, phymm_end * PAGE_SIZE,
		(phymm_end - phymm_begin) * PAGE_SIZE,
		phymm_kernel_used * PAGE_SIZE, phymm_user_used * PAGE_SIZE,
		pgc_top * PAGE_SIZE, pgc_count * PAGE_SIZE, page_fault_count,
		(int)page_falut_total_time / 1000,
		(int)page_falut_total_time % 1000, heap_quota, heap_quota_high);
}

static void meminfo_timeout(timer_t *timer, void *ctx)
{
	page_falut_total_time = page_fault_count = 0;
}

void debugfs_mm_init(mount_point *mp)
{
	mount_add_file(mp, "/proc/meminfo", fill);
	timer_start(meminfo_timeout, 2000, 1, NULL);
}
