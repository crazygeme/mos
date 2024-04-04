#include <int.h>
#include <keyboard.h>
#include <ps.h>
#include <klib.h>
#include <dsr.h>
#include <mm.h>
#include <macro.h>

/* Sends an end-of-interrupt signal to the PIC for the given IRQ.
If we don't acknowledge the IRQ, it will never be delivered to
us again, so this is important. */
static void pic_end_of_interrupt(int irq)
{
	/* Acknowledge master PIC. */
	write_port(0x20, 0x20);

	/* Acknowledge slave PIC if this is a slave interrupt. */
	if (irq >= 0x28)
		write_port(0xa0, 0x20);
}

static char *intr_names[IDT_SIZE];
static int_callback in_callbacks[IDT_SIZE];

void int_register(int vec_no, int_callback fn, int is_trap, int dpl)
{
	unsigned long f = 0;
	if (vec_no < 0 || vec_no >= IDT_SIZE)
		return;

	f = intr_stubs[vec_no];
	if (is_trap) {
		idt[vec_no] = MAKE_TRAP_GATE(f, dpl);
	} else {
		idt[vec_no] = MAKE_INTR_GATE(f, dpl);
	}

	in_callbacks[vec_no] = fn;
}

void int_unregister(int vec_no)
{
	if (vec_no < 0 || vec_no >= IDT_SIZE) {
		printf("fatal error: vec number %x invalid!!\n", vec_no);
		return;
	}

	in_callbacks[vec_no] = 0;
	idt[vec_no] = 0;
}

void intr_handler(intr_frame *frame)
{
	// printf("interrupt: [%x] %s\n", frame->vec_no, intr_names[frame->vec_no]);
	int external = frame->vec_no >= 0x20 && frame->vec_no < 0x30;
	int_callback fn = 0;

	if (frame->vec_no < 0 || frame->vec_no >= IDT_SIZE) {
		printf("fatal error: vec number %x invalid!!\n", frame->vec_no);
		return;
	}

	fn = in_callbacks[frame->vec_no];
	if (fn)
		fn(frame);

	if (external)
		pic_end_of_interrupt(frame->vec_no);

	// if has dsr, call task_sched
	// task_sched function will pick dsr process first
	if (dsr_has_task() && ps_enabled())
		task_sched();
}

void intr_syscall_handler(intr_frame *frame)
{
	int_callback fn = 0;
	fn = in_callbacks[frame->vec_no];
	if (fn)
		fn(frame);
	return;
}

void int_enable_all()
{
	int i = 0;
	unsigned long long idtr = 0;
	unsigned long long gdtr = 0;

	idtr = MAKE_IDTR_OPERAND(idt_size - 1, idt);
	gdtr = MAKE_GDTR_OPERAND(gdt_size - 1, gdt);
	SET_IDT(idtr);
	SET_GDT(gdtr);
	SET_CS(KERNEL_CODE_SELECTOR);
	SET_DS(KERNEL_DATA_SELECTOR);

	write_port(0x21, 0x0);
	write_port(0xA1, 0x0);

	/* Initialize intr_names. */
	for (i = 0; i < IDT_SIZE; i++) {
		intr_names[i] = "unknown";
		in_callbacks[i] = 0;
	}
	intr_names[0] = "#DE Divide Error";
	intr_names[1] = "#DB Debug Exception";
	intr_names[2] = "NMI Interrupt";
	intr_names[3] = "#BP Breakpoint Exception";
	intr_names[4] = "#OF Overflow Exception";
	intr_names[5] = "#BR BOUND Range Exceeded Exception";
	intr_names[6] = "#UD Invalid Opcode Exception";
	intr_names[7] = "#NM Device Not Available Exception";
	intr_names[8] = "#DF Double Fault Exception";
	intr_names[9] = "Coprocessor Segment Overrun";
	intr_names[10] = "#TS Invalid TSS Exception";
	intr_names[11] = "#NP Segment Not Present";
	intr_names[12] = "#SS Stack Fault Exception";
	intr_names[13] = "#GP General Protection Exception";
	intr_names[14] = "#PF Page-Fault Exception";
	intr_names[16] = "#MF x87 FPU Floating-Point Error";
	intr_names[17] = "#AC Alignment Check Exception";
	intr_names[18] = "#MC Machine-Check Exception";
	intr_names[19] = "#XF SIMD Floating-Point Exception";
	intr_names[32] = "timer";
	intr_names[33] = "keyboard";
}

