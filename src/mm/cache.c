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
	unsigned phy;
} anon_shared_map_entry;

typedef struct _anon_shared_ref {
	unsigned id; /* must be first */
	unsigned refs;
} anon_shared_ref;

typedef struct _file_shared_map_key {
	void *tag;
	uint64_t ino;
	unsigned offset;
} file_shared_map_key;

typedef struct _file_shared_map_entry {
	file_shared_map_key key; /* must be first */
	unsigned phy;
} file_shared_map_entry;

static hash_table *anon_shared_map = NULL;
static hash_table *anon_shared_refs = NULL;
static mutex_t anon_shared_map_lock;
static hash_table *file_shared_map = NULL;
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

static void anon_shared_map_evict(const key_value_pair *pair)
{
	anon_shared_map_entry *evict = pair->val;
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

static void anon_shared_ref_evict(const key_value_pair *pair)
{
	free(pair->val);
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

static void file_shared_map_evict(const key_value_pair *pair)
{
	file_shared_map_entry *evict = pair->val;
	unsigned page_index = PHY_TO_PAGE_IDX(evict->phy);

	if (phymm_dereference_page(page_index) == 0)
		phymm_free_user(page_index);
	free(evict);
}

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

	anon_shared_map =
		hash_create(anon_shared_map_key_comp, anon_shared_map_evict);
	anon_shared_refs =
		hash_create(anon_shared_ref_comp, anon_shared_ref_evict);
	file_shared_map =
		hash_create(file_shared_map_key_comp, file_shared_map_evict);
	mutex_init(&anon_shared_map_lock);
	mutex_init(&file_shared_map_lock);
	mm_cache_ready = 1;
}

unsigned mm_anon_shared_find(unsigned anon_id, unsigned offset)
{
	anon_shared_map_key tmp;
	key_value_pair *pair;
	anon_shared_map_entry *entry;

	if (!mm_cache_ready || anon_id == 0)
		return 0;

	tmp.id = anon_id | ANON_SHARED_BIT;
	tmp.offset = offset;

	mutex_lock(&anon_shared_map_lock);
	pair = hash_find(anon_shared_map, &tmp);
	if (!pair) {
		mutex_unlock(&anon_shared_map_lock);
		return 0;
	}
	entry = pair->val;
	mutex_unlock(&anon_shared_map_lock);
	return entry->phy;
}

void mm_anon_shared_add(unsigned anon_id, unsigned offset, unsigned phy)
{
	anon_shared_map_entry *entry;
	anon_shared_map_key tmp;

	if (!mm_cache_ready || anon_id == 0 || phy == 0)
		return;

	tmp.id = anon_id | ANON_SHARED_BIT;
	tmp.offset = offset;

	mutex_lock(&anon_shared_map_lock);
	if (hash_find(anon_shared_map, &tmp)) {
		mutex_unlock(&anon_shared_map_lock);
		return;
	}

	entry = malloc(sizeof(*entry));
	entry->key = tmp;
	entry->phy = phy;
	hash_insert(anon_shared_map, &entry->key, entry);
	phymm_reference_page(PHY_TO_PAGE_IDX(phy));
	mutex_unlock(&anon_shared_map_lock);
}

void mm_anon_shared_get(unsigned anon_id)
{
	key_value_pair *pair;
	anon_shared_ref *entry;

	if (!mm_cache_ready || anon_id == 0)
		return;

	mutex_lock(&anon_shared_map_lock);
	pair = hash_find(anon_shared_refs, &anon_id);
	if (pair) {
		entry = pair->val;
		entry->refs++;
		mutex_unlock(&anon_shared_map_lock);
		return;
	}

	entry = malloc(sizeof(*entry));
	entry->id = anon_id;
	entry->refs = 1;
	hash_insert(anon_shared_refs, &entry->id, entry);
	mutex_unlock(&anon_shared_map_lock);
}

void mm_anon_shared_put(unsigned anon_id)
{
	key_value_pair *pair;
	anon_shared_ref *entry;

	if (!mm_cache_ready || anon_id == 0)
		return;

	mutex_lock(&anon_shared_map_lock);
	pair = hash_find(anon_shared_refs, &anon_id);
	if (!pair) {
		mutex_unlock(&anon_shared_map_lock);
		return;
	}

	entry = pair->val;
	if (--entry->refs != 0) {
		mutex_unlock(&anon_shared_map_lock);
		return;
	}

	hash_remove(anon_shared_refs, &anon_id);
	pair = hash_first(anon_shared_map);
	while (pair != NULL) {
		anon_shared_map_entry *map_entry = pair->val;
		anon_shared_map_key key = map_entry->key;

		if (key.id != (anon_id | ANON_SHARED_BIT))
			pair = hash_next(anon_shared_map, pair);
		else {
			hash_remove(anon_shared_map, &key);
			pair = hash_first(anon_shared_map);
		}
	}
	mutex_unlock(&anon_shared_map_lock);
}

unsigned mm_file_shared_find(file *f, unsigned offset)
{
	file_shared_map_key tmp;
	key_value_pair *pair;
	file_shared_map_entry *entry;

	if (!mm_cache_ready || !file_shared_map_make_key(f, offset, &tmp))
		return 0;

	mutex_lock(&file_shared_map_lock);
	pair = hash_find(file_shared_map, &tmp);
	if (!pair) {
		mutex_unlock(&file_shared_map_lock);
		return 0;
	}
	entry = pair->val;
	mutex_unlock(&file_shared_map_lock);
	return entry->phy;
}

void mm_file_shared_add(file *f, unsigned offset, unsigned phy)
{
	file_shared_map_entry *entry;
	file_shared_map_key tmp;

	if (!mm_cache_ready || phy == 0 ||
	    !file_shared_map_make_key(f, offset, &tmp))
		return;

	mutex_lock(&file_shared_map_lock);
	if (hash_find(file_shared_map, &tmp)) {
		mutex_unlock(&file_shared_map_lock);
		return;
	}

	entry = malloc(sizeof(*entry));
	entry->key = tmp;
	entry->phy = phy;
	hash_insert(file_shared_map, &entry->key, entry);
	phymm_reference_page(PHY_TO_PAGE_IDX(phy));
	mutex_unlock(&file_shared_map_lock);
}
