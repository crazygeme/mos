#include "common.h"

static void fill(void *buf, size_t size)
{
	strcpy(buf, "fsck.mode=skip fastboot");
}

DEFINE_PROC_FILE(cmdline, fill);
