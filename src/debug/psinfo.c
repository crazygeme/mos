#include <fs/mount.h>
#include <fs/super/debugfs.h>
#include <ps/ps.h>

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

static void debugfs_ps_init(super_block *mp)
{
	vfs_create_file(mp, "/proc/self", fill);
}

DEBUGFS_INIT(debugfs_ps_init);