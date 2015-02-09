#ifndef DEVICES_SERIAL_H
#define DEVICES_SERIAL_H



void serial_init_queue (void);
void serial_putc (unsigned char);
void serial_flush (void);
void serial_notify (void);

#endif /* devices/serial.h */
