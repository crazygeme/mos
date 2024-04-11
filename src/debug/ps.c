#include <mount.h>
#include <ps.h>
#include <debugfs.h>

static void fill(void *buf, size_t size)
{
	task_struct *cur = CURRENT_TASK();
	memset(buf, 0, size);
	sprintf(buf,
		"id:      %d\n"
		"command: %s\n"
		"cwd:     %s\n"
		"\n",
		cur->psid, cur->command, cur->cwd);
}
void debugfs_ps_init(mount_point *mp)
{
	mount_add_file(mp, "/proc/self", fill);
}