#include <mount.h>
#include <debugfs.h>
#include <mm.h>
#include <klib.h>

extern unsigned page_fault_count;
extern unsigned page_falut_total_time;
extern unsigned int heap_quota;
extern unsigned int heap_quota_high;
extern unsigned int cur_block_top;
extern unsigned phymm_valid;
extern unsigned phymm_max;
extern unsigned pgc_count;
extern unsigned pgc_top;

static void fill(void *buf, size_t size)
{
	memset(buf, 0, size);
	sprintf(buf,
		"Physical memory Max:      %h\n"
		"Physical memory Valid:    %h\n"
		"Page table cache Highest: %h\n"
		"Page table cache Current: %h\n"
		"Page fault count:         %d\n"
		"Page fault spent:         %d ms\n"
		"kernel heap quota         %h\n"
		"kernel heap Highest       %h\n"
		"\n",
		phymm_max * PAGE_SIZE, phymm_valid * PAGE_SIZE,
		pgc_top * PAGE_SIZE, pgc_count * PAGE_SIZE, page_fault_count,
		page_falut_total_time, heap_quota, heap_quota_high);
}

void debugfs_mm_init(mount_point *mp)
{
	mount_add_file(mp, "/proc/meminfo", fill);
}
