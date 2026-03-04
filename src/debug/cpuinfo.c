#include <mount.h>
#include <debugfs.h>

static void fill(void *buf, size_t size)
{
}

void debugfs_cpu_init(super_block *mp)
{
	vfs_create_file(mp, "/proc/cpuinfo", fill);
}

DEBUGFS_INIT(debugfs_cpu_init);
