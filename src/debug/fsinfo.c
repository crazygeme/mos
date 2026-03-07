#include <block.h>
#include <mount.h>
#include <debugfs.h>
#include <timer.h>

extern unsigned cache_hit;
extern unsigned long long cache_search_time;
extern unsigned cache_search_count;
extern unsigned fs_cache_size;
extern unsigned max_fs_cache_size;
extern unsigned fs_read_size;
extern unsigned fs_write_size;
extern unsigned fs_cache_read_size;
extern unsigned fs_cache_write_size;
extern unsigned disk_read_size;
extern unsigned disk_write_size;
extern unsigned long long elf_read_time;
unsigned fs_read_size_total = 0;
unsigned fs_write_size_total = 0;
unsigned fs_cache_read_size_total = 0;
unsigned fs_cache_write_size_total = 0;
unsigned cache_search_count_total = 0;
unsigned cache_search_time_total = 0;
unsigned disk_read_size_total = 0;
unsigned disk_write_size_total = 0;
unsigned long long elf_read_time_total = 0;

static void fill(void *buf, size_t size)
{
	unsigned rate = 0;
	rate = cache_search_count ? cache_hit * 100 / cache_search_count : 0;
	fs_read_size_total += fs_read_size;
	fs_write_size_total += fs_write_size;
	fs_cache_read_size_total += fs_cache_read_size;
	fs_cache_write_size_total += fs_cache_write_size;
	disk_read_size_total += disk_read_size;
	disk_write_size_total += disk_write_size;
	cache_search_time_total += cache_search_count;
	cache_search_count_total += cache_search_count;
	elf_read_time_total += elf_read_time;
	memset(buf, 0, size);
	sprintf(buf,
		"ELF read spent:              %d.%d ms\n"
		"ELF read spent(Total):       %d.%d ms\n"
		"FS read size:                %h\n"
		"FS read size(Total):         %h\n"
		"FS write size:               %h\n"
		"FS write size(Total):        %h\n"
		"FS cache read size:          %h\n"
		"FS cache read size(Total):   %h\n"
		"FS cache write size:         %h\n"
		"FS cache write size(Total):  %h\n"
		"Disk read size:              %h\n"
		"Disk read size(Total):       %h\n"
		"Disk write size:             %h\n"
		"Disk write size(Total):      %h\n"
		"FS cache hit rate:           %d%%\n"
		"FS cache search:             %d\n"
		"FS cache search(Total):      %d\n"
		"FS cache spent:              %d.%d ms\n"
		"FS cache spent(Total):       %d.%d ms\n"
		"FS cache size(Current):      %h\n"
		"FS cache size(Max):          %h\n"
		"\n\n",
		(int)elf_read_time / 1000, (int)elf_read_time % 1000,
		(int)elf_read_time_total / 1000,
		(int)elf_read_time_total % 1000, fs_read_size,
		fs_read_size_total, fs_write_size, fs_write_size_total,
		fs_cache_read_size, fs_cache_read_size_total,
		fs_cache_write_size, fs_cache_write_size_total, disk_read_size,
		disk_read_size_total, disk_write_size, disk_write_size_total,
		rate, cache_search_count, cache_search_count_total,
		(int)cache_search_time / 1000, (int)cache_search_time % 1000,
		(int)cache_search_time_total / 1000,
		(int)cache_search_time_total % 1000,
		fs_cache_size * BLOCK_SECTOR_SIZE,
		max_fs_cache_size * BLOCK_SECTOR_SIZE);
	cache_hit = cache_search_count = cache_search_time = 0;
	fs_cache_read_size = fs_cache_write_size = fs_read_size =
		fs_write_size = disk_read_size = disk_write_size = 0;
	elf_read_time = 0;
}

void debugfs_fs_init(super_block *mp)
{
	vfs_create_file(mp, "/proc/fsinfo", fill);
}

DEBUGFS_INIT(debugfs_fs_init);