/*
 * address should be a physical address
 */
void int_update_tss(void *address)
{
	unsigned int base = (unsigned int)address;
	gdt[TSS_SELECTOR / 8] = MAKE_SEG_DESC(base, 0x67, SEG_CLASS_SYSTEM, 9,
					      KERNEL_PRIVILEGE, SEG_BASE_1);
	SET_TSS(TSS_SELECTOR);
}

unsigned int_is_intr_enabled()
{
	unsigned int flags;

	GET_INTR_FLAG(flags);

	return (flags & 0x00000200) == 0x00000200;
}

unsigned int_intr_enable()
{
	unsigned old = int_is_intr_enabled();

	ENABLE_INTR();

	return old;
}

unsigned int_intr_disable()
{
	unsigned old = int_is_intr_enabled();

	DISABLE_INTR();

	return old;
}

void int_intr_setlevel(unsigned enabled)
{
	if (enabled) {
		int_intr_enable();
	} else {
		int_intr_disable();
	}
}

/* Reads CNT bytes from PORT, one after another, and stores them
   into the buffer starting at ADDR. */
void _read_sb(unsigned short port, void *addr, unsigned cnt)
{
	/* See [IA32-v2a] "INS". */
	asm volatile("rep insb" : "+D"(addr), "+c"(cnt) : "d"(port) : "memory");
}

/* Reads and returns 16 bits from PORT. */
unsigned short _read_word(unsigned short port)
{
	unsigned short data;
	/* See [IA32-v2a] "IN". */
	asm volatile("inw %w1, %w0" : "=a"(data) : "Nd"(port));
	return data;
}

/* Reads CNT 16-bit (halfword) units from PORT, one after
   another, and stores them into the buffer starting at ADDR. */
void _read_wb(unsigned short port, void *addr, unsigned cnt)
{
	/* See [IA32-v2a] "INS". */
	asm volatile("rep insw" : "+D"(addr), "+c"(cnt) : "d"(port) : "memory");
}

/* Reads and returns 32 bits from PORT. */
unsigned int _read_dword(unsigned short port)
{
	/* See [IA32-v2a] "IN". */
	unsigned int data;
	asm volatile("inl %w1, %0" : "=a"(data) : "Nd"(port));
	return data;
}

/* Reads CNT 32-bit (word) units from PORT, one after another,
   and stores them into the buffer starting at ADDR. */
void _read_dwb(unsigned short port, void *addr, unsigned cnt)
{
	/* See [IA32-v2a] "INS". */
	asm volatile("rep insl" : "+D"(addr), "+c"(cnt) : "d"(port) : "memory");
}

/* Writes to PORT each byte of data in the CNT-byte buffer
   starting at ADDR. */
void _write_sb(unsigned short port, const void *addr, unsigned cnt)
{
	/* See [IA32-v2b] "OUTS". */
	asm volatile("rep outsb" : "+S"(addr), "+c"(cnt) : "d"(port));
}

/* Writes the 16-bit DATA to PORT. */
void _write_word(unsigned short port, unsigned short data)
{
	/* See [IA32-v2b] "OUT". */
	asm volatile("outw %w0, %w1" : : "a"(data), "Nd"(port));
}

/* Writes to PORT each 16-bit unit (halfword) of data in the
   CNT-halfword buffer starting at ADDR. */
void _write_wb(unsigned short port, const void *addr, unsigned cnt)
{
	/* See [IA32-v2b] "OUTS". */
	asm volatile("rep outsw" : "+S"(addr), "+c"(cnt) : "d"(port));
}

/* Writes the 32-bit DATA to PORT. */
void _write_dword(unsigned short port, unsigned int data)
{
	/* See [IA32-v2b] "OUT". */
	asm volatile("outl %0, %w1" : : "a"(data), "Nd"(port));
}

/* Writes to PORT each 32-bit unit (word) of data in the CNT-word
   buffer starting at ADDR. */
void _write_dwb(unsigned short port, const void *addr, unsigned cnt)
{
	/* See [IA32-v2b] "OUTS". */
	asm volatile("rep outsl" : "+S"(addr), "+c"(cnt) : "d"(port));
}
