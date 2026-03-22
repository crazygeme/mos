#include <config.h>
#include "generic.h"

static void fill(void *buf, size_t size)
{
	sprintf(buf, "%s\n", UTS_RELEASE);
}

DEFINE_PROC_FILE_AT("/sys/kernel/osrelease", osrelease, fill);
