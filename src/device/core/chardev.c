#include <device/chardev.h>
#include <lib/klib.h>
#include <errno.h>

#define MAX_ENDPOINTS 256

typedef struct char_endpoint_entry {
	unsigned mode_type; /* S_IFCHR or S_IFBLK */
	unsigned major;
	unsigned minor_base;
	unsigned minor_count;
	const char *name;
	file *(*open)(super_block *sb, unsigned rdev, int flag);
	struct rb_node major_node;
	list_entry major_ranges;
} char_endpoint_entry;

static char_endpoint_entry char_endpoint_table[MAX_ENDPOINTS];
static int char_endpoint_count;
static struct rb_root char_endpoint_majors = _RBTREE_ROOT_INIT;

static int char_endpoint_compare(unsigned mode, unsigned major,
				 const char_endpoint_entry *entry)
{
	if (mode != entry->mode_type)
		return mode < entry->mode_type ? -1 : 1;
	return major < entry->major ? -1 : major != entry->major;
}

static char_endpoint_entry *char_endpoint_find_major(unsigned mode,
						     unsigned major)
{
	struct rb_node *node = char_endpoint_majors.rb_node;
	while (node) {
		char_endpoint_entry *entry =
			rb_entry(node, char_endpoint_entry, major_node);
		int order = char_endpoint_compare(mode, major, entry);
		if (!order)
			return entry;
		node = order < 0 ? node->rb_left : node->rb_right;
	}
	return NULL;
}

static void
char_endpoint_register(unsigned mode_type, unsigned major, unsigned minor_base,
		       unsigned minor_count, const char *name,
		       file *(*open)(super_block *sb, unsigned rdev, int flag))
{
	struct rb_node **link = &char_endpoint_majors.rb_node, *parent = NULL;
	char_endpoint_entry *entry, *head = NULL;
	if (char_endpoint_count >= MAX_ENDPOINTS)
		return;
	entry = &char_endpoint_table[char_endpoint_count++];
	entry->mode_type = mode_type;
	entry->major = major;
	entry->minor_base = minor_base;
	entry->minor_count = minor_count;
	entry->name = name;
	entry->open = open;
	while (*link) {
		char_endpoint_entry *existing =
			rb_entry(*link, char_endpoint_entry, major_node);
		int order = char_endpoint_compare(mode_type, major, existing);
		if (!order) {
			head = existing;
			break;
		}
		parent = *link;
		link = order < 0 ? &parent->rb_left : &parent->rb_right;
	}
	list_init(&entry->major_ranges);
	if (head) {
		list_insert_tail(&head->major_ranges, &entry->major_ranges);
		RB_CLEAR_NODE(&entry->major_node);
		return;
	}
	rb_init_node(&entry->major_node);
	rb_link_node(&entry->major_node, parent, link);
	rb_insert_color(&entry->major_node, &char_endpoint_majors);
}

void chardev_for_each_class(device_number_iter_fn fn, void *data)
{
	int i;
	if (!fn)
		return;
	for (i = 0; i < char_endpoint_count; i++)
		if (!RB_EMPTY_NODE(&char_endpoint_table[i].major_node))
			fn(char_endpoint_table[i].mode_type,
			   char_endpoint_table[i].major,
			   char_endpoint_table[i].name, data);
}

static file *char_endpoint_open(super_block *sb, unsigned mode, unsigned devno,
				int flags, int *matched)
{
	char_endpoint_entry *head = char_endpoint_find_major(mode & S_IFMT,
							     MAJOR(devno)),
			    *entry;
	*matched = 0;
	if (!head)
		return NULL;
	entry = head;
	do {
		unsigned minor = MINOR(devno);
		if (entry->open && minor >= entry->minor_base &&
		    minor - entry->minor_base < entry->minor_count) {
			*matched = 1;
			return entry->open(sb, devno, flags);
		}
		entry = container_of(entry->major_ranges.next,
				     char_endpoint_entry, major_ranges);
	} while (entry != head);
	return NULL;
}

