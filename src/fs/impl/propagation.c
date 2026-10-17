#include <fs/vfs.h>
#include <fs/fcntl.h>
#include <lib/klib.h>
#include <lib/list.h>
#include <lib/lock.h>
#include <macro.h>
#include <errno.h>

#define PROP_UNBINDABLE (1U << 17)
#define PROP_PRIVATE (1U << 18)
#define PROP_SLAVE (1U << 19)
#define PROP_SHARED (1U << 20)
#define PROP_RECURSIVE (1U << 14)
struct namespace_root {
	list_entry link;
	void *token;
	super_block *root;
};
static list_entry roots;
static rmutex_t propagation_lock;
static unsigned next_peer;
static int initialized;
static void propagation_init(void)
{
	list_init(&roots);
	rmutex_init(&propagation_lock);
	initialized = 1;
}
KERNEL_INIT(1, propagation_init);

void vfs_namespace_track(void *token, super_block *root)
{
	if (!initialized)
		return;
	LOCK_GUARD(&propagation_lock);
	for (list_entry *p = roots.next; p != &roots; p = p->next) {
		struct namespace_root *entry =
			container_of(p, struct namespace_root, link);
		if (entry->token == token) {
			entry->root = root;
			return;
		}
	}
	struct namespace_root *entry = malloc(sizeof(*entry));
	if (!entry)
		return;
	entry->token = token;
	entry->root = root;
	list_insert_tail(&roots, &entry->link);
}
void vfs_namespace_untrack(void *token)
{
	if (!initialized)
		return;
	LOCK_GUARD(&propagation_lock);
	for (list_entry *p = roots.next; p != &roots; p = p->next) {
		struct namespace_root *entry =
			container_of(p, struct namespace_root, link);
		if (entry->token == token) {
			list_remove_entry(p);
			free(entry);
			return;
		}
	}
}
struct owner_search {
	const char *path;
	super_block *owner;
	size_t length;
};
static void find_owner(const super_block *mount, void *opaque)
{
	struct owner_search *s = opaque;
	size_t n = strlen(mount->s_mountpoint);
	if (n >= s->length && n && !strncmp(s->path, mount->s_mountpoint, n) &&
	    (n == 1 || !s->path[n] || s->path[n] == '/')) {
		s->owner = (super_block *)mount;
		s->length = n;
	}
}
static super_block *owner_of(super_block *root, const char *path)
{
	struct owner_search s = { .path = path };
	vfs_mount_walk(root, find_owner, &s);
	return s.owner;
}
static void change_one(super_block *mount, unsigned flags)
{
	if (flags & PROP_SHARED) {
		if (!mount->s_peer_group)
			mount->s_peer_group = ++next_peer;
		mount->s_unbindable = 0;
	} else if (flags & PROP_SLAVE) {
		if (mount->s_peer_group)
			mount->s_master_group = mount->s_peer_group;
		mount->s_peer_group = 0;
		mount->s_unbindable = 0;
	} else {
		mount->s_peer_group = mount->s_master_group = 0;
		mount->s_unbindable = !!(flags & PROP_UNBINDABLE);
	}
}
struct change_context {
	const char *prefix;
	unsigned flags;
};
static void change_descendant(const super_block *mount, void *opaque)
{
	struct change_context *c = opaque;
	size_t n = strlen(c->prefix);
	if (!strncmp(mount->s_mountpoint, c->prefix, n) &&
	    (n == 1 || !mount->s_mountpoint[n] ||
	     mount->s_mountpoint[n] == '/'))
		change_one((super_block *)mount, c->flags);
}
int vfs_set_propagation(super_block *root, const char *target, unsigned flags)
{
	unsigned type = flags & (PROP_PRIVATE | PROP_SHARED | PROP_SLAVE |
				 PROP_UNBINDABLE);
	if (!type || (type & (type - 1)) || flags & ~(type | PROP_RECURSIVE))
		return -EINVAL;
	LOCK_GUARD(&propagation_lock);
	super_block *owner = owner_of(root, target);
	if (!owner || strcmp(owner->s_mountpoint, target))
		return -EINVAL;
	if (flags & PROP_RECURSIVE) {
		struct change_context c = { target, flags };
		vfs_mount_walk(root, change_descendant, &c);
	} else
		change_one(owner, flags);
	vfs_namespace_changed();
	return 0;
}
struct propagation_item {
	list_entry link;
	super_block *root, *mount, *parent;
	char *path;
	int committed;
};
struct propagation_plan {
	list_entry items;
	super_block *source_parent, *source_mount;
	const char *suffix;
	unsigned group, child_group, depth;
	int error;
};
static int already_planned(struct propagation_plan *plan, super_block *root,
			   const char *path)
{
	for (list_entry *p = plan->items.next; p != &plan->items; p = p->next) {
		struct propagation_item *item =
			container_of(p, struct propagation_item, link);
		if (item->root == root && !strcmp(item->path, path))
			return 1;
	}
	return 0;
}
struct receiver_context {
	struct propagation_plan *plan;
	super_block *root;
};
static void plan_receiver(const super_block *receiver_const, void *opaque)
{
	struct receiver_context *c = opaque;
	struct propagation_plan *plan = c->plan;
	super_block *receiver = (super_block *)receiver_const;
	if (plan->error || receiver == plan->source_parent ||
	    (receiver->s_peer_group != plan->group &&
	     receiver->s_master_group != plan->group))
		return;
	char *path = name_get();
	if (!path) {
		plan->error = -ENOMEM;
		return;
	}
	const char *base = !strcmp(receiver->s_mountpoint, "/") ?
				   "" :
				   receiver->s_mountpoint;
	if (strlen(base) + strlen(plan->suffix) >= MAX_PATH) {
		name_put(path);
		plan->error = -ENAMETOOLONG;
		return;
	}
	sprintf(path, "%s%s", base, plan->suffix);
	if (!*path)
		strcpy(path, "/");
	if (already_planned(plan, c->root, path)) {
		name_put(path);
		return;
	}
	struct propagation_item *item = zalloc(sizeof(*item));
	if (!item) {
		name_put(path);
		plan->error = -ENOMEM;
		return;
	}
	item->mount = vfs_clone_mount_tree(plan->source_mount);
	if (!item->mount) {
		name_put(path);
		free(item);
		plan->error = -ENOMEM;
		return;
	}
	item->root = c->root;
	item->parent = receiver;
	item->path = strdup(path);
	name_put(path);
	if (!item->path) {
		sb_put(item->mount);
		free(item);
		plan->error = -ENOMEM;
		return;
	}
	if (receiver->s_peer_group != plan->group) {
		item->mount->s_master_group = plan->child_group;
		item->mount->s_peer_group = 0;
		if (receiver->s_peer_group) {
			for (list_entry *p = plan->items.next;
			     p != &plan->items; p = p->next) {
				struct propagation_item *prior = container_of(
					p, struct propagation_item, link);
				if (prior->parent->s_peer_group ==
					    receiver->s_peer_group &&
				    prior->mount->s_master_group ==
					    plan->child_group)
					item->mount->s_peer_group =
						prior->mount->s_peer_group;
			}
			if (!item->mount->s_peer_group)
				item->mount->s_peer_group = ++next_peer;
		}
	}
	list_insert_tail(&plan->items, &item->link);
}
static void collect_receivers(struct propagation_plan *plan)
{
	for (list_entry *p = roots.next; p != &roots; p = p->next) {
		struct namespace_root *entry =
			container_of(p, struct namespace_root, link);
		struct receiver_context c = { plan, entry->root };
		vfs_mount_walk(entry->root, plan_receiver, &c);
	}
}
int vfs_mount_propagated(super_block *root, const char *target,
			 super_block *mount)
{
	LOCK_GUARD(&propagation_lock);
	super_block *owner = owner_of(root, target);
	if (!owner || !owner->s_peer_group)
		return vfs_mount(root, target, mount);
	struct propagation_plan plan = { .source_parent = owner,
					 .source_mount = mount,
					 .group = owner->s_peer_group };
	list_init(&plan.items);
	if (!mount->s_peer_group)
		mount->s_peer_group = ++next_peer;
	plan.child_group = mount->s_peer_group;
	plan.suffix = !strcmp(owner->s_mountpoint, "/") ?
			      target :
			      target + strlen(owner->s_mountpoint);
	collect_receivers(&plan);
	/* Each slave peer group can itself have downstream slaves. */
	for (list_entry *p = plan.items.next; p != &plan.items && !plan.error;
	     p = p->next) {
		struct propagation_item *item =
			container_of(p, struct propagation_item, link);
		if (!item->parent->s_peer_group ||
		    item->parent->s_peer_group == plan.group)
			continue;
		super_block *saved_parent = plan.source_parent,
			    *saved_mount = plan.source_mount;
		unsigned saved_group = plan.group,
			 saved_child = plan.child_group;
		plan.source_parent = item->parent;
		plan.source_mount = item->mount;
		plan.group = item->parent->s_peer_group;
		plan.child_group = item->mount->s_peer_group;
		collect_receivers(&plan);
		plan.source_parent = saved_parent;
		plan.source_mount = saved_mount;
		plan.group = saved_group;
		plan.child_group = saved_child;
	}
	int result = plan.error;
	if (!result)
		result = vfs_mount(root, target, mount);
	int source_committed = !result;
	for (list_entry *p = plan.items.next; p != &plan.items && !result;
	     p = p->next) {
		struct propagation_item *item =
			container_of(p, struct propagation_item, link);
		result = vfs_mount(item->root, item->path, item->mount);
		if (!result)
			item->committed = 1;
	}
	while (!list_is_empty(&plan.items)) {
		struct propagation_item *item =
			container_of(list_remove_tail(&plan.items),
				     struct propagation_item, link);
		if (result && item->committed)
			vfs_umount(item->root, item->path);
		if (!item->committed)
			sb_put(item->mount);
		free(item->path);
		free(item);
	}
	if (result && source_committed) {
		/* Retain the caller's reference when the transaction is rolled back. */
		sb_get(mount);
		vfs_umount(root, target);
	}
	return result;
}
struct unmount_context {
	super_block *source, *source_parent, *root;
	list_entry *items;
	const char *suffix;
	unsigned group;
	int error;
};
static void plan_unmount(const super_block *parent, void *opaque)
{
	struct unmount_context *c = opaque;
	if (parent == c->source_parent || !c->group ||
	    (parent->s_peer_group != c->group &&
	     parent->s_master_group != c->group))
		return;
	char *path = name_get();
	if (!path) {
		c->error = -ENOMEM;
		return;
	}
	const char *base =
		!strcmp(parent->s_mountpoint, "/") ? "" : parent->s_mountpoint;
	if (strlen(base) + strlen(c->suffix) >= MAX_PATH) {
		name_put(path);
		c->error = -ENAMETOOLONG;
		return;
	}
	sprintf(path, "%s%s", base, c->suffix);
	super_block *mount = owner_of(c->root, path);
	if (!mount || mount == c->source || strcmp(mount->s_mountpoint, path)) {
		name_put(path);
		return;
	}
	for (list_entry *p = c->items->next; p != c->items; p = p->next) {
		struct propagation_item *prior =
			container_of(p, struct propagation_item, link);
		if (prior->root == c->root && !strcmp(prior->path, path)) {
			name_put(path);
			return;
		}
	}
	struct propagation_item *item = zalloc(sizeof(*item));
	if (!item) {
		name_put(path);
		c->error = -ENOMEM;
		return;
	}
	item->path = strdup(path);
	name_put(path);
	if (!item->path) {
		free(item);
		c->error = -ENOMEM;
		return;
	}
	item->root = c->root;
	item->mount = mount;
	item->parent = (super_block *)parent;
	sb_get(item->mount);
	list_insert_tail(c->items, &item->link);
}
static void collect_unmount_receivers(struct unmount_context *c)
{
	for (list_entry *p = roots.next; p != &roots; p = p->next) {
		struct namespace_root *entry =
			container_of(p, struct namespace_root, link);
		c->root = entry->root;
		vfs_mount_walk(entry->root, plan_unmount, c);
	}
}
int vfs_umount_propagated(super_block *root, const char *path)
{
	LOCK_GUARD(&propagation_lock);
	super_block *source = owner_of(root, path);
	if (!source || strcmp(source->s_mountpoint, path))
		return -EINVAL;
	if (!strcmp(path, "/"))
		return vfs_umount(root, path);
	char *parent_path = strdup(path);
	if (!parent_path)
		return -ENOMEM;
	char *slash = strrchr(parent_path, '/');
	if (slash == parent_path)
		slash[1] = 0;
	else if (slash)
		*slash = 0;
	super_block *parent = owner_of(root, parent_path);
	free(parent_path);
	if (!parent || !parent->s_peer_group)
		return vfs_umount(root, path);
	list_entry items;
	list_init(&items);
	/* Unmount events propagate through the parent mount's peer and slave groups. */
	struct unmount_context c = {
		.source = source,
		.source_parent = parent,
		.items = &items,
		.group = parent->s_peer_group,
		.suffix = !strcmp(parent->s_mountpoint, "/") ?
				  path :
				  path + strlen(parent->s_mountpoint)
	};
	collect_unmount_receivers(&c);
	for (list_entry *p = items.next; p != &items && !c.error; p = p->next) {
		struct propagation_item *item =
			container_of(p, struct propagation_item, link);
		if (!item->parent->s_peer_group ||
		    item->parent->s_peer_group == parent->s_peer_group)
			continue;
		c.source_parent = item->parent;
		c.group = item->parent->s_peer_group;
		collect_unmount_receivers(&c);
	}
	int result = c.error ? c.error : vfs_umount(root, path);
	while (!list_is_empty(&items)) {
		struct propagation_item *item =
			container_of(list_remove_tail(&items),
				     struct propagation_item, link);
		if (!result)
			vfs_umount(item->root, item->path);
		sb_put(item->mount);
		free(item->path);
		free(item);
	}
	return result;
}
