/*
 * /proc/mos — MOS kernel internals snapshot.
 *
 * All non-standard per-interval counters that were previously scattered
 * across fsinfo, meminfo, and sched are consolidated here.
 *
 * Format: "Key: value\n", grouped by subsystem.  Time values are in
 * microseconds (us).  Size values are in bytes unless noted.
 */
#include <mm/mm.h>
#include <hw/hdd.h>
#include "generic.h"

/* ---- mm/fault ---- */
extern unsigned page_fault_cow;
extern unsigned page_fault_invalid;
extern unsigned page_fault_file;
extern unsigned page_fault_file_cache_hit;
extern unsigned page_fault_file_read;
extern unsigned page_fault_perm;
extern unsigned long long page_fault_cow_spent;
extern unsigned long long page_fault_invalid_spent;
extern unsigned long long page_fault_file_spent;
extern unsigned long long page_fault_file_search_spent;
extern unsigned long long page_fault_perm_spent;
extern unsigned int heap_quota;
extern unsigned int heap_quota_high;

/* ---- fs/cache ---- */
extern unsigned cache_hit;
extern unsigned long long cache_search_time;
extern unsigned cache_search_count;
extern unsigned fs_cache_size;
extern unsigned max_fs_cache_size;
extern unsigned fs_read_size;
extern unsigned fs_write_size;
extern unsigned fs_cache_read_size;
extern unsigned fs_cache_write_size;
extern unsigned disk_read_size;
extern unsigned disk_write_size;
extern unsigned long long elf_read_time;

/* ---- scheduler ---- */
extern unsigned long long task_schedule_time;
extern unsigned task_schedule_count;
extern unsigned timer_wakeup_times;
extern unsigned timer_process_times;
extern unsigned select_loop_times;

static void fill(void *buf, size_t size)
{
	char *p = buf;
	unsigned pf_cache_rate =
		page_fault_file ?
			page_fault_file_cache_hit * 100 / page_fault_file :
			0;
	unsigned fs_cache_rate =
		cache_search_count ? cache_hit * 100 / cache_search_count : 0;

	memset(buf, 0, size);

	/* ---- Memory / kernel heap ---- */
	p += sprintf(p, "KernelHeapBytes:       %8u\n", heap_quota);
	p += sprintf(p, "KernelHeapPeakBytes:   %8u\n", heap_quota_high);

	/* ---- Page fault counters ---- */
	p += sprintf(p, "PfCow:                 %8u\n", page_fault_cow);
	p += sprintf(p, "PfCowTimeUs:           %8u\n",
		     (unsigned)page_fault_cow_spent);
	p += sprintf(p, "PfInvalid:             %8u\n", page_fault_invalid);
	p += sprintf(p, "PfInvalidTimeUs:       %8u\n",
		     (unsigned)page_fault_invalid_spent);
	p += sprintf(p, "PfFile:                %8u\n", page_fault_file);
	p += sprintf(p, "PfFileTimeUs:          %8u\n",
		     (unsigned)page_fault_file_spent);
	p += sprintf(p, "PfFileSearchTimeUs:    %8u\n",
		     (unsigned)page_fault_file_search_spent);
	p += sprintf(p, "PfFileCacheHit:        %8u (%u%%)\n",
		     page_fault_file_cache_hit, pf_cache_rate);
	p += sprintf(p, "PfFileRead:            %8u\n", page_fault_file_read);
	p += sprintf(p, "PfPerm:                %8u\n", page_fault_perm);
	p += sprintf(p, "PfPermTimeUs:          %8u\n",
		     (unsigned)page_fault_perm_spent);

	/* ---- Filesystem I/O ---- */
	p += sprintf(p, "FsReadBytes:           %8u\n", fs_read_size);
	p += sprintf(p, "FsWriteBytes:          %8u\n", fs_write_size);
	p += sprintf(p, "FsCacheReadBytes:      %8u\n", fs_cache_read_size);
	p += sprintf(p, "FsCacheWriteBytes:     %8u\n", fs_cache_write_size);
	p += sprintf(p, "FsCacheBytes:          %8u\n",
		     fs_cache_size * BLOCK_SECTOR_SIZE);
	p += sprintf(p, "FsCacheMaxBytes:       %8u\n",
		     max_fs_cache_size * BLOCK_SECTOR_SIZE);
	p += sprintf(p, "FsCacheSearches:       %8u\n", cache_search_count);
	p += sprintf(p, "FsCacheHits:           %8u (%u%%)\n", cache_hit,
		     fs_cache_rate);
	p += sprintf(p, "FsCacheSearchTimeUs:   %8u\n",
		     (unsigned)cache_search_time);
	p += sprintf(p, "DiskReadBytes:         %8u\n", disk_read_size);
	p += sprintf(p, "DiskWriteBytes:        %8u\n", disk_write_size);
	p += sprintf(p, "ElfLoadTimeUs:         %8u\n",
		     (unsigned)elf_read_time);

	/* ---- Scheduler ---- */
	p += sprintf(p, "SchedCalls:            %8u\n", task_schedule_count);
	p += sprintf(p, "SchedTimeUs:           %8u\n",
		     (unsigned)task_schedule_time);
	p += sprintf(p, "SchedSelectLoop:       %8u\n", select_loop_times);
	p += sprintf(p, "TimerWakeups:          %8u\n", timer_wakeup_times);
	p += sprintf(p, "TimerProcessed:        %8u\n", timer_process_times);

	(void)p;
}

DEFINE_PROC_FILE(mos, fill);
