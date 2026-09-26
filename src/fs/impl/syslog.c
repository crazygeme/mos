/* Shared kernel log records for klogctl, /proc/kmsg, and /dev/kmsg. */
#include <fs/syslog.h>
#include <fs/fcntl.h>
#include <fs/poll.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <hw/time.h>
#include <ps/ps.h>
#include <macro.h>
#include <errno.h>

#define LOG_RECORDS 64
#define LOG_TEXT 512
#define LOG_FORMAT (LOG_TEXT * 4 + 96)

/* Linux lseek origins supported by the record device. */
enum { KMSG_SEEK_SET = 0, KMSG_SEEK_END = 2, KMSG_SEEK_DATA = 3 };

struct log_record {
	unsigned long long sequence, timestamp;
	unsigned priority, length;
	char text[LOG_TEXT];
};
struct log_cursor {
	unsigned long long sequence;
	unsigned offset, rdev;
};
struct log_waiter {
	list_entry node;
	task_struct *task;
};
static struct log_record records[LOG_RECORDS];
static unsigned long long first_sequence, next_sequence, clear_sequence;
static struct log_cursor stream_cursor;
static spinlock_t log_lock;
static list_entry waiters;
static int initialized;

static void syslog_init(void)
{
	spinlock_init(&log_lock);
	list_init(&waiters);
	initialized = 1;
}
KERNEL_INIT(1, syslog_init);

/* Producers never wait for readers. Full rings discard the oldest record. */
void syslog_emit(unsigned priority, const char *text, unsigned length)
{
	struct log_record *record;
	list_entry *entry;
	int irq;

	if (!initialized || !length)
		return;
	if (length >= LOG_TEXT)
		length = LOG_TEXT - 1;
	if (text[length - 1] == '\n')
		length--;
	spinlock_lock(&log_lock, &irq);
	record = &records[next_sequence % LOG_RECORDS];
	record->sequence = next_sequence;
	record->timestamp = time_now_us();
	record->priority = priority;
	record->length = length;
	memcpy(record->text, text, length);
	next_sequence++;
	if (next_sequence - first_sequence > LOG_RECORDS)
		first_sequence = next_sequence - LOG_RECORDS;
	for (entry = waiters.next; entry != &waiters; entry = entry->next) {
		struct log_waiter *waiter = container_of(entry, struct log_waiter, node);
		ps_put_to_ready_queue(waiter->task);
	}
	spinlock_unlock(&log_lock, irq);
}

static void log_deregister(void *opaque, task_struct *task)
{
	struct log_waiter *waiter = opaque;
	int irq;
	(void)task;
	spinlock_lock(&log_lock, &irq);
	list_remove_entry(&waiter->node);
	spinlock_unlock(&log_lock, irq);
	free(waiter);
}

static void log_register(poll_table *pt)
{
	struct log_waiter *waiter;
	int irq;

	if (!pt)
		return;
	waiter = zalloc(sizeof(*waiter));
	if (!waiter) {
		pt->unsupported = 1;
		return;
	}
	waiter->task = pt->task;
	list_init(&waiter->node);
	spinlock_lock(&log_lock, &irq);
	list_insert_tail(&waiters, &waiter->node);
	spinlock_unlock(&log_lock, irq);
	if (poll_table_add(pt, waiter, log_deregister) < 0)
		log_deregister(waiter, pt->task);
}

struct log_wait {
	struct log_cursor *cursor;
	poll_table table;
};
static int log_wait_check(void *opaque)
{
	struct log_wait *wait = opaque;
	int irq, ready;
	spinlock_lock(&log_lock, &irq);
	ready = wait->cursor->sequence < next_sequence;
	spinlock_unlock(&log_lock, irq);
	if (!ready && (current->signal->sig_pending & ~current->signal->sig_mask))
		return -EINTR;
	return ready;
}
static int log_wait_register(void *opaque)
{
	struct log_wait *wait = opaque;
	log_register(&wait->table);
	return wait->table.unsupported;
}
static void log_wait_deregister(void *opaque)
{
	struct log_wait *wait = opaque;
	poll_table_cleanup(&wait->table);
}
static const struct poll_ops log_wait_ops = {
	.check = log_wait_check,
	.reg = log_wait_register,
	.dereg = log_wait_deregister,
};

