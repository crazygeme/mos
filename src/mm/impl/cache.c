#include <mm/cache.h>
#include <mm/mm.h>
#include <mm/phymm.h>
#include <fs/fs.h>
#include <lib/klib.h>
#include <lib/lock.h>
#include <lib/rbtree.h>

#define ANON_SHARED_BIT 0x80000000U

typedef struct _anon_shared_map_key {
	unsigned id;
	unsigned offset;
} anon_shared_map_key;

typedef struct _anon_shared_map_entry {
	anon_shared_map_key key; /* must be first */
	paddr_t phy;
	struct rb_node rb_node;
} anon_shared_map_entry;

typedef struct _anon_shared_ref {
	unsigned id; /* must be first */
	unsigned refs;
	struct rb_node rb_node;
} anon_shared_ref;

typedef struct _file_shared_map_key {
	void *tag;
	uint64_t ino;
	unsigned offset;
} file_shared_map_key;

typedef struct _file_shared_map_entry {
	file_shared_map_key key; /* must be first */
	paddr_t phy;
	struct rb_node rb_node;
} file_shared_map_entry;

static struct rb_root anon_shared_map = _RBTREE_ROOT_INIT;
static struct rb_root anon_shared_refs = _RBTREE_ROOT_INIT;
static mutex_t anon_shared_map_lock;
static struct rb_root file_shared_map = _RBTREE_ROOT_INIT;
static mutex_t file_shared_map_lock;
static int mm_cache_ready = 0;

static int anon_shared_map_key_comp(const void *k1, const void *k2)
{
	const anon_shared_map_key *key1 = k1;
	const anon_shared_map_key *key2 = k2;
	int ret = (int)key1->id - (int)key2->id;

	if (ret == 0)
		ret = (int)key1->offset - (int)key2->offset;
	return ret;
}

static void anon_shared_map_evict(anon_shared_map_entry *evict)
{
	unsigned page_index = PHY_TO_PAGE_IDX(evict->phy);

	if (phymm_dereference_page(page_index) == 0)
		phymm_free_user(page_index);
	free(evict);
}

static int anon_shared_ref_comp(const void *k1, const void *k2)
{
	unsigned id1 = *(const unsigned *)k1;
	unsigned id2 = *(const unsigned *)k2;

	if (id1 < id2)
		return -1;
	if (id1 > id2)
		return 1;
	return 0;
}

