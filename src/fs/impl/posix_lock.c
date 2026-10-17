#include <proc/impl/common/common.h>
#include <device/devnode.h>
#include <lib/auto_free.h>
#include <fs/fs.h>
#include <fs/fcntl.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <errno.h>

/* Process-owned byte ranges are independent of BSD open-file locks. */
typedef struct posix_range {
	list_entry list;
	void *identity;
	uint64_t ino;
	unsigned owner, dev;
	int type;
	int64_t start, end;
} posix_range;

static list_entry ranges = { &ranges, &ranges };
static list_entry flocks = { &flocks, &flocks };
static spinlock_t range_lock = SPINLOCK_INITIALIZER;
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
		task_struct *task =
			(container_of(entry, task_schedule, ps_list)->task);
		ps_put_to_ready_queue(task);
	}
}

static void free_ranges(list_entry *list)
{
	while (!list_is_empty(list))
		free(container_of(list_remove_head(list), posix_range, list));
}

void fs_posix_lock_release(file *fp, unsigned owner)
{
	task_thread *group = current->thread;
	if (group && group->tgid == owner &&
	    !__atomic_load_n(&group->posix_locks_used, __ATOMIC_ACQUIRE))
		return;
	list_entry garbage = { &garbage, &garbage };
	int irq;
	spinlock_lock(&range_lock, &irq);
	for (list_entry *node = ranges.next; node != &ranges;) {
		posix_range *r = container_of(node, posix_range, list);
		node = node->next;
		if (r->owner == owner && (!fp || same_file(r, fp))) {
			list_remove_entry(&r->list);
			list_insert_head(&garbage, &r->list);
		}
	}
	if (!list_is_empty(&garbage))
		wake_waiters();
	spinlock_unlock(&range_lock, irq);
	free_ranges(&garbage);
}

int fs_posix_lock(file *fp, int cmd, struct flock64 *fl)
{
	task_struct *cur = CURRENT_TASK();
	posix_range *fresh = NULL, *split = NULL;
	list_entry garbage = { &garbage, &garbage };
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
		/* Publish before acquiring range_lock, so a concurrent close either
		 * precedes this request or joins the normal serialized release. */
		__atomic_store_n(&cur->thread->posix_locks_used, 1,
				 __ATOMIC_RELEASE);
		fresh = malloc(sizeof(*fresh));
		split = malloc(sizeof(*split));
		if (!fresh || !split) {
			free(fresh);
			free(split);
			return -ENOMEM;
		}
	}
	struct stat st;
	unsigned dev = fp->f_fop->getattr && !fp->f_fop->getattr(fp, &st) ?
			       st.st_dev :
			       0;
	spinlock_lock(&range_lock, &irq);
	for (;;) {
		posix_range *conflict = NULL;
		for (list_entry *node = ranges.next; node != &ranges;
		     node = node->next) {
			posix_range *r = container_of(node, posix_range, list);
			if (r->owner != cur->thread->tgid && same_file(r, fp) &&
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
		if (ps_prepare_interruptible_wait(cur, &range_wait, 0) < 0) {
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
	for (list_entry *node = ranges.next; node != &ranges;) {
		posix_range *r = container_of(node, posix_range, list);
		node = node->next;
		if (r->owner != cur->thread->tgid || !same_file(r, fp) ||
		    start > r->end || end < r->start)
			continue;
		if (r->start < start && r->end > end) {
			*split = *r;
			split->start = end + 1;
			list_insert_head(&r->list, &split->list);
			r->end = start - 1;
			split = NULL;
			break;
		} else if (r->start < start)
			r->end = start - 1;
		else if (r->end > end)
			r->start = end + 1;
		else {
			list_remove_entry(&r->list);
			list_insert_head(&garbage, &r->list);
		}
	}
	if (fl->l_type != F_UNLCK) {
		*fresh = (posix_range){ .identity = lock_identity(fp),
					.ino = fp->f_inode->i_ino,
					.owner = cur->thread->tgid,
					.dev = dev,
					.type = fl->l_type,
					.start = start,
					.end = end };
		list_insert_head(&ranges, &fresh->list);
		fresh = NULL;
	}
	wake_waiters();
out:
	spinlock_unlock(&range_lock, irq);
	free(fresh);
	free(split);
	free_ranges(&garbage);
	return ret;
}

/* The inode lock serializes updates; range_lock protects snapshot storage. */
void fs_flock_inventory(file *fp, unsigned dev)
{
	int irq;
	spinlock_lock(&range_lock, &irq);
	if (fp->f_lock_type) {
		list_remove_entry(&fp->f_lock_inventory);
		fp->f_lock_type = 0;
	}
	if (fp->f_flock) {
		fp->f_lock_pid = current->thread->tgid;
		fp->f_lock_dev = dev;
		fp->f_lock_type = fp->f_flock;
		list_insert_head(&flocks, &fp->f_lock_inventory);
	}
	spinlock_unlock(&range_lock, irq);
}
struct lock_snapshot {
	uint64_t ino;
	int64_t start, end;
	unsigned owner, dev;
	int write, flock;
};
static void locks_fill(proc_buf_t *pb)
{
	struct lock_snapshot *snapshot AUTO_FREE = NULL;
	unsigned capacity = 16, count;
	int irq;
	for (;;) {
		free(snapshot);
		snapshot = malloc(capacity * sizeof(*snapshot));
		if (!snapshot)
			return;
		count = 0;
		spinlock_lock(&range_lock, &irq);
		for (list_entry *node = ranges.next; node != &ranges;
		     node = node->next) {
			posix_range *r = container_of(node, posix_range, list);
			if (count < capacity)
				snapshot[count] = (struct lock_snapshot){
					.ino = r->ino,
					.start = r->start,
					.end = r->end,
					.owner = r->owner,
					.dev = r->dev,
					.write = r->type == F_WRLCK
				};
			count++;
		}
		for (list_entry *node = flocks.next; node != &flocks;
		     node = node->next) {
			file *fp = container_of(node, file, f_lock_inventory);
			if (count < capacity)
				snapshot[count] = (struct lock_snapshot){
					.ino = fp->f_inode->i_ino,
					.end = INT64_MAX,
					.owner = fp->f_lock_pid,
					.dev = fp->f_lock_dev,
					.write = fp->f_lock_type == LOCK_EX,
					.flock = 1
				};
			count++;
		}
		spinlock_unlock(&range_lock, irq);
		if (count <= capacity)
			break;
		capacity = count;
	}
	for (unsigned i = 0; i < count; i++) {
		struct lock_snapshot *r = &snapshot[i];
		proc_buf_printf(pb,
				"%u: %s ADVISORY %s %u %02x:%02x:%llu %lld ",
				i + 1, r->flock ? "FLOCK" : "POSIX",
				r->write ? "WRITE" : "READ", r->owner,
				MAJOR(r->dev), MINOR(r->dev), r->ino, r->start);
		if (r->end == INT64_MAX)
			proc_buf_printf(pb, "EOF\n");
		else
			proc_buf_printf(pb, "%lld\n", r->end);
	}
}
DEFINE_PROC_FILE(locks, locks_fill);
