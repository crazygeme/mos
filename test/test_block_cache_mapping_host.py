"""Exercise production ATA cache storage and alias lifetime on both architectures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

PREAMBLE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define BLOCK_SECTOR_SIZE 512U
#define PREREAD_SECTOR 8U
#define PHYMM_INVALID (~0U)
typedef uint64_t paddr_t;
typedef struct { unsigned file_pages, block_pages, reserve_pages; } phymm_cache_policy;
typedef struct { unsigned dummy; } rb_node;
typedef struct { unsigned dummy; } list_entry;
typedef struct {
    int sector; unsigned dirty; int loading; unsigned page_index; void *buf;
    rb_node hash_node; list_entry time_list;
} block_cache_item;
typedef struct { unsigned sector_count; } partition;
static unsigned char storage[PAGE_SIZE];
static unsigned maps, unmaps, refs, frees, writes, cache_count;
static unsigned hdd_cache_size;
static int map_failure;
static void *alias(paddr_t phy) { assert(phy == 0x90000000ULL); return storage; }
#define PHY_TO_VIRT(phy) alias(phy)
static int mm_kmap_phys(paddr_t phy) {
    assert(phy == 0x90000000ULL);
    if (map_failure) return -1;
    maps++; return 1;
}
static void mm_kunmap_phys(paddr_t phy) {
    assert(phy == 0x90000000ULL); unmaps++;
}
static void phymm_get_cache_policy(phymm_cache_policy *p) { p->block_pages = 100000; }
static unsigned phymm_alloc_cache_pages(unsigned n) { assert(n == 1); return 0x90000; }
static unsigned phymm_alloc_dma(unsigned n) { (void)n; abort(); }
static unsigned phymm_reclaim_kernel_cache(unsigned n) { (void)n; abort(); }
static unsigned hdd_cache_reclaim(unsigned n) { (void)n; abort(); }
static void phymm_free_kernel(unsigned page, unsigned n) {
    assert(page == 0x90000 && n == 1); frees++;
}
static void phymm_reference_page(unsigned page) { assert(page == 0x90000); refs++; }
static void phymm_dereference_page(unsigned page) { assert(page == 0x90000 && refs); refs--; }
static void rb_init_node(rb_node *n) { (void)n; }
static void list_init(list_entry *n) { (void)n; }
#define kmalloc malloc
#define kfree free
#define klog(...) ((void)0)
static void partition_write(partition *p, int sector, void *buf, unsigned len) {
    assert(p->sector_count == 100 && sector == 8);
    assert(buf == storage && len == PAGE_SIZE && storage[0] == 0xa5);
    writes++;
}
'''

MAIN = r'''
int main(void) {
    partition p = { .sector_count = 100 };
    block_cache_item *item = block_cache_item_create();
    assert(item && refs == 1 && cache_count == 1);
#if MOS_PAGING_LEVELS == 2
    assert(!item->buf && maps == 0);
#else
    assert(item->buf && maps == 1);
#endif
    for (unsigned i = 0; i < 100000; i++) {
        assert(block_cache_item_map(item) == 0);
        assert(block_cache_item_map(item) == 0);
        ((unsigned char *)item->buf)[0] = 0xa5;
        block_cache_item_unmap(item);
#if MOS_PAGING_LEVELS == 2
        assert(!item->buf && maps == unmaps);
#else
        assert(item->buf && maps == 1 && unmaps == 0);
#endif
    }
    item->sector = 8; item->dirty = 1;
#if MOS_PAGING_LEVELS == 2
    map_failure = 1;
    assert(hdd_cache_flush(&p, item) == -1);
    assert(item->dirty && !item->buf && writes == 0 && refs == 1);
    map_failure = 0;
#endif
    assert(hdd_cache_flush(&p, item) == 0);
    assert(!item->dirty && writes == 1);
    unsigned before = maps;
    assert(hdd_cache_flush(&p, item) == 0);
    assert(maps == before && writes == 1);
    /* A flush nested in an active write must preserve its caller's alias. */
    assert(block_cache_item_map(item) == 0);
    item->dirty = 1;
    assert(hdd_cache_flush(&p, item) == 0 && item->buf);
    block_cache_item_unmap(item);
    block_cache_item_remove(item);
    assert(refs == 0 && cache_count == 0 && frees == 1 && maps == unmaps);
#if MOS_PAGING_LEVELS == 4
    map_failure = 1;
    assert(block_cache_item_create() == NULL);
    assert(refs == 0 && cache_count == 0 && frees == 2);
#endif
    return 0;
}
'''


class BlockCacheMappingHost(unittest.TestCase):
    def test_alias_lifetime_and_failed_flush(self):
        source = (ROOT / "src/driver/storage/ata.c").read_text()
        storage = source[source.index("static int block_cache_item_map("):
                         source.index("static void init_partition_cache(")]
        flush = source[source.index("static void hdd_cache_flush_buf("):
                       source.index("static int hdd_cache_fill_line(")]
        with tempfile.TemporaryDirectory(prefix="mos-block-cache-") as tmp:
            probe = Path(tmp) / "probe.c"
            probe.write_text(PREAMBLE + storage + flush + MAIN)
            for levels in (2, 4):
                with self.subTest(paging_levels=levels):
                    binary = Path(tmp) / f"probe-{levels}"
                    subprocess.run([
                        "cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        f"-DMOS_PAGING_LEVELS={levels}", str(probe), "-o", str(binary),
                    ], check=True, capture_output=True, text=True)
                    subprocess.run([str(binary)], check=True,
                                   capture_output=True, text=True)
