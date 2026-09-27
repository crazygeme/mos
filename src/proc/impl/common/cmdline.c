#include "common.h"
#include <mm/mm.h>

static void fill(proc_buf_t *pb)
{
	proc_buf_printf(pb, "%s\n", g_cmdline);
}

DEFINE_PROC_FILE(cmdline, fill);
