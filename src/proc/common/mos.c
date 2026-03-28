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
#include "common.h"

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
extern unsigned task_schedule_count;
extern unsigned timer_wakeup_times;
extern unsigned timer_process_times;
extern unsigned select_loop_times;

static void fill(proc_buf_t *pb)
{
	unsigned pf_cache_rate =
		page_fault_file ?
			page_fault_file_cache_hit * 100 / page_fault_file :
			0;
	unsigned fs_cache_rate =
		cache_search_count ? cache_hit * 100 / cache_search_count : 0;

	/* ---- Memory / kernel heap ---- */
	proc_buf_printf(pb, "KernelHeapBytes:       %8u\n", heap_quota);
	proc_buf_printf(pb, "KernelHeapPeakBytes:   %8u\n", heap_quota_high);

	/* ---- Page fault counters ---- */
	proc_buf_printf(pb, "PfCow:                 %8u\n", page_fault_cow);
	proc_buf_printf(pb, "PfCowTimeUs:           %8u\n",
			(unsigned)page_fault_cow_spent);
	proc_buf_printf(pb, "PfInvalid:             %8u\n", page_fault_invalid);
	proc_buf_printf(pb, "PfInvalidTimeUs:       %8u\n",
			(unsigned)page_fault_invalid_spent);
	proc_buf_printf(pb, "PfFile:                %8u\n", page_fault_file);
	proc_buf_printf(pb, "PfFileTimeUs:          %8u\n",
			(unsigned)page_fault_file_spent);
	proc_buf_printf(pb, "PfFileSearchTimeUs:    %8u\n",
			(unsigned)page_fault_file_search_spent);
	proc_buf_printf(pb, "PfFileCacheHit:        %8u (%u%%)\n",
			page_fault_file_cache_hit, pf_cache_rate);
	proc_buf_printf(pb, "PfFileRead:            %8u\n",
			page_fault_file_read);
	proc_buf_printf(pb, "PfPerm:                %8u\n", page_fault_perm);
	proc_buf_printf(pb, "PfPermTimeUs:          %8u\n",
			(unsigned)page_fault_perm_spent);

	/* ---- Filesystem I/O ---- */
	proc_buf_printf(pb, "FsReadBytes:           %8u\n", fs_read_size);
	proc_buf_printf(pb, "FsWriteBytes:          %8u\n", fs_write_size);
	proc_buf_printf(pb, "FsCacheReadBytes:      %8u\n", fs_cache_read_size);
	proc_buf_printf(pb, "FsCacheWriteBytes:     %8u\n",
			fs_cache_write_size);
	proc_buf_printf(pb, "FsCacheBytes:          %8u\n",
			fs_cache_size * BLOCK_SECTOR_SIZE);
	proc_buf_printf(pb, "FsCacheMaxBytes:       %8u\n",
			max_fs_cache_size * BLOCK_SECTOR_SIZE);
	proc_buf_printf(pb, "FsCacheSearches:       %8u\n", cache_search_count);
	proc_buf_printf(pb, "FsCacheHits:           %8u (%u%%)\n", cache_hit,
			fs_cache_rate);
	proc_buf_printf(pb, "FsCacheSearchTimeUs:   %8u\n",
			(unsigned)cache_search_time);
	proc_buf_printf(pb, "DiskReadBytes:         %8u\n", disk_read_size);
	proc_buf_printf(pb, "DiskWriteBytes:        %8u\n", disk_write_size);
	proc_buf_printf(pb, "ElfLoadTimeUs:         %8u\n",
			(unsigned)elf_read_time);

	/* ---- Scheduler ---- */
	proc_buf_printf(pb, "SchedCalls:            %8u\n",
			task_schedule_count);
	proc_buf_printf(pb, "SchedSelectLoop:       %8u\n", select_loop_times);
	proc_buf_printf(pb, "TimerWakeups:          %8u\n", timer_wakeup_times);
	proc_buf_printf(pb, "TimerProcessed:        %8u\n",
			timer_process_times);
}

DEFINE_PROC_FILE(mos, fill);
