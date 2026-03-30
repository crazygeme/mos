#include <lib/klib.h>
#include <lib/port.h>
#include <int/int.h>

/* Register definitions for the 16550A UART used in PCs.
   The 16550A has a lot more going on than shown here, but this
   is all we need.

   Refer to [PC16650D] for hardware information. */

/* I/O port base address for the first serial port. */
#define IO_BASE 0x3f8

/* DLAB=0 registers. */
#define RBR_REG (IO_BASE + 0) /* Receiver Buffer Reg. (read-only). */
#define THR_REG (IO_BASE + 0) /* Transmitter Holding Reg. (write-only). */
#define IER_REG (IO_BASE + 1) /* Interrupt Enable Reg.. */

/* DLAB=1 registers. */
#define LS_REG (IO_BASE + 0) /* Divisor Latch (LSB). */
#define MS_REG (IO_BASE + 1) /* Divisor Latch (MSB). */

/* DLAB-insensitive registers. */
#define IIR_REG (IO_BASE + 2) /* Interrupt Identification Reg. (read-only) */
#define FCR_REG (IO_BASE + 2) /* FIFO Control Reg. (write-only). */
#define LCR_REG (IO_BASE + 3) /* Line Control Register. */
#define MCR_REG (IO_BASE + 4) /* MODEM Control Register. */
#define LSR_REG (IO_BASE + 5) /* Line Status Register (read-only). */

/* Interrupt Enable Register bits. */
#define IER_RECV 0x01 /* Interrupt when data received. */
#define IER_XMIT 0x02 /* Interrupt when transmit finishes. */

/* Line Control Register bits. */
#define LCR_N81 0x03 /* No parity, 8 data bits, 1 stop bit. */
#define LCR_DLAB 0x80 /* Divisor Latch Access Bit (DLAB). */

/* MODEM Control Register. */
#define MCR_OUT2 0x08 /* Output line 2. */

/* Line Status Register. */
#define LSR_DR 0x01 /* Data Ready: received data byte is in RBR. */
#define LSR_THRE 0x20 /* THR Empty. */

#define outb port_write_byte
#define inb port_read_byte

/* Transmission mode. */
static enum { UNINIT, POLL, QUEUE } mode = UNINIT;

/* TX ring buffer. All accesses occur with interrupts disabled. */
#define TXQ_SIZE 4096
static unsigned char txq_buf[TXQ_SIZE];
static unsigned txq_head = 0; /* read index */
static unsigned txq_tail = 0; /* write index */
static unsigned txq_len = 0;

static int txq_isempty(void)
{
	return txq_len == 0;
}
static int txq_isfull(void)
{
	return txq_len == TXQ_SIZE;
}

static void txq_putc(unsigned char c)
{
	txq_buf[txq_tail] = c;
	txq_tail = (txq_tail + 1) % TXQ_SIZE;
	txq_len++;
}

static unsigned char txq_getc(void)
{
	unsigned char c = txq_buf[txq_head];
	txq_head = (txq_head + 1) % TXQ_SIZE;
	txq_len--;
	return c;
}

static void set_serial(int bps);
static void putc_poll(unsigned char c);
static void write_ier(void);
static void serial_interrupt(intr_frame *frame);

/* Initializes the serial port device for polling mode.
   Polling mode busy-waits for the serial port to become free
   before writing to it.  It's slow, but until interrupts have
   been initialized it's all we can do. */
static void init_poll(void)
{
	outb(IER_REG, 0); /* Turn off all interrupts. */
	outb(FCR_REG, 0); /* Disable FIFO. */
	set_serial(115200); /* 115.2 kbps, N-8-1. */
	outb(MCR_REG, MCR_OUT2); /* Required to enable interrupts. */
	mode = POLL;
}

/* Initializes the serial port device for queued interrupt-driven
   I/O.  With interrupt-driven I/O we don't waste CPU time
   waiting for the serial device to become ready. */
void serial_init_queue(void)
{
	unsigned old_level;

	if (mode == UNINIT)
		init_poll();

	int_register(0x20 + 4, serial_interrupt, 0, 0);
	mode = QUEUE;
	old_level = int_intr_disable();
	write_ier();
	int_intr_setlevel(old_level);
}

/* Sends BYTE to the serial port. */
void serial_putc(unsigned char byte)
{
	unsigned old_level = int_intr_disable();

	if (mode != QUEUE) {
		/* If we're not set up for interrupt-driven I/O yet,
		   use dumb polling to transmit a byte. */
		if (mode == UNINIT)
			init_poll();
		putc_poll(byte);
	} else {
		/* Otherwise, queue a byte and update the interrupt enable
		   register. */
		/* Interrupts are now off (disabled above), so the TX interrupt
		   cannot drain the queue.  If the queue is full we drop rather
		   than poll — polling with interrupts off for each character
		   would delay other IRQs long enough to miss them. */
		if (byte == '\n') {
			if (!txq_isfull())
				txq_putc('\r');
		}
		if (!txq_isfull())
			txq_putc(byte);
		write_ier();
	}

	int_intr_setlevel(old_level);
}

/* Flushes anything in the serial buffer out the port in polling
   mode. */
void serial_flush(void)
{
	unsigned old_level = int_intr_disable();
	while (!txq_isempty())
		putc_poll(txq_getc());
	int_intr_setlevel(old_level);
}

/* The fullness of the input buffer may have changed.  Reassess
   whether we should block receive interrupts.
   Called by the input buffer routines when characters are added
   to or removed from the buffer. */
void serial_notify(void)
{
	if (mode == QUEUE)
		write_ier();
}

/* Configures the serial port for BPS bits per second. */
static void set_serial(int bps)
{
	int base_rate = 1843200 / 16; /* Base rate of 16550A, in Hz. */
	unsigned short divisor = base_rate / bps; /* Clock rate divisor. */

	/* Enable DLAB. */
	outb(LCR_REG, LCR_N81 | LCR_DLAB);

	/* Set data rate. */
	outb(LS_REG, divisor & 0xff);
	outb(MS_REG, divisor >> 8);

	/* Reset DLAB. */
	outb(LCR_REG, LCR_N81);
}

/* Update interrupt enable register. */
static void write_ier(void)
{
	unsigned char ier = 0;

	/* Enable transmit interrupt if we have any characters to
	   transmit. */
	if (!txq_isempty())
		ier |= IER_XMIT;

	/* Enable receive interrupt if we have room to store any
	   characters we receive. */
	if (!txq_isfull())
		ier |= IER_RECV;

	outb(IER_REG, ier);
}

/* Polls the serial port until it's ready,
   and then transmits BYTE. */
static void putc_poll(unsigned char byte)
{
	while ((inb(LSR_REG) & LSR_THRE) == 0)
		continue;
	outb(THR_REG, byte);
}

/* Serial interrupt handler. */
static void serial_interrupt(intr_frame *f)
{
	/* Inquire about interrupt in UART.  Without this, we can
	   occasionally miss an interrupt running under QEMU. */
	inb(IIR_REG);

	/* As long as we have room to receive a byte, and the hardware
	   has a byte for us, receive a byte.  */
	while (!txq_isfull() && (inb(LSR_REG) & LSR_DR) != 0)
		txq_putc(inb(RBR_REG));

	/* As long as we have a byte to transmit, and the hardware is
	   ready to accept a byte for transmission, transmit a byte. */
	while (!txq_isempty() && (inb(LSR_REG) & LSR_THRE) != 0)
		outb(THR_REG, txq_getc());

	/* Update interrupt enable register based on queue status. */
	write_ier();
}