/* Called with log_lock held; device formatting requires LOG_FORMAT bytes. */
static unsigned log_format(const struct log_record *record, char *buf, int device)
{
	unsigned length, i;
	static const char hex[] = "0123456789abcdef";

	if (device) {
		length = sprintf(buf, "%u,%llu,%llu,-;", record->priority,
				 record->sequence, record->timestamp);
		for (i = 0; i < record->length; i++) {
			unsigned char c = record->text[i];
			if (c < 32 || c >= 127 || c == '\\') {
				buf[length++] = '\\';
				buf[length++] = 'x';
				buf[length++] = hex[c >> 4];
				buf[length++] = hex[c & 15];
			} else
				buf[length++] = c;
		}
	} else {
		length = sprintf(buf, "<%u>", record->priority);
		memcpy(buf + length, record->text, record->length);
		length += record->length;
	}
	buf[length++] = '\n';
	return length;
}

static ssize_t log_read(struct log_cursor *cursor, void *buf, size_t size,
			int device, int nonblock)
{
	char formatted[LOG_FORMAT];
	int irq, result;
	unsigned length, full, copied = 0;

	if (!size)
		return 0;
	if (!buf)
		return -EFAULT;
	for (;;) {
		spinlock_lock(&log_lock, &irq);
		if (cursor->sequence < first_sequence) {
			cursor->sequence = first_sequence;
			cursor->offset = 0;
			if (device) {
				spinlock_unlock(&log_lock, irq);
				return -EPIPE;
			}
		}
		while (cursor->sequence < next_sequence && copied < size) {
			full = log_format(&records[cursor->sequence % LOG_RECORDS],
					  formatted, device);
			length = full;
			if (device && size < length) {
				spinlock_unlock(&log_lock, irq);
				return -EINVAL;
			}
			length -= cursor->offset;
			if (length > size - copied)
				length = size - copied;
			memcpy((char *)buf + copied, formatted + cursor->offset, length);
			copied += length;
			cursor->offset += length;
			if (cursor->offset == full) {
				cursor->sequence++;
				cursor->offset = 0;
			}
			if (device)
				break;
		}
		spinlock_unlock(&log_lock, irq);
		if (copied)
			return copied;
		if (nonblock)
			return -EAGAIN;
		{
			poll_table_entry entry;
			struct log_wait wait = { .cursor = cursor };
			poll_table_init(&wait.table, current, &entry, 1);
			result = poll_wait_loop(&log_wait_ops, &wait, 0, 1, 0);
		}
		if (result < 0)
			return result;
	}
}

