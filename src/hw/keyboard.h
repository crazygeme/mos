#ifndef _HW_KEYBOARD_H_
#define _HW_KEYBOARD_H_
#define KB_DATA 0x60
#define KB_UP_MASK 0x80
#include <int/interrupt.h>
struct kbentry;
void kb_init();
void kb_process(intr_frame *frame);
int kbd_get_kbentry(struct kbentry *kbe);
int kbd_set_kbentry(const struct kbentry *kbe);
struct kbsentry;
int kbd_get_kbsentry(struct kbsentry *kbs);
int kbd_set_kbsentry(const struct kbsentry *kbs);
#endif
