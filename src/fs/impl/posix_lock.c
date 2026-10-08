#include <fs/fs.h>
#include <fs/fcntl.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <errno.h>

/* Process-owned byte ranges are independent of BSD open-file locks. */
typedef struct posix_range {
	struct posix_range *next;
	void *identity;
	uint64_t ino;
	unsigned owner;
	int type;
	int64_t start, end;
} posix_range;

static posix_range *ranges;
static spinlock_t range_lock = { .inited = 1 };
static list_entry range_wait = { &range_wait, &range_wait };

static void *lock_identity(file *fp)
{
	return fp->f_inode->i_pgcache_tag ? fp->f_inode->i_pgcache_tag :
					    (void *)fp->f_inode;
}

static int same_file(posix_range *r, file *fp)
{
	return r->identity == lock_identity(fp) && r->ino == fp->f_inode->i_ino;
}

static void wake_waiters(void)
{
	while (!list_is_empty(&range_wait)) {
		list_entry *entry = list_remove_tail(&range_wait);
		task_struct *task = container_of(entry, task_struct, ps_list);
		ps_put_to_ready_queue(task);
	}
}

static void free_ranges(posix_range *r)
{
	while (r) {
		posix_range *next = r->next;
		free(r);
		r = next;
	}
}

void fs_posix_lock_release(file *fp, unsigned owner)
{
	posix_range **link, *garbage = NULL;
	int irq;
	spinlock_lock(&range_lock, &irq);
	for (link = &ranges; *link;) {
		posix_range *r = *link;
		if (r->owner == owner && (!fp || same_file(r, fp))) {
			*link = r->next;
			r->next = garbage;
			garbage = r;
		} else {
			link = &r->next;
		}
	}
	if (garbage)
		wake_waiters();
	spinlock_unlock(&range_lock, irq);
	free_ranges(garbage);
}

int fs_posix_lock(file *fp, int cmd, struct flock64 *fl)
{
	task_struct *cur = CURRENT_TASK();
	posix_range *fresh = NULL, *split = NULL, *garbage = NULL;
	int64_t base, start, end;
	int irq, ret = 0;
	if (fl->l_type != F_RDLCK && fl->l_type != F_WRLCK &&
	    fl->l_type != F_UNLCK)
		return -EINVAL;
	if (cmd == F_GETLK64 && fl->l_type == F_UNLCK)
		return -EINVAL;
	if (cmd != F_GETLK64 && fl->l_type == F_RDLCK &&
	    (fp->f_mode & O_ACCMODE) == O_WRONLY)
		return -EBADF;
	if (cmd != F_GETLK64 && fl->l_type == F_WRLCK &&
	    (fp->f_mode & O_ACCMODE) == O_RDONLY)
		return -EBADF;
	switch (fl->l_whence) {
	case 0:
		base = 0;
		break;
	case 1:
		base = fp->f_pos;
		break;
	case 2:
		if (fp->f_inode->i_size > INT64_MAX)
			return -EOVERFLOW;
		base = fp->f_inode->i_size;
		break;
	default:
		return -EINVAL;
	}
	if (__builtin_add_overflow(base, fl->l_start, &start))
		return -EOVERFLOW;
	if (fl->l_len > 0) {
		if (__builtin_add_overflow(start, fl->l_len - 1, &end))
			return -EOVERFLOW;
	} else if (fl->l_len < 0) {
		if (start <= 0)
			return -EINVAL;
		end = start - 1;
		if (__builtin_add_overflow(start, fl->l_len, &start))
			return -EOVERFLOW;
	} else {
		end = INT64_MAX;
	}
	if (start < 0)
		return -EINVAL;
	if (cmd != F_GETLK64) {
		fresh = malloc(sizeof(*fresh));
		split = malloc(sizeof(*split));
		if (!fresh || !split) {
			free(fresh);
			free(split);
			return -ENOMEM;
		}
	}
	spinlock_lock(&range_lock, &irq);
	for (;;) {
		posix_range *conflict = NULL;
		for (posix_range *r = ranges; r; r = r->next) {
			if (r->owner != cur->tgid && same_file(r, fp) &&
			    start <= r->end && end >= r->start &&
			    (fl->l_type == F_WRLCK || r->type == F_WRLCK)) {
				conflict = r;
				break;
			}
		}
		if (cmd == F_GETLK64) {
			fl->l_type = conflict ? conflict->type : F_UNLCK;
			if (conflict) {
				fl->l_whence = 0;
				fl->l_start = conflict->start;
				fl->l_len = conflict->end == INT64_MAX ?
						    0 :
						    conflict->end -
							    conflict->start + 1;
				fl->l_pid = conflict->owner;
			}
			goto out;
		}
		if (!conflict || fl->l_type == F_UNLCK)
			break;
		if (cmd == F_SETLK64) {
			ret = -EAGAIN;
			goto out;
		}
		if (ps_prepare_interruptible_wait(cur, &range_wait, 0,
						  __func__) < 0) {
			ret = -EINTR;
			goto out;
		}
		spinlock_unlock(&range_lock, irq);
		task_sched();
		ps_finish_timed_wait(cur);
		if (ps_interrupting_signals(cur)) {
			free(fresh);
			free(split);
			return -EINTR;
		}
		spinlock_lock(&range_lock, &irq);
	}
	/* Replacement or unlocking preserves the portions outside the request. */
	for (posix_range **link = &ranges; *link;) {
		posix_range *r = *link;
		if (r->owner != cur->tgid || !same_file(r, fp) ||
		    start > r->end || end < r->start) {
			link = &r->next;
			continue;
		}
		if (r->start < start && r->end > end) {
			*split = *r;
			split->start = end + 1;
			split->next = r->next;
			r->next = split;
			r->end = start - 1;
			split = NULL;
			break;
		} else if (r->start < start) {
			r->end = start - 1;
			link = &r->next;
		} else if (r->end > end) {
			r->start = end + 1;
			link = &r->next;
		} else {
			*link = r->next;
			r->next = garbage;
			garbage = r;
		}
	}
	if (fl->l_type != F_UNLCK) {
		*fresh = (posix_range){ .next = ranges,
					.identity = lock_identity(fp),
					.ino = fp->f_inode->i_ino,
					.owner = cur->tgid,
					.type = fl->l_type,
					.start = start,
					.end = end };
		ranges = fresh;
		fresh = NULL;
	}
	wake_waiters();
out:
	spinlock_unlock(&range_lock, irq);
	free(fresh);
	free(split);
	free_ranges(garbage);
	return ret;
}
