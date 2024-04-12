#include <mount.h>
#include <debugfs.h>

static void fill(void *buf, size_t size)
{
}

void debugfs_cpu_init(mount_point *mp)
{
	mount_add_file(mp, "/proc/cpuinfo", fill);
}
