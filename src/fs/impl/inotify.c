#include <fs/inotify.h>
#include <fs/fcntl.h>
#include <fs/ioctl.h>
#include <fs/poll.h>
#include <fs/vfs.h>
#include <syscall/syscall.h>
#include <lib/klib.h>
#include <lib/rbtree.h>
#include <ps/ps.h>
#include <macro.h>
#include <errno.h>

typedef struct {
	super_block *sb;
	uint64_t ino;
	char *path;
} notify_key;

struct inotify_node {
	notify_key key, parent;
	char *name;
	unsigned mode, links;
	int unlinked, parent_lost;
	list_entry open_node;
};

typedef struct {
	list_entry node;
	struct inotify_event event;
	char *name;
} notify_event;

typedef struct notify_user {
	struct rb_node tree;
	unsigned uid, instances, watches;
} notify_user;

typedef struct notify_instance {
	file *fp;
	list_entry instance_node;
	notify_user *user;
	struct rb_root watches;
	list_entry watch_list;
	unsigned next_wd, max_events, queued, uid, gid;
	uint64_t bytes;
	spinlock_t queue_lock;
	list_entry events, waiters;
	/* A reserved overflow record remains available under allocation failure. */
	notify_event overflow;
	int overflow_queued;
} notify_instance;

typedef struct notify_mark {
	struct rb_node tree;
	notify_key key;
	list_entry watches;
} notify_mark;

typedef struct {
	struct rb_node tree;
	list_entry mark_node, instance_node;
	notify_mark *mark;
	notify_instance *instance;
	unsigned mask;
	int wd;
} notify_watch;

static mutex_t notify_lock;
static mutex_t capture_lock;
static struct rb_root marks = _RBTREE_ROOT_INIT;
static struct rb_root users = _RBTREE_ROOT_INIT;
static list_entry open_files;
static list_entry tracked_files;
static list_entry instances;
static unsigned limits[] = { 8192, 128, 16384 };
static unsigned watch_count, rename_cookie;
static unsigned capture_epoch;
static int notify_ready;
static const file_operations inotify_fops;
static void capture_open_files(void);
static void capture_file(file *fp, int opened);
static void lost_event_locked(void);

static unsigned active_watches(void)
{
	return __atomic_load_n(&watch_count, __ATOMIC_ACQUIRE);
}

static void notify_init(void)
{
	mutex_init(&notify_lock);
	mutex_init(&capture_lock);
	list_init(&open_files);
	list_init(&tracked_files);
	list_init(&instances);
	notify_ready = 1;
}
KERNEL_INIT(1, notify_init);

unsigned inotify_limit_get(unsigned which)
{
	unsigned value = 0;
	mutex_lock(&notify_lock);
	if (which < sizeof(limits) / sizeof(limits[0]))
		value = limits[which];
	mutex_unlock(&notify_lock);
	return value;
}

int inotify_limit_set(unsigned which, unsigned value)
{
	if (which >= sizeof(limits) / sizeof(limits[0]) || value > 0x7fffffffu)
		return -EINVAL;
	mutex_lock(&notify_lock);
	limits[which] = value;
	mutex_unlock(&notify_lock);
	return 0;
}

static void key_put(notify_key *key)
{
	if (key->sb)
		sb_put(key->sb);
	free(key->path);
	memset(key, 0, sizeof(*key));
}

static int key_copy(notify_key *dest, const notify_key *source)
{
	memset(dest, 0, sizeof(*dest));
	if (!source->sb)
		return 0;
	if (source->path) {
		dest->path = strdup(source->path);
		if (!dest->path)
			return -ENOMEM;
	}
	dest->sb = source->sb;
	dest->ino = source->ino;
	sb_get(dest->sb);
	return 0;
}

static int key_compare(const notify_key *a, const notify_key *b)
{
	if (a->sb != b->sb)
		return (uintptr_t)a->sb < (uintptr_t)b->sb ? -1 : 1;
	if (!a->sb)
		return 0;
	if (a->ino != b->ino)
		return a->ino < b->ino ? -1 : 1;
	return a->ino ? 0 : strcmp(a->path, b->path);
}

static int key_from_file(notify_key *key, super_block *root, const char *path,
			 file *fp)
{
	super_block *sb = fp->f_sb;
	const char *relative = fp->f_relative_path;
	char *resolved;
	memset(key, 0, sizeof(*key));
	if (!sb) {
		if (!sb_path_resolve(root, path, &sb, &resolved))
			return -ENOENT;
		relative = resolved;
	}
	key->ino = fp->f_inode->i_ino;
	if (!key->ino) {
		if (!relative)
			return -ENOMEM;
		key->path = strdup(relative);
		if (!key->path)
			return -ENOMEM;
	}
	key->sb = sb;
	sb_get(sb);
	return 0;
}

