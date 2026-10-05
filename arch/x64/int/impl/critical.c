#include <int/interrupt.h>
#include <ps/smp.h>
#include <macro.h>
#include <lib/port.h>
/* Critical interrupts do not enter the scheduler or acquire subsystem locks.
 * Their IST stack is independent of an interrupted SYSCALL's stack switch. */
void arch_critical_interrupt(arch_intr_frame *frame)
{
	if (frame->vec_no == 2) {
		smp_tlb_poll();
		return;
	}
	const char *message = frame->vec_no == 8 ? "MOS: double fault\r\n" :
						   "MOS: machine check\r\n";
	while (*message) {
		while (!(port_read_byte(0x3fd) & 0x20))
			PAUSE();
		OUT_PORT(0x3f8, *message++);
	}
	DISABLE_INTR();
	for (;;)
		HLT();
}
