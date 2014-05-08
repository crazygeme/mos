#ifndef _KEYBOARD_H_
#define _KEYBOARD_H_
#define KB_DATA 0x60
#define KB_UP_MASK 0x80
typedef struct _intr_frame intr_frame;
void kb_init();
void kb_process(intr_frame *frame);
#endif
