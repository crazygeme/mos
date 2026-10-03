#!/usr/bin/env python3
"""Check the real ATA filesystem callbacks with simulated sector transfers."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/driver/impl/storage/ata.c').read_text()
start = source.rindex('static int hdd_bdev_bread(')
end = source.index('static int hdd_bdev_close(', start)
functions = source[start:end]
prelude = r'''
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#define HDD_CACHE_OPEN 1
#define BLOCK_SECTOR_SIZE 512
#define HDD_IO_MAX_SECTORS 8
typedef struct { int unused; } partition;
struct iface { uint64_t ph_bcnt; };
struct ext4_blockdev { void *aux; struct iface *bdif; };
static int calls, transfer = 512;
static int partition_cache_read(partition *p,unsigned sector,void *buf,unsigned len)
{ calls++; return transfer; }
static int partition_cache_write(partition *p,unsigned sector,void *buf,unsigned len)
{ calls++; return transfer; }
'''
main = r'''
int main(void) {
 partition p={0}; struct iface iface={100};
 struct ext4_blockdev bdev={&p,&iface}; char buf[1024];
 assert(hdd_bdev_bread(&bdev,buf,14819236616ULL,1)==EIO);
 assert(hdd_bdev_bwrite(&bdev,buf,0x100000001ULL,1)==EIO);
 assert(hdd_bdev_bread(&bdev,buf,UINT64_MAX,2)==EIO);
 assert(hdd_bdev_bwrite(&bdev,buf,99,2)==EIO);
 assert(calls==0);
 assert(hdd_bdev_bread(&bdev,buf,99,1)==0 && calls==1);
 assert(hdd_bdev_bwrite(&bdev,buf,99,1)==0 && calls==2);
 transfer=-1;
 assert(hdd_bdev_bread(&bdev,buf,0,1)==EIO);
 transfer=0;
 assert(hdd_bdev_bwrite(&bdev,buf,0,1)==EIO);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prelude + functions + main)
    subprocess.run(['gcc', '-std=gnu11', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('ATA block range and transfer errors: PASS')
