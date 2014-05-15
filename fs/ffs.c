#include <fs/ffs.h>
#include <fs/vfs.h>
#ifdef WIN32
#include <osdep.h>
#include <time.h>
#else
#include <lib/klib.h>
#endif

#define INODE_PER_SECTOR (BLOCK_SECTOR_SIZE / 4)
#define META_PER_SECTOR (BLOCK_SECTOR_SIZE / (sizeof(struct ffs_meta_info)))
#define MAX_FILE_SIZE ((1 + INODE_PER_SECTOR + INODE_PER_SECTOR*INODE_PER_SECTOR) * BLOCK_SECTOR_SIZE)
#define MAX_DENTRY_COUNT ((1 + INODE_PER_SECTOR + INODE_PER_SECTOR*INODE_PER_SECTOR) * META_PER_SECTOR)

static INODE ffs_create_inode(struct filesys_type*);
static void ffs_destroy_inode(struct filesys_type*, INODE);
static void ffs_write_super(struct filesys_type*, SUPORBLOCK);
static void ffs_write_desc(struct filesys_type*, DESC);
static void ffs_free_inode(struct filesys_type*, INODE);
static INODE ffs_get_root(struct filesys_type*);
static unsigned ffs_get_mode(INODE inode);
static unsigned ffs_read_file(INODE inode, unsigned int offset, char* buf, unsigned len);
static unsigned ffs_write_file(INODE inode, unsigned int offset, char* buf, unsigned len);
static DIR ffs_open_dir(INODE inode);
static INODE ffs_read_dir(DIR dir);
static void ffs_add_dir_entry(DIR dir, unsigned mode, char* name);
static void ffs_del_dir_entry(DIR dir, char* name);
static void ffs_close_dir(DIR dir);
static char* ffs_get_name(INODE node);
static unsigned int ffs_get_size(INODE node);
static int ffs_copy_stat(INODE node, struct stat* s);

static void ffs_set_bitmap(block* b, struct ffs_bitmap_cache* cache, unsigned sector, unsigned used);
static void ffs_load_bitmap(block* b, struct ffs_bitmap_cache* cache);
static void ffs_flush_bitmap(block* b, struct ffs_bitmap_cache* cache);

static void ffs_set_bitmap(block* b, struct ffs_bitmap_cache* cache, unsigned sector, unsigned used)
{
	unsigned index = sector / (BLOCK_SECTOR_SIZE * 8);
	unsigned idx_inside_sector = sector % (BLOCK_SECTOR_SIZE * 8);
	unsigned idx_inside_node = idx_inside_sector / 8;
	unsigned idx_inside_byte = idx_inside_sector % 8;
	struct ffs_bitmap_node* node = 0;
	unsigned mask = 1 << idx_inside_byte;

	if (index != cache->sector){
		ffs_flush_bitmap(b, cache);
		cache->sector = index;
		ffs_load_bitmap(b, cache);
	}

	node = &cache->node;

	if (used){
		node->bits[idx_inside_node] |= mask;
	}else{
		node->bits[idx_inside_node] &= ~mask;
	}
}
static void ffs_load_bitmap(block* b, struct ffs_bitmap_cache* cache)
{
	b->read(b->aux, cache->sector, &(cache->node.bits[0]), BLOCK_SECTOR_SIZE);
}

static void ffs_flush_bitmap(block* b, struct ffs_bitmap_cache* cache)
{
	b->write(b->aux, cache->sector, &(cache->node.bits[0]), BLOCK_SECTOR_SIZE);
}

static unsigned ffs_find_free_sector(struct ffs_bitmap_cache* cache)
{
	unsigned bitmap_sector = cache->sector;
	unsigned bitmap_byte_index, bitmap_byte_offset;
	int i = 0;
	unsigned char byte;

	for (i = 0; i < BLOCK_SECTOR_SIZE; i++){
		if (cache->node.bits[i] != 0xff){
			byte = cache->node.bits[i];
			bitmap_byte_index = i;
			break;
		}
	}

	if (i == BLOCK_SECTOR_SIZE)
		return 0;

	for (i = 0; i < 8; i++){
		if ((byte & 0x1) == 0x1){
			bitmap_byte_offset = i;
			break;
		}
		byte = byte >> 1;
	}

	if (i == 8)
		return 0;

	return (bitmap_sector*BLOCK_SECTOR_SIZE * 8 + bitmap_byte_index * 8 + bitmap_byte_offset);
}

