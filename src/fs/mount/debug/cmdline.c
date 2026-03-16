#include "generic.h"

static void fill(void *buf, size_t size)
{
	strcpy(buf, "fsck.mode=skip fastboot");
}

DEFINE_DEBUG_FS_FILE(cmdline, fill);