/*
 * /proc/mos — MOS kernel internals snapshot and control toggle.
 *
 * Reads return the same kernel counters snapshot as before.
 * Writes accept:
 *   "verbose"    -> TestControl.verbos = TEST_LOG_INFO
 *   "verbose=N"  -> TestControl.verbos = N
 */
#include <errno.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <hw/hdd.h>
#include "common.h"

/* ---- mm/fault ---- */
extern unsigned page_fault_cow;
extern unsigned page_fault_invalid;
extern unsigned page_fault_file;
extern unsigned page_fault_file_cache_hit;
extern unsigned page_fault_file_read;
extern unsigned page_fault_perm;
extern unsigned int heap_quota;
extern unsigned int heap_quota_high;

/* ---- fs/cache ---- */
extern unsigned fs_read_size;
extern unsigned fs_write_size;
extern unsigned disk_read_size;
extern unsigned disk_write_size;
extern unsigned fs_page_cache_pages;
extern unsigned fs_page_cache_max_pages;
extern unsigned fs_page_cache_searches;
extern unsigned fs_page_cache_hits;

#if HDD_CACHE_OPEN
extern unsigned hdd_cache_hit;
extern unsigned hdd_cache_read_size;
extern unsigned hdd_cache_write_size;
extern unsigned hdd_cache_size;
extern unsigned hdd_cache_max_size;
extern unsigned hdd_cache_search_count;
#endif

/* ---- scheduler ---- */
extern unsigned task_schedule_count;

#define MOS_TABLE_LINE \
	"+------------------------+--------------+--------------+--------------+\n"

static void mos_table_begin(proc_buf_t *pb, const char *title)
{
	proc_buf_printf(pb, "%s\n", title);
	proc_buf_printf(pb, MOS_TABLE_LINE);
	proc_buf_printf(pb, "| %-22s | %12s | %12s | %-12s |\n", "Metric",
			"Value", "Raw", "Notes");
	proc_buf_printf(pb, MOS_TABLE_LINE);
}

static void mos_table_end(proc_buf_t *pb)
{
	proc_buf_printf(pb, MOS_TABLE_LINE);
	proc_buf_printf(pb, "\n");
}

static void mos_print_row(proc_buf_t *pb, const char *name, const char *value,
			  unsigned raw, const char *notes)
{
	proc_buf_printf(pb, "| %-22s | %12s | %12u | %-12s |\n", name, value,
			raw, notes);
}

static void mos_print_bytes(proc_buf_t *pb, const char *name, unsigned bytes)
{
	char value[32];

	sprintf(value, "%h", bytes);
	mos_print_row(pb, name, value, bytes, "bytes");
}

static void mos_print_count(proc_buf_t *pb, const char *name, unsigned count)
{
	char value[32];

	sprintf(value, "%h", count);
	mos_print_row(pb, name, value, count, "count");
}

static void mos_print_count_rate(proc_buf_t *pb, const char *name,
				 unsigned count, unsigned rate)
{
	char value[32];
	char notes[16];

	sprintf(value, "%h", count);
	sprintf(notes, "%u%% hit", rate);
	mos_print_row(pb, name, value, count, notes);
}

static void mos_print_usage(proc_buf_t *pb, const char *name, unsigned pages,
			    unsigned total_pages)
{
	unsigned bytes = pages * PAGE_SIZE;
	unsigned pct = total_pages ? pages * 100 / total_pages : 0;
	char value[32];
	char notes[16];

	sprintf(value, "%h", bytes);
	sprintf(notes, "%u%% used", pct);
	mos_print_row(pb, name, value, bytes, notes);
}

static void fill(proc_buf_t *pb)
{
	phymm_usage mem;
	unsigned pf_cache_rate =
		page_fault_file ?
			page_fault_file_cache_hit * 100 / page_fault_file :
			0;
	unsigned inode_cache_rate =
		fs_page_cache_searches ?
			fs_page_cache_hits * 100 / fs_page_cache_searches :
			0;
#if HDD_CACHE_OPEN
	unsigned hdd_cache_rate =
		hdd_cache_search_count ?
			hdd_cache_hit * 100 / hdd_cache_search_count :
			0;
#endif
	phymm_get_usage(&mem);

	/* ---- Memory / kernel heap ---- */
	mos_table_begin(pb, "Memory");
	mos_print_bytes(pb, "kmalloc current", heap_quota);
	mos_print_bytes(pb, "kmalloc peak", heap_quota_high);
	mos_print_bytes(pb, "low phys total", mem.low_total_pages * PAGE_SIZE);
	mos_print_usage(pb, "low phys used", mem.low_used_pages,
			mem.low_total_pages);
	mos_print_bytes(pb, "low phys free", mem.low_free_pages * PAGE_SIZE);
	mos_print_bytes(pb, "high phys total",
			mem.high_total_pages * PAGE_SIZE);
	mos_print_usage(pb, "high phys used", mem.high_used_pages,
			mem.high_total_pages);
	mos_print_bytes(pb, "high phys free", mem.high_free_pages * PAGE_SIZE);
	mos_table_end(pb);

	/* ---- Page fault counters ---- */
	mos_table_begin(pb, "Page faults");
	mos_print_count(pb, "COW", page_fault_cow);
	mos_print_count(pb, "invalid", page_fault_invalid);
	mos_print_count(pb, "file-backed", page_fault_file);
	mos_print_count_rate(pb, "file cache hit", page_fault_file_cache_hit,
			     pf_cache_rate);
	mos_print_count(pb, "file read", page_fault_file_read);
	mos_print_count(pb, "permission", page_fault_perm);
	mos_table_end(pb);

	/* ---- Filesystem I/O ---- */
	mos_table_begin(pb, "I/O");
	mos_print_bytes(pb, "fs read", fs_read_size);
	mos_print_bytes(pb, "fs write", fs_write_size);
	mos_print_bytes(pb, "disk read", disk_read_size);
	mos_print_bytes(pb, "disk write", disk_write_size);
	mos_table_end(pb);

	/* ---- Inode / filesystem page cache ---- */
	mos_table_begin(pb, "Inode/filesystem page cache");
	mos_print_bytes(pb, "current", fs_page_cache_pages * PAGE_SIZE);
	mos_print_bytes(pb, "peak", fs_page_cache_max_pages * PAGE_SIZE);
	mos_print_count(pb, "lookups", fs_page_cache_searches);
	mos_print_count_rate(pb, "hits", fs_page_cache_hits, inode_cache_rate);
	mos_table_end(pb);

#if HDD_CACHE_OPEN
	/* ---- HDD block cache ---- */
	mos_table_begin(pb, "HDD block cache");
	mos_print_bytes(pb, "cached sectors",
			hdd_cache_size * BLOCK_SECTOR_SIZE);
	mos_print_bytes(pb, "peak sectors",
			hdd_cache_max_size * BLOCK_SECTOR_SIZE);
	mos_print_bytes(pb, "read served", hdd_cache_read_size);
	mos_print_bytes(pb, "write served", hdd_cache_write_size);
	mos_print_count(pb, "lookups", hdd_cache_search_count);
	mos_print_count_rate(pb, "hits", hdd_cache_hit, hdd_cache_rate);
	mos_table_end(pb);
#endif

	/* ---- Scheduler ---- */
	mos_table_begin(pb, "Scheduler");
	mos_print_count(pb, "schedule calls", task_schedule_count);
	mos_table_end(pb);
}

