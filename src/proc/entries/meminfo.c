/*
 * /proc/meminfo — memory statistics.
 *
 * Standard fields match the Linux /proc/meminfo format (values in kB).
 * MOS-specific diagnostics live in /proc/mos.
 */
#include <ps/ps.h>
#include <mm/mm.h>
#include "generic.h"

extern unsigned phymm_begin;
extern unsigned phymm_end;
extern unsigned phymm_used;
extern unsigned cache_count; /* current page-cache pages */
extern unsigned pgc_top; /* page-table cache pages allocated */
extern unsigned buffer_count;

/* Convert pages to kB */
#define PG_KB(n) (((unsigned long)(n)) * (PAGE_SIZE / 1024))

static void fill(void *buf, size_t size)
{
	unsigned total_pages = phymm_end - phymm_begin;
	unsigned free_pages = total_pages - phymm_used;

	char *p = buf;
	memset(buf, 0, size);

	p += sprintf(p, "MemTotal:          %8u kB\n", PG_KB(total_pages));
	p += sprintf(p, "MemFree:           %8u kB\n", PG_KB(free_pages));
	p += sprintf(p, "MemAvailable:      %8u kB\n", PG_KB(free_pages));
	p += sprintf(p, "Buffers:           %8u kB\n", PG_KB(buffer_count));
	p += sprintf(p, "Cached:            %8u kB\n", PG_KB(cache_count));
	p += sprintf(p, "SwapCached:        %8u kB\n", 0u);
	p += sprintf(p, "Active:            %8u kB\n", 0u);
	p += sprintf(p, "Inactive:          %8u kB\n", 0u);
	p += sprintf(p, "SwapTotal:         %8u kB\n", 0u);
	p += sprintf(p, "SwapFree:          %8u kB\n", 0u);
	p += sprintf(p, "Dirty:             %8u kB\n", 0u);
	p += sprintf(p, "Writeback:         %8u kB\n", 0u);
	p += sprintf(p, "PageTables:        %8u kB\n", PG_KB(1024 - pgc_top));
	p += sprintf(p, "KernelStack:       %8u kB\n", PG_KB(ps_total_count()));
	p += sprintf(p, "CommitLimit:       %8u kB\n", PG_KB(total_pages));
	p += sprintf(p, "Committed_AS:      %8u kB\n", PG_KB(phymm_used));

	(void)p;
}

DEFINE_PROC_FILE(meminfo, fill);
