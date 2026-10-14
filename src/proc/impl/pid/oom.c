#include "proc_pid.h"
#include <errno.h>
extern spinlock_t ps_lock;

struct oom_file {
	task_thread *group;
	unsigned pid, uid;
};
static int oom_getattr(file *fp, struct stat *s)
{
	struct oom_file *state = fp->f_inode->i_private;
	memset(s, 0, sizeof(*s));
	s->st_mode = S_IFREG | 0644;
	s->st_uid = state->uid;
	s->st_ino = fp->f_inode->i_ino;
	s->st_nlink = 1;
	s->st_blksize = PAGE_SIZE;
	return 0;
}
static ssize_t oom_read(file *fp, void *buffer, size_t count, loff_t *position)
{
	struct oom_file *state = fp->f_inode->i_private;
	char text[16];
	int irq;
	spinlock_lock(&ps_lock, &irq);
	unsigned n = sprintf(text, "%d\n", state->group->oom_score_adj);
	spinlock_unlock(&ps_lock, irq);
	if (*position < 0)
		return -EINVAL;
	if (*position >= n)
		return 0;
	n -= *position;
	if (n > count)
		n = count;
	memcpy(buffer, text + *position, n);
	*position += n;
	return n;
}
static ssize_t oom_write(file *fp, const void *buffer, size_t count,
			 loff_t *position)
{
	struct oom_file *state = fp->f_inode->i_private;
	char text[32];
	if (!count || count >= sizeof(text))
		return -EINVAL;
	if (ps_read_process_memory(current, buffer, text, count))
		return -EFAULT;
	text[count] = 0;
	char *p = text;
	int negative = 0, score = 0;
	if (*p == '-' || *p == '+') {
		negative = *p == '-';
		p++;
	}
	if (*p < '0' || *p > '9')
		return -EINVAL;
	while (*p >= '0' && *p <= '9') {
		score = score * 10 + *p++ - '0';
		if (score > 1000)
			return -EINVAL;
	}
	while (*p == ' ' || *p == '\n' || *p == '\t')
		p++;
	if (*p)
		return -EINVAL;
	if (negative)
		score = -score;
	task_struct *target __attribute__((cleanup(proc_put_task))) =
		ps_find_process_ref(state->pid);
	if (!target || target->thread != state->group)
		return -ESRCH;
	if (!ps_may_access_process(target))
		return -EACCES;
	int irq, ret = 0;
	spinlock_lock(&ps_lock, &irq);
	if (score < state->group->oom_score_adj &&
	    !ps_has_capability(current, 24))
		ret = -EPERM;
	else
		state->group->oom_score_adj = score;
	spinlock_unlock(&ps_lock, irq);
	if (ret)
		return ret;
	*position += count;
	return count;
}
static int oom_release(file *fp)
{
	struct oom_file *state = fp->f_inode->i_private;
	ref_count_put(state->group);
	free(state);
	free(fp->f_inode);
	free(fp);
	return 0;
}
static unsigned oom_poll(file *fp, unsigned events, poll_table *pt)
{
	(void)fp;
	(void)pt;
	return events & (FS_POLL_READ | FS_POLL_WRITE);
}
static const file_operations oom_fops = {
	.getattr = oom_getattr,
	.read = oom_read,
	.write = oom_write,
	.release = oom_release,
	.poll = oom_poll,
};
file *pid_oom_open(task_struct *task)
{
	file *fp = zalloc(sizeof(*fp));
	inode *node = zalloc(sizeof(*node));
	struct oom_file *state = zalloc(sizeof(*state));
	if (!fp || !node || !state) {
		free(fp);
		free(node);
		free(state);
		return NULL;
	}
	state->group = ref_count_get(task->thread);
	state->pid = task->life->psid;
	state->uid = task->credentials->euid;
	node->i_private = state;
	node->i_mode = S_IFREG | 0644;
	node->i_ino = ((uint64_t)state->pid << 16) | 0x31;
	fp->f_inode = node;
	fp->f_fop = &oom_fops;
	fp->f_count = 1;
	return fp;
}
