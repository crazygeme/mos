#include <test/test.h>
#include <fs/cache.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <lib/klib.h>
#include <errno.h>

static unsigned loads;
static int load_error;
static inode test_inode;
static file test_file;

static int cache_read_page(file *fp, uint64_t offset, void *buf)
{
	(void)fp;
	loads++;
	if (load_error)
		return -EIO;
	memset(buf, 0x40 + offset / PAGE_SIZE, PAGE_SIZE);
	return 0;
}

static file_operations cache_ops = { .read_page = cache_read_page };

static void cache_fixture(void)
{
	memset(&test_inode, 0, sizeof(test_inode));
	memset(&test_file, 0, sizeof(test_file));
	test_inode.i_pgcache_tag = &test_inode;
	test_inode.i_ino = 1;
	test_inode.i_size = 4 * PAGE_SIZE;
	test_file.f_inode = &test_inode;
	test_file.f_fop = &cache_ops;
	loads = 0;
	load_error = 0;
}

KTEST(FileCacheTest, HitAlignmentAndInvalidation)
{
	paddr_t first, hit, reloaded;
	int cached;
	cache_fixture();
	first = fs_page_cache_get(&test_file, 0, &cached);
	ASSERT_NE(first, 0);
	EXPECT_EQ(cached, 0);
	hit = fs_page_cache_get(&test_file, 17, &cached);
	ASSERT_NE(hit, 0);
	EXPECT_EQ(hit, first);
	EXPECT_EQ(cached, 1);
	EXPECT_EQ(loads, 1);
	fs_page_cache_put(hit);
	fs_page_cache_invalidate(&test_file);
	/* Invalidating the index must preserve retained page contents. */
	ASSERT_EQ(mm_kmap_phys(first), 1);
	EXPECT_EQ(*(unsigned char *)PHY_TO_VIRT(first), 0x40);
	mm_kunmap_phys(first);
	reloaded = fs_page_cache_get(&test_file, 0, &cached);
	ASSERT_NE(reloaded, 0);
	EXPECT_NE(reloaded, first);
	EXPECT_EQ(cached, 0);
	EXPECT_EQ(loads, 2);
	fs_page_cache_put(first);
	fs_page_cache_put(reloaded);
	fs_page_cache_invalidate(&test_file);
	return 0;
}

KTEST(FileCacheTest, ReclaimCountsOnlyFreedPages)
{
	paddr_t pinned, disposable, reloaded;
	int hit;
	/* Isolate the reclaim accounting from caches populated during boot. */
	fs_page_cache_reclaim(~0U);
	cache_fixture();
	pinned = fs_page_cache_get(&test_file, 0, NULL);
	ASSERT_NE(pinned, 0);
	disposable = fs_page_cache_get(&test_file, PAGE_SIZE, NULL);
	ASSERT_NE(disposable, 0);
	fs_page_cache_put(disposable);
	EXPECT_EQ(fs_page_cache_reclaim(1), 1);
	/* Reclaim skipped the pinned page in its freed count and continued. */
	EXPECT_EQ(phymm_pages[PHY_TO_PAGE_IDX(pinned)].ref_count, 1);
	ASSERT_EQ(mm_kmap_phys(pinned), 1);
	EXPECT_EQ(*(unsigned char *)PHY_TO_VIRT(pinned), 0x40);
	mm_kunmap_phys(pinned);
	EXPECT_EQ(fs_page_cache_reclaim(1), 0);
	reloaded = fs_page_cache_get(&test_file, PAGE_SIZE, &hit);
	ASSERT_NE(reloaded, 0);
	EXPECT_EQ(hit, 0);
	EXPECT_EQ(loads, 3);
	fs_page_cache_put(reloaded);
	fs_page_cache_put(pinned);
	fs_page_cache_invalidate(&test_file);
	return 0;
}

KTEST(FileCacheTest, FailedLoadCanRetry)
{
	paddr_t page;
	int hit;
	cache_fixture();
	load_error = 1;
	EXPECT_EQ(fs_page_cache_get(&test_file, 0, &hit), 0);
	EXPECT_EQ(hit, 0);
	load_error = 0;
	page = fs_page_cache_get(&test_file, 0, &hit);
	ASSERT_NE(page, 0);
	EXPECT_EQ(hit, 0);
	EXPECT_EQ(loads, 2);
	fs_page_cache_put(page);
	fs_page_cache_invalidate(&test_file);
	return 0;
}

KTEST(FileCacheTest, ReadAcrossPagesAndEOF)
{
	char buf[32];
	loff_t pos = PAGE_SIZE - 16;
	cache_fixture();
	EXPECT_EQ(fs_page_cache_read(&test_file, buf, sizeof(buf), &pos), 32);
	EXPECT_EQ(pos, PAGE_SIZE + 16);
	for (unsigned i = 0; i < sizeof(buf); i++)
		EXPECT_EQ(buf[i], i < 16 ? 0x40 : 0x41);
	pos = test_inode.i_size - 8;
	EXPECT_EQ(fs_page_cache_read(&test_file, buf, sizeof(buf), &pos), 8);
	EXPECT_EQ(pos, test_inode.i_size);
	EXPECT_EQ(fs_page_cache_read(&test_file, buf, sizeof(buf), &pos), 0);
	fs_page_cache_invalidate(&test_file);
	return 0;
}
