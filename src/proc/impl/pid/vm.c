/*
 * vm.c — VM statistics helpers for /proc/{pid}.
 *
 * Virtual totals count each VMA once. Resident totals count mapped physical
 * pages within ordinary VMAs, excluding direct physical device mappings.
 */
#include "proc_pid.h"
#include <mm/mmap.h>
#include <mm/mm.h>
#include <config.h>
#include <macro.h>

typedef struct {
	vm_struct_t vm;
	vm_region *resident_region;
	uint64_t total;
	uint64_t text;
	uint64_t data;
	uint64_t stack;
	uint64_t file;
	uint64_t anon;
} statm_ctx;

static void statm_region_cb(vm_region *region, void *arg)
{
	statm_ctx *ctx = arg;
	uint64_t pages = (region->end - region->begin) / PAGE_SIZE;
	uint64_t stack_pages = 0;
	vaddr_t stack_begin = ctx->vm->start_stack;
	vaddr_t stack_end = ctx->vm->task_size;

	if (stack_begin && region->end > stack_begin &&
	    region->begin < stack_end) {
		vaddr_t begin = region->begin > stack_begin ? region->begin :
							      stack_begin;
		vaddr_t end = region->end < stack_end ? region->end : stack_end;
		stack_pages = (end - begin) / PAGE_SIZE;
	}

	ctx->total += pages;
	ctx->stack += stack_pages;
	if (region->prot & PROT_EXEC)
		ctx->text += pages;
	if (region->prot & PROT_WRITE)
		ctx->data += pages - stack_pages;
}

static void statm_resident_cb(void *arg, vaddr_t address, paddr_t physical)
{
	statm_ctx *ctx = arg;
	vm_region *region = ctx->resident_region;
	(void)physical;

	if (!region || address < region->begin || address >= region->end) {
		region = vm_find_map(ctx->vm, address);
		ctx->resident_region = region;
	}
	if (!region || (region->vm_flags & VM_REGION_F_DIRECT_PHYS))
		return;
	if (region->fp)
		ctx->file++;
	else
		ctx->anon++;
}

void vm_get_stats(task_struct *task, vm_stats_t *out)
{
	statm_ctx ctx = { .vm = task->user->vm };

	memset(out, 0, sizeof(*out));
	if (!ctx.vm)
		return;
	vm_enum(ctx.vm, statm_region_cb, &ctx);
	ps_enum_user_map(task, statm_resident_cb, &ctx);
	out->stk_kb = ctx.stack * (PAGE_SIZE / 1024);
	out->text_kb = ctx.text * (PAGE_SIZE / 1024);
	out->data_kb = ctx.data * (PAGE_SIZE / 1024);
	out->total_kb = ctx.total * (PAGE_SIZE / 1024);
	out->rss_file_kb = ctx.file * (PAGE_SIZE / 1024);
	out->rss_anon_kb = ctx.anon * (PAGE_SIZE / 1024);
}

void vm_fill_statm(proc_buf_t *pb, task_struct *task)
{
	vm_stats_t stats;
	const unsigned kb_per_page = PAGE_SIZE / 1024;
	vm_get_stats(task, &stats);
	proc_buf_printf(
		pb, "%llu %llu %llu %llu 0 %llu 0\n",
		(unsigned long long)(stats.total_kb / kb_per_page),
		(unsigned long long)((stats.rss_file_kb + stats.rss_anon_kb) /
				     kb_per_page),
		(unsigned long long)(stats.rss_file_kb / kb_per_page),
		(unsigned long long)(stats.text_kb / kb_per_page),
		(unsigned long long)((stats.data_kb + stats.stk_kb) /
				     kb_per_page));
}