static inotify_node *node_from_file(super_block *root, const char *path,
				    file *fp)
{
	inotify_node *node;
	file *parent_fp = NULL;
	char *parent_path, *slash;
	struct stat st;
	if (!fp || !fp->f_inode || !fp->f_fop || !fp->f_fop->getattr ||
	    fp->f_fop->getattr(fp, &st))
		return NULL;
	if (fp->f_name && fp->f_name[0] == '/')
		path = fp->f_name;
	node = zalloc(sizeof(*node));
	parent_path = strdup(path);
	if (!node || !parent_path)
		goto fail;
	for (size_t length = strlen(parent_path);
	     length > 1 && parent_path[length - 1] == '/';)
		parent_path[--length] = 0;
	node->mode = st.st_mode;
	node->links = st.st_nlink;
	if (key_from_file(&node->key, root, parent_path, fp))
		goto fail;
	if (fp->f_fop->notify_parent) {
		super_block *owner;
		uint64_t ino;
		const char *name;
		int result = fp->f_fop->notify_parent(fp, &owner, &ino, &name);
		if (result < 0) {
			node->parent_lost = 1;
			result = 0;
		}
		node->name = strdup(result ? name : "");
		if (!node->name)
			goto fail;
		if (result) {
			node->parent.sb = owner;
			node->parent.ino = ino;
			sb_get(owner);
		}
	} else {
		slash = strrchr(parent_path, '/');
		if (!slash)
			goto fail;
		node->name = strdup(slash + 1);
		if (!node->name)
			goto fail;
		if (slash == parent_path)
			parent_path[1] = 0;
		else
			*slash = 0;
		if (node->name[0]) {
			parent_fp = vfs_open(root, parent_path, O_PATH);
			if (!parent_fp || !parent_fp->f_inode ||
			    !S_ISDIR(parent_fp->f_inode->i_mode) ||
			    key_from_file(&node->parent, root, parent_path,
					  parent_fp)) {
				key_put(&node->parent);
				node->parent_lost = 1;
			}
			if (parent_fp)
				fs_put_file(parent_fp);
		}
	}
	free(parent_path);
	list_init(&node->open_node);
	return node;
fail:
	if (parent_fp)
		fs_put_file(parent_fp);
	free(parent_path);
	inotify_snapshot_put(node);
	return NULL;
}

inotify_node *inotify_snapshot(super_block *root, const char *path)
{
	file *fp;
	inotify_node *node;
	if (!notify_ready)
		return NULL;
	/* Preserve open-inode and parent state before namespace mutations. */
	capture_open_files();
	fp = vfs_open(root, path, O_PATH | O_NOFOLLOW);
	if (!fp)
		return NULL;
	node = node_from_file(root, path, fp);
	fs_put_file(fp);
	return node;
}

void inotify_snapshot_put(inotify_node *node)
{
	if (!node)
		return;
	key_put(&node->key);
	key_put(&node->parent);
	free(node->name);
	free(node);
}

static notify_mark *mark_find(const notify_key *key)
{
	struct rb_node *cursor = marks.rb_node;
	while (cursor) {
		notify_mark *mark = rb_entry(cursor, notify_mark, tree);
		int order = key_compare(key, &mark->key);
		if (!order)
			return mark;
		cursor = order < 0 ? cursor->rb_left : cursor->rb_right;
	}
	return NULL;
}

static notify_mark *mark_create(const notify_key *key)
{
	struct rb_node **link = &marks.rb_node, *parent = NULL;
	notify_mark *mark = zalloc(sizeof(*mark));
	if (!mark)
		return NULL;
	if (key_copy(&mark->key, key)) {
		free(mark);
		return NULL;
	}
	while (*link) {
		notify_mark *other = rb_entry(*link, notify_mark, tree);
		parent = *link;
		link = key_compare(key, &other->key) < 0 ? &parent->rb_left :
							   &parent->rb_right;
	}
	list_init(&mark->watches);
	rb_init_node(&mark->tree);
	rb_link_node(&mark->tree, parent, link);
	rb_insert_color(&mark->tree, &marks);
	return mark;
}

static void mark_put_empty(notify_mark *mark)
{
	if (!list_is_empty(&mark->watches))
		return;
	rb_erase(&mark->tree, &marks);
	key_put(&mark->key);
	free(mark);
}

static notify_user *user_get(unsigned uid)
{
	struct rb_node **link = &users.rb_node, *parent = NULL;
	notify_user *user;
	while (*link) {
		user = rb_entry(*link, notify_user, tree);
		if (user->uid == uid)
			return user;
		parent = *link;
		link = uid < user->uid ? &parent->rb_left : &parent->rb_right;
	}
	user = zalloc(sizeof(*user));
	if (!user)
		return NULL;
	user->uid = uid;
	rb_init_node(&user->tree);
	rb_link_node(&user->tree, parent, link);
	rb_insert_color(&user->tree, &users);
	return user;
}

static void user_put_empty(notify_user *user)
{
	if (user->instances || user->watches)
		return;
	rb_erase(&user->tree, &users);
	free(user);
}

static notify_watch *watch_find(notify_instance *instance, int wd)
{
	struct rb_node *cursor = instance->watches.rb_node;
	while (cursor) {
		notify_watch *watch = rb_entry(cursor, notify_watch, tree);
		if (wd == watch->wd)
			return watch;
		cursor = wd < watch->wd ? cursor->rb_left : cursor->rb_right;
	}
	return NULL;
}

