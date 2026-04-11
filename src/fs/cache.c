#include <fs/cache.h>
#include <lib/klib.h>
#include <lib/list.h>
#include <lib/lock.h>
#include <lib/rbtree.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <errno.h>

extern unsigned cache_count;

typedef struct _fs_page_cache_key {
	void *tag;
	uint64_t ino;
	unsigned offset;
} fs_page_cache_key;

typedef struct _fs_page_cache_entry {
	fs_page_cache_key key;
	unsigned phy;
	list_entry lru;
} fs_page_cache_entry;

static hash_table *fs_page_cache = NULL;
static list_entry fs_page_cache_lru;
static mutex_t fs_page_cache_lock;
static int fs_page_cache_ready = 0;

static int fs_page_cache_key_comp(const void *k1, const void *k2)
{
	const fs_page_cache_key *key1 = k1;
	const fs_page_cache_key *key2 = k2;

	if (key1->tag < key2->tag)
		return -1;
	if (key1->tag > key2->tag)
		return 1;
	if (key1->ino < key2->ino)
		return -1;
	if (key1->ino > key2->ino)
		return 1;
	return (int)key1->offset - (int)key2->offset;
}

static void fs_page_cache_entry_evict(const key_value_pair *pair)
{
	fs_page_cache_entry *evict = pair->val;
	unsigned page_index = PHY_TO_PAGE_IDX(evict->phy);

	if (phymm_dereference_page(page_index) == 0)
		phymm_free_user(page_index);
	cache_count--;
	free(evict);
}

static void fs_page_cache_ensure_init(void)
{
	if (fs_page_cache_ready)
		return;

	fs_page_cache =
		hash_create(fs_page_cache_key_comp, fs_page_cache_entry_evict);
	list_init(&fs_page_cache_lru);
	mutex_init(&fs_page_cache_lock);
	fs_page_cache_ready = 1;
}

static int fs_page_cache_can_use(inode *inode)
{
	return inode && inode->i_pgcache_tag && inode->i_op &&
	       inode->i_op->read_page;
}

static unsigned fs_page_cache_load(inode *inode, unsigned offset)
{
	unsigned page_index;
	unsigned phy;

	page_index = phymm_alloc_user();
	if (page_index == PHYMM_INVALID)
		return 0;

	phy = page_index * PAGE_SIZE;
	if (mm_kmap_phys(phy) != 1) {
		phymm_free_user(page_index);
		return 0;
	}

	if (inode->i_op->read_page(inode, offset, (void *)PHY_TO_VIRT(phy)) !=
	    0) {
		phymm_free_user(page_index);
		return 0;
	}

	return phy;
}

static void fs_page_cache_evict_one_locked(void)
{
	fs_page_cache_entry *evict;

	if (!fs_page_cache || list_is_empty(&fs_page_cache_lru))
		return;

	evict = container_of(list_remove_head(&fs_page_cache_lru),
			     fs_page_cache_entry, lru);
	hash_remove(fs_page_cache, &evict->key);
}

unsigned fs_page_cache_get(inode *inode, unsigned offset, int *cache_hit)
{
	fs_page_cache_key tmp;
	key_value_pair *pair;
	fs_page_cache_entry *entry;
	unsigned phy;

	if (cache_hit)
		*cache_hit = 0;

	if (!fs_page_cache_can_use(inode))
		return 0;

	fs_page_cache_ensure_init();
	tmp.tag = inode->i_pgcache_tag;
	tmp.ino = inode->i_ino;
	tmp.offset = offset & PAGE_SIZE_MASK;

	mutex_lock(&fs_page_cache_lock);
	pair = hash_find(fs_page_cache, &tmp);
	if (pair) {
		entry = pair->val;
		list_remove_entry(&entry->lru);
		list_insert_tail(&fs_page_cache_lru, &entry->lru);
		phy = entry->phy;
		mutex_unlock(&fs_page_cache_lock);
		if (cache_hit)
			*cache_hit = 1;
		return phy;
	}

	if (hash_size(fs_page_cache) >= PAGE_CACHE_SIZE)
		fs_page_cache_evict_one_locked();
	mutex_unlock(&fs_page_cache_lock);

	phy = fs_page_cache_load(inode, tmp.offset);
	if (phy == 0)
		return 0;

	mutex_lock(&fs_page_cache_lock);
	pair = hash_find(fs_page_cache, &tmp);
	if (pair) {
		mutex_unlock(&fs_page_cache_lock);
		phymm_free_user(PHY_TO_PAGE_IDX(phy));
		if (cache_hit)
			*cache_hit = 1;
		return ((fs_page_cache_entry *)pair->val)->phy;
	}

	entry = malloc(sizeof(*entry));
	if (!entry) {
		mutex_unlock(&fs_page_cache_lock);
		phymm_free_user(PHY_TO_PAGE_IDX(phy));
		return 0;
	}
	entry->key = tmp;
	entry->phy = phy;
	list_insert_tail(&fs_page_cache_lru, &entry->lru);
	hash_insert(fs_page_cache, &entry->key, entry);
	phymm_reference_page(PHY_TO_PAGE_IDX(phy));
	cache_count++;
	mutex_unlock(&fs_page_cache_lock);
	return phy;
}

void fs_page_cache_invalidate(inode *inode)
{
	key_value_pair *pair;

	if (!fs_page_cache_can_use(inode) || !fs_page_cache_ready)
		return;

	mutex_lock(&fs_page_cache_lock);
	pair = hash_first(fs_page_cache);
	while (pair != NULL) {
		fs_page_cache_entry *entry = pair->val;
		fs_page_cache_key key = entry->key;

		if (key.tag != inode->i_pgcache_tag || key.ino != inode->i_ino)
			pair = hash_next(fs_page_cache, pair);
		else {
			list_remove_entry(&entry->lru);
			hash_remove(fs_page_cache, &key);
			pair = hash_first(fs_page_cache);
		}
	}
	mutex_unlock(&fs_page_cache_lock);
}

ssize_t fs_page_cache_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	size_t done = 0;
	inode *inode;

	if (!fp || !fp->f_inode || !pos)
		return -EINVAL;

	inode = fp->f_inode;
	if (!fs_page_cache_can_use(inode))
		return -EINVAL;

	while (done < size && (uint64_t)(*pos) < inode->i_size) {
		unsigned base = ((unsigned)(*pos)) & PAGE_SIZE_MASK;
		unsigned page_off = ((unsigned)(*pos)) & ~PAGE_SIZE_MASK;
		size_t chunk = PAGE_SIZE - page_off;
		unsigned phy;

		if (chunk > size - done)
			chunk = size - done;
		if ((uint64_t)(*pos) + chunk > inode->i_size)
			chunk = (size_t)(inode->i_size - (uint64_t)(*pos));

		phy = fs_page_cache_get(inode, base, NULL);
		if (phy == 0)
			return done ? (ssize_t)done : -EIO;

		if (mm_kmap_phys(phy) != 1)
			return done ? (ssize_t)done : -EIO;

		memcpy((char *)buf + done,
		       (void *)(PHY_TO_VIRT(phy) + page_off), chunk);
		done += chunk;
		*pos += (loff_t)chunk;
	}

	return (ssize_t)done;
}
