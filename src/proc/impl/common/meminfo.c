/*
 * /proc/meminfo — memory statistics.
 *
 * Standard fields match the Linux /proc/meminfo format (values in kB).
 * MOS-specific diagnostics live in /proc/mos.
 */
#include <mm/mm.h>
#include <mm/phymm.h>
#include "common.h"

extern unsigned cache_count;
extern unsigned buffer_count;

/* Convert pages to kB */
#define PG_KB(n) (((unsigned long)(n)) * (PAGE_SIZE / 1024))
#define PG_BYTES(n) (((unsigned long)(n)) * PAGE_SIZE)

static void print_kb(proc_buf_t *pb, const char *name, unsigned long kb)
{
	proc_buf_printf(pb, "%-13s %8lu kB\n", name, kb);
}

static void fill(proc_buf_t *pb)
{
	phymm_usage usage;
	unsigned total_pages;
	unsigned free_pages;
	unsigned used_pages;
	unsigned reclaim_pages;
	unsigned active_anon_pages;
	unsigned active_cache_pages;
	unsigned active_pages;
	unsigned inactive_clean_pages;
	unsigned inactive_target_pages;

	phymm_get_usage(&usage);

	total_pages = usage.low_total_pages + usage.high_total_pages;
	free_pages = usage.low_free_pages + usage.high_free_pages;
	used_pages = total_pages > free_pages ? total_pages - free_pages : 0;
	reclaim_pages = buffer_count + cache_count;
	active_anon_pages =
		used_pages > reclaim_pages ? used_pages - reclaim_pages : 0;
	active_cache_pages = cache_count;
	active_pages = active_anon_pages + active_cache_pages;
	inactive_clean_pages = free_pages;
	inactive_target_pages = free_pages / 4;

	proc_buf_printf(
		pb,
		"        total:    used:    free:  shared: buffers:  cached:\n");
	proc_buf_printf(pb, "Mem:  %8lu %8lu %8lu %8lu %8lu %8lu\n",
			PG_BYTES(total_pages), PG_BYTES(used_pages),
			PG_BYTES(free_pages), 0ul, PG_BYTES(buffer_count),
			PG_BYTES(cache_count));
	proc_buf_printf(pb, "Swap: %8lu %8lu %8lu\n", 0ul, 0ul, 0ul);
	print_kb(pb, "MemTotal:", PG_KB(total_pages));
	print_kb(pb, "MemFree:", PG_KB(free_pages));
	print_kb(pb, "MemShared:", 0);
	print_kb(pb, "Buffers:", PG_KB(buffer_count));
	print_kb(pb, "Cached:", PG_KB(cache_count));
	print_kb(pb, "SwapCached:", 0);
	print_kb(pb, "Active:", PG_KB(active_pages));
	print_kb(pb, "ActiveAnon:", PG_KB(active_anon_pages));
	print_kb(pb, "ActiveCache:", PG_KB(active_cache_pages));
	print_kb(pb, "Inact_dirty:", 0);
	print_kb(pb, "Inact_laundry:", 0);
	print_kb(pb, "Inact_clean:", PG_KB(inactive_clean_pages));
	print_kb(pb, "Inact_target:", PG_KB(inactive_target_pages));
	print_kb(pb, "HighTotal:", PG_KB(usage.high_total_pages));
	print_kb(pb, "HighFree:", PG_KB(usage.high_free_pages));
	print_kb(pb, "LowTotal:", PG_KB(usage.low_total_pages));
	print_kb(pb, "LowFree:", PG_KB(usage.low_free_pages));
	print_kb(pb, "SwapTotal:", 0);
	print_kb(pb, "SwapFree:", 0);
}

DEFINE_PROC_FILE(meminfo, fill);
