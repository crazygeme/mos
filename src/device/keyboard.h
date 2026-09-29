#ifndef _DEVICE_KEYBOARD_H_
#define _DEVICE_KEYBOARD_H_
struct kbentry;
void kb_start(void);
int kbd_get_kbentry(struct kbentry *kbe);
int kbd_set_kbentry(const struct kbentry *kbe);
struct kbsentry;
int kbd_get_kbsentry(struct kbsentry *kbs);
int kbd_set_kbsentry(const struct kbsentry *kbs);
#endif
