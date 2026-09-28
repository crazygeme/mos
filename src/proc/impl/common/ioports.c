#include "common.h"

/* Fixed x86 platform I/O resources used by the kernel. */
static void fill(proc_buf_t *pb)
{
	proc_buf_printf(pb, "0020-0021 : pic1\n"
			    "0040-0043 : timer\n"
			    "0060-0060 : keyboard\n"
			    "0064-0064 : keyboard\n"
			    "0070-0071 : rtc\n"
			    "00a0-00a1 : pic2\n"
			    "0cf8-0cff : PCI conf1\n");
}

DEFINE_PROC_FILE(ioports, fill);
