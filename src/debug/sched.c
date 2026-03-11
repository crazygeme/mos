#include <mount.h>
#include <debugfs.h>
#include <timer.h>

extern unsigned long long task_schedule_time;
extern unsigned task_schedule_count;
extern unsigned timer_wakeup_times;
extern unsigned timer_process_times;
extern unsigned select_loop_times;
unsigned timer_process_times_total = 0;
unsigned select_loop_times_total = 0;
unsigned long long task_schedule_time_total = 0;

static void fill(void *buf, size_t size)
{
	timer_process_times_total += timer_process_times;
	select_loop_times_total += select_loop_times;
	task_schedule_time_total += task_schedule_time;
	memset(buf, 0, size);
	sprintf(buf,
		"Schedule called times:         %d\n"
		"Schedule call spent:           %d.%d ms\n"
		"Schedule call spent(Total):    %d.%d ms\n"
		"Select loop times:             %d\n"
		"Select loop times(Total):      %d\n"
		"Timer wakeup times:            %d\n"
		"Timer process times:           %d\n"
		"Timer process times(Total):    %d\n"
		"\n\n",
		task_schedule_count, (int)task_schedule_time / 1000,
		(int)task_schedule_time % 1000,
		(int)task_schedule_time_total / 1000,
		(int)task_schedule_time_total % 1000, select_loop_times,
		select_loop_times_total, timer_wakeup_times,
		timer_process_times, timer_process_times_total);
	task_schedule_count = task_schedule_time = 0;
	timer_wakeup_times = timer_process_times = 0;
	select_loop_times = 0;
}

static void debugfs_sched_init(super_block *mp)
{
	vfs_create_file(mp, "/proc/sched", fill);
}

DEBUGFS_INIT(debugfs_sched_init);
