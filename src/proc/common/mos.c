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
#include <hw/hdd.h>
#include "common.h"

/* ---- mm/fault ---- */
extern unsigned page_fault_cow;
extern unsigned page_fault_invalid;
extern unsigned page_fault_file;
extern unsigned page_fault_file_cache_hit;
extern unsigned page_fault_file_read;
extern unsigned page_fault_perm;
extern unsigned long long page_fault_cow_spent;
extern unsigned long long page_fault_invalid_spent;
extern unsigned long long page_fault_file_spent;
extern unsigned long long page_fault_file_search_spent;
extern unsigned long long page_fault_perm_spent;
extern unsigned int heap_quota;
extern unsigned int heap_quota_high;

/* ---- fs/cache ---- */
extern unsigned fs_read_size;
extern unsigned fs_write_size;
extern unsigned disk_read_size;
extern unsigned disk_write_size;
extern unsigned long long elf_read_time;

#if HDD_CACHE_OPEN
extern unsigned cache_hit;
extern unsigned fs_cache_read_size;
extern unsigned fs_cache_write_size;
extern unsigned fs_cache_size;
extern unsigned max_fs_cache_size;
extern unsigned cache_search_count;
extern unsigned long long cache_search_time;
#endif

/* ---- scheduler ---- */
extern unsigned task_schedule_count;

static void fill(proc_buf_t *pb)
{
	unsigned pf_cache_rate =
		page_fault_file ?
			page_fault_file_cache_hit * 100 / page_fault_file :
			0;
#if HDD_CACHE_OPEN
	unsigned fs_cache_rate =
		cache_search_count ? cache_hit * 100 / cache_search_count : 0;
#endif
	/* ---- Memory / kernel heap ---- */
	proc_buf_printf(pb, "KernelHeapBytes:       %8u\n", heap_quota);
	proc_buf_printf(pb, "KernelHeapPeakBytes:   %8u\n", heap_quota_high);

	/* ---- Page fault counters ---- */
	proc_buf_printf(pb, "PfCow:                 %8u\n", page_fault_cow);
	proc_buf_printf(pb, "PfCowTimeUs:           %8u\n",
			(unsigned)page_fault_cow_spent);
	proc_buf_printf(pb, "PfInvalid:             %8u\n", page_fault_invalid);
	proc_buf_printf(pb, "PfInvalidTimeUs:       %8u\n",
			(unsigned)page_fault_invalid_spent);
	proc_buf_printf(pb, "PfFile:                %8u\n", page_fault_file);
	proc_buf_printf(pb, "PfFileTimeUs:          %8u\n",
			(unsigned)page_fault_file_spent);
	proc_buf_printf(pb, "PfFileSearchTimeUs:    %8u\n",
			(unsigned)page_fault_file_search_spent);
	proc_buf_printf(pb, "PfFileCacheHit:        %8u (%u%%)\n",
			page_fault_file_cache_hit, pf_cache_rate);
	proc_buf_printf(pb, "PfFileRead:            %8u\n",
			page_fault_file_read);
	proc_buf_printf(pb, "PfPerm:                %8u\n", page_fault_perm);
	proc_buf_printf(pb, "PfPermTimeUs:          %8u\n",
			(unsigned)page_fault_perm_spent);

	/* ---- Filesystem I/O ---- */
	proc_buf_printf(pb, "FsReadBytes:           %8u\n", fs_read_size);
	proc_buf_printf(pb, "FsWriteBytes:          %8u\n", fs_write_size);
#if HDD_CACHE_OPEN
	proc_buf_printf(pb, "FsCacheReadBytes:      %8u\n", fs_cache_read_size);
	proc_buf_printf(pb, "FsCacheWriteBytes:     %8u\n",
			fs_cache_write_size);
	proc_buf_printf(pb, "FsCacheBytes:          %8u\n",
			fs_cache_size * BLOCK_SECTOR_SIZE);
	proc_buf_printf(pb, "FsCacheMaxBytes:       %8u\n",
			max_fs_cache_size * BLOCK_SECTOR_SIZE);
	proc_buf_printf(pb, "FsCacheSearches:       %8u\n", cache_search_count);
	proc_buf_printf(pb, "FsCacheHits:           %8u (%u%%)\n", cache_hit,
			fs_cache_rate);
	proc_buf_printf(pb, "FsCacheSearchTimeUs:   %8u\n",
			(unsigned)cache_search_time);
#endif
	proc_buf_printf(pb, "DiskReadBytes:         %8u\n", disk_read_size);
	proc_buf_printf(pb, "DiskWriteBytes:        %8u\n", disk_write_size);
	proc_buf_printf(pb, "ElfLoadTimeUs:         %8u\n",
			(unsigned)elf_read_time);

	/* ---- Scheduler ---- */
	proc_buf_printf(pb, "SchedCalls:            %8u\n",
			task_schedule_count);
}

static int mos_getattr(inode *inode, struct stat *s)
{
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
		TestControl.verbos = level;
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

static inode_operations mos_iops = {
	.getattr = mos_getattr,
};

static file_operations mos_fops = {
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
	node->i_op = &mos_iops;
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
