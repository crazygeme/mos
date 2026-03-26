#include <config.h>
#include "common.h"

static void fill(proc_buf_t *pb)
{
	proc_buf_printf(pb, "%s\n", UTS_RELEASE);
}

DEFINE_PROC_FILE_AT("/sys/kernel/osrelease", osrelease, fill);
