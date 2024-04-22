#include <mount.h>
#include <debugfs.h>
#include <mm.h>
#include <klib.h>
#include <timer.h>

extern unsigned page_fault_cow;
extern unsigned page_fault_invalid;
extern unsigned page_fault_file;
extern unsigned page_fault_file_read;
extern unsigned page_fault_perm;
extern unsigned long long page_fault_cow_spent;
extern unsigned long long page_fault_invalid_spent;
extern unsigned long long page_fault_file_spent;
extern unsigned long long page_fault_perm_spent;
extern unsigned int heap_quota;
extern unsigned int heap_quota_high;
extern unsigned int cur_block_top;
extern unsigned phymm_begin;
extern unsigned phymm_end;
extern unsigned pgc_count;
extern unsigned pgc_top;
extern unsigned phymm_used;
extern unsigned long long phymm_alloc_spent;
unsigned page_fault_file_total = 0;
unsigned page_fault_file_read_total = 0;
unsigned long long page_fault_cow_spent_total = 0;
unsigned long long page_fault_invalid_spent_total = 0;
unsigned long long page_fault_file_spent_total = 0;
unsigned long long page_fault_perm_spent_total = 0;
unsigned long long phymm_alloc_spent_total = 0;

static void fill(void *buf, size_t size)
{
	memset(buf, 0, size);
	sprintf(buf,
		"Physical memory Begin:                  %h\n"
		"Physical memory End:                    %h\n"
		"Physical memory Total:                  %h\n"
		"Physical memory Used:                   %h\n"
		"Physical memory alloc spent:            %d.%d\n"
		"Physical memory alloc spent(Total):     %d.%d\n"
		"Page table cache Highest:               %h\n"
		"Page table cache Current:               %h\n"
		"Page fault file count:                  %d\n"
		"Page fault file count(Total):           %d,%d\n"
		"Page fault file spent:                  %d.%d ms\n"
		"Page fault file spent(Total):           %d.%d ms\n"
		"Page fault invalid count:               %d\n"
		"Page fault invalid spent:               %d.%d ms\n"
		"Page fault invalid spent(Total):        %d.%d ms\n"
		"Page fault cow count:                   %d\n"
		"Page fault cow spent:                   %d.%d ms\n"
		"Page fault cow spent(Total):            %d.%d ms\n"
		"Page fault perm count:                  %d\n"
		"Page fault perm spent:                  %d.%d ms\n"
		"Page fault perm spent(Total):           %d.%d ms\n"
		"Page fault read file:                   %h\n"
		"Page fault read file(Total):            %h\n"
		"kernel heap quota                       %h\n"
		"kernel heap Highest                     %h\n"
		"\n\n",
		phymm_begin * PAGE_SIZE, phymm_end * PAGE_SIZE,
		(phymm_end - phymm_begin) * PAGE_SIZE, phymm_used * PAGE_SIZE,
		(int)phymm_alloc_spent / 1000, (int)phymm_alloc_spent % 1000,
		(int)phymm_alloc_spent_total / 1000,
		(int)phymm_alloc_spent_total % 1000, pgc_top * PAGE_SIZE,
		pgc_count * PAGE_SIZE, page_fault_file,
		(int)page_fault_file_total / 1000,
		(int)page_fault_file_total % 1000,
		(int)page_fault_file_spent / 1000,
		(int)page_fault_file_spent % 1000,
		(int)page_fault_file_spent_total / 1000,
		(int)page_fault_file_spent_total % 1000, page_fault_invalid,
		(int)page_fault_invalid_spent / 1000,
		(int)page_fault_invalid_spent % 1000,
		(int)page_fault_invalid_spent_total / 1000,
		(int)page_fault_invalid_spent_total % 1000, page_fault_cow,
		(int)page_fault_cow_spent / 1000,
		(int)page_fault_cow_spent % 1000,
		(int)page_fault_cow_spent_total / 1000,
		(int)page_fault_cow_spent_total % 1000, page_fault_perm,
		(int)page_fault_perm_spent / 1000,
		(int)page_fault_perm_spent % 1000,
		(int)page_fault_perm_spent_total / 1000,
		(int)page_fault_perm_spent_total % 1000, page_fault_file_read,
		page_fault_file_read_total, heap_quota, heap_quota_high);
}

static void meminfo_timeout(timer_t *timer, void *ctx)
{
	page_fault_cow_spent_total += page_fault_cow_spent;
	page_fault_invalid_spent_total += page_fault_invalid_spent;
	page_fault_file_spent_total += page_fault_file_spent;
	page_fault_perm_spent_total += page_fault_perm_spent;
	page_fault_file_read_total += page_fault_file_read;
	page_fault_file_total += page_fault_file;
	phymm_alloc_spent_total += phymm_alloc_spent;
	page_fault_cow = page_fault_invalid = page_fault_file =
		page_fault_perm = 0;
	page_fault_cow_spent = page_fault_invalid_spent =
		page_fault_file_spent = page_fault_perm_spent = 0;
	page_fault_file_read = 0;
	phymm_alloc_spent = 0;
}

void debugfs_mm_init(mount_point *mp)
{
	mount_add_file(mp, "/proc/meminfo", fill);
	timer_start(meminfo_timeout, 2000, 1, NULL);
}