static void watch_insert(notify_watch *watch)
{
	struct rb_root *root = &watch->instance->watches;
	struct rb_node **link = &root->rb_node, *parent = NULL;
	while (*link) {
		notify_watch *other = rb_entry(*link, notify_watch, tree);
		parent = *link;
		link = watch->wd < other->wd ? &parent->rb_left :
					       &parent->rb_right;
	}
	rb_init_node(&watch->tree);
	rb_link_node(&watch->tree, parent, link);
	rb_insert_color(&watch->tree, root);
	list_insert_tail(&watch->mark->watches, &watch->mark_node);
	list_insert_tail(&watch->instance->watch_list, &watch->instance_node);
	watch->instance->user->watches++;
	__atomic_add_fetch(&watch_count, 1, __ATOMIC_RELEASE);
}

static file *instance_file(int fd)
{
	file *fp = NULL;
	if (fd < 0 || fd >= MAX_FD)
		return NULL;
	mutex_lock(&current->files->lock);
	fp = current->fds[fd];
	if (fp)
		fs_get_file(fp);
	mutex_unlock(&current->files->lock);
	return fp;
}

static unsigned event_size(const notify_event *event)
{
	return sizeof(event->event) + event->event.len;
}

static void event_free(notify_instance *instance, notify_event *event)
{
	if (event != &instance->overflow)
		free(event);
}

/* Caller holds the instance's queue lock. */
static void overflow_queue(notify_instance *instance)
{
	if (instance->overflow_queued)
		return;
	list_insert_tail(&instance->events, &instance->overflow.node);
	instance->overflow_queued = 1;
	instance->queued++;
	instance->bytes += sizeof(struct inotify_event);
}

static void event_queue(notify_instance *instance, int wd, unsigned mask,
			unsigned cookie, const char *name)
{
	unsigned name_len = name && *name ? (strlen(name) + 1 + 15) & ~15u : 0;
	notify_event *event = NULL, *last;
	int irq, owner = 0, signal = SIGIO;
	if (instance->max_events)
		event = zalloc(sizeof(*event) + name_len);
	if (event) {
		event->event.wd = wd;
		event->event.mask = mask;
		event->event.cookie = cookie;
		event->event.len = name_len;
		if (name_len) {
			event->name = (char *)(event + 1);
			strcpy(event->name, name);
		}
	}
	spinlock_lock(&instance->queue_lock, &irq);
	if (!list_is_empty(&instance->events)) {
		last = container_of(instance->events.prev, notify_event, node);
		if (last->event.wd == wd && last->event.mask == mask &&
		    last->event.cookie == cookie &&
		    last->event.len == name_len &&
		    (!name_len || !strcmp(last->name, name))) {
			spinlock_unlock(&instance->queue_lock, irq);
			free(event);
			return;
		}
	}
	if (!event || instance->queued >= instance->max_events) {
		overflow_queue(instance);
	} else {
		list_insert_tail(&instance->events, &event->node);
		instance->queued++;
		instance->bytes += event_size(event);
		event = NULL;
	}
	poll_notify(&instance->waiters);
	if ((instance->fp->f_flag & FASYNC) && instance->fp->f_owner) {
		owner = instance->fp->f_owner;
		if (instance->fp->f_sigio > 0)
			signal = instance->fp->f_sigio;
	}
	spinlock_unlock(&instance->queue_lock, irq);
	free(event);
	if (owner)
		ps_send_signal_owner(owner, signal);
}

static void watch_remove(notify_watch *watch, int ignored, int release_mark)
{
	notify_mark *mark = watch->mark;
	notify_instance *instance = watch->instance;
	if (ignored)
		event_queue(instance, watch->wd, IN_IGNORED, 0, NULL);
	rb_erase(&watch->tree, &instance->watches);
	list_remove_entry(&watch->mark_node);
	list_remove_entry(&watch->instance_node);
	instance->user->watches--;
	__atomic_sub_fetch(&watch_count, 1, __ATOMIC_RELEASE);
	free(watch);
	if (release_mark)
		mark_put_empty(mark);
}

/* Dispatch to marks indexed by filesystem and inode identity. */
static void key_event(const notify_key *key, unsigned mask, unsigned cookie,
		      const char *name, int unlinked)
{
	notify_mark *mark;
	list_entry *cursor;
	if (!key->sb || !(mark = mark_find(key)))
		return;
	if (mask & (IN_DELETE_SELF | IN_MOVE_SELF))
		mask &= ~IN_ISDIR;
	cursor = mark->watches.next;
	while (cursor != &mark->watches) {
		notify_watch *watch =
			container_of(cursor, notify_watch, mark_node);
		cursor = cursor->next;
		if (!(mask & watch->mask & IN_ALL_EVENTS) &&
		    !(mask & IN_UNMOUNT))
			continue;
		if (unlinked && name && (watch->mask & IN_EXCL_UNLINK))
			continue;
		event_queue(watch->instance, watch->wd, mask, cookie, name);
		if (watch->mask & IN_ONESHOT)
			watch_remove(watch, 1, 0);
	}
	mark_put_empty(mark);
}

static void key_destroy(const notify_key *key)
{
	notify_mark *mark = key->sb ? mark_find(key) : NULL;
	if (!mark)
		return;
	while (!list_is_empty(&mark->watches)) {
		notify_watch *watch = container_of(mark->watches.next,
						   notify_watch, mark_node);
		watch_remove(watch, 1, 0);
	}
	mark_put_empty(mark);
}

