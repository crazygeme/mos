/*
 * files.c — /proc/{pid}/comm, cmdline, environ, and maps.
 */
#include "proc_pid.h"
#include <mm/mmap.h>
#include <config.h>
#include <macro.h>

void fill_comm(proc_buf_t *pb, task_struct *task)
{
	const char *command = task->memory->command ?
				      (const char *)task->memory->command : "";
	const char *slash = strrchr(command, '/');
	const char *name = slash ? slash + 1 : command;
	char comm[16];

	strncpy(comm, name, sizeof(comm) - 1);
	comm[sizeof(comm) - 1] = '\0';
	proc_buf_printf(pb, "%s\n", comm);
}

/* ── /proc/{pid}/cmdline ─────────────────────────────────────────────── */

void fill_cmdline(proc_buf_t *pb, task_struct *task)
{
	if (task->memory->command && task->memory->cmd_len)
		proc_buf_copy(pb, (char *)task->memory->command,
			      task->memory->cmd_len);
}

/* ── /proc/{pid}/environ ─────────────────────────────────────────────── */

void fill_environ(proc_buf_t *pb, task_struct *task)
{
	if (task->memory->environment && task->memory->env_len)
		proc_buf_copy(pb, task->memory->environment,
			      task->memory->env_len);
}

/* ── /proc/{pid}/maps ────────────────────────────────────────────────── */

typedef struct {
	proc_buf_t *pb;
	task_struct *task;
} maps_ctx;

static void maps_region_cb(vm_region *region, void *data)
{
	maps_ctx *ctx = data;
	char perms[5];
	const char *name;
	vaddr_t stack_begin = ctx->task->memory->start_stack;
	vaddr_t stack_end = ctx->task->memory->task_size;
	uint64_t ino = 0;

	perms[0] = (region->prot & PROT_READ) ? 'r' : '-';
	perms[1] = (region->prot & PROT_WRITE) ? 'w' : '-';
	perms[2] = (region->prot & PROT_EXEC) ? 'x' : '-';
	perms[3] = (region->flag & MAP_SHARED) ? 's' : 'p';
	perms[4] = '\0';

	if (region->fp) {
		name = region->fp->f_name;
		ino = region->fp->f_inode->i_ino;
	} else if (region->begin >= ctx->task->memory->start_brk &&
		   region->end <= ctx->task->memory->brk)
		name = "[heap]";
	else if (region->begin >= stack_begin && region->end <= stack_end)
		name = "[stack]";
	else
		name = "";

	proc_buf_printf(ctx->pb, "%08lx-%08lx %s %08llx 00:00 %-10llu %s\n",
			(unsigned long)region->begin,
			(unsigned long)region->end, perms,
			(unsigned long long)region->offset,
			(unsigned long long)ino, name);
}

void fill_maps(proc_buf_t *pb, task_struct *task)
{
	maps_ctx ctx = { pb, task };

	if (task->memory)
		vm_enum(task->memory, maps_region_cb, &ctx);
}
