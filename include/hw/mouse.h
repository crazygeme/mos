#ifndef _HW_MOUSE_H_
#define _HW_MOUSE_H_

#include <fs/fs.h>

void ps2mouse_init(void);

void ps2mouse_reader_open(void);
void ps2mouse_reader_close(void);
void ps2mouse_register_file(file *fp);
void ps2mouse_unregister_file(file *fp);

ssize_t ps2mouse_read(void *buf, size_t size, int nonblock);
ssize_t ps2mouse_write(const void *buf, size_t size);
unsigned ps2mouse_poll(unsigned events, poll_table *pt);

void ps2mouse_flush(void);
int ps2mouse_fionread(void);

#endif
