#include "common.h"

static void fill(proc_buf_t *pb)
{
	proc_buf_printf(pb, "fsck.mode=skip fastboot");
}

DEFINE_PROC_FILE(cmdline, fill);