static unsigned ffs_alloc_free_sector(struct ffs_inode* node, struct ffs_bitmap_cache* cache)
{
	block* b = node->type->dev;
	struct ffs_super_node* super = node->type->sb;
	int i = 1;
	unsigned bitmap_sector_count = (super->total_size - 1) / (BLOCK_SECTOR_SIZE * 8) + 1;
	unsigned sector = ffs_find_free_sector(cache);
	if (sector){
		ffs_set_bitmap(b, cache, sector, 1);
		return sector;
	}

	for (i = 1; i <= bitmap_sector_count; i++){
		cache->sector = i;
		ffs_load_bitmap(b, cache);
		sector = ffs_find_free_sector(cache);
		if (sector){
			ffs_set_bitmap(b, cache, sector, 1);
			return sector;
		}
	}

	return 0;
}

static void ffs_update_node_meta(struct ffs_inode* node)
{

}

static struct super_operations ffs_super_operations = {
	ffs_create_inode,
	ffs_destroy_inode,
	ffs_write_super,
	ffs_write_desc,
	ffs_free_inode,
	ffs_get_root
};

static struct inode_opreations ffs_inode_operations = {
	ffs_get_mode,
	ffs_read_file,
	ffs_write_file,
	ffs_open_dir,
	ffs_read_dir,
	ffs_add_dir_entry,
	ffs_del_dir_entry,
	ffs_close_dir,
	ffs_get_name,
	ffs_get_size,
	ffs_copy_stat
};

void ffs_attach(block* b)
{
	struct ffs_super_node* super = kmalloc(BLOCK_SECTOR_SIZE);
	struct ffs_bitmap_cache* desc = 0;
	struct filesys_type* type = 0;
	int i = 0;

	b->read(b->aux, 0, super, BLOCK_SECTOR_SIZE);
	desc = kmalloc(sizeof(*desc));
	desc->sector = 1;
	ffs_load_bitmap(b, desc);

	type = register_vfs(super, desc, b, &ffs_super_operations, &ffs_inode_operations, "ffs");
	
}

void ffs_format(block* b)
{
	int super_block_len = sizeof(struct ffs_super_node);
	char* buf = 0;
	int bitmap_sector_count;
	struct ffs_super_node* super = 0;
	struct ffs_bitmap_cache* bitmap = 0;
	int i = 0;

	if (super_block_len != BLOCK_SECTOR_SIZE)
		return;

	buf = kmalloc(super_block_len);
	memset(buf, 0, BLOCK_SECTOR_SIZE);
	super = buf;
	super->bitmap_sector = 1;
	super->magic = 0xffff5a5a;
	super->root.len = 0;
	super->root.mode = (S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH | S_IFDIR);
	super->root.mt_access = super->root.mt_create = super->root.mt_modify =
		(unsigned)time(0);
	strcpy(super->root.name, "/");
	super->root.sectors[0] = super->root.sectors[1] = 
		super->root.sectors[3] = 0;
	super->total_size = b->sector_size;
	
	// init bitmap
	// 1 bit for 1 sector, so one sector can describe 512*8 sectors
	bitmap_sector_count = (super->total_size - 1) / (BLOCK_SECTOR_SIZE * 8) + 1;
	super->first_valid_sector = 1 + bitmap_sector_count;
	super->used_size = 1 + bitmap_sector_count;

	b->write(b->aux, 0, buf, BLOCK_SECTOR_SIZE);

	memset(buf, 0, BLOCK_SECTOR_SIZE);
	for (i = 1; i <= bitmap_sector_count; i++){
		b->write(b->aux, i, buf, BLOCK_SECTOR_SIZE);
	}
	kfree(buf);

	bitmap = kmalloc(sizeof(*bitmap));
	bitmap->sector = 1;
	for (i = 0; i <= bitmap_sector_count; i++)
	{
		ffs_set_bitmap(b, bitmap, i, 1);
	}

	ffs_flush_bitmap(b, bitmap);

	kfree(bitmap);




}

static INODE ffs_create_inode(struct filesys_type* t)
{
	UNIMPL;
	return 0;
}

static void ffs_destroy_inode(struct filesys_type* t, INODE n)
{
	UNIMPL;
	return;
}

static void ffs_write_super(struct filesys_type* t, SUPORBLOCK s)
{
	block* b = t->dev;

	b->write(b->aux, 0, s, BLOCK_SECTOR_SIZE);
}

