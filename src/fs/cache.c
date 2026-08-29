#include <fs/cache.h>
#include <lib/klib.h>
#include <lib/list.h>
#include <lib/lock.h>
#include <lib/rbtree.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <errno.h>

extern unsigned cache_count;

unsigned fs_page_cache_pages = 0;
unsigned fs_page_cache_max_pages = 0;
unsigned fs_page_cache_searches = 0;
unsigned fs_page_cache_hits = 0;

typedef struct _fs_page_cache_key {
	void *tag;
	uint64_t ino;
	unsigned offset;
} fs_page_cache_key;

typedef struct _fs_page_cache_entry {
	fs_page_cache_key key;
	unsigned phy;
	struct rb_node rb_node;
	list_entry lru;
} fs_page_cache_entry;

static struct rb_root fs_page_cache = _RBTREE_ROOT_INIT;
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

static fs_page_cache_entry *fs_page_cache_find(const fs_page_cache_key *key)
{
	struct rb_node *node = fs_page_cache.rb_node;
	while (node) {
		fs_page_cache_entry *entry =
			rb_entry(node, fs_page_cache_entry, rb_node);
		int comp = fs_page_cache_key_comp(key, &entry->key);
		if (comp < 0)
			node = node->rb_left;
		else if (comp > 0)
			node = node->rb_right;
		else
			return entry;
	}
	return NULL;
}

static void fs_page_cache_insert(fs_page_cache_entry *entry)
{
	struct rb_node **link = &fs_page_cache.rb_node, *parent = NULL;
	while (*link) {
		fs_page_cache_entry *cur =
			rb_entry(*link, fs_page_cache_entry, rb_node);
		parent = *link;
		if (fs_page_cache_key_comp(&entry->key, &cur->key) < 0)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}
	rb_link_node(&entry->rb_node, parent, link);
	rb_insert_color(&entry->rb_node, &fs_page_cache);
}

static void fs_page_cache_remove(fs_page_cache_entry *evict)
{
	unsigned page_index = PHY_TO_PAGE_IDX(evict->phy);

	rb_erase(&evict->rb_node, &fs_page_cache);
	if (phymm_dereference_page(page_index) == 0)
		phymm_free_user(page_index);
	cache_count--;
	fs_page_cache_pages--;
	free(evict);
}

static void fs_page_cache_ensure_init(void)
{
	if (fs_page_cache_ready)
		return;

	fs_page_cache = _RBTREE_ROOT_INIT;
	list_init(&fs_page_cache_lru);
	mutex_init(&fs_page_cache_lock);
	fs_page_cache_ready = 1;
}

static int fs_page_cache_can_use(file *fp)
{
	return fp && fp->f_inode && fp->f_inode->i_pgcache_tag && fp->f_fop &&
	       fp->f_fop->read_page;
}

static unsigned fs_page_cache_load(file *fp, unsigned offset)
{
	unsigned page_index;
	unsigned phy;

	/* Reclaimable file data should not consume scarce lowmem while highmem
	 * is available.  Reclaim before using the small-machine fallback. */
	page_index = phymm_alloc_cache();
	if (page_index == PHYMM_INVALID) {
		phymm_reclaim_user_cache(32);
		page_index = phymm_alloc_cache();
		if (page_index == PHYMM_INVALID)
			page_index = phymm_alloc_user();
		if (page_index == PHYMM_INVALID) {
			klog("fs_page_cache: phymm_alloc_user failed offset=%x\n",
			     offset);
			return 0;
		}
	}

	phy = page_index * PAGE_SIZE;
	if (mm_kmap_phys(phy) != 1) {
		phymm_free_user(page_index);
		klog("fs_page_cache: mm_kmap_phys failed phy=%x offset=%x\n",
		     phy, offset);
		return 0;
	}

	if (fp->f_fop->read_page(fp, offset, (void *)PHY_TO_VIRT(phy)) != 0) {
		mm_kunmap_phys(phy);
		phymm_free_user(page_index);
		return 0;
	}

	mm_kunmap_phys(phy);
	return phy;
}

static void fs_page_cache_evict_one_locked(void)
{
	fs_page_cache_entry *evict;

	if (list_is_empty(&fs_page_cache_lru))
		return;

	evict = container_of(list_remove_head(&fs_page_cache_lru),
			     fs_page_cache_entry, lru);
	fs_page_cache_remove(evict);
}

