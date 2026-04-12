/*
 * /proc/meminfo — memory statistics.
 *
 * Standard fields match the Linux /proc/meminfo format (values in kB).
 * MOS-specific diagnostics live in /proc/mos.
 */
#include <ps/ps.h>
#include <mm/mm.h>
#include "common.h"

extern unsigned phymm_begin;
extern unsigned phymm_end;
extern unsigned phymm_used;
extern unsigned cache_count;
extern unsigned pgc_top; /* page-table cache pages allocated */
extern unsigned buffer_count;

/* Convert pages to kB */
#define PG_KB(n) (((unsigned long)(n)) * (PAGE_SIZE / 1024))

static void fill(proc_buf_t *pb)
{
	unsigned total_pages = phymm_end - phymm_begin;
	unsigned free_pages = total_pages - phymm_used;

	proc_buf_printf(pb, "MemTotal:          %8u kB\n", PG_KB(total_pages));
	proc_buf_printf(pb, "MemFree:           %8u kB\n", PG_KB(free_pages));
	proc_buf_printf(pb, "MemAvailable:      %8u kB\n", PG_KB(free_pages));
	proc_buf_printf(pb, "Buffers:           %8u kB\n", PG_KB(buffer_count));
	proc_buf_printf(pb, "Cached:            %8u kB\n", PG_KB(cache_count));
	proc_buf_printf(pb, "SwapCached:        %8u kB\n", 0u);
	proc_buf_printf(pb, "Active:            %8u kB\n", 0u);
	proc_buf_printf(pb, "Inactive:          %8u kB\n", 0u);
	proc_buf_printf(pb, "SwapTotal:         %8u kB\n", 0u);
	proc_buf_printf(pb, "SwapFree:          %8u kB\n", 0u);
	proc_buf_printf(pb, "Dirty:             %8u kB\n", 0u);
	proc_buf_printf(pb, "Writeback:         %8u kB\n", 0u);
	proc_buf_printf(pb, "PageTables:        %8u kB\n",
			PG_KB(PAGE_TABLE_CACHE_PAGES - pgc_top));
	proc_buf_printf(pb, "KernelStack:       %8u kB\n",
			PG_KB(ps_total_count()));
	proc_buf_printf(pb, "CommitLimit:       %8u kB\n", PG_KB(total_pages));
	proc_buf_printf(pb, "Committed_AS:      %8u kB\n", PG_KB(phymm_used));
}

DEFINE_PROC_FILE(meminfo, fill);