static void ffs_write_desc(struct filesys_type* t, DESC d)
{
	ffs_flush_bitmap(t->dev, d);
}

static void ffs_free_inode(struct filesys_type* t, INODE d)
{
	kfree(d);
}

static INODE ffs_get_root(struct filesys_type* t)
{
	struct ffs_inode* node = kmalloc(sizeof(*node));
	struct ffs_super_node* super = t->sb;

	node->type = t;
	memcpy(&node->meta, &super->root, sizeof(super->root));
	node->meta_sector = 0;
	node->meta_offset = &(((struct ffs_super_node*)0)->root);

	return node;
}

static unsigned ffs_get_mode(INODE inode)
{
	struct ffs_inode* node = inode;

	return node->meta.mode;
}

static unsigned int ffs_file_get_ino(struct ffs_inode* node, unsigned index, void* buf)
{
	unsigned int ino;
	block* b = node->type->dev;

	if (index < 1) // directly
	{
		ino = node->meta.sectors[0];
	}
	else if (index < (INODE_PER_SECTOR + 1)) // indirect
	{
		unsigned i = node->meta.sectors[1];
		unsigned* table;
		if (i == 0)
			return 0;

		index -= 1;
		b->read(b->aux, i, buf, BLOCK_SECTOR_SIZE);
		table = (unsigned*)buf;
		ino = table[index];

	}
	else if (index < (INODE_PER_SECTOR*INODE_PER_SECTOR + INODE_PER_SECTOR + 1)) // double indirect
	{
		unsigned i = node->meta.sectors[2];
		unsigned level1_index, level2_index;
		unsigned* table;

		if (!i)
			return 0;

		index -= (INODE_PER_SECTOR + 1);
		level1_index = index / INODE_PER_SECTOR;
		level2_index = index % INODE_PER_SECTOR;

		b->read(b->aux, i, buf, BLOCK_SECTOR_SIZE);
		table = (unsigned*)buf;
		i = table[level1_index];
		if (!i)
			return 0;

		b->read(b->aux, i, buf, BLOCK_SECTOR_SIZE);
		table = (unsigned*)buf;
		ino = table[level2_index];

	}
	else // not possible
	{
		return 0;
	}

	return ino;
}

static int ffs_dentry_get_meta(struct ffs_inode* node, unsigned index, struct ffs_meta_info* meta, void* buf)
{
	unsigned ino;
	block* b = node->type->dev;
	unsigned offset = index % META_PER_SECTOR;
	struct ffs_meta_info* table;

	ino = ffs_file_get_ino(node, index / META_PER_SECTOR, buf);
	if (!ino)
		return 0;

	b->read(b->aux, ino, buf, BLOCK_SECTOR_SIZE);
	table = buf;
	memcpy(meta, &(table[offset]), sizeof(*table));
	return 0;

}

static unsigned ffs_create_inode_at_table(struct ffs_inode* node, unsigned table_ino, 
	unsigned index, void* buf, int replace)
{
	block* b = node->type->dev;
	unsigned *table;
	unsigned ino;

	b->read(b->aux, table_ino, buf, BLOCK_SECTOR_SIZE);
	table = buf;
	if (table[index] && !replace)
		return table[index];

	ino = ffs_alloc_free_sector(node, node->type->desc);
	if (!ino)
		return 0;

	table[index] = ino;
	b->write(b->aux, table_ino, buf, BLOCK_SECTOR_SIZE);
	return ino;
}

static unsigned ffs_file_create_inode(struct ffs_inode* node, unsigned index, void* buf)
{
	unsigned ino;
	block* b = node->type->dev;

	if (index < 1) // directly
	{
		ino = ffs_alloc_free_sector(node, node->type->desc);
		if (!ino)
			return 0;

		node->meta.sectors[0] = ino;
	}
	else if (index < (INODE_PER_SECTOR + 1)) // indirect
	{
		unsigned indirect_ino = node->meta.sectors[1];
		unsigned* table;
		if (!indirect_ino){
			indirect_ino = ffs_alloc_free_sector(node, node->type->desc);
			if (!indirect_ino)
				return 0;
			node->meta.sectors[1] = indirect_ino;
			
		}
		index -= 1;

		ino = ffs_create_inode_at_table(node, indirect_ino, index, buf, 1);
		return ino;

	}
	else if (index < (INODE_PER_SECTOR*INODE_PER_SECTOR + INODE_PER_SECTOR + 1)) // double indirect
	{
		unsigned indirect_ino = node->meta.sectors[2];
		unsigned* table;
		unsigned level1_index, level2_index;
		unsigned level1_ino, level2_ino;

		index -= (INODE_PER_SECTOR + 1);
		level1_index = index / INODE_PER_SECTOR;
		level2_index = index % INODE_PER_SECTOR;

		if (!indirect_ino){
			indirect_ino = ffs_alloc_free_sector(node, node->type->desc);
			if (!indirect_ino)
				return 0;
			node->meta.sectors[2] = indirect_ino;
		}

		level1_ino = ffs_create_inode_at_table(node, indirect_ino, level1_index, buf, 0);

		level2_ino = ffs_create_inode_at_table(node, level1_ino, level2_index, buf, 0);

		return level2_ino;
	}
	else // not possible
	{
		return 0;
	}

	return ino;

}

