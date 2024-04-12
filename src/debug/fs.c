#include <mount.h>
#include <debugfs.h>
#include <timer.h>

extern unsigned cache_hit;
extern unsigned long long cache_search_time;
extern unsigned cache_search_count;
extern unsigned max_cache_size;

static void fill(void *buf, size_t size)
{
	unsigned rate = cache_hit * 100 / cache_search_count;
	memset(buf, 0, size);
	sprintf(buf,
		"HDD cache hit rate:  %d%%\n"
		"HDD cache spent:     %d.%d ms\n"
		"HDD cache max depth: %d\n"
		"\n",
		rate, (int)cache_search_time / 1000,
		(int)cache_search_time % 1000, max_cache_size);
	cache_hit = cache_search_count = 0;
}

static void fsinfo_timeout(timer_t *timer, void *ctx)
{
	cache_hit = cache_search_count = 0;
}

void debugfs_fs_init(mount_point *mp)
{
	mount_add_file(mp, "/proc/fsinfo", fill);
	timer_start(fsinfo_timeout, 2000, 1, NULL);
}