int sys_inotify_add_watch(int fd, const char *path, unsigned mask)
{
	const unsigned valid = IN_ALL_EVENTS | IN_UNMOUNT | IN_Q_OVERFLOW |
			       IN_IGNORED | IN_ONLYDIR | IN_DONT_FOLLOW |
			       IN_EXCL_UNLINK | IN_MASK_CREATE | IN_MASK_ADD |
			       IN_ISDIR | IN_ONESHOT;
	file *fp, *target;
	notify_instance *instance;
	notify_mark *mark;
	notify_watch *watch = NULL;
	notify_key key = { 0 };
	char *name = NULL, *input = NULL;
	struct stat st;
	list_entry *cursor;
	int result;
	if (!mask || (mask & ~valid) ||
	    ((mask & IN_MASK_ADD) && (mask & IN_MASK_CREATE)))
		return -EINVAL;
	fp = instance_file(fd);
	if (!fp)
		return -EBADF;
	if (fp->f_fop != &inotify_fops) {
		result = -EINVAL;
		goto out;
	}
	if (!path) {
		result = -EFAULT;
		goto out;
	}
	name = name_get();
	input = name_get();
	if (!name || !input) {
		result = -ENOMEM;
		goto out;
	}
	for (unsigned length = 0;; length++) {
		if (length == MAX_PATH) {
			result = -ENAMETOOLONG;
			goto out;
		}
		if (ps_read_process_memory(current, path + length,
					   input + length, 1) < 0) {
			result = -EFAULT;
			goto out;
		}
		if (!input[length])
			break;
	}
	if (!*input) {
		result = -ENOENT;
		goto out;
	}
	if (resolve_path(input, name)) {
		result = -ENAMETOOLONG;
		goto out;
	}
	target = vfs_open(current->root, name,
			  O_PATH | ((mask & IN_DONT_FOLLOW) ? O_NOFOLLOW : 0));
	if (!target) {
		result = -ENOENT;
		goto out;
	}
	if (!target->f_fop || !target->f_fop->getattr ||
	    target->f_fop->getattr(target, &st))
		result = -EACCES;
	else if ((mask & IN_ONLYDIR) && !S_ISDIR(st.st_mode))
		result = -ENOTDIR;
	else
		result = fs_check_perm(&st, 4);
	if (!result)
		result = key_from_file(&key, current->root, name, target);
	fs_put_file(target);
	if (result)
		goto out;
	instance = fp->f_inode->i_private;
	/* Existing descriptions participate in watches added after their open. */
	capture_open_files();
	mutex_lock(&notify_lock);
	mark = mark_find(&key);
	if (mark) {
		for (cursor = mark->watches.next; cursor != &mark->watches;
		     cursor = cursor->next) {
			notify_watch *existing =
				container_of(cursor, notify_watch, mark_node);
			if (existing->instance == instance) {
				watch = existing;
				break;
			}
		}
	}
	if (watch) {
		if (mask & IN_MASK_CREATE)
			result = -EEXIST;
		else {
			unsigned events = mask & (IN_ALL_EVENTS | IN_ONESHOT |
						  IN_EXCL_UNLINK);
			watch->mask = (mask & IN_MASK_ADD) ?
					      watch->mask | events :
					      events;
			result = watch->wd;
		}
		goto unlock;
	}
	if (instance->user->watches >= limits[INOTIFY_MAX_USER_WATCHES]) {
		result = -ENOSPC;
		goto unlock;
	}
	watch = zalloc(sizeof(*watch));
	if (!watch) {
		result = -ENOMEM;
		goto unlock;
	}
	if (!mark)
		mark = mark_create(&key);
	if (!mark) {
		free(watch);
		result = -ENOMEM;
		goto unlock;
	}
	do {
		instance->next_wd = instance->next_wd == 0x7fffffffu ?
					    1 :
					    instance->next_wd + 1;
	} while (watch_find(instance, instance->next_wd));
	watch->wd = instance->next_wd;
	watch->mask = mask & (IN_ALL_EVENTS | IN_ONESHOT | IN_EXCL_UNLINK);
	watch->mark = mark;
	watch->instance = instance;
	watch_insert(watch);
	result = watch->wd;
unlock:
	mutex_unlock(&notify_lock);
out:
	key_put(&key);
	if (name)
		name_put(name);
	if (input)
		name_put(input);
	fs_put_file(fp);
	return result;
}

int sys_inotify_rm_watch(int fd, int wd)
{
	file *fp = instance_file(fd);
	notify_watch *watch;
	int result = -EINVAL;
	if (!fp)
		return -EBADF;
	if (fp->f_fop == &inotify_fops) {
		mutex_lock(&notify_lock);
		watch = watch_find(fp->f_inode->i_private, wd);
		if (watch) {
			watch_remove(watch, 1, 1);
			result = 0;
		}
		mutex_unlock(&notify_lock);
	}
	fs_put_file(fp);
	return result;
}

typedef struct {
	notify_instance *instance;
	notify_event *event;
	size_t capacity;
	poll_table table;
	poll_table_entry entry;
} notify_read;

