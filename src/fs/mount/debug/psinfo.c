#include <ps/ps.h>
#include "generic.h"

static void fill(void *buf, size_t size)
{
	task_struct *cur = CURRENT_TASK();
	memset(buf, 0, size);
	sprintf(buf,
		"id:   %d\n"
		"cmd:  %s\n"
		"cwd:  %s\n"
		"\n\n",
		cur->psid, cur->command, cur->cwd);
}

DEFINE_DEBUG_FS_FILE(psinfo, fill);
