#!/usr/bin/env python3
"""Check lookup-cache collisions, buffer lifetimes, and fresh inode metadata."""
from pathlib import Path
import os
import unittest
import subprocess
import tempfile

from test_fd_callback_lifetime_host import ROOT, function

PROBE = r'''
#include <lib/klib.h>
#include <ext4_bcache.h>
#include <ext4_blockdev.h>
#include <ext4_block_group.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_trans.h>
#include <ext4_debug.h>
#include <errno.h>

static unsigned flushed;
int ext4_block_flush_buf(struct ext4_blockdev *dev, struct ext4_buf *buf)
{
    flushed++;
    if (buf->on_dirty_list)
        ext4_bcache_remove_dirty_node(dev->bc, buf);
    ext4_bcache_clear_flag(buf, BC_DIRTY);
    return 0;
}
static struct ext4_buf *create_buffer(struct ext4_bcache *bc, uint64_t lba,
                                     unsigned char value)
{
    struct ext4_block block = { .lb_id = lba };
    bool is_new;
    assert(!ext4_bcache_alloc(bc, &block, &is_new) && is_new);
    memset(block.data, value, bc->itemsize);
    ext4_bcache_set_flag(block.buf, BC_UPTODATE);
    struct ext4_buf *buf = block.buf;
    assert(!ext4_bcache_free(bc, &block));
    return buf;
}
static void check_buffer(struct ext4_bcache *bc, uint64_t lba, unsigned char value)
{
    struct ext4_block block = {0};
    assert(ext4_bcache_find_get(bc, &block, lba));
    assert(block.buf->lba == lba && block.data[0] == value);
    assert(!ext4_bcache_free(bc, &block));
}
static void buffer_tests(void)
{
    struct ext4_bcache cache;
    struct ext4_blockdev dev = { .bc = &cache, .cache_write_back = 1 };
    assert(!ext4_bcache_init_dynamic(&cache, 32, 64));
    cache.bdev = &dev;
    struct ext4_buf *a = create_buffer(&cache, 1, 'a');
    struct ext4_buf *b = create_buffer(&cache, 17, 'b');
    uint64_t high = (1ULL << 32) | 16;
    create_buffer(&cache, high, 'h');
    for (unsigned i = 0; i < 100; ++i) {
        /* These LBAs collide, including a different high 32-bit word. */
        check_buffer(&cache, 1, 'a');
        check_buffer(&cache, 17, 'b');
        check_buffer(&cache, high, 'h');
    }
    check_buffer(&cache, 1, 'a');
    ext4_bcache_drop_buf(&cache, a);
    struct ext4_block block = {0};
    assert(!ext4_bcache_find_get(&cache, &block, 1));
    assert(ext4_bcache_find_get(&cache, &block, 17) == b);
    ext4_bcache_invalidate_buf(&cache, b);
    assert(!ext4_bcache_free(&cache, &block));
    assert(!ext4_bcache_find_get(&cache, &block, 17));
    create_buffer(&cache, 33, 't');
    assert(ext4_bcache_find_get(&cache, &block, 33));
    ext4_bcache_set_flag(block.buf, BC_TMP);
    assert(!ext4_bcache_free(&cache, &block));
    assert(!ext4_bcache_find_get(&cache, &block, 33));
    create_buffer(&cache, 49, 'd');
    assert(ext4_bcache_find_get(&cache, &block, 49));
    ext4_bcache_set_dirty(block.buf);
    assert(!ext4_bcache_free(&cache, &block));
    assert(ext4_bcache_find_get(&cache, &block, 49));
    assert(!block.buf->on_dirty_list);
    assert(!ext4_bcache_free(&cache, &block));
    for (unsigned i = 0; i < 10000; ++i) {
        uint64_t lba = 65 + i % 16;
        struct ext4_buf *buf = create_buffer(&cache, lba, (unsigned char)i);
        check_buffer(&cache, lba, (unsigned char)i);
        ext4_bcache_drop_buf(&cache, buf);
        assert(!ext4_bcache_find_get(&cache, &block, lba));
    }
    ext4_bcache_cleanup(&cache);
    assert(flushed && !cache.ref_blocks);
    assert(!ext4_bcache_fini_dynamic(&cache));
}

static struct ext4_fs first_fs, second_fs;
static struct ext4_bgroup groups[2][4];
static unsigned descriptor_reads, inode_reads, checksum_checks;
static uint64_t failed_block;
static struct { uint64_t lba; unsigned char data[1024]; } blocks[64];
static unsigned block_count;

int ext4_fs_get_block_group_ref(struct ext4_fs *fs, uint32_t group,
                              struct ext4_block_group_ref *ref)
{
    assert(group < 4);
    descriptor_reads++;
    ref->block_group = &groups[fs == &second_fs][group];
    return 0;
}
int ext4_fs_put_block_group_ref(struct ext4_block_group_ref *ref)
{ (void)ref; return 0; }
int ext4_trans_block_get(struct ext4_blockdev *dev, struct ext4_block *block,
                         uint64_t lba)
{
    (void)dev;
    inode_reads++;
    if (lba == failed_block)
        return EIO;
    unsigned i;
    for (i = 0; i < block_count && blocks[i].lba != lba; ++i) {}
    if (i == block_count) {
        assert(block_count < 64);
        blocks[block_count++].lba = lba;
    }
    block->lb_id = lba;
    block->data = blocks[i].data;
    return 0;
}
#define ext4_fs_verify_inode_csum(ref) (++checksum_checks, true)
@@GET_INODE@@

static struct ext4_inode_ref load(struct ext4_fs *fs, unsigned index)
{
    struct ext4_inode_ref ref;
    assert(!__ext4_fs_get_inode_ref(fs, index, &ref, true));
    assert(ref.index == index && ref.fs == fs && !ref.dirty);
    return ref;
}
static void inode_tests(void)
{
    struct ext4_blockdev dev[2] = {0};
    struct ext4_fs *filesystems[2] = { &first_fs, &second_fs };
    for (unsigned f = 0; f < 2; ++f) {
        struct ext4_fs *fs = filesystems[f];
        fs->bdev = &dev[f];
        fs->sb.inodes_per_group = to_le32(32);
        fs->sb.inode_size = to_le16(128);
        for (unsigned g = 0; g < 4; ++g)
            groups[f][g].inode_table_first_block_lo = to_le32(1000 + f * 4000 + g * 10);
    }
    struct ext4_inode_ref a = load(&first_fs, 1);
    assert(descriptor_reads == 1 && inode_reads == 1);
    a.inode->mode = to_le16(0600);
    a = load(&first_fs, 1);
    assert(descriptor_reads == 1 && inode_reads == 2 && checksum_checks == 2);
    assert(to_le16(a.inode->mode) == 0600);
    a.inode->mode = to_le16(0644);
    a = load(&first_fs, 1);
    assert(to_le16(a.inode->mode) == 0644 && descriptor_reads == 1);
    struct ext4_inode_ref b = load(&first_fs, 17);
    assert(b.block.lb_id == 1002 && b.inode != a.inode);
    a = load(&first_fs, 1);
    assert(a.block.lb_id == 1000 && to_le16(a.inode->mode) == 0644);
    b = load(&first_fs, 33);
    assert(b.block.lb_id == 1010); /* Different inode group, same cache slot. */
    a = load(&second_fs, 1);
    assert(a.block.lb_id == 5000 && to_le16(a.inode->mode) == 0);
    a = load(&first_fs, 13);
    unsigned reads = descriptor_reads;
    failed_block = 1021;
    struct ext4_inode_ref failed;
    assert(__ext4_fs_get_inode_ref(&first_fs, 77, &failed, true) == EIO);
    failed_block = 0;
    b = load(&first_fs, 13);
    assert(descriptor_reads == reads + 1 && a.inode == b.inode);
    unsigned verified = checksum_checks;
    assert(!__ext4_fs_get_inode_ref(&first_fs, 13, &b, false));
    assert(checksum_checks == verified); /* Allocations skip the old checksum. */
}
int main(void)
{
    buffer_tests();
    inode_tests();
    puts("lwext4 lookup collisions, buffer lifetimes, and fresh metadata: PASS");
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix='mos-lwext4-lookup-') as raw:
        work = Path(raw)
        shim = work / 'lib/klib.h'
        shim.parent.mkdir()
        shim.write_text('''#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
static inline void *zalloc(size_t size) { return calloc(1, size); }
''')
        source = (ROOT / 'third_party/lwext4/src/ext4_fs.c').read_text()
        probe = work / 'probe.c'
        probe.write_text(PROBE.replace('@@GET_INODE@@', function(
            source, 'static int __ext4_fs_get_inode_ref(')))
        executable = work / 'probe'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-DCONFIG_DEBUG_ASSERT=1', '-DCONFIG_DEBUG_PRINTF=0',
                        '-DCONFIG_HAVE_OWN_ASSERT=0', '-DCONFIG_HAVE_OWN_ERRNO=0',
                        '-I' + str(work), '-I' + str(ROOT / 'third_party/lwext4/include'),
                        str(probe), str(ROOT / 'third_party/lwext4/src/ext4_bcache.c'),
                        '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)


class Lwext4LookupHostTest(unittest.TestCase):
    def test_production_contract(self):
        main()


if __name__ == '__main__':
    unittest.main()
