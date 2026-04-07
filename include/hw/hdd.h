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

int hdd_partition_total(void);
const char *hdd_partition_name(unsigned idx);
unsigned hdd_partition_size_sectors(unsigned idx);
int hdd_partition_read(unsigned idx, unsigned sector, void *buf, unsigned len);
int hdd_partition_write(unsigned idx, unsigned sector, void *buf, unsigned len);

void hdd_flush();

/* Flush all partition caches and release block device resources. */
void hdd_close(void);

#endif
