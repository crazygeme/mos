#include <device/ps2.h>
#include <driver/driver.h>
#include <int/int.h>
#include <lib/port.h>
#include <lib/klib.h>
#include <errno.h>
#include <macro.h>

#define I8042_DATA 0x60
#define I8042_STATUS 0x64
#define I8042_CMD 0x64

#define I8042_STATUS_OBF 0x01
#define I8042_STATUS_IBF 0x02
#define I8042_STATUS_AUX 0x20

#define I8042_CMD_READ_CONFIG 0x20
#define I8042_CMD_WRITE_CONFIG 0x60
#define I8042_CMD_ENABLE_AUX 0xA8
#define I8042_CMD_DISABLE_KBD 0xAD
#define I8042_CMD_ENABLE_KBD 0xAE
#define I8042_CMD_DISABLE_AUX 0xA7

#define I8042_CFG_IRQ12 0x02
#define I8042_CFG_IRQ1 0x01
#define I8042_CFG_KBD_CLOCK_DISABLE 0x10
#define I8042_CFG_AUX_CLOCK_DISABLE 0x20

#define I8042_WAIT_SPINS 100000

static device_t ports[2] = {
	{ .bus = DEVICE_BUS_PS2, .address = PS2_PORT_KEYBOARD },
	{ .bus = DEVICE_BUS_PS2, .address = PS2_PORT_AUX },
};
static volatile unsigned command_busy[2];
static unsigned scanned, ports_ready;

static void ps2_receive(unsigned port, unsigned char byte)
{
	driver_t *driver = ports[port].driver;
	if (driver && driver->receive_ps2)
		driver->receive_ps2(byte);
}

static int i8042_wait_input_empty(void)
{
	int spins;

	for (spins = 0; spins < I8042_WAIT_SPINS; spins++) {
		if ((port_read_byte(I8042_STATUS) & I8042_STATUS_IBF) == 0)
			return 0;
		PAUSE();
	}

	return -ETIMEDOUT;
}

static int i8042_wait_output(unsigned char *status, int spins)
{
	int i;

	for (i = 0; i < spins; i++) {
		unsigned char st = port_read_byte(I8042_STATUS);

		if (st & I8042_STATUS_OBF) {
			if (status)
				*status = st;
			return 0;
		}
		PAUSE();
	}

	return -ETIMEDOUT;
}

static int i8042_write_cmd(unsigned char cmd)
{
	if (i8042_wait_input_empty() < 0)
		return -ETIMEDOUT;
	port_write_byte(I8042_CMD, cmd);
	return 0;
}

static int i8042_write_data(unsigned char data)
{
	if (i8042_wait_input_empty() < 0)
		return -ETIMEDOUT;
	port_write_byte(I8042_DATA, data);
	return 0;
}

int ps2_write_byte(unsigned port, unsigned char data)
{
	if (port > PS2_PORT_AUX)
		return -EINVAL;
	if (port == PS2_PORT_AUX && i8042_write_cmd(0xD4) < 0)
		return -ETIMEDOUT;
	return i8042_write_data(data);
}

static int i8042_read_data(unsigned char *data, int spins)
{
	if (i8042_wait_output(NULL, spins) < 0)
		return -ETIMEDOUT;
	*data = port_read_byte(I8042_DATA);
	return 0;
}

int ps2_read_reply(unsigned port, unsigned char *data, int spins)
{
	int i;
	if (port > PS2_PORT_AUX || !data)
		return -EINVAL;

	for (i = 0; i < spins; i++) {
		unsigned irq = int_intr_disable();
		unsigned char st = port_read_byte(I8042_STATUS);

		if (st & I8042_STATUS_OBF) {
			unsigned char byte = port_read_byte(I8042_DATA);
			if (!!(st & I8042_STATUS_AUX) == port) {
				*data = byte;
				int_intr_setlevel(irq);
				return 0;
			}
			ps2_receive(!!(st & I8042_STATUS_AUX), byte);
		}
		int_intr_setlevel(irq);
		PAUSE();
	}

	return -ETIMEDOUT;
}