static int read_dequeue(void *opaque)
{
	notify_read *read = opaque;
	notify_instance *instance = read->instance;
	notify_event *event;
	unsigned size;
	int irq;
	spinlock_lock(&instance->queue_lock, &irq);
	if (list_is_empty(&instance->events)) {
		spinlock_unlock(&instance->queue_lock, irq);
		return 0;
	}
	event = container_of(instance->events.next, notify_event, node);
	size = event_size(event);
	if (size > read->capacity) {
		spinlock_unlock(&instance->queue_lock, irq);
		return -EINVAL;
	}
	list_remove_entry(&event->node);
	instance->queued--;
	instance->bytes -= size;
	if (event == &instance->overflow)
		instance->overflow_queued = 0;
	read->event = event;
	spinlock_unlock(&instance->queue_lock, irq);
	return size;
}

static void read_event_free(notify_read *read)
{
	notify_event *event = read->event;
	read->event = NULL;
	if (event)
		event_free(read->instance, event);
}

static void read_cancel(void *opaque)
{
	notify_read *read = opaque;
	poll_table_cleanup(&read->table);
	read_event_free(read);
}

static int read_register(void *opaque)
{
	notify_read *read = opaque;
	poll_subscribe(&read->table, &read->instance->waiters,
		       &read->instance->queue_lock);
	current->io_wait = read;
	current->cancel_io_wait = read_cancel;
	return read->table.unsupported;
}

static void read_unregister(void *opaque)
{
	notify_read *read = opaque;
	poll_table_cleanup(&read->table);
	current->io_wait = read;
	current->cancel_io_wait = read_cancel;
}

static const struct poll_ops read_wait_ops = {
	.check = read_dequeue,
	.reg = read_register,
	.dereg = read_unregister,
};

static ssize_t inotify_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	notify_read read = { .instance = fp->f_inode->i_private };
	size_t copied = 0;
	int result;
	/* The VFS transfer scope retains fp through waits and userspace copies. */
	current->io_wait = &read;
	current->cancel_io_wait = read_cancel;
	for (;;) {
		read.capacity = count - copied;
		read.event = NULL;
		poll_table_init(&read.table, current, &read.entry, 1);
		result = poll_wait_loop(&read_wait_ops, &read,
					copied || (fp->f_flag & O_NONBLOCK), 1,
					0);
		if (result <= 0) {
			result = copied ? (int)copied :
				 result ? result :
					  -EAGAIN;
			break;
		}
		if (ps_write_process_memory(current, (char *)buf + copied,
					    &read.event->event,
					    sizeof(read.event->event)) < 0 ||
		    (read.event->event.len &&
		     ps_write_process_memory(
			     current,
			     (char *)buf + copied + sizeof(read.event->event),
			     read.event->name, read.event->event.len) < 0)) {
			read_event_free(&read);
			result = -EFAULT;
			break;
		}
		read_event_free(&read);
		copied += result;
	}
	if (current->io_wait == &read) {
		current->io_wait = NULL;
		current->cancel_io_wait = NULL;
	}
	return result;
}

/* The legacy read callback consumes complete records within each iovec. */
static ssize_t inotify_readv(file *fp, const struct iovec *iov, int count)
{
	ssize_t total = 0;
	for (int index = 0; index < count; index++) {
		ssize_t result;
		if (!iov[index].iov_len)
			continue;
		result = inotify_read(fp, iov[index].iov_base,
				      iov[index].iov_len, &fp->f_pos);
		if (result < 0)
			return total ? total : result;
		total += result;
		if ((size_t)result != iov[index].iov_len)
			break;
	}
	return total;
}

static unsigned inotify_poll(file *fp, unsigned events, poll_table *pt)
{
	notify_instance *instance = fp->f_inode->i_private;
	unsigned ready;
	int irq;
	if (pt)
		poll_subscribe(pt, &instance->waiters, &instance->queue_lock);
	spinlock_lock(&instance->queue_lock, &irq);
	ready = !list_is_empty(&instance->events) ? events & FS_POLL_READ : 0;
	spinlock_unlock(&instance->queue_lock, irq);
	return ready;
}

static int inotify_ioctl(file *fp, unsigned cmd, void *arg)
{
	notify_instance *instance = fp->f_inode->i_private;
	int irq, bytes;
	if (cmd != FIONREAD)
		return -ENOTTY;
	spinlock_lock(&instance->queue_lock, &irq);
	bytes = instance->bytes > 0x7fffffffu ? 0x7fffffff :
						(int)instance->bytes;
	spinlock_unlock(&instance->queue_lock, irq);
	return ps_write_process_memory(current, arg, &bytes, sizeof(bytes)) <
			       0 ?
		       -EFAULT :
		       0;
}

static loff_t inotify_llseek(file *fp, loff_t offset, int whence)
{
	return fp->f_pos;
}

static int inotify_getattr(file *fp, struct stat *st)
{
	notify_instance *instance = fp->f_inode->i_private;
	memset(st, 0, sizeof(*st));
	st->st_mode = 0600;
	st->st_uid = instance->uid;
	st->st_gid = instance->gid;
	st->st_nlink = 1;
	return 0;
}

