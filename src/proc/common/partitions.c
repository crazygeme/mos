/*
 * /proc/partitions — list of block device partitions.
 *
 * Format matches Linux /proc/partitions:
 *   major minor  #blocks  name
 *
 * One whole-disk row is synthesised per unique disk (e.g. "hda") with minor=0
 * and size = sum of its partitions.  Partition rows follow with minor 1..N.
 *
 * Sector → 1 KiB-block conversion: blocks = sectors / 2.
 */
#include <hw/hdd.h>
#include <dev/loopdev.h>
#include "common.h"

#define HDD_MAJOR 3
#define LOOP_MAJOR 7

/*
 * Disk-to-minor-base mapping (Linux IDE convention):
 *   hda → 0, hdb → 64, hdc → 128, hdd → 192
 */
static int disk_minor_base(const char *disk)
{
	/* disk is "hda", "hdb", etc. */
	if (disk[2] >= 'a' && disk[2] <= 'd')
		return (disk[2] - 'a') * 64;
	return 0;
}

/* Strip trailing digits from a partition name to get the disk name. */
static void part_to_disk(const char *part, char *disk, size_t dsize)
{
	size_t n = 0;
	size_t i;

	while (part[n])
		n++;

	/* copy all but trailing decimal digits */
	i = n;
	while (i > 0 && part[i - 1] >= '0' && part[i - 1] <= '9')
		i--;

	if (i == 0 || i >= dsize)
		i = dsize - 1;

	memcpy(disk, part, i);
	disk[i] = '\0';
}

/* Return the partition number suffix (e.g. "hda1" → 1). */
static int part_number(const char *name)
{
	int n = 0;
	while (*name && (*name < '0' || *name > '9'))
		name++;
	while (*name >= '0' && *name <= '9')
		n = n * 10 + (*name++ - '0');
	return n;
}

static void fill(proc_buf_t *pb)
{
	char prev_disk[32] = "";
	char disk[32];
	unsigned disk_sectors;
	int base, i;

	proc_buf_printf(pb, "major minor  #blocks  name\n\n");

	for (i = 0; i < hdd_partition_total(); i++) {
		const char *part_name = hdd_partition_name(i);
		unsigned part_size = hdd_partition_size_sectors(i);

		part_to_disk(part_name, disk, sizeof(disk));

		/* Emit whole-disk row when the disk name changes. */
		if (strcmp(disk, prev_disk) != 0) {
			int j;
			disk_sectors = 0;
			for (j = i; j < hdd_partition_total(); j++) {
				char d2[32];
				part_to_disk(hdd_partition_name(j), d2,
					     sizeof(d2));
				if (strcmp(d2, disk) != 0)
					break;
				disk_sectors += hdd_partition_size_sectors(j);
			}
			base = disk_minor_base(disk);
			proc_buf_printf(pb, "%4d %5d %9u %s\n", HDD_MAJOR, base,
					disk_sectors / 2, disk);
			memcpy(prev_disk, disk, sizeof(prev_disk));
		}

		base = disk_minor_base(disk);
		proc_buf_printf(pb, "%4d %5d %9u %s\n", HDD_MAJOR,
				base + part_number(part_name), part_size / 2,
				part_name);
	}

	/* Loop devices — one row each (no synthetic whole-disk row) */
	for (i = 0; i < LOOP_MAX_DEVS; i++) {
		if (!loop_devs[i].name[0])
			continue;
		proc_buf_printf(pb, "%4d %5d %9llu %s\n", LOOP_MAJOR, i,
				loop_devs[i].size_bytes / 1024,
				loop_devs[i].name);
	}
}

DEFINE_PROC_FILE(partitions, fill);
