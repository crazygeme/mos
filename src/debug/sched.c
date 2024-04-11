#include <mount.h>
#include <debugfs.h>

extern unsigned task_schedule_time;
extern unsigned task_schedule_count;

static void fill(void *buf, size_t size)
{
	memset(buf, 0, size);
	sprintf(buf,
		"Schedule called times:      %d\n"
		"Schedule call spent:        %d ms\n"
		"\n",
		task_schedule_count, task_schedule_time);
}

void debugfs_sched_init(mount_point *mp)
{
	mount_add_file(mp, "/proc/sched", fill);
}
