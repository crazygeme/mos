#include <errno.h>
#include <fs/fcntl.h>
#include <fs/fs.h>
#include <hw/mouse.h>
#include <int/dsr.h>
#include <int/int.h>
#include <lib/cyclebuf.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <lib/port.h>
#include <macro.h>
#include <ps/ps.h>
#include <ps/signal.h>

#define I8042_DATA 0x60
#define I8042_STATUS 0x64
#define I8042_CMD 0x64

#define I8042_STATUS_OBF 0x01
#define I8042_STATUS_IBF 0x02
#define I8042_STATUS_AUX 0x20

#define I8042_CMD_READ_CONFIG 0x20
#define I8042_CMD_WRITE_CONFIG 0x60
#define I8042_CMD_ENABLE_AUX 0xA8

#define I8042_CFG_IRQ12 0x02
#define I8042_CFG_AUX_CLOCK_DISABLE 0x20

#define PS2_MOUSE_ACK 0xFA
#define PS2_MOUSE_BAT_OK 0xAA
#define PS2_MOUSE_ID_STANDARD 0x00
#define PS2_MOUSE_ID_IMPS2 0x03

#define PS2_MOUSE_CMD_RESET 0xFF
#define PS2_MOUSE_CMD_RESEND 0xFE
#define PS2_MOUSE_CMD_SET_DEFAULTS 0xF6
#define PS2_MOUSE_CMD_DISABLE_REPORTING 0xF5
#define PS2_MOUSE_CMD_ENABLE_REPORTING 0xF4
#define PS2_MOUSE_CMD_SET_SAMPLE_RATE 0xF3
#define PS2_MOUSE_CMD_GET_ID 0xF2
#define PS2_MOUSE_CMD_SET_REMOTE_MODE 0xF0
#define PS2_MOUSE_CMD_READ_DATA 0xEB
#define PS2_MOUSE_CMD_SET_STREAM_MODE 0xEA
#define PS2_MOUSE_CMD_STATUS_REQUEST 0xE9
#define PS2_MOUSE_CMD_SET_RESOLUTION 0xE8
#define PS2_MOUSE_CMD_SET_SCALING21 0xE7
#define PS2_MOUSE_CMD_SET_SCALING11 0xE6

#define MOUSE_IRQ_VECTOR (INT_VECTOR_IRQ8 + 4)

#define I8042_WAIT_SPINS 100000
#define I8042_WAIT_SPINS_RESET 500000

static cy_buf *mouse_rxbuf;
static mutex_t mouse_cmd_lock;
static spinlock_t mouse_state_lock;
static volatile int mouse_cmd_busy;
static int mouse_present;
static int mouse_dsr_armed;
#define PS2MOUSE_MAX_FILES 8
static file *mouse_files[PS2MOUSE_MAX_FILES];

static unsigned char mouse_packet[4];
static unsigned mouse_packet_idx;
static unsigned mouse_packet_len = 3;
static unsigned char mouse_last_packet[4] = { 0x08, 0x00, 0x00, 0x00 };
static unsigned mouse_last_packet_len = 3;

static int mouse_expect_param;
static unsigned char mouse_expect_param_cmd;
static unsigned char mouse_last_sample[3];

static void ps2mouse_dsr(void *param);

static void mouse_queue_bytes(const unsigned char *buf, unsigned len)
{
	int irq;
	int owners[PS2MOUSE_MAX_FILES];
	int sigs[PS2MOUSE_MAX_FILES];
	unsigned nnotify = 0;
	unsigned i;

	if (!mouse_rxbuf || !buf || len == 0)
		return;
	cyb_putbuf(mouse_rxbuf, (unsigned char *)buf, len, 0, 0);

	spinlock_lock(&mouse_state_lock, &irq);
	for (i = 0; i < PS2MOUSE_MAX_FILES; i++) {
		file *fp = mouse_files[i];

		if (!fp || !(fp->f_flag & FASYNC) || !fp->f_owner)
			continue;
		owners[nnotify] = fp->f_owner;
		sigs[nnotify] = fp->f_sigio > 0 ? fp->f_sigio : SIGIO;
		nnotify++;
	}
	spinlock_unlock(&mouse_state_lock, irq);

	for (i = 0; i < nnotify; i++) {
		ps_send_signal_owner(owners[i], sigs[i]);
	}
}