static void ps2_discard_input(unsigned port)
{
	int i;

	for (i = 0; i < 256; i++) {
		unsigned irq = int_intr_disable();
		unsigned char st = port_read_byte(I8042_STATUS);

		if ((st & I8042_STATUS_OBF) == 0) {
			int_intr_setlevel(irq);
			return;
		}
		unsigned char byte = port_read_byte(I8042_DATA);
		if (!!(st & I8042_STATUS_AUX) != port)
			ps2_receive(!!(st & I8042_STATUS_AUX), byte);
		int_intr_setlevel(irq);
	}
}

static int i8042_configure(void)
{
	unsigned char cfg;
	unsigned irq = int_intr_disable();
	int ret = -ETIMEDOUT;

	/* Bootstrap runs before AP startup. The controller configuration reply
	 * uses the keyboard data channel; IRQ1 must not consume it as input. */
	if (i8042_write_cmd(I8042_CMD_DISABLE_KBD) < 0 ||
	    i8042_write_cmd(I8042_CMD_DISABLE_AUX) < 0)
		goto out;
	/* Quiesce both ports before reading the untagged configuration reply. */
	ps2_discard_input(PS2_PORT_AUX);
	if (i8042_write_cmd(I8042_CMD_READ_CONFIG) < 0)
		goto out;
	if (i8042_read_data(&cfg, I8042_WAIT_SPINS) < 0)
		goto out;

	cfg |= I8042_CFG_IRQ1 | I8042_CFG_IRQ12;
	cfg &= ~(I8042_CFG_KBD_CLOCK_DISABLE | I8042_CFG_AUX_CLOCK_DISABLE);

	if (i8042_write_cmd(I8042_CMD_WRITE_CONFIG) < 0)
		goto out;
	ret = i8042_write_data(cfg);
out:
	i8042_write_cmd(I8042_CMD_ENABLE_KBD);
	i8042_write_cmd(I8042_CMD_ENABLE_AUX);
	int_intr_setlevel(irq);
	return ret;
}

/* Both IRQ lines drain one shared controller output in byte order. */
void ps2_drain_input(void)
{
	unsigned budget = 64;
	unsigned irq = int_intr_disable();
	while (budget--) {
		unsigned char status = port_read_byte(I8042_STATUS);
		unsigned port = !!(status & I8042_STATUS_AUX);
		if (!(status & I8042_STATUS_OBF) || command_busy[port])
			break;
		ps2_receive(port, port_read_byte(I8042_DATA));
	}
	int_intr_setlevel(irq);
}

static void ps2_irq(intr_frame *frame)
{
	(void)frame;
	ps2_drain_input();
}

void ps2_command_begin(unsigned port)
{
	if (port > PS2_PORT_AUX)
		return;
	command_busy[port] = 1;
	BARRIER();
	ps2_discard_input(port);
}

void ps2_command_end(unsigned port)
{
	if (port > PS2_PORT_AUX)
		return;
	command_busy[port] = 0;
	BARRIER();
	ps2_drain_input();
}

const device_t *ps2_device(unsigned port)
{
	return ports_ready && port <= PS2_PORT_AUX ? &ports[port] : NULL;
}

/* Bootstrap CPU only, after process/IRQ setup and before AP startup. */
void ps2_scan(void)
{
	unsigned irq;
	if (scanned)
		return;
	scanned = 1;
	irq = int_intr_disable();
	if (i8042_configure() < 0) {
		printk("ps2: controller configuration failed\n");
		int_intr_setlevel(irq);
		return;
	}
	int_register(0x21, ps2_irq, 0, 0);
	int_register(INT_VECTOR_IRQ8 + 4, ps2_irq, 0, 0);
	device_register(&ports[PS2_PORT_KEYBOARD]);
	device_register(&ports[PS2_PORT_AUX]);
	ports_ready = 1;
	int_intr_setlevel(irq);
	ps2_drain_input();
}
