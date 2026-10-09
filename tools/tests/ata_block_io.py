#!/usr/bin/env python3
"""Check the real ATA filesystem callbacks with simulated sector transfers."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/driver/storage/ata.c').read_text()
start = source.index('static int ata_block_read(')
end = source.index('static const blockdev_io ata_block_ops', start)
functions = source[start:end]
prelude = r'''
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#define HDD_CACHE_OPEN 1
#define BLOCK_SECTOR_SIZE 512
#define HDD_IO_MAX_SECTORS 8
typedef struct { int unused; } partition;
typedef struct { unsigned size; void *aux; int (*read)(void *,unsigned,void *,unsigned); int (*write)(void *,unsigned,void *,unsigned); } hdd_partition_info;
static int calls, transfer = 512;
static int partition_cache_read(void *p,unsigned sector,void *buf,unsigned len)
{ calls++; return transfer; }
static int partition_cache_write(void *p,unsigned sector,void *buf,unsigned len)
{ calls++; return transfer; }
'''
main = r'''
int main(void) {
 partition p={0}; hdd_partition_info bdev={100,&p,partition_cache_read,partition_cache_write}; char buf[1024];
 assert(ata_block_read(&bdev,buf,14819236616ULL,1)==-EIO);
 assert(ata_block_write(&bdev,buf,0x100000001ULL,1)==-EIO);
 assert(ata_block_read(&bdev,buf,UINT64_MAX,2)==-EIO);
 assert(ata_block_write(&bdev,buf,99,2)==-EIO);
 assert(calls==0);
 assert(ata_block_read(&bdev,buf,99,1)==0 && calls==1);
 assert(ata_block_write(&bdev,buf,99,1)==0 && calls==2);
 transfer=-1;
 assert(ata_block_read(&bdev,buf,0,1)==-EIO);
 transfer=0;
 assert(ata_block_write(&bdev,buf,0,1)==-EIO);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prelude + functions + main)
    subprocess.run(['gcc', '-std=gnu11', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('ATA block range and transfer errors: PASS')
