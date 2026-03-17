#ifndef _HW_HDD_H_
#define _HW_HDD_H_
#include <fs/fs.h>

/* Size of a block device sector in bytes.
   All IDE disks use this sector size, as do most USB and SCSI
   disks.  It's not worth it to try to cater to other sector
   sizes in Pintos (yet). */
#define BLOCK_SECTOR_SIZE 512

/* Maximum number of partitions tracked across all disks. */
#define HDD_MAX_PARTITIONS 16

/*
 * hdd_partition_info - public descriptor for one disk partition.
 *
 * Filled by hdd.c during partition scanning and consumed by hdd_dev.c to
 * create /dev/hdXN block device nodes.
 */
typedef struct {
	char name[32]; /* e.g. "hda1", "hdb2" */
	unsigned int size; /* partition size in sectors */
	unsigned char part_type; /* MBR partition type byte */
	void *aux; /* opaque handle passed to read/write */
	int (*read)(void *aux, unsigned sector, void *buf, unsigned len);
	int (*write)(void *aux, unsigned sector, void *buf, unsigned len);
} hdd_partition_info;

extern hdd_partition_info hdd_partitions[HDD_MAX_PARTITIONS];
extern int hdd_partition_count;

void hdd_flush();

/* Flush all partition caches and release block device resources. */
void hdd_close(void);

#endif
