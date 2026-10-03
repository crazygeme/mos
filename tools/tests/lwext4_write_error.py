#!/usr/bin/env python3
"""Inject transfer and cleanup failures into the actual ext4_fwrite source.

Run on the host with Python and GCC. Dependency stubs keep the test independent
of a disk image and verify that inode cleanup cannot hide the transfer error.
"""
from pathlib import Path
import subprocess,tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'third_party/lwext4/src/ext4.c').read_text()
function=source[source.index('int ext4_fwrite('):source.index('\nint ext4_fseek(')]
prelude=r'''
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <assert.h>
#include <errno.h>
#define EOK 0
#define O_RDONLY 1
#define EXT4_MP_LOCK(mp) ((void)0)
#define EXT4_MP_UNLOCK(mp) ((void)0)
#define ext4_assert assert
typedef uint64_t ext4_fsblk_t;
struct ext4_sblock { int unused; };
struct ext4_fs { int read_only; void *bdev; struct ext4_sblock sb; };
struct mount { struct ext4_fs fs; };
struct ext4_inode_ref { void *inode; int dirty; };
typedef struct { struct mount *mp; int flags, inode; uint64_t fpos,fsize; } ext4_file;
static int aborted, stopped, injected_error, cleanup_error;
static int ext4_trans_start(){return 0;}
static int ext4_trans_abort(){aborted++;return 0;}
static int ext4_trans_stop(){stopped++;return 0;}
static int ext4_fs_get_inode_ref(){return 0;}
static int ext4_fs_put_inode_ref(){return cleanup_error;}
static uint64_t ext4_inode_get_size(){return 0;}
static int ext4_sb_get_block_size(){return 4096;}
static int ext4_block_cache_write_back(){return injected_error;}
static int ext4_fs_init_inode_dblk_idx(){return 0;}
static int ext4_block_writebytes(){return 0;}
static int ext4_fs_append_inode_dblk(){return 0;}
static int ext4_blocks_set_direct(){return 0;}
static int ext4_inode_set_size(){return 0;}
'''
main=r'''
int main(void) {
 struct mount mp={0}; ext4_file f={.mp=&mp}; size_t written=99; char byte='x';
 injected_error=EIO; cleanup_error=0;
 assert(ext4_fwrite(&f,&byte,1,&written)==EIO);
 assert(written==0 && aborted==1 && stopped==0);
 injected_error=ENOSPC; cleanup_error=EIO; aborted=stopped=0;
 assert(ext4_fwrite(&f,&byte,1,&written)==ENOSPC);
 assert(aborted==1 && stopped==0);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
 path=Path(directory); (path/'test.c').write_text(prelude+function+main)
 subprocess.run(['gcc','-std=gnu11','-Wno-unused-function',str(path/'test.c'),'-o',str(path/'test')],check=True)
 subprocess.run([str(path/'test')],check=True)
print('lwext4 write error propagation: PASS')
