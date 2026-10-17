#!/usr/bin/env python3
"""Check the real ATA filesystem callbacks with simulated sector transfers."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/driver/storage/ata.c').read_text()
start = source.index('static int ata_fixed_rw(')
end = source.index('static int ata_fixed_flush', start)
functions = source[start:end]
prelude = r'''
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#define HDD_IO_MAX_SECTORS 8
typedef struct { int unused; } partition;
struct fixed_disk { uint64_t sectors; void *private; };
static int calls, transfer = -2;
static int partition_read(void *p,unsigned sector,void *buf,unsigned len)
{ calls++; return transfer==-2?len:transfer; }
static int partition_write(void *p,unsigned sector,void *buf,unsigned len)
{ calls++; return transfer==-2?len:transfer; }
'''
main = r'''
int main(void) {
 partition p={0}; struct fixed_disk bdev={100,&p}; char buf[8192];
 assert(ata_fixed_rw(&bdev,buf,14819236616ULL,1,0)==-EIO);
 assert(ata_fixed_rw(&bdev,buf,0x100000001ULL,1,1)==-EIO);
 assert(ata_fixed_rw(&bdev,buf,UINT64_MAX,2,0)==-EIO);
 assert(ata_fixed_rw(&bdev,buf,99,2,1)==-EIO);
 assert(calls==0);
 assert(ata_fixed_rw(&bdev,buf,99,1,0)==0 && calls==1);
 assert(ata_fixed_rw(&bdev,buf,0,16,1)==0 && calls==3);
 transfer=-1;
 assert(ata_fixed_rw(&bdev,buf,0,1,0)==-EIO);
 transfer=0;
 assert(ata_fixed_rw(&bdev,buf,0,1,1)==-EIO);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prelude + functions + main)
    subprocess.run(['gcc', '-std=gnu11', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('ATA block range and transfer errors: PASS')