static unsigned ffs_read_file(INODE inode, unsigned int offset, char* buf, unsigned len)
{
	struct ffs_inode* node = inode;
	block* b = node->type->dev;
	unsigned int inode_index = offset / BLOCK_SECTOR_SIZE;
	unsigned int offset_in_sec = offset % BLOCK_SECTOR_SIZE;
	unsigned int ino;
	void* tmp = kmalloc(BLOCK_SECTOR_SIZE);
	unsigned total = ((node->meta.len - offset) > len) ? len : (node->meta.len - offset);
	unsigned left = total;




	while (left){
		char* t = 0;
		unsigned copy_size = BLOCK_SECTOR_SIZE - offset_in_sec;
		copy_size = (copy_size > left) ? left : copy_size;
		ino = ffs_file_get_ino(node, inode_index, tmp);
		if (!ino)
			break;

		b->read(b->aux, ino, tmp, BLOCK_SECTOR_SIZE);
		t = tmp;
		t += offset_in_sec;
		memcpy(buf, t, copy_size);

		buf += copy_size;
		left -= copy_size;
		offset_in_sec = 0;
		inode_index++;
	}

	kfree(tmp);
	return (total - left);
}

static unsigned ffs_write_file(INODE inode, unsigned int offset, char* buf, unsigned len)
{
	struct ffs_inode* node = inode;
	block* b = node->type->dev;
	unsigned int inode_index = offset / BLOCK_SECTOR_SIZE;
	unsigned int offset_in_sec = offset % BLOCK_SECTOR_SIZE;
	unsigned int ino;
	void* tmp;
	unsigned left = len;

	if ((offset + len) > MAX_FILE_SIZE)
		return 0;

	tmp = kmalloc(BLOCK_SECTOR_SIZE);
	while (left){
		char* t = 0;
		unsigned copy_size = BLOCK_SECTOR_SIZE - offset_in_sec;
		copy_size = (copy_size > left) ? left : copy_size;

		ino = ffs_file_get_ino(node, inode_index, tmp);
		if (!ino){
			ino = ffs_file_create_inode(node, inode_index, tmp);
			if (!ino)
				break;
		}

		if (copy_size == BLOCK_SECTOR_SIZE){
			b->write(b->aux, ino, buf, copy_size);
		}
		else{
			b->read(b->aux, ino, tmp, BLOCK_SECTOR_SIZE);
			t = tmp;
			t += offset_in_sec;
			memcpy(t, buf, copy_size);
			b->write(b->aux, ino, tmp, BLOCK_SECTOR_SIZE);
		}

		buf += copy_size;
		left -= copy_size;
		offset_in_sec = 0;
		inode_index++;

	}

	node->meta.len = (offset + len - left);
	node->meta.mt_modify = time(0);
	ffs_update_node_meta(node);

	kfree(tmp);
	return (len - left);
}

static DIR ffs_open_dir(INODE inode)
{
	return 0;
}

static INODE ffs_read_dir(DIR dir)
{
	return 0;
}

static void ffs_add_dir_entry(DIR dir, unsigned mode, char* name)
{
	return;
}

static void ffs_del_dir_entry(DIR dir, char* name)
{
	return;
}

static void ffs_close_dir(DIR dir)
{
	return;
}

static char* ffs_get_name(INODE node)
{
	struct ffs_inode* n = node;
	return n->meta.name;
}

static unsigned int ffs_get_size(INODE node)
{
	struct ffs_inode* n = node;
	return n->meta.len;
}

static int ffs_copy_stat(INODE node, struct stat* s)
{
	struct ffs_inode* n = node;
	s->st_atime = n->meta.mt_access;
	return 1;
}