unsigned fs_page_cache_get(file *fp, unsigned offset, int *cache_hit)
{
	fs_page_cache_key tmp;
	fs_page_cache_entry *entry;
	unsigned phy;
	inode *inode;

	if (cache_hit)
		*cache_hit = 0;

	if (!fs_page_cache_can_use(fp))
		return 0;

	inode = fp->f_inode;
	fs_page_cache_ensure_init();
	tmp.tag = inode->i_pgcache_tag;
	tmp.ino = inode->i_ino;
	tmp.offset = offset & PAGE_SIZE_MASK;
	fs_page_cache_searches++;

	mutex_lock(&fs_page_cache_lock);
	entry = fs_page_cache_find(&tmp);
	if (entry) {
		list_remove_entry(&entry->lru);
		list_insert_tail(&fs_page_cache_lru, &entry->lru);
		phy = entry->phy;
		mutex_unlock(&fs_page_cache_lock);
		if (cache_hit)
			*cache_hit = 1;
		fs_page_cache_hits++;
		return phy;
	}

	mutex_unlock(&fs_page_cache_lock);

	phy = fs_page_cache_load(fp, tmp.offset);
	if (phy == 0)
		return 0;

	mutex_lock(&fs_page_cache_lock);
	entry = fs_page_cache_find(&tmp);
	if (entry) {
		mutex_unlock(&fs_page_cache_lock);
		phymm_free_user(PHY_TO_PAGE_IDX(phy));
		if (cache_hit)
			*cache_hit = 1;
		fs_page_cache_hits++;
		return entry->phy;
	}

	entry = malloc(sizeof(*entry));
	if (!entry) {
		mutex_unlock(&fs_page_cache_lock);
		phymm_free_user(PHY_TO_PAGE_IDX(phy));
		return 0;
	}
	entry->key = tmp;
	entry->phy = phy;
	rb_init_node(&entry->rb_node);
	list_insert_tail(&fs_page_cache_lru, &entry->lru);
	fs_page_cache_insert(entry);
	phymm_reference_page(PHY_TO_PAGE_IDX(phy));
	cache_count++;
	fs_page_cache_pages++;
	if (fs_page_cache_max_pages < fs_page_cache_pages)
		fs_page_cache_max_pages = fs_page_cache_pages;
	mutex_unlock(&fs_page_cache_lock);
	return phy;
}

void fs_page_cache_invalidate(file *fp)
{
	struct rb_node *node, *next;
	inode *inode;

	if (!fs_page_cache_can_use(fp) || !fs_page_cache_ready)
		return;

	inode = fp->f_inode;
	mutex_lock(&fs_page_cache_lock);
	for (node = rb_first(&fs_page_cache); node; node = next) {
		fs_page_cache_entry *entry =
			rb_entry(node, fs_page_cache_entry, rb_node);
		next = rb_next(node);
		if (entry->key.tag == inode->i_pgcache_tag &&
		    entry->key.ino == inode->i_ino) {
			list_remove_entry(&entry->lru);
			fs_page_cache_remove(entry);
		}
	}
	mutex_unlock(&fs_page_cache_lock);
}

unsigned fs_page_cache_reclaim(unsigned target_pages)
{
	unsigned freed = 0;

	if (!fs_page_cache_ready || target_pages == 0)
		return 0;

	mutex_lock(&fs_page_cache_lock);
	while (freed < target_pages && !list_is_empty(&fs_page_cache_lru)) {
		fs_page_cache_evict_one_locked();
		freed++;
	}
	mutex_unlock(&fs_page_cache_lock);
	return freed;
}

ssize_t fs_page_cache_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	size_t done = 0;
	inode *inode;

	if (!fp || !fp->f_inode || !pos)
		return -EINVAL;

	inode = fp->f_inode;
	if (!fs_page_cache_can_use(fp))
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

		phy = fs_page_cache_get(fp, base, NULL);
		if (phy == 0)
			return done ? (ssize_t)done : -EIO;

		if (mm_kmap_phys(phy) != 1)
			return done ? (ssize_t)done : -EIO;

		memcpy((char *)buf + done,
		       (void *)(PHY_TO_VIRT(phy) + page_off), chunk);
		mm_kunmap_phys(phy);
		done += chunk;
		*pos += (loff_t)chunk;
	}

	return (ssize_t)done;
}