static ssize_t log_file_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	int device = S_ISCHR(fp->f_inode->i_mode);
	(void)pos;
	return log_read(device ? fp->f_inode->i_private : &stream_cursor,
			buf, size, device, (fp->f_flag & O_NONBLOCK) != 0);
}
static ssize_t log_file_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	const char *text = buf;
	unsigned priority = 14, prefix = 0, value = 0, i;
	(void)pos;
	if (!S_ISCHR(fp->f_inode->i_mode))
		return -EBADF;
	if (size >= LOG_TEXT)
		return -EMSGSIZE;
	if (!size)
		return 0;
	if (!buf)
		return -EFAULT;
	if (text[0] == '<') {
		for (i = 1; i < size && i <= 3 && text[i] >= '0' && text[i] <= '9'; i++)
			value = value * 10 + text[i] - '0';
		if (i > 1 && i < size && text[i] == '>' && value <= 191) {
			priority = value;
			prefix = i + 1;
			if (!(priority & ~7U))
				priority |= 8;
		}
	}
	syslog_emit(priority, text + prefix, size - prefix);
	return size;
}
static unsigned log_file_poll(file *fp, unsigned events, poll_table *pt)
{
	int device = S_ISCHR(fp->f_inode->i_mode);
	struct log_cursor *cursor = device ? fp->f_inode->i_private : &stream_cursor;
	unsigned ready = 0;
	int irq;
	spinlock_lock(&log_lock, &irq);
	if ((events & FS_POLL_READ) && cursor->sequence < next_sequence)
		ready |= FS_POLL_READ;
	if (device && cursor->sequence < first_sequence)
		ready |= FS_POLL_ERR;
	if (device && (events & FS_POLL_WRITE))
		ready |= FS_POLL_WRITE;
	spinlock_unlock(&log_lock, irq);
	if (!ready)
		log_register(pt);
	return ready;
}
static loff_t log_file_seek(file *fp, loff_t offset, int whence)
{
	struct log_cursor *cursor = fp->f_inode->i_private;
	int irq;
	if (!S_ISCHR(fp->f_inode->i_mode))
		return -ESPIPE;
	if (offset || (whence != KMSG_SEEK_SET && whence != KMSG_SEEK_END &&
		       whence != KMSG_SEEK_DATA))
		return -EINVAL;
	spinlock_lock(&log_lock, &irq);
	cursor->sequence = whence == KMSG_SEEK_END ? next_sequence : first_sequence;
	if (whence == KMSG_SEEK_DATA && clear_sequence > cursor->sequence)
		cursor->sequence = clear_sequence;
	cursor->offset = 0;
	spinlock_unlock(&log_lock, irq);
	return 0;
}
static int log_file_stat(file *fp, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = fp->f_inode->i_mode;
	st->st_rdev = ((struct log_cursor *)fp->f_inode->i_private)->rdev;
	st->st_ino = 0x180;
	st->st_nlink = 1;
	st->st_blksize = LOG_FORMAT;
	return 0;
}
static int log_file_release(file *fp)
{
	free(fp->f_inode->i_private);
	free(fp->f_inode);
	free(fp);
	return 0;
}
static const file_operations log_fops = {
	.read = log_file_read, .write = log_file_write, .poll = log_file_poll,
	.llseek = log_file_seek, .getattr = log_file_stat, .release = log_file_release,
};
file *syslog_open(unsigned mode, unsigned rdev)
{
	file *fp = zalloc(sizeof(*fp));
	inode *node = zalloc(sizeof(*node));
	struct log_cursor *cursor = zalloc(sizeof(*cursor));
	int irq;
	if (!fp || !node || !cursor) {
		free(fp); free(node); free(cursor);
		return NULL;
	}
	spinlock_lock(&log_lock, &irq);
	cursor->sequence = first_sequence;
	spinlock_unlock(&log_lock, irq);
	node->i_mode = mode;
	cursor->rdev = rdev;
	node->i_private = cursor;
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &log_fops;
	return fp;
}

int sys_syslog(int type, char *buf, int len)
{
	char formatted[LOG_TEXT + 16];
	unsigned long long sequence, begin;
	unsigned length, total = 0, skip, copied = 0;
	int irq;

	if (type == 0 || type == 1 || type == 6 || type == 7 || type == 8)
		return 0;
	if (type == 10)
		return LOG_RECORDS * LOG_TEXT;
	if (type == 2) {
		if (len < 0)
			return -EINVAL;
		return log_read(&stream_cursor, buf, len, 0, 0);
	}
	if (type != 3 && type != 4 && type != 5 && type != 9)
		return -EINVAL;
	if ((type == 3 || type == 4) && (len < 0 || (!buf && len)))
		return -EINVAL;
	spinlock_lock(&log_lock, &irq);
	begin = type == 9 ? stream_cursor.sequence : clear_sequence;
	if (begin < first_sequence)
		begin = first_sequence;
	for (sequence = begin; sequence < next_sequence; sequence++)
		total += log_format(&records[sequence % LOG_RECORDS], formatted, 0);
	if (type == 9) {
		if (stream_cursor.sequence >= first_sequence)
			total -= stream_cursor.offset;
		spinlock_unlock(&log_lock, irq);
		return total;
	}
	if (type == 3 || type == 4) {
		skip = total > (unsigned)len ? total - len : 0;
		for (sequence = begin; sequence < next_sequence; sequence++) {
			length = log_format(&records[sequence % LOG_RECORDS], formatted, 0);
			if (skip >= length) {
				skip -= length;
				continue;
			}
			memcpy(buf + copied, formatted + skip, length - skip);
			copied += length - skip;
			skip = 0;
		}
	}
	if (type == 4 || type == 5)
		clear_sequence = next_sequence;
	spinlock_unlock(&log_lock, irq);
	return copied;
}
