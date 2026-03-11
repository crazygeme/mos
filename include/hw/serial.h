#ifndef _HW_SERIAL_H
#define _HW_SERIAL_H

void serial_init_queue(void);
void serial_putc(unsigned char);
void serial_flush(void);
void serial_notify(void);

#endif /* devices/serial.h */