static void mouse_queue_byte(unsigned char b)
{
	mouse_queue_bytes(&b, 1);
}

static void mouse_update_packet_mode_locked(unsigned char id)
{
	if (id == PS2_MOUSE_ID_IMPS2) {
		mouse_packet_len = 4;
		mouse_last_packet_len = 4;
		mouse_last_packet[3] = 0x00;
	} else {
		mouse_packet_len = 3;
		mouse_last_packet_len = 3;
	}
	mouse_packet_idx = 0;
}

static void mouse_reset_runtime_state(void)
{
	int irq;

	spinlock_lock(&mouse_state_lock, &irq);
	mouse_expect_param = 0;
	mouse_expect_param_cmd = 0;
	mouse_packet_idx = 0;
	mouse_last_sample[0] = 0;
	mouse_last_sample[1] = 0;
	mouse_last_sample[2] = 0;
	mouse_update_packet_mode_locked(PS2_MOUSE_ID_STANDARD);
	spinlock_unlock(&mouse_state_lock, irq);
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

static int ps2mouse_write_device_byte(unsigned char data)
{
	if (i8042_write_cmd(0xD4) < 0)
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

static int ps2mouse_read_reply(unsigned char *data, int spins)
{
	int i;

	for (i = 0; i < spins; i++) {
		unsigned char st = port_read_byte(I8042_STATUS);

		if ((st & (I8042_STATUS_OBF | I8042_STATUS_AUX)) ==
		    (I8042_STATUS_OBF | I8042_STATUS_AUX)) {
			*data = port_read_byte(I8042_DATA);
			return 0;
		}
		PAUSE();
	}

	return -ETIMEDOUT;
}

static void i8042_drain_aux(void)
{
	int i;

	for (i = 0; i < 32; i++) {
		unsigned char st = port_read_byte(I8042_STATUS);

		if ((st & I8042_STATUS_OBF) == 0)
			return;
		if ((st & I8042_STATUS_AUX) == 0)
			return;
		(void)port_read_byte(I8042_DATA);
	}
}

static int ps2mouse_send_cmd(unsigned char cmd)
{
	return ps2mouse_write_device_byte(cmd);
}

static int ps2mouse_send_cmd_expect_ack(unsigned char cmd)
{
	unsigned char reply;

	if (ps2mouse_send_cmd(cmd) < 0)
		return -ETIMEDOUT;
	if (ps2mouse_read_reply(&reply, I8042_WAIT_SPINS_RESET) < 0)
		return -ETIMEDOUT;
	return reply == PS2_MOUSE_ACK ? 0 : -EIO;
}

static int ps2mouse_enable_irq12(void)
{
	unsigned char cfg;

	if (i8042_write_cmd(I8042_CMD_ENABLE_AUX) < 0)
		return -ETIMEDOUT;
	if (i8042_write_cmd(I8042_CMD_READ_CONFIG) < 0)
		return -ETIMEDOUT;
	if (i8042_read_data(&cfg, I8042_WAIT_SPINS) < 0)
		return -ETIMEDOUT;

	cfg |= I8042_CFG_IRQ12;
	cfg &= ~I8042_CFG_AUX_CLOCK_DISABLE;

	if (i8042_write_cmd(I8042_CMD_WRITE_CONFIG) < 0)
		return -ETIMEDOUT;
	if (i8042_write_data(cfg) < 0)
		return -ETIMEDOUT;

	return 0;
}

static void mouse_track_sample_rate_locked(unsigned char rate)
{
	mouse_last_sample[0] = mouse_last_sample[1];
	mouse_last_sample[1] = mouse_last_sample[2];
	mouse_last_sample[2] = rate;

	if (mouse_last_sample[0] == 200 && mouse_last_sample[1] == 100 &&
	    mouse_last_sample[2] == 80)
		mouse_update_packet_mode_locked(PS2_MOUSE_ID_IMPS2);
}

static void ps2mouse_process_byte(unsigned char data)
{
	unsigned char packet[4];
	unsigned packet_len = 0;
	int irq;

	spinlock_lock(&mouse_state_lock, &irq);

	if (mouse_packet_idx == 0 && (data & 0x08) == 0) {
		spinlock_unlock(&mouse_state_lock, irq);
		return;
	}

	mouse_packet[mouse_packet_idx++] = data;
	if (mouse_packet_idx == mouse_packet_len) {
		unsigned i;

		for (i = 0; i < mouse_packet_len; i++)
			packet[i] = mouse_packet[i];
		packet_len = mouse_packet_len;
		for (i = 0; i < packet_len; i++)
			mouse_last_packet[i] = packet[i];
		mouse_last_packet_len = packet_len;
		mouse_packet_idx = 0;
	}

	spinlock_unlock(&mouse_state_lock, irq);

	if (packet_len)
		mouse_queue_bytes(packet, packet_len);
}

static void ps2mouse_irq(intr_frame *frame)
{
	(void)frame;

	if (mouse_cmd_busy)
		return;

	if (!mouse_dsr_armed) {
		mouse_dsr_armed = 1;
		if (!dsr_add(ps2mouse_dsr, NULL))
			mouse_dsr_armed = 0;
	}
}

static void ps2mouse_dsr(void *param)
{
	(void)param;
	mouse_dsr_armed = 0;

	while (!mouse_cmd_busy) {
		unsigned char st = port_read_byte(I8042_STATUS);

		if ((st & (I8042_STATUS_OBF | I8042_STATUS_AUX)) !=
		    (I8042_STATUS_OBF | I8042_STATUS_AUX))
			break;
		unsigned char data = port_read_byte(I8042_DATA);
		ps2mouse_process_byte(data);
	}
}

void ps2mouse_init(void)
{
	mouse_rxbuf = cyb_create_named(1);
	if (mouse_rxbuf)
		cyb_writer_open(mouse_rxbuf);

	mutex_init(&mouse_cmd_lock);
	spinlock_init(&mouse_state_lock);
	mouse_reset_runtime_state();

	int_register(MOUSE_IRQ_VECTOR, ps2mouse_irq, 0, 0);
	mouse_cmd_busy = 1;
	BARRIER();

	if (ps2mouse_enable_irq12() < 0) {
		mouse_cmd_busy = 0;
		BARRIER();
		printk("mouse: failed to enable PS/2 auxiliary port\n");
		return;
	}

	i8042_drain_aux();

	if (ps2mouse_send_cmd_expect_ack(PS2_MOUSE_CMD_SET_DEFAULTS) < 0) {
		mouse_cmd_busy = 0;
		BARRIER();
		printk("mouse: PS/2 mouse not responding\n");
		return;
	}

	if (ps2mouse_send_cmd_expect_ack(PS2_MOUSE_CMD_ENABLE_REPORTING) < 0) {
		mouse_cmd_busy = 0;
		BARRIER();
		printk("mouse: failed to enable PS/2 mouse reporting\n");
		return;
	}

	mouse_cmd_busy = 0;
	BARRIER();
	mouse_present = 1;
	printk("mouse: PS/2 mouse initialized on IRQ12\n");
}

void ps2mouse_reader_open(void)
{
	if (mouse_rxbuf)
		cyb_reader_open(mouse_rxbuf);
}

void ps2mouse_reader_close(void)
{
	if (mouse_rxbuf)
		cyb_reader_close(mouse_rxbuf);
}

void ps2mouse_register_file(file *fp)
{
	unsigned i;
	int irq;

	if (!fp)
		return;

	spinlock_lock(&mouse_state_lock, &irq);
	for (i = 0; i < PS2MOUSE_MAX_FILES; i++) {
		if (mouse_files[i] == fp) {
			spinlock_unlock(&mouse_state_lock, irq);
			return;
		}
	}
	for (i = 0; i < PS2MOUSE_MAX_FILES; i++) {
		if (!mouse_files[i]) {
			mouse_files[i] = fp;
			break;
		}
	}
	spinlock_unlock(&mouse_state_lock, irq);
}

void ps2mouse_unregister_file(file *fp)
{
	unsigned i;
	int irq;

	if (!fp)
		return;

	spinlock_lock(&mouse_state_lock, &irq);
	for (i = 0; i < PS2MOUSE_MAX_FILES; i++) {
		if (mouse_files[i] == fp) {
			mouse_files[i] = NULL;
			break;
		}
	}
	spinlock_unlock(&mouse_state_lock, irq);
}

ssize_t ps2mouse_read(void *buf, size_t size, int nonblock)
{
	int ret;

	if (!buf || size < 1)
		return 0;
	if (!mouse_present || !mouse_rxbuf)
		return -EIO;

	ret = cyb_getbuf(mouse_rxbuf, buf, (int)size, !nonblock, 1);
	if (ret < 0)
		return -EINTR;
	if (ret == 0 && nonblock)
		return -EAGAIN;
	return ret;
}

static int ps2mouse_queue_reply(const unsigned char *reply, unsigned len)
{
	unsigned i;

	for (i = 0; i < len; i++)
		mouse_queue_byte(reply[i]);
	return 0;
}

static int ps2mouse_command_one(unsigned char byte, int *expect_param,
				unsigned char *expect_cmd)
{
	unsigned char reply[5];
	unsigned len = 0;
	int irq;

	if (*expect_param) {
		if (ps2mouse_write_device_byte(byte) < 0)
			return -EIO;
		if (ps2mouse_read_reply(&reply[len], I8042_WAIT_SPINS_RESET) <
		    0)
			return -EIO;
		len++;

		spinlock_lock(&mouse_state_lock, &irq);
		if (reply[0] == PS2_MOUSE_ACK) {
			if (*expect_cmd == PS2_MOUSE_CMD_SET_SAMPLE_RATE)
				mouse_track_sample_rate_locked(byte);
		}
		*expect_param = 0;
		mouse_expect_param = 0;
		*expect_cmd = 0;
		mouse_expect_param_cmd = 0;
		spinlock_unlock(&mouse_state_lock, irq);
		return ps2mouse_queue_reply(reply, len);
	}

	if (ps2mouse_write_device_byte(byte) < 0)
		return -EIO;
	if (ps2mouse_read_reply(&reply[len], I8042_WAIT_SPINS_RESET) < 0)
		return -EIO;
	len++;

	switch (byte) {
	case PS2_MOUSE_CMD_RESET:
		if (ps2mouse_read_reply(&reply[len], I8042_WAIT_SPINS_RESET) <
		    0)
			return -EIO;
		len++;
		if (ps2mouse_read_reply(&reply[len], I8042_WAIT_SPINS_RESET) <
		    0)
			return -EIO;
		len++;

		spinlock_lock(&mouse_state_lock, &irq);
		mouse_last_sample[0] = 0;
		mouse_last_sample[1] = 0;
		mouse_last_sample[2] = 0;
		mouse_update_packet_mode_locked(reply[len - 1]);
		*expect_param = 0;
		*expect_cmd = 0;
		mouse_expect_param = 0;
		mouse_expect_param_cmd = 0;
		spinlock_unlock(&mouse_state_lock, irq);
		break;
	case PS2_MOUSE_CMD_GET_ID:
		if (ps2mouse_read_reply(&reply[len], I8042_WAIT_SPINS_RESET) <
		    0)
			return -EIO;
		len++;

		spinlock_lock(&mouse_state_lock, &irq);
		mouse_update_packet_mode_locked(reply[len - 1]);
		spinlock_unlock(&mouse_state_lock, irq);
		break;
	case PS2_MOUSE_CMD_READ_DATA: {
		unsigned packet_len;

		spinlock_lock(&mouse_state_lock, &irq);
		packet_len = mouse_packet_len;
		spinlock_unlock(&mouse_state_lock, irq);

		while (packet_len-- > 0) {
			if (ps2mouse_read_reply(&reply[len],
						I8042_WAIT_SPINS_RESET) < 0)
				return -EIO;
			len++;
		}
		break;
	}
	case PS2_MOUSE_CMD_STATUS_REQUEST:
		if (ps2mouse_read_reply(&reply[len], I8042_WAIT_SPINS_RESET) <
		    0)
			return -EIO;
		len++;
		if (ps2mouse_read_reply(&reply[len], I8042_WAIT_SPINS_RESET) <
		    0)
			return -EIO;
		len++;
		if (ps2mouse_read_reply(&reply[len], I8042_WAIT_SPINS_RESET) <
		    0)
			return -EIO;
		len++;
		break;
	case PS2_MOUSE_CMD_SET_SAMPLE_RATE:
	case PS2_MOUSE_CMD_SET_RESOLUTION:
		spinlock_lock(&mouse_state_lock, &irq);
		*expect_param = 1;
		*expect_cmd = byte;
		mouse_expect_param = 1;
		mouse_expect_param_cmd = byte;
		spinlock_unlock(&mouse_state_lock, irq);
		break;
	case PS2_MOUSE_CMD_SET_DEFAULTS:
		spinlock_lock(&mouse_state_lock, &irq);
		mouse_last_sample[0] = 0;
		mouse_last_sample[1] = 0;
		mouse_last_sample[2] = 0;
		mouse_update_packet_mode_locked(PS2_MOUSE_ID_STANDARD);
		mouse_expect_param = 0;
		mouse_expect_param_cmd = 0;
		spinlock_unlock(&mouse_state_lock, irq);
		break;
	default:
		break;
	}

	return ps2mouse_queue_reply(reply, len);
}

ssize_t ps2mouse_write(const void *buf, size_t size)
{
	const unsigned char *src = buf;
	size_t i;
	int expect_param;
	unsigned char expect_cmd;

	if (!buf || size < 1)
		return 0;
	if (!mouse_present)
		return -EIO;

	mutex_lock(&mouse_cmd_lock);
	mouse_cmd_busy = 1;
	BARRIER();
	i8042_drain_aux();

	expect_param = mouse_expect_param;
	expect_cmd = mouse_expect_param_cmd;
	for (i = 0; i < size; i++) {
		if (ps2mouse_command_one(src[i], &expect_param, &expect_cmd) <
		    0) {
			mouse_cmd_busy = 0;
			BARRIER();
			mutex_unlock(&mouse_cmd_lock);
			return -EIO;
		}
	}

	mouse_cmd_busy = 0;
	BARRIER();
	mutex_unlock(&mouse_cmd_lock);
	return (ssize_t)size;
}

unsigned ps2mouse_poll(unsigned events, poll_table *pt)
{
	unsigned ready = 0;

	if ((events & FS_POLL_READ) && mouse_rxbuf && !cyb_isempty(mouse_rxbuf))
		ready |= FS_POLL_READ;
	if (events & FS_POLL_WRITE)
		ready |= FS_POLL_WRITE;
	if (!ready && pt && (events & FS_POLL_READ) && mouse_rxbuf)
		cyb_poll_read(mouse_rxbuf, pt);
	return ready;
}

void ps2mouse_flush(void)
{
	if (mouse_rxbuf)
		cyb_flush(mouse_rxbuf);
}

int ps2mouse_fionread(void)
{
	return mouse_rxbuf ? cyb_get_buf_len(mouse_rxbuf) : 0;
}
