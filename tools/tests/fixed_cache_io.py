#!/usr/bin/env python3
"""Validate shared fixed-storage cache durability and transfer failures."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/driver/storage/fixed.c').read_text()
start = source.index('#define CACHE_LINES')
end = source.index('static int disk_rw(', start)
functions = source[start:end]
prelude = r'''
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <errno.h>
#define PAGE_SIZE 4096
struct fixed_cache;
struct fixed_disk {
 struct fixed_cache *cache; uint64_t sectors; unsigned sector_size;
 int (*rw)(struct fixed_disk *,void *,uint64_t,unsigned,int);
 int (*flush)(struct fixed_disk *);
};
static unsigned reads,writes,flushes; static int failure;
static unsigned char media[4*1024*1024];
static int transfer(struct fixed_disk *d,void *b,uint64_t s,unsigned n,int w)
{
 assert(s<=d->sectors && n<=d->sectors-s);
 if(failure) return -EIO;
 if(w) { writes++;memcpy(media+s*d->sector_size,b,n*d->sector_size); }
 else { reads++;memcpy(b,media+s*d->sector_size,n*d->sector_size); }
 return 0;
}
static int flush(struct fixed_disk *d) { flushes++;return failure?-EIO:0; }
'''
main = r'''
int main(void) {
 for(unsigned ss=512;ss<=4096;ss*=8) {
  struct fixed_cache c={0}; c.data=calloc(CACHE_LINES,CACHE_BYTES);assert(c.data);
  struct fixed_disk d={&c,sizeof(media)/ss,ss,transfer,flush};
  unsigned char *b=malloc(CACHE_BYTES),*out=malloc(CACHE_BYTES); assert(b && out);
  memset(media,0,sizeof(media));memset(b,0x73,CACHE_BYTES);reads=writes=flushes=0;
  assert(cache_rw(&d,b,UINT64_MAX,1,0)==-EIO && reads==0);
  assert(cache_rw(&d,b,d.sectors-1,2,1)==-EIO && writes==0);
  assert(!cache_rw(&d,b,1,1,1) && writes==0 && reads==1);
  assert(!cache_rw(&d,out,1,1,0) && !memcmp(b,out,ss) && reads==1);
  failure=1;assert(cache_flush(&d)==-EIO);failure=0;
  assert(!cache_flush(&d) && writes==1 && media[ss]==0x73);
  assert(!cache_rw(&d,b,0,CACHE_BYTES/ss,1));
  for(unsigned i=1;i<=CACHE_LINES;i++) assert(!cache_rw(&d,out,i*CACHE_BYTES/ss,1,0));
  assert(media[0]==0x73);
  failure=1;
  assert(cache_rw(&d,out,(CACHE_LINES+1)*CACHE_BYTES/ss,1,0)==-EIO);
  failure=0;
  assert(!cache_rw(&d,out,(CACHE_LINES+1)*CACHE_BYTES/ss,1,0));
  assert(!cache_flush(&d));free(b);free(out);free(c.data);
 }
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prelude + functions + main)
    subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Werror', '-fsanitize=address,undefined', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('Fixed-storage cache, eviction, durability and failure handling: PASS')