int chardev_register(unsigned devno, const char *name, device_node_open_fn open)
{
	char_endpoint_entry *head = char_endpoint_find_major(S_IFCHR,
							     MAJOR(devno)),
			    *entry = head;
	if (head)
		do {
			if (entry->open && entry->minor_count &&
			    entry->minor_base == MINOR(devno))
				return entry->open == open ? 0 : -EEXIST;
			entry = container_of(entry->major_ranges.next,
					     char_endpoint_entry, major_ranges);
		} while (entry != head);
	if (char_endpoint_count >= MAX_ENDPOINTS)
		return -ENOSPC;
	char_endpoint_register(S_IFCHR, MAJOR(devno), MINOR(devno), 1, name,
			       open);
	return 0;
}

file *chardev_open(super_block *sb, unsigned devno, int flags, int *matched)
{
	return char_endpoint_open(sb, S_IFCHR, devno, flags, matched);
}

void chardev_register_class(unsigned major, const char *name)
{
	if (!char_endpoint_find_major(S_IFCHR, major))
		char_endpoint_register(S_IFCHR, major, 0, 0, name, NULL);
}

static const chardev_console_ops *console_ops;
static const chardev_keyboard_ops *keyboard_ops;

int chardev_register_console(const chardev_console_ops *ops)
{
	if (!ops || !ops->emit || !ops->lock || !ops->unlock)
		return -EINVAL;
	if (console_ops && console_ops != ops)
		return -EBUSY;
	console_ops = ops;
	return 0;
}
int chardev_console_ready(void)
{
	return console_ops != NULL;
}
void chardev_console_emit(char byte, void *context)
{
	if (console_ops)
		console_ops->emit(byte, context);
}
void chardev_console_lock(int *saved_irq)
{
	if (console_ops)
		console_ops->lock(saved_irq);
	else
		*saved_irq = 0;
}
void chardev_console_unlock(int saved_irq)
{
	if (console_ops)
		console_ops->unlock(saved_irq);
}
void chardev_console_select(int index)
{
	if (console_ops && console_ops->select)
		console_ops->select(index);
}
void chardev_console_input(unsigned char byte)
{
	if (console_ops && console_ops->input)
		console_ops->input(byte);
}
int chardev_console_input_mode(void)
{
	return console_ops && console_ops->input_mode ?
		       console_ops->input_mode() :
		       0;
}
void chardev_console_flush(void)
{
	if (console_ops && console_ops->flush)
		console_ops->flush();
}

int chardev_console_needs_refresh(void)
{
	return console_ops && console_ops->needs_refresh &&
	       console_ops->needs_refresh();
}

void chardev_console_refresh(void)
{
	if (console_ops && console_ops->refresh)
		console_ops->refresh();
}
char *chardev_console_snapshot(unsigned *length)
{
	return console_ops && console_ops->snapshot ?
		       console_ops->snapshot(length) :
		       NULL;
}

int chardev_register_keyboard(const chardev_keyboard_ops *ops)
{
	if (!ops)
		return -EINVAL;
	if (keyboard_ops && keyboard_ops != ops)
		return -EBUSY;
	keyboard_ops = ops;
	return 0;
}
int chardev_keyboard_get_map(struct kbentry *entry)
{
	return keyboard_ops && keyboard_ops->get_map ?
		       keyboard_ops->get_map(entry) :
		       -ENODEV;
}
int chardev_keyboard_set_map(const struct kbentry *entry)
{
	return keyboard_ops && keyboard_ops->set_map ?
		       keyboard_ops->set_map(entry) :
		       -ENODEV;
}
int chardev_keyboard_get_string(struct kbsentry *entry)
{
	return keyboard_ops && keyboard_ops->get_string ?
		       keyboard_ops->get_string(entry) :
		       -ENODEV;
}
int chardev_keyboard_set_string(const struct kbsentry *entry)
{
	return keyboard_ops && keyboard_ops->set_string ?
		       keyboard_ops->set_string(entry) :
		       -ENODEV;
}
