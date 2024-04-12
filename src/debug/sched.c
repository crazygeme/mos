#include <mount.h>
#include <debugfs.h>
#include <timer.h>

extern unsigned long long task_schedule_time;
extern unsigned task_schedule_count;
extern unsigned timer_wakeup_times;
extern unsigned timer_process_times;
unsigned total_timer_process_times = 0;

static void fill(void *buf, size_t size)
{
	memset(buf, 0, size);
	sprintf(buf,
		"Schedule called times:      %d\n"
		"Schedule call spent:        %d.%d ms\n"
		"Timer wakeup times:         %d\n"
		"Timer process times:        %d\n"
		"Timer process times(Total): %d\n"
		"\n",
		task_schedule_count, (int)task_schedule_time / 1000,
		(int)task_schedule_time % 1000, timer_wakeup_times,
		timer_process_times, total_timer_process_times);
}

static void schedinfo_timeout(timer_t *timer, void *ctx)
{
	total_timer_process_times += timer_process_times;
	task_schedule_count = task_schedule_time = 0;
	timer_wakeup_times = timer_process_times = 0;
}

void debugfs_sched_init(mount_point *mp)
{
	mount_add_file(mp, "/proc/sched", fill);
	timer_start(schedinfo_timeout, 2000, 1, NULL);
}
