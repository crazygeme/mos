#ifndef _PROC_PID_PROC_PID_H
#define _PROC_PID_PROC_PID_H

#include <fs/fs.h>
#include <ps/ps.h>
#include <proc/proc.h>
#include <lib/klib.h>
#include "../common/common.h"

/* ── Shared file/dir/symlink constructors (common.c) ─────────────────── */

file *make_pid_file(proc_buf_t *pb);
file *make_pid_dir(proc_buf_t *pb);
file *make_pid_symlink(const char *target);

/* ── State helpers (common.c) ────────────────────────────────────────── */

char pid_state_char(ps_status st);
const char *pid_state_name(ps_status st);

/* ── VM stats (vm.c) ─────────────────────────────────────────────────── */

typedef struct {
	unsigned total_kb;
	unsigned text_kb;
	unsigned data_kb;
	unsigned stk_kb;
	unsigned rss_file_kb;
	unsigned rss_anon_kb;
} vm_stats_t;

void vm_get_stats(task_struct *task, vm_stats_t *out);
void vm_fill_statm(proc_buf_t *pb, task_struct *task);

/* ── Per-file fill functions ─────────────────────────────────────────── */

/* status.c */
void fill_status(proc_buf_t *pb, task_struct *task);
void fill_stat(proc_buf_t *pb, task_struct *task);
void fill_statm(proc_buf_t *pb, task_struct *task);

/* files.c */
void fill_cmdline(proc_buf_t *pb, task_struct *task);
void fill_environ(proc_buf_t *pb, task_struct *task);
void fill_maps(proc_buf_t *pb, task_struct *task);

/* ── fd directory (fd.c) ─────────────────────────────────────────────── */

file *pid_fd_dir_open(task_struct *task);

#endif /* _PROC_PID_PROC_PID_H */
