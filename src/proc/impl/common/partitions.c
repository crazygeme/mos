#include <device/blockdev.h>
#include "common.h"

static void show_partition(const blockdev_info *device, void *data)
{
	proc_buf_t *buffer = data;
	proc_buf_printf(buffer, "%4u %5u %9llu %s\n", device->major,
			device->minor, device->size_bytes / 1024, device->name);
}

static void fill(proc_buf_t *buffer)
{
	proc_buf_printf(buffer, "major minor  #blocks  name\n\n");
	blockdev_for_each(show_partition, buffer);
}
DEFINE_PROC_FILE(partitions, fill);