static int mos_getattr(file *file, struct stat *s)
{
	inode *inode = file->f_inode;
	unsigned long now = time_now_sec();

	s->st_atime = now;
	s->st_mode = inode->i_mode;
	s->st_size = (loff_t)inode->i_size;
	s->st_blksize = PAGE_SIZE;
	s->st_blocks = 0;
	s->st_ctime = now;
	s->st_dev = 5;
	s->st_gid = 0;
	s->st_ino = (unsigned long)inode;
	s->st_mtime = now;
	s->st_uid = 0;
	s->st_nlink = 1;
	return 0;
}

static int mos_release(file *file)
{
	proc_buf_free(file->f_inode->i_private);
	free(file->f_inode);
	free(file);
	return 0;
}

static ssize_t mos_read(file *file, void *buf, size_t size, loff_t *pos)
{
	loff_t fsize = (loff_t)file->f_inode->i_size;
	loff_t offset = *pos;
	ssize_t left = (ssize_t)(fsize - offset);
	ssize_t read_size = (ssize_t)size > left ? left : (ssize_t)size;
	proc_buf_t *pb = file->f_inode->i_private;

	if (read_size <= 0)
		return 0;
	memcpy(buf, (char *)pb->buf + offset, (size_t)read_size);
	*pos = offset + read_size;
	return read_size;
}

static ssize_t mos_write(file *file, const void *buf, size_t size, loff_t *pos)
{
	char value[16];
	unsigned len = (unsigned)size;
	unsigned i;

	(void)file;
	(void)pos;

	if (len >= sizeof(value))
		len = sizeof(value) - 1;

	for (i = 0; i < len; i++)
		value[i] = ((const char *)buf)[i];
	while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r' ||
			   value[len - 1] == ' ' || value[len - 1] == '\t'))
		len--;
	value[len] = '\0';

	{
		int level;

		if (strcmp(value, "verbose") == 0)
			level = TEST_LOG_INFO;
		else if (strncmp(value, "verbose=", 8) == 0)
			level = atoi(value + 8);
		else
			return -EINVAL;

		if (level < TEST_LOG_OFF || level > TEST_LOG_INFO)
			return -EINVAL;
		TestControl.verbose = level;
	}

	return (ssize_t)size;
}

static loff_t mos_llseek(file *file, loff_t offset, int whence)
{
	loff_t fsize = (loff_t)file->f_inode->i_size;
	loff_t newpos;

	switch (whence) {
	case SEEK_SET:
		newpos = offset;
		break;
	case SEEK_CUR:
		newpos = file->f_pos + offset;
		break;
	case SEEK_END:
		newpos = fsize + offset;
		break;
	default:
		return -EINVAL;
	}

	if (newpos < 0)
		return -EINVAL;

	file->f_pos = newpos;
	return newpos;
}

static file_operations mos_fops = {
	.getattr = mos_getattr,
	.release = mos_release,
	.read = mos_read,
	.write = mos_write,
	.llseek = mos_llseek,
};

static file *mos_open_root(super_block *sb, int flag)
{
	proc_buf_t *pb = proc_buf_new();
	inode *node;
	file *fp;

	(void)sb;
	(void)flag;

	fill(pb);

	node = zalloc(sizeof(*node));
	node->i_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR |
		       S_IWGRP | S_IWOTH;
	node->i_private = pb;
	node->i_size = pb->len;

	fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &mos_fops;
	return fp;
}

static super_operations mos_sops = {
	.open_root = mos_open_root,
};

static void mos_proc_register(super_block *proc_sb)
{
	vfs_mount(proc_sb, "/mos", sget(&mos_sops));
}

PROC_INIT(mos_proc_register);
