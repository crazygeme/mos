#ifndef MOS_DEVICE_CHARDEV_H
#define MOS_DEVICE_CHARDEV_H
#include <device/devnode.h>

/* Device-number registration is independent of filesystem node placement. */
int chardev_register(unsigned devno, const char *name,
		     device_node_open_fn open);
void chardev_register_class(unsigned major, const char *name);
void chardev_for_each_class(device_number_iter_fn fn, void *data);
file *chardev_open(super_block *sb, unsigned devno, int flags, int *matched);
/* A bound character driver may provide the active kernel console. */
typedef struct {
	void (*emit)(char byte, void *context);
	void (*lock)(int *saved_irq);
	void (*unlock)(int saved_irq);
	void (*flush)(void);
	void (*select)(int index);
	void (*input)(unsigned char byte);
	int (*input_mode)(void);
	int (*needs_refresh)(void);
	void (*refresh)(void);
	char *(*snapshot)(unsigned *length);
} chardev_console_ops;
int chardev_register_console(const chardev_console_ops *ops);
int chardev_console_ready(void);
void chardev_console_emit(char byte, void *context);
void chardev_console_lock(int *saved_irq);
void chardev_console_unlock(int saved_irq);
void chardev_console_flush(void);
void chardev_console_select(int index);
void chardev_console_input(unsigned char byte);
int chardev_console_input_mode(void);
int chardev_console_needs_refresh(void);
void chardev_console_refresh(void);
char *chardev_console_snapshot(unsigned *length);

/* Keyboard control is registered by the input driver, independent of terminals. */
struct kbentry;
struct kbsentry;
typedef struct {
	int (*get_map)(struct kbentry *entry);
	int (*set_map)(const struct kbentry *entry);
	int (*get_string)(struct kbsentry *entry);
	int (*set_string)(const struct kbsentry *entry);
} chardev_keyboard_ops;
int chardev_register_keyboard(const chardev_keyboard_ops *ops);
int chardev_keyboard_get_map(struct kbentry *entry);
int chardev_keyboard_set_map(const struct kbentry *entry);
int chardev_keyboard_get_string(struct kbsentry *entry);
int chardev_keyboard_set_string(const struct kbsentry *entry);
#endif
