#include <config.h>
#include "generic.h"

static void fill(void *buf, size_t size)
{
	sprintf(buf, "%s version %s %s (%s)\n",
		UTS_SYSNAME, UTS_RELEASE, UTS_VERSION, UTS_MACHINE);
}

DEFINE_PROC_FILE(version, fill);
