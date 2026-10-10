#include "proc_pid.h"
#include <ext4.h>
#include <macro.h>

typedef struct {
	unsigned tgid;
	unsigned count;
	proc_buf_t *buffer;
} task_directory;

static int task_group_member(task_struct *task, unsigned tgid)
{
	return task->thread->tgid == tgid && task->life->type != ps_kernel &&
	       task->life->psid != 0xffffffff &&
	       task->sched->status != ps_dying;
}

static void count_thread(task_struct *task, void *opaque)
{
	task_directory *directory = opaque;
	if (task_group_member(task, directory->tgid))
		directory->count++;
}

unsigned proc_thread_count(unsigned tgid)
{
	task_directory directory = { .tgid = tgid };
	ps_enum_all(count_thread, &directory);
	return directory.count;
}

static void task_directory_entry(proc_buf_t *buffer, const char *name)
{
	union {
		uint64_t alignment;
		char bytes[32];
	} entry;
	memset(entry.bytes, 0, sizeof(entry.bytes));
	struct linux_dirent *dirent = (struct linux_dirent *)entry.bytes;
	dirent->d_ino = PROC_INODE;
	dirent->d_reclen = ROUND_UP(NAME_OFFSET() + strlen(name) + 1);
	dirent->d_off = buffer->len + dirent->d_reclen;
	strcpy(dirent->d_name, name);
	proc_buf_copy(buffer, dirent, dirent->d_reclen);
}

static void list_thread(task_struct *task, void *opaque)
{
	task_directory *directory = opaque;
	char name[12];
	if (!task_group_member(task, directory->tgid))
		return;
	sprintf(name, "%u", task->life->psid);
	task_directory_entry(directory->buffer, name);
}

file *pid_task_dir_open(task_struct *task)
{
	proc_buf_t *buffer = proc_buf_new();
	task_directory directory = { .tgid = task->thread->tgid,
				     .buffer = buffer };
	task_directory_entry(buffer, ".");
	task_directory_entry(buffer, "..");
	ps_enum_all(list_thread, &directory);
	return make_pid_task_dir(buffer, task);
}

/* Resolve a thread component only within the selected process's group. */
task_struct *proc_resolve_thread(task_struct *task, const char **rest)
{
	const char *path = *rest;
	unsigned tid = 0;
	task_struct *thread;
	if (strncmp(path, "/task/", 6) != 0)
		return task;
	task_struct *owner __attribute__((cleanup(proc_put_task))) = task;
	path += 6;
	if (*path < '0' || *path > '9')
		return NULL;
	while (*path >= '0' && *path <= '9') {
		unsigned digit = (unsigned)(*path++ - '0');
		if (tid > (UINT32_MAX - digit) / 10)
			return NULL;
		tid = tid * 10 + digit;
	}
	if (*path && *path != '/')
		return NULL;
	thread = ps_find_process_ref(tid);
	if (!thread || !task_group_member(thread, task->thread->tgid)) {
		ps_put_process_ref(thread);
		return NULL;
	}
	*rest = path;
	return thread;
}