static int inotify_release(file *fp)
{
	notify_instance *instance = fp->f_inode->i_private;
	mutex_lock(&notify_lock);
	while (!list_is_empty(&instance->watch_list)) {
		notify_watch *watch = container_of(instance->watch_list.next,
						   notify_watch, instance_node);
		watch_remove(watch, 0, 1);
	}
	instance->user->instances--;
	list_remove_entry(&instance->instance_node);
	user_put_empty(instance->user);
	mutex_unlock(&notify_lock);
	while (!list_is_empty(&instance->events)) {
		notify_event *event =
			container_of(list_remove_head(&instance->events),
				     notify_event, node);
		event_free(instance, event);
	}
	free(instance);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations inotify_fops = {
	.read = inotify_read,
	.readv = inotify_readv,
	.poll = inotify_poll,
	.ioctl = inotify_ioctl,
	.llseek = inotify_llseek,
	.getattr = inotify_getattr,
	.release = inotify_release,
};

int sys_inotify_init1(int flags)
{
	notify_instance *instance;
	file *fp;
	inode *node;
	int fd, result;
	if (flags & ~(O_NONBLOCK | O_CLOEXEC))
		return -EINVAL;
	instance = zalloc(sizeof(*instance));
	fp = zalloc(sizeof(*fp));
	node = zalloc(sizeof(*node));
	if (!instance || !fp || !node) {
		result = -ENOMEM;
		goto fail;
	}
	mutex_lock(&notify_lock);
	instance->user = user_get(current->user->uid);
	if (!instance->user) {
		result = -ENOMEM;
		goto unlock_fail;
	}
	if (instance->user->instances >= limits[INOTIFY_MAX_USER_INSTANCES]) {
		user_put_empty(instance->user);
		result = -EMFILE;
		goto unlock_fail;
	}
	instance->user->instances++;
	instance->uid = current->user->euid;
	instance->gid = current->user->egid;
	instance->max_events = limits[INOTIFY_MAX_QUEUED_EVENTS];
	instance->fp = fp;
	list_init(&instance->watch_list);
	list_init(&instance->events);
	list_init(&instance->waiters);
	spinlock_init(&instance->queue_lock);
	instance->overflow.event.wd = -1;
	instance->overflow.event.mask = IN_Q_OVERFLOW;
	node->i_private = instance;
	node->i_mode = 0600;
	fp->f_inode = node;
	fp->f_fop = &inotify_fops;
	fp->f_count = 1;
	fp->f_mode = O_RDONLY;
	fp->f_flag = flags & O_NONBLOCK;
	list_insert_tail(&instances, &instance->instance_node);
	mutex_unlock(&notify_lock);
	fd = fs_install_fd(fp, flags);
	if (fd < 0)
		fs_put_file(fp);
	return fd < 0 ? -EMFILE : fd;
unlock_fail:
	mutex_unlock(&notify_lock);
fail:
	free(instance);
	free(fp);
	free(node);
	return result;
}

int sys_inotify_init(void)
{
	return sys_inotify_init1(0);
}

/* Metadata allocation loss is reported instead of inventing event identity. */
static void lost_event_locked(void)
{
	list_entry *cursor;
	for (cursor = instances.next; cursor != &instances;
	     cursor = cursor->next) {
		notify_instance *instance =
			container_of(cursor, notify_instance, instance_node);
		int irq, owner = 0, signal = SIGIO;
		spinlock_lock(&instance->queue_lock, &irq);
		overflow_queue(instance);
		poll_notify(&instance->waiters);
		if ((instance->fp->f_flag & FASYNC) && instance->fp->f_owner) {
			owner = instance->fp->f_owner;
			if (instance->fp->f_sigio > 0)
				signal = instance->fp->f_sigio;
		}
		spinlock_unlock(&instance->queue_lock, irq);
		if (owner)
			ps_send_signal_owner(owner, signal);
	}
}

static void lost_event(void)
{
	if (!notify_ready)
		return;
	mutex_lock(&notify_lock);
	lost_event_locked();
	mutex_unlock(&notify_lock);
}

static unsigned node_directory_flag(const inotify_node *node)
{
	return S_ISDIR(node->mode) ? IN_ISDIR : 0;
}

static void node_event(inotify_node *node, unsigned mask)
{
	unsigned directory = node_directory_flag(node);
	key_event(&node->key, mask | directory, 0, NULL, 0);
	if (node->parent.sb && node->name[0])
		key_event(&node->parent, mask | directory, 0, node->name,
			  node->unlinked);
	if (node->parent_lost)
		lost_event_locked();
}

static void capture_file(file *fp, int opened)
{
	inotify_node *node;
	if (fp->f_notify || !fp->f_notify_root)
		return;
	node = node_from_file(fp->f_notify_root, fp->f_name, fp);
	if (!node) {
		lost_event();
		return;
	}
	mutex_lock(&notify_lock);
	if (!fp->f_notify) {
		fp->f_notify = node;
		list_insert_tail(&open_files, &node->open_node);
		if (opened && !(fp->f_flag & O_PATH))
			node_event(node, IN_OPEN);
		node = NULL;
	}
	mutex_unlock(&notify_lock);
	inotify_snapshot_put(node);
}

/* Cache dormant descriptions before a watch or namespace mutation needs them. */
static int file_try_get(file *fp)
{
	unsigned count = __atomic_load_n(&fp->f_count, __ATOMIC_RELAXED);
	while (count) {
		if (__atomic_compare_exchange_n(&fp->f_count, &count, count + 1,
						0, __ATOMIC_ACQUIRE,
						__ATOMIC_RELAXED))
			return 1;
	}
	return 0;
}

static void capture_open_files(void)
{
	list_entry *cursor;
	unsigned epoch;
	if (!notify_ready)
		return;
	mutex_lock(&capture_lock);
	epoch = ++capture_epoch;
	for (;;) {
		file *fp = NULL;
		mutex_lock(&notify_lock);
		for (cursor = tracked_files.next; cursor != &tracked_files;
		     cursor = cursor->next) {
			file *candidate =
				container_of(cursor, file, f_notify_node);
			if (!candidate->f_notify &&
			    candidate->f_notify_epoch != epoch &&
			    file_try_get(candidate)) {
				candidate->f_notify_epoch = epoch;
				fp = candidate;
				break;
			}
		}
		mutex_unlock(&notify_lock);
		if (!fp)
			break;
		capture_file(fp, 0);
		fs_put_file(fp);
	}
	mutex_unlock(&capture_lock);
}

void inotify_file_open(file *fp, super_block *root)
{
	int watched;
	if (!notify_ready || !root || fp->f_notify_root || !fp->f_name ||
	    fp->f_name[0] != '/')
		return;
	mutex_lock(&notify_lock);
	fp->f_notify_root = root;
	sb_get(root);
	list_insert_tail(&tracked_files, &fp->f_notify_node);
	watched = active_watches() != 0;
	mutex_unlock(&notify_lock);
	if (watched)
		capture_file(fp, 1);
}

void inotify_file_event(file *fp, unsigned mask)
{
	inotify_node *node;
	if (!active_watches() || (fp->f_flag & O_PATH))
		return;
	if (!fp->f_notify)
		capture_file(fp, 0);
	node = fp->f_notify;
	if (!node)
		return;
	mutex_lock(&notify_lock);
	node_event(node, mask);
	mutex_unlock(&notify_lock);
}

void inotify_file_close(file *fp)
{
	inotify_node *node;
	super_block *root = fp->f_notify_root;
	list_entry *cursor;
	int retained = 0;
	if (!root)
		return;
	if (active_watches() && !fp->f_notify)
		capture_file(fp, 0);
	mutex_lock(&notify_lock);
	list_remove_entry(&fp->f_notify_node);
	fp->f_notify_root = NULL;
	node = fp->f_notify;
	if (!node)
		goto out;
	if (!(fp->f_flag & O_PATH))
		node_event(node, fp->f_mode == O_RDONLY ? IN_CLOSE_NOWRITE :
							  IN_CLOSE_WRITE);
	list_remove_entry(&node->open_node);
	fp->f_notify = NULL;
	if (!node->links) {
		for (cursor = open_files.next; cursor != &open_files;
		     cursor = cursor->next) {
			inotify_node *other =
				container_of(cursor, inotify_node, open_node);
			if (!key_compare(&other->key, &node->key)) {
				retained = 1;
				break;
			}
		}
		if (!retained) {
			key_event(&node->key, IN_DELETE_SELF, 0, NULL, 0);
			key_destroy(&node->key);
		}
	}
out:
	mutex_unlock(&notify_lock);
	inotify_snapshot_put(node);
	sb_put(root);
}

void inotify_created(super_block *root, const char *path)
{
	inotify_node *node;
	if (!notify_ready || !active_watches())
		return;
	node = inotify_snapshot(root, path);
	if (!node) {
		lost_event();
		return;
	}
	mutex_lock(&notify_lock);
	if (node->parent.sb && node->name[0])
		key_event(&node->parent, IN_CREATE | node_directory_flag(node),
			  0, node->name, 0);
	if (node->parent_lost)
		lost_event_locked();
	mutex_unlock(&notify_lock);
	inotify_snapshot_put(node);
}

static void inode_unlinked(inotify_node *node)
{
	list_entry *cursor;
	int retained = 0;
	if (!S_ISDIR(node->mode))
		key_event(&node->key, IN_ATTRIB, 0, NULL, 0);
	if (S_ISDIR(node->mode) || node->links <= 1) {
		for (cursor = open_files.next; cursor != &open_files;
		     cursor = cursor->next) {
			inotify_node *opened =
				container_of(cursor, inotify_node, open_node);
			if (!key_compare(&opened->key, &node->key)) {
				opened->links = 0;
				retained = 1;
			}
		}
		if (!retained) {
			key_event(&node->key, IN_DELETE_SELF, 0, NULL, 0);
			key_destroy(&node->key);
		}
	}
}

void inotify_removed(inotify_node *node)
{
	list_entry *cursor;
	if (!node) {
		lost_event();
		return;
	}
	mutex_lock(&notify_lock);
	if (node->parent.sb && node->name[0])
		key_event(&node->parent, IN_DELETE | node_directory_flag(node),
			  0, node->name, 0);
	if (node->parent_lost)
		lost_event_locked();
	inode_unlinked(node);
	for (cursor = open_files.next; cursor != &open_files;
	     cursor = cursor->next) {
		inotify_node *opened =
			container_of(cursor, inotify_node, open_node);
		if (!key_compare(&opened->key, &node->key) &&
		    !key_compare(&opened->parent, &node->parent) &&
		    !strcmp(opened->name, node->name))
			opened->unlinked = 1;
	}
	mutex_unlock(&notify_lock);
}

void inotify_linked(inotify_node *oldnode, super_block *root, const char *path)
{
	if (oldnode) {
		mutex_lock(&notify_lock);
		key_event(&oldnode->key, IN_ATTRIB, 0, NULL, 0);
		mutex_unlock(&notify_lock);
	}
	inotify_created(root, path);
}

void inotify_renamed(inotify_node *source, inotify_node *replacement,
		     super_block *root, const char *newpath)
{
	inotify_node *dest;
	list_entry *cursor;
	int lost = 0;
	unsigned cookie;
	if (!source) {
		lost_event();
		return;
	}
	if (replacement && !key_compare(&source->key, &replacement->key))
		return;
	dest = inotify_snapshot(root, newpath);
	if (!dest) {
		mutex_lock(&notify_lock);
		for (cursor = open_files.next; cursor != &open_files;
		     cursor = cursor->next) {
			inotify_node *opened =
				container_of(cursor, inotify_node, open_node);
			if (!key_compare(&opened->key, &source->key) &&
			    !key_compare(&opened->parent, &source->parent) &&
			    !strcmp(opened->name, source->name))
				key_put(&opened->parent);
		}
		mutex_unlock(&notify_lock);
		lost_event();
		return;
	}
	mutex_lock(&notify_lock);
	if (!++rename_cookie)
		++rename_cookie;
	cookie = rename_cookie;
	if (source->parent.sb)
		key_event(&source->parent,
			  IN_MOVED_FROM | node_directory_flag(source), cookie,
			  source->name, 0);
	if (dest->parent.sb)
		key_event(&dest->parent,
			  IN_MOVED_TO | node_directory_flag(source), cookie,
			  dest->name, 0);
	if (source->parent_lost || dest->parent_lost)
		lost_event_locked();
	if (replacement)
		inode_unlinked(replacement);
	key_event(&source->key, IN_MOVE_SELF, 0, NULL, 0);
	for (cursor = open_files.next; cursor != &open_files;
	     cursor = cursor->next) {
		inotify_node *opened =
			container_of(cursor, inotify_node, open_node);
		if (replacement &&
		    !key_compare(&opened->key, &replacement->key) &&
		    !key_compare(&opened->parent, &replacement->parent) &&
		    !strcmp(opened->name, replacement->name))
			opened->unlinked = 1;
		if (!key_compare(&opened->key, &source->key) &&
		    !key_compare(&opened->parent, &source->parent) &&
		    !strcmp(opened->name, source->name)) {
			notify_key parent;
			char *name = strdup(dest->name);
			if (!name || key_copy(&parent, &dest->parent)) {
				free(name);
				key_put(&opened->parent);
				lost = 1;
				continue;
			}
			key_put(&opened->parent);
			free(opened->name);
			opened->parent = parent;
			opened->name = name;
		}
	}
	mutex_unlock(&notify_lock);
	inotify_snapshot_put(dest);
	if (lost)
		lost_event();
}

void inotify_path_event(super_block *root, const char *path, unsigned mask)
{
	inotify_node *node;
	file *fp;
	if (!notify_ready || !active_watches())
		return;
	fp = vfs_open(root, path, O_PATH);
	node = fp ? node_from_file(root, path, fp) : NULL;
	if (fp)
		fs_put_file(fp);
	if (!node) {
		lost_event();
		return;
	}
	mutex_lock(&notify_lock);
	node_event(node, mask);
	mutex_unlock(&notify_lock);
	inotify_snapshot_put(node);
}

static int sb_contains(super_block *root, super_block *target)
{
	struct rb_node *cursor;
	int found = root == target;
	if (found)
		return 1;
	mutex_lock(&root->s_lock);
	for (cursor = rb_first(&root->s_mounts); cursor && !found;
	     cursor = rb_next(cursor)) {
		vfs_mount_node *mount =
			rb_entry(cursor, vfs_mount_node, rb_node);
		found = sb_contains(mount->sb, target);
	}
	mutex_unlock(&root->s_lock);
	return found;
}

void inotify_unmounted(super_block *sb)
{
	struct rb_node *cursor;
	if (!notify_ready)
		return;
	mutex_lock(&notify_lock);
	cursor = rb_first(&marks);
	while (cursor) {
		notify_mark *mark = rb_entry(cursor, notify_mark, tree);
		cursor = rb_next(cursor);
		if (sb_contains(sb, mark->key.sb)) {
			while (!list_is_empty(&mark->watches)) {
				notify_watch *watch =
					container_of(mark->watches.next,
						     notify_watch, mark_node);
				event_queue(watch->instance, watch->wd,
					    IN_UNMOUNT, 0, NULL);
				watch_remove(watch, 1, 0);
			}
			mark_put_empty(mark);
		}
	}
	mutex_unlock(&notify_lock);
}
