#ifndef _BLOCK_H_
#define _BLOCK_H_
#include <lib/list.h>

/* Size of a block device sector in bytes.
   All IDE disks use this sector size, as do most USB and SCSI
   disks.  It's not worth it to try to cater to other sector
   sizes in Pintos (yet). */
#define BLOCK_SECTOR_SIZE 512

#define BLOCK_BYTE_SIZE(b) \
	(b->sector_size * BLOCK_SECTOR_SIZE)

typedef int (*fpblock_read)(void* aux, unsigned sector, void* buf, unsigned len);

typedef int (*fpblock_write)(void* aux, unsigned sector, void* buf, unsigned len);

typedef enum _block_type
{
	BLOCK_KERNEL,
	BLOCK_FILESYS,
    BLOCK_LINUX,
	BLOCK_SCRATCH,
	BLOCK_SWAP,
	BLOCK_RAW,
	BLOCK_UNKNOW,
	BLOCK_MAX
}block_type;

typedef struct _block
{
	unsigned int id;
	char name[16];
	void* aux;
	fpblock_read read;
	fpblock_write write;
	block_type type;
	unsigned int sector_size;
	LIST_ENTRY block_list;
}block;


void block_init();

block* block_register(void* aux, char* name, fpblock_read read, fpblock_write write, 
			block_type type, unsigned int sector_size);

block* block_get_by_id(unsigned int id);

unsigned int block_get_by_type(block_type type, block** blocks);


#endif