static int file_shared_map_key_comp(const void *k1, const void *k2)
{
	const file_shared_map_key *key1 = k1;
	const file_shared_map_key *key2 = k2;

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

static void file_shared_map_evict(file_shared_map_entry *evict)
{
	unsigned page_index = PHY_TO_PAGE_IDX(evict->phy);

	if (phymm_dereference_page(page_index) == 0)
		phymm_free_user(page_index);
	free(evict);
}

#define DEFINE_CACHE_TREE(name, type, key_type, member, compare)       \
	static type *name##_find(const key_type *key)                  \
	{                                                              \
		struct rb_node *node = name.rb_node;                   \
		while (node) {                                         \
			type *entry = rb_entry(node, type, rb_node);   \
			int comp = compare(key, &entry->member);       \
			if (comp < 0)                                  \
				node = node->rb_left;                  \
			else if (comp > 0)                             \
				node = node->rb_right;                 \
			else                                           \
				return entry;                          \
		}                                                      \
		return NULL;                                           \
	}                                                              \
	static void name##_insert(type *entry)                         \
	{                                                              \
		struct rb_node **link = &name.rb_node, *parent = NULL; \
		while (*link) {                                        \
			type *cur = rb_entry(*link, type, rb_node);    \
			parent = *link;                                \
			if (compare(&entry->member, &cur->member) < 0) \
				link = &(*link)->rb_left;              \
			else                                           \
				link = &(*link)->rb_right;             \
		}                                                      \
		rb_link_node(&entry->rb_node, parent, link);           \
		rb_insert_color(&entry->rb_node, &name);               \
	}

DEFINE_CACHE_TREE(anon_shared_map, anon_shared_map_entry, anon_shared_map_key,
		  key, anon_shared_map_key_comp)
DEFINE_CACHE_TREE(anon_shared_refs, anon_shared_ref, unsigned, id,
		  anon_shared_ref_comp)
DEFINE_CACHE_TREE(file_shared_map, file_shared_map_entry, file_shared_map_key,
		  key, file_shared_map_key_comp)

static int file_shared_map_make_key(file *f, unsigned offset,
				    file_shared_map_key *key)
{
	void *tag;

	if (!f || !f->f_inode || !key)
		return 0;

	tag = f->f_inode->i_pgcache_tag ? f->f_inode->i_pgcache_tag :
					  f->f_inode->i_private;
	if (!tag)
		return 0;

	key->tag = tag;
	key->ino = f->f_inode->i_ino;
	key->offset = offset & PAGE_SIZE_MASK;
	return 1;
}

void mm_cache_init(void)
{
	if (mm_cache_ready)
		return;

	anon_shared_map = _RBTREE_ROOT_INIT;
	anon_shared_refs = _RBTREE_ROOT_INIT;
	file_shared_map = _RBTREE_ROOT_INIT;
	mutex_init(&anon_shared_map_lock);
	mutex_init(&file_shared_map_lock);
	mm_cache_ready = 1;
}

paddr_t mm_anon_shared_find(unsigned anon_id, unsigned offset)
{
	anon_shared_map_key tmp;
	anon_shared_map_entry *entry;

	if (!mm_cache_ready || anon_id == 0)
		return 0;

	tmp.id = anon_id | ANON_SHARED_BIT;
	tmp.offset = offset;

	mutex_lock(&anon_shared_map_lock);
	entry = anon_shared_map_find(&tmp);
	if (!entry) {
		mutex_unlock(&anon_shared_map_lock);
		return 0;
	}
	mutex_unlock(&anon_shared_map_lock);
	return entry->phy;
}

void mm_anon_shared_add(unsigned anon_id, unsigned offset, paddr_t phy)
{
	anon_shared_map_entry *entry;
	anon_shared_map_key tmp;

	if (!mm_cache_ready || anon_id == 0 || phy == 0)
		return;

	tmp.id = anon_id | ANON_SHARED_BIT;
	tmp.offset = offset;

	mutex_lock(&anon_shared_map_lock);
	if (anon_shared_map_find(&tmp)) {
		mutex_unlock(&anon_shared_map_lock);
		return;
	}

	entry = malloc(sizeof(*entry));
	entry->key = tmp;
	entry->phy = phy;
	rb_init_node(&entry->rb_node);
	anon_shared_map_insert(entry);
	phymm_reference_page(PHY_TO_PAGE_IDX(phy));
	mutex_unlock(&anon_shared_map_lock);
}

void mm_anon_shared_get(unsigned anon_id)
{
	anon_shared_ref *entry;

	if (!mm_cache_ready || anon_id == 0)
		return;

	mutex_lock(&anon_shared_map_lock);
	entry = anon_shared_refs_find(&anon_id);
	if (entry) {
		entry->refs++;
		mutex_unlock(&anon_shared_map_lock);
		return;
	}

	entry = malloc(sizeof(*entry));
	entry->id = anon_id;
	entry->refs = 1;
	rb_init_node(&entry->rb_node);
	anon_shared_refs_insert(entry);
	mutex_unlock(&anon_shared_map_lock);
}

void mm_anon_shared_put(unsigned anon_id)
{
	struct rb_node *node, *next;
	anon_shared_ref *entry;

	if (!mm_cache_ready || anon_id == 0)
		return;

	mutex_lock(&anon_shared_map_lock);
	entry = anon_shared_refs_find(&anon_id);
	if (!entry) {
		mutex_unlock(&anon_shared_map_lock);
		return;
	}

	if (--entry->refs != 0) {
		mutex_unlock(&anon_shared_map_lock);
		return;
	}

	rb_erase(&entry->rb_node, &anon_shared_refs);
	free(entry);
	for (node = rb_first(&anon_shared_map); node; node = next) {
		anon_shared_map_entry *map_entry =
			rb_entry(node, anon_shared_map_entry, rb_node);
		next = rb_next(node);
		if (map_entry->key.id == (anon_id | ANON_SHARED_BIT)) {
			rb_erase(&map_entry->rb_node, &anon_shared_map);
			anon_shared_map_evict(map_entry);
		}
	}
	mutex_unlock(&anon_shared_map_lock);
}

paddr_t mm_file_shared_find(file *f, unsigned offset)
{
	file_shared_map_key tmp;
	file_shared_map_entry *entry;

	if (!mm_cache_ready || !file_shared_map_make_key(f, offset, &tmp))
		return 0;

	mutex_lock(&file_shared_map_lock);
	entry = file_shared_map_find(&tmp);
	if (!entry) {
		mutex_unlock(&file_shared_map_lock);
		return 0;
	}
	mutex_unlock(&file_shared_map_lock);
	return entry->phy;
}

void mm_file_shared_add(file *f, unsigned offset, paddr_t phy)
{
	file_shared_map_entry *entry;
	file_shared_map_key tmp;

	if (!mm_cache_ready || phy == 0 ||
	    !file_shared_map_make_key(f, offset, &tmp))
		return;

	mutex_lock(&file_shared_map_lock);
	if (file_shared_map_find(&tmp)) {
		mutex_unlock(&file_shared_map_lock);
		return;
	}

	entry = malloc(sizeof(*entry));
	entry->key = tmp;
	entry->phy = phy;
	rb_init_node(&entry->rb_node);
	file_shared_map_insert(entry);
	phymm_reference_page(PHY_TO_PAGE_IDX(phy));
	mutex_unlock(&file_shared_map_lock);
}
