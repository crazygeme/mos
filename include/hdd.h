#ifndef _BLOCK_H_
#define _BLOCK_H_
#include <fs.h>

/* Size of a block device sector in bytes.
   All IDE disks use this sector size, as do most USB and SCSI
   disks.  It's not worth it to try to cater to other sector
   sizes in Pintos (yet). */
#define BLOCK_SECTOR_SIZE 512

/* Name of the first registered ext4 block device (set by hdd.c). */
extern char hdd_first_dev_name[32];

/* Flush all partition caches and release block device resources. */
void hdd_close(void);

#endif
