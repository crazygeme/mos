#include <config.h>
#include "common.h"

static void fill(proc_buf_t *pb)
{
	proc_buf_printf(pb, "%s version %s %s (%s)\n", UTS_SYSNAME, UTS_RELEASE,
			UTS_VERSION, UTS_MACHINE);
}

DEFINE_PROC_FILE(version, fill);
