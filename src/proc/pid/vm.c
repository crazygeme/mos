/*
 * vm.c — VM statistics helpers for /proc/{pid}.
 *
 * Provides vm_get_stats() which walks a task's vm_region list and
 * aggregates page counts into a vm_stats_t.  Used by status.c and
 * statm (in status.c).
 */
#include "proc_pid.h"
#include <mm/mmap.h>
#include <mm/mm.h>
#include <config.h>
#include <macro.h>

typedef struct {
	unsigned total;
	unsigned text;
	unsigned data;
	unsigned file; /* pages backed by a file (fp != NULL) */
	unsigned anon; /* pages with no file backing */
} statm_ctx;

static void statm_region_cb(vm_region *region, void *arg)
{
	statm_ctx *ctx = arg;
	unsigned pages = (region->end - region->begin) / PAGE_SIZE;

	ctx->total += pages;
	if (region->prot & PROT_EXEC)
		ctx->text += pages;
	else if (region->prot & PROT_WRITE)
		ctx->data += pages;
	if (region->fp)
		ctx->file += pages;
	else
		ctx->anon += pages;
}

void vm_get_stats(task_struct *task, vm_stats_t *out)
{
	statm_ctx ctx = { 0, 0, 0, 0, 0 };
	unsigned heap_pages = 0;

	if (task->user->vm)
		vm_enum(task->user->vm, statm_region_cb, &ctx);
	if (task->user->brk > task->user->start_brk)
		heap_pages =
			(task->user->brk - task->user->start_brk) / PAGE_SIZE;

	unsigned stack_pages =
		(KERNEL_OFFSET - task->user->stack_bottom) / PAGE_SIZE;
	out->stk_kb = stack_pages * (PAGE_SIZE / 1024);
	out->text_kb = ctx.text * (PAGE_SIZE / 1024);
	out->data_kb = (ctx.data + heap_pages) * (PAGE_SIZE / 1024);
	out->total_kb =
		(ctx.total + heap_pages + stack_pages) * (PAGE_SIZE / 1024);
	out->rss_file_kb = ctx.file * (PAGE_SIZE / 1024);
	out->rss_anon_kb =
		(ctx.anon + heap_pages + stack_pages) * (PAGE_SIZE / 1024);
}

/* Also expose statm_ctx/statm_region_cb for fill_statm in status.c */
void vm_fill_statm(proc_buf_t *pb, task_struct *task)
{
	statm_ctx ctx = { 0, 0, 0, 0, 0 };
	unsigned heap_pages = 0;
	unsigned stack_pages =
		(KERNEL_OFFSET - task->user->stack_bottom) / PAGE_SIZE;

	if (task->user->vm)
		vm_enum(task->user->vm, statm_region_cb, &ctx);

	if (task->user->brk > task->user->start_brk)
		heap_pages =
			(task->user->brk - task->user->start_brk) / PAGE_SIZE;

	ctx.total += heap_pages + stack_pages;
	ctx.data += heap_pages + stack_pages;

	proc_buf_printf(pb, "%u %u 0 %u 0 %u 0\n", ctx.total, ctx.total,
			ctx.text, ctx.data);
}
