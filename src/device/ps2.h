#ifndef MOS_DEVICE_PS2_H
#define MOS_DEVICE_PS2_H
#include <device/device.h>

#define PS2_PORT_KEYBOARD 0
#define PS2_PORT_AUX 1
#define PS2_REPLY_SPINS 500000

void ps2_scan(void);
const device_t *ps2_device(unsigned port);
void ps2_drain_input(void);
/* The protocol driver serializes command transactions on its port. */
void ps2_command_begin(unsigned port);
void ps2_command_end(unsigned port);
int ps2_write_byte(unsigned port, unsigned char data);
int ps2_read_reply(unsigned port, unsigned char *data, int spins);
void ps2_sysfs_register(void);
#endif
