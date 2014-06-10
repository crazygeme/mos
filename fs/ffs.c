#include <fs/ffs.h>
#include <fs/vfs.h>
#ifdef WIN32
#include <osdep.h>
#include <time.h>
#elif MACOS
#include <osdep.h>
#include <time.h>
#else
#include <lib/klib.h>
#include <int/timer.h>
#include <syscall/unistd.h>
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
static int ffs_copy_stat(INODE node, struct stat* s, int is_dir);

static void ffs_set_bitmap(block* b, struct ffs_bitmap_cache* cache, unsigned sector, unsigned used);
static void ffs_load_bitmap(block* b, struct ffs_bitmap_cache* cache);
static void ffs_flush_bitmap(block* b, struct ffs_bitmap_cache* cache);

#ifdef DEBUG_FFS
static unsigned long break_find_ino = 0;
static unsigned long break_write = 0;
static time_t find_time;
static time_t write_time;

static void break_find_begin()
{
	timer_current(&find_time);
}

static void break_write_begin()
{
	timer_current(&write_time);
}

static void break_find_end()
{
	time_t now;
	unsigned long span;
	timer_current(&now);
	span = now.seconds*1000 + now.milliseconds - 
		find_time.seconds*1000 - find_time.milliseconds;
	break_find_ino += span;
}

static void break_write_end()
{
	time_t now;
	unsigned long span;
	timer_current(&now);
	span = now.seconds*1000 + now.milliseconds - 
		write_time.seconds*1000 - write_time.milliseconds;
	break_write += span;
}

void report_time()
{
	printk("find ino use %u milli-seconds, write use %u milli-seconds\n",
		   break_find_ino, break_write);
	break_write = break_find_ino = 0;
}

#endif



/**
 * mark sector $sector into $used 
 * supply cache to spped up 
 * @author ejzheng (6/4/2014)
 * 
 * @param b block device
 * @param cache cache to speed up, which contains the last 
 *  			accessed sector data
 * @param sector sector number
 * @param used used or not
 */
static void ffs_set_bitmap(block* b, struct ffs_bitmap_cache* cache, unsigned sector, unsigned used)
{
	unsigned index = sector / (BLOCK_SECTOR_SIZE * 8) + 1;
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

/**
 * Reload sector cache
 * cache number described in cache->sector 
 *  
 * @author ejzheng (6/4/2014)
 * 
 * @param b block device
 * @param cache cache to speed up, which contains the last 
 *  			accessed sector data
 */
static void ffs_load_bitmap(block* b, struct ffs_bitmap_cache* cache)
{
	b->read(b->aux, cache->sector, &(cache->node.bits[0]), BLOCK_SECTOR_SIZE);
}

/**
 * Flush sector cache
 * cache number described in cache->sector 
 *  
 * @author ejzheng (6/4/2014)
 * 
 * @param b block device
 * @param cache cache to speed up, which contains the last 
 *  			accessed sector data
 */
static void ffs_flush_bitmap(block* b, struct ffs_bitmap_cache* cache)
{
	b->write(b->aux, cache->sector, &(cache->node.bits[0]), BLOCK_SECTOR_SIZE);
}

/**
 * Find a free sector , seek inside cache
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param cache 
 * 
 * @return unsigned 1 for found , 0 for not found
 */
static unsigned ffs_find_free_sector(struct ffs_bitmap_cache* cache)
{
	unsigned bitmap_sector = cache->sector-1;
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
		if (((~byte) & 0x1) == 0x1){
			bitmap_byte_offset = i;
			break;
		}
		byte = byte >> 1;
	}

	if (i == 8)
		return 0;


	return (bitmap_sector*BLOCK_SECTOR_SIZE * 8 + bitmap_byte_index * 8 + bitmap_byte_offset);
}

/**
 * Find a free sector for INODE, first seek inside cache, if not
 * found enum all bitmaps and find a free one, then reload cache 
 * into that sector 
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node INODE which needs a new sector
 * @param cache to speed up
 * 
 * @return unsigned $sector_number for found, 0 for not found
 */
static unsigned ffs_alloc_free_sector(struct ffs_inode* node, struct ffs_bitmap_cache* cache)
{
	block* b = node->type->dev;
	struct ffs_super_node* super = node->type->sb;
	char* tmp = 0;
	int i = 1;
	unsigned bitmap_sector_count = (super->total_size - 1) / (BLOCK_SECTOR_SIZE * 8) + 1;
	unsigned sector = ffs_find_free_sector(cache);

	tmp = kmalloc(BLOCK_SECTOR_SIZE);
	memset(tmp, 0, BLOCK_SECTOR_SIZE);
	if (sector){
		ffs_set_bitmap(b, cache, sector, 1);
		b->write(b->aux, sector, tmp, BLOCK_SECTOR_SIZE);
		super->used_size++;
		kfree(tmp);

		return sector;
	}

	ffs_flush_bitmap(b, cache);

	for (i = 1; i <= bitmap_sector_count; i++){
		cache->sector = i;
		ffs_load_bitmap(b, cache);
		sector = ffs_find_free_sector(cache);
		if (sector){
			ffs_set_bitmap(b, cache, sector, 1);
			b->write(b->aux, sector, tmp, BLOCK_SECTOR_SIZE);
			super->used_size++;
			kfree(tmp);

			return sector;
		}
	}

	kfree(tmp);
	return 0;
}


/**
 * update meta data for INODE
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node INODE which want to update its meta
 * @param buf as a temporary memory area, to reduce malloc/free 
 *  		  times
 */
static void ffs_update_node_meta(struct ffs_inode* node, char* buf)
{
	block* b = node->type->dev;
	struct ffs_meta_info* info;
	struct ffs_super_node* super = node->type->sb;

	b->read(b->aux, node->meta_sector, buf, BLOCK_SECTOR_SIZE);
	info = (struct ffs_meta_info*)(buf + node->meta_offset);
	memcpy(info, &node->meta, sizeof(*info));
	b->write(b->aux, node->meta_sector, buf, BLOCK_SECTOR_SIZE);

	if (node->meta_sector == 0){
		memcpy(&super->root, &node->meta, sizeof(*info));
	}

}

/**
 * Remove an INODE which store its meta in $meta
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node parent inode
 * @param meta meta for inode needs remove
 */
static void ffs_remove_ino_in_meta(struct ffs_inode* node, struct ffs_meta_info* meta)
{
	char* buf = 0;
	unsigned ino;
	block* dev = node->type->dev;
	int i, j;

	buf = kmalloc(BLOCK_SECTOR_SIZE);
	memset(buf, 0, BLOCK_SECTOR_SIZE);
	ino = meta->sectors[0];
	if (ino){
		ffs_set_bitmap(node->type->dev, node->type->desc, ino, 0);

		meta->sectors[0] = 0;
	}

	ino = meta->sectors[1];
	if (ino){
		unsigned* table = buf;
		dev->read(dev->aux, ino, buf, BLOCK_SECTOR_SIZE);
		for (i = 0; i < INODE_PER_SECTOR; i++){
			if (table[i])
				ffs_set_bitmap(node->type->dev, node->type->desc, table[i], 0);
		}
		ffs_set_bitmap(node->type->dev, node->type->desc, ino, 0);
	}

	ino = meta->sectors[2];
	if (ino){
		char* tmp = 0;
		unsigned* table = tmp;

		tmp = kmalloc(BLOCK_SECTOR_SIZE);
		memset(tmp, 0, BLOCK_SECTOR_SIZE);
		dev->read(dev->aux, ino, tmp, BLOCK_SECTOR_SIZE);
		for (i = 0; i < INODE_PER_SECTOR; i++){
			unsigned* level2_table = buf;
			unsigned level2 = table[i];
			if (!level2)
				continue;

			dev->read(dev->aux, level2, buf, BLOCK_SECTOR_SIZE);
			for (j = 0; j < INODE_PER_SECTOR; j++){
				if (level2_table[j])
					ffs_set_bitmap(node->type->dev, node->type->desc, level2_table[j], 0);
			}
			ffs_set_bitmap(node->type->dev, node->type->desc, level2, 0);
		}
		ffs_set_bitmap(node->type->dev, node->type->desc, ino, 0);
		kfree(tmp);
	}

	kfree(buf);
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

/**
 * Attach file system into hardware (most likely a harddisk)
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param b block struct for a block device
 */
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

	type = register_vfs(super, desc, b, &ffs_super_operations, &ffs_inode_operations, "rootfs");
	
}

/**
 * Format a block device into ffs filesystem
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param b block struct for a block device
 */
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
		super->root.sectors[2] = 0;
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
	ffs_load_bitmap(b, bitmap);
	for (i = 0; i <= bitmap_sector_count; i++)
	{
		ffs_set_bitmap(b, bitmap, i, 1);
	}

	ffs_flush_bitmap(b, bitmap);

	kfree(bitmap);




}

/**
 * useless!!
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param t 
 * 
 * @return INODE 
 */
static INODE ffs_create_inode(struct filesys_type* t)
{
	UNIMPL;
	return 0;
}


/**
 * useless!!
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param t 
 * @param n 
 */
static void ffs_destroy_inode(struct filesys_type* t, INODE n)
{
	UNIMPL;
	return;
}

/**
 * Flush super block
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param t file system type
 * @param s super block
 */
static void ffs_write_super(struct filesys_type* t, SUPORBLOCK s)
{
	block* b = t->dev;

	b->write(b->aux, 0, s, BLOCK_SECTOR_SIZE);
}

/**
 * Flush descriptor( bitmaps in ffs case)
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param t file system type
 * @param d descriptor
 */
static void ffs_write_desc(struct filesys_type* t, DESC d)
{
	ffs_flush_bitmap(t->dev, d);
}

/**
 * Free an INODE memory
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param t filesystem type
 * @param d inode needs free
 */
static void ffs_free_inode(struct filesys_type* t, INODE d)
{
	kfree(d);
}

/**
 * Get the root INODE in this block
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param t filesystem type
 * 
 * @return INODE root INODE
 */
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

/**
 * get file mode for an INODE
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param inode 
 * 
 * @return unsigned file mode
 */
static unsigned ffs_get_mode(INODE inode)
{
	struct ffs_inode* node = inode;

	return node->meta.mode;
}

/**
 * Get sector number at $index
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node 
 * @param index 
 * @param buf a temporaty memory buf to reduce malloc/free times
 * 
 * @return unsigned int $sector_number if found, 0 if not found
 */
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
		unsigned level1_ino = 0, level2_ino = 0;

		if (!i)
			return 0;

		index -= (INODE_PER_SECTOR + 1);
		level1_index = index / INODE_PER_SECTOR;
		level2_index = index % INODE_PER_SECTOR;

		memset(buf, 0, BLOCK_SECTOR_SIZE);
		b->read(b->aux, i, buf, BLOCK_SECTOR_SIZE);
		table = (unsigned*)buf;
		i = table[level1_index];
		level1_ino = i;
		if (!i)
			return 0;

		memset(buf, 0, BLOCK_SECTOR_SIZE);
		b->read(b->aux, i, buf, BLOCK_SECTOR_SIZE);
		table = (unsigned*)buf;
		ino = table[level2_index];
		level2_ino = ino;



	}
	else // not possible
	{
		return 0;
	}

	return ino;
}

/**
 * callbacks for ffs_enum_dentry, trigger when empty sector at 
 * the end of this file if found 
 * $index sector in $node is empty, you can use $buf as 
 * temporary memory 
 */
typedef void (*fp_empty_ino_callback)(struct ffs_inode* node, unsigned index, void* buf, void* aux);

/**
 * callbacks for ffs_enum_dentry, trigger when a valid sector is 
 * found 
 * sector $ino contains valid inode, whose parent is $node, the
 * $offset's data in $ino sector, dentry table represent in 
 * $sector_ctx , it's index in table is $count 
 * retrn 1 to continue search, 0 to break immediately 
 */
typedef int (*fp_valid_dentry_callback)(struct ffs_inode* node, unsigned ino, 
	unsigned offset, unsigned count, 
	void* sector_ctx,
	void* aux);

/**
 * callbacks for ffs_enum_dentry, trigger when a hole is found 
 * (a hole is an entry that was created then deleted) 
 *  sector $ino contains a hole, whose parent is $node, the
 *  $offset's entry in $ino is a hole, dentry table represent in
 *  $sector_ctx
 *  retrn 1 to continue search, 0 to break immediately 
 */
typedef int(*fp_dentry_hole_found)(struct ffs_inode* node, unsigned ino, unsigned offset, void* sector_ctx, void* aux);

/**
 * enum a directory entry, trigger callbacks when issue detected
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node a directory inode
 * @param buf temporary memory
 * @param valid valid callback
 * @param empty empty callback
 * @param hole hole callback
 * @param aux 
 */
static void ffs_enum_dentry(struct ffs_inode* node, void* buf, 
	fp_valid_dentry_callback valid,
	fp_empty_ino_callback empty,
	fp_dentry_hole_found hole,
	void* aux)
{
	unsigned ino;
	block* b = node->type->dev;
	struct ffs_meta_info* table;
	unsigned max = (INODE_PER_SECTOR*INODE_PER_SECTOR + INODE_PER_SECTOR + 1)*META_PER_SECTOR;
	unsigned int i, j, count, offset;


	max = (INODE_PER_SECTOR*INODE_PER_SECTOR + INODE_PER_SECTOR + 1)*META_PER_SECTOR;
	count = 0;
	for (i = 0; i < max ; i++){
		ino = ffs_file_get_ino(node, i , buf);
		if (!ino){
			if (empty)
				empty(node, i , buf, aux);
			return;
		}

		b->read(b->aux, ino, buf, BLOCK_SECTOR_SIZE);
		table = buf;

		offset = 0;
		for (j = 0; j < META_PER_SECTOR; j++){
			if (table[j].mode){
				int _continue = 1;
				count++;
				if (valid)
					_continue = valid(node, ino, j, count, buf, aux);
				if (!_continue)
					return;
				
			}
			else{
				int _continue = 1;
				if (hole)
					_continue = hole(node, ino, j, buf, aux);
				if (!_continue)
					return;
			}
		}


	}
	return;

}

typedef struct _ffs_dentry_enum_block
{
	struct ffs_meta_info* meta;
	unsigned index;
	unsigned* no;
	unsigned* off;
	int ok;
}ffs_dentry_enum_block;

/**
 * valid callback for dentry reading
 * 
 * @author ejzheng (6/4/2014)
 */
static int valid_dentry_callback(struct ffs_inode* node, unsigned ino,
	unsigned offset, unsigned count, void* sector_ctx, void* aux)
{
	ffs_dentry_enum_block* block = aux;
	struct ffs_meta_info* table = sector_ctx;

	if (block->index == (count - 1)){
		block->ok = 1;
		*block->no = ino;
		*block->off = offset;
		memcpy(block->meta, &table[offset], sizeof(*table));
		return 0;
	}

	return 1;
}

/**
 * read $index's dentry meta inside $node
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node 
 * @param index 
 * @return meta meta info read
 * @return no sector number contains this meta
 * @return off offset inside sector
 * @param buf temporary memory
 * 
 * @return int 1 for found, 0 for not found
 */
static int ffs_dentry_get_meta(struct ffs_inode* node, unsigned index, 
				struct ffs_meta_info* meta, 
				unsigned* no, unsigned* off,
				void* buf)
{
	ffs_dentry_enum_block block;

	if (index >= node->meta.len){
		*no = 0;
		return 0;
	}

	block.ok = 0;
	block.meta = meta;
	block.no = no;
	block.off = off;
	block.index = index;

	ffs_enum_dentry(node, buf, valid_dentry_callback, 0, 0, &block);

	if (block.ok){
		*off = *off * sizeof(*meta);
		return block.no;
	}

	return 0;

}

/**
 * add an inode inside $node 
 * !! this is a file operation 
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node which needs to add new inode
 * @param table_ino sector number for inode table
 * @param index index inside inode table
 * @param buf 
 * @param replace replace old one if set to 1, orelse just 
 *  			  returns
 * 
 * @return unsigned 1 for success, 0 for fail
 */
static unsigned ffs_create_inode_at_table(struct ffs_inode* node, unsigned table_ino, 
	unsigned index, void* buf, int replace)
{
	block* b = node->type->dev;
	unsigned *table;
	unsigned ino;

	memset(buf, 0, BLOCK_SECTOR_SIZE);
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

/**
 * Create new node inside $node at $index
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node 
 * @param index 
 * @param buf 
 * 
 * @return unsigned 1 for success, 0 for fail
 */
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
		unsigned level1_ino = 0, level2_ino = 0;

		index -= (INODE_PER_SECTOR + 1);
		level1_index = index / INODE_PER_SECTOR;
		level2_index = index % INODE_PER_SECTOR;

		if (!indirect_ino){
			indirect_ino = ffs_alloc_free_sector(node, node->type->desc);
			if (!indirect_ino)
				return 0;
			node->meta.sectors[2] = indirect_ino;
		}else{
			memset(buf, 0, BLOCK_SECTOR_SIZE);
			b->read(b->aux, indirect_ino, buf, BLOCK_SECTOR_SIZE);
			table = (unsigned*)buf;
			level1_ino = table[level1_index];
		}

		if (!level1_ino){
			level1_ino = ffs_create_inode_at_table(node, indirect_ino, level1_index, buf, 0);
		}

		level2_ino = ffs_create_inode_at_table(node, level1_ino, level2_index, buf, 0);

		return level2_ino;
	}
	else // not possible
	{
		return 0;
	}

	return ino;

}

/**
 * read data in $inode at offset $offset, put data in $buf
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param inode which present a file
 * @param offset read offset
 * @param buf 
 * @param len 
 * 
 * @return unsigned data readed
 */
static unsigned ffs_read_file(INODE inode, unsigned int offset, char* buf, unsigned len)
{
	struct ffs_inode* node = inode;
	block* b = node->type->dev;
	unsigned int inode_index = offset / BLOCK_SECTOR_SIZE;
	unsigned int offset_in_sec = offset % BLOCK_SECTOR_SIZE;
	unsigned int ino;
	void* tmp = 0;
	unsigned total = ((node->meta.len - offset) > len) ? len : (node->meta.len - offset);
	unsigned left = total;

    if (offset >= node->meta.len) {
        return 0;
    }

	tmp = kmalloc(BLOCK_SECTOR_SIZE);
	memset(tmp, 0, BLOCK_SECTOR_SIZE);
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

/**
 * write data in $inode at offset $offset, get data in $buf
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param inode which present a file
 * @param offset write offset
 * @param buf 
 * @param len 
 * 
 * @return unsigned data wrote
 */
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
	memset(tmp, 0, BLOCK_SECTOR_SIZE);
	while (left){
		char* t = 0;
		unsigned copy_size = BLOCK_SECTOR_SIZE - offset_in_sec;
		copy_size = (copy_size > left) ? left : copy_size;
#ifdef DEBUG_FFS
		break_find_begin();
#endif
		ino = ffs_file_get_ino(node, inode_index, tmp);
		if (!ino){
			ino = ffs_file_create_inode(node, inode_index, tmp);
			if (!ino)
				break;
		}
#ifdef DEBUG_FFS
		break_find_end();
#endif

#ifdef DEBUG_FFS
		break_write_begin();
#endif
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
#ifdef DEBUG_FFS
		break_write_end();
#endif
		buf += copy_size;
		left -= copy_size;
		offset_in_sec = 0;
		inode_index++;

	}

	node->meta.len = (offset + len - left);
	node->meta.mt_modify = time(0);
	ffs_update_node_meta(node, tmp);

	kfree(tmp);
	return (len - left);
}


/**
 * create a DIR struct, and a reading stream for $inode
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param inode represent a directory
 * 
 * @return DIR struct for upper layer usage
 */
static DIR ffs_open_dir(INODE inode)
{
	struct ffs_dir* dir = kmalloc(sizeof(*dir));
	struct ffs_inode* node = inode;

	if (!S_ISDIR(node->meta.mode)){
		kfree(dir);
		return 0;
	}

	dir->type = inode->type;
	dir->cur = 0;
	memcpy(&dir->self, node, sizeof(*node));
	return dir;
}

/**
 * read dentry inside dir, 0 when reach ends
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param d 
 * 
 * @return INODE 
 */
static INODE ffs_read_dir(DIR d)
{
	struct ffs_inode* ret = 0;
	struct ffs_dir* dir = d;
	char* tmp;


	ret = kmalloc(sizeof(*ret));
	tmp = kmalloc(BLOCK_SECTOR_SIZE);
	memset(tmp, 0, BLOCK_SECTOR_SIZE);
	ffs_dentry_get_meta(&dir->self, dir->cur, &ret->meta, &ret->meta_sector, &ret->meta_offset, tmp);
	if (!ret->meta_sector){
		kfree(ret);
		kfree(tmp);
		return 0;
	}
	kfree(tmp);

	ret->type = dir->type;
	dir->cur++;
	return ret;
	
	
}

typedef struct _dir_add_del_block
{
	unsigned mode;
	char* name;
	int ok;
}dir_add_del_block;

/**
 * empty callbacks for dir add operation
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node 
 * @param index 
 * @param buf 
 * @param aux 
 */
static void dir_add_empty_ino_callback(struct ffs_inode* node, unsigned index, void* buf, void* aux)
{
	dir_add_del_block* b = aux;
	block* dev = node->type->dev;
	struct ffs_meta_info* table = buf;

	unsigned ino = ffs_file_create_inode(node, index, buf);
	if (!ino){
		b->ok = 0;
		return;
	}

	dev->read(dev->aux, ino, buf, BLOCK_SECTOR_SIZE);
	table[0].len = 0;
	table[0].mode = b->mode;
	table[0].mt_access = table[0].mt_create = table[0].mt_modify = time(0);
	table[0].sectors[0] = table[0].sectors[1] = table[0].sectors[2] = 0;
	strcpy(table[0].name, b->name);
	dev->write(dev->aux, ino, buf, BLOCK_SECTOR_SIZE);
	b->ok = 1;
}

/**
 * hole callbacks for dir add operation
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node 
 * @param ino 
 * @param offset 
 * @param sector_ctx 
 * @param aux 
 * 
 * @return int 
 */
static int dir_add_dentry_hole_found(struct ffs_inode* node, unsigned ino, unsigned offset, void* sector_ctx, void* aux)
{
	struct ffs_meta_info* table = sector_ctx;
	dir_add_del_block* b = aux;
	block* dev = node->type->dev;

	table[offset].len = 0;
	table[offset].mode = b->mode;
	table[offset].mt_access = table[offset].mt_create = table[offset].mt_modify = time(0);
	table[offset].sectors[0] = table[offset].sectors[1] = table[offset].sectors[2] = 0;
	strcpy(table[offset].name, b->name);

	dev->write(dev->aux, ino, sector_ctx, BLOCK_SECTOR_SIZE);
	b->ok = 1;
	return 0;
}

/**
 * valid callbacks for dir del operation
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node 
 * @param index 
 * @param buf 
 * @param aux 
 */
static int dir_delete_dentry_callback(struct ffs_inode* node, unsigned ino,
	unsigned offset, unsigned count, void* sector_ctx, void* aux)
{
	struct ffs_meta_info* table = sector_ctx;
	dir_add_del_block* b = aux;
	block* dev = node->type->dev;

	if (!strcmp(table[offset].name, b->name)){
		ffs_remove_ino_in_meta(node, &table[offset]);

		memset(&table[offset], 0, sizeof(*table));
		dev->write(dev->aux, ino, sector_ctx, BLOCK_SECTOR_SIZE);
		b->ok = 1;
		return 0;
	}

	return 1;
}


/**
 * Add an entry 
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param d dir which needs an new entry
 * @param mode for new entry
 * @param name for new entry
 */
static void ffs_add_dir_entry(DIR d, unsigned mode, char* name)
{
	struct ffs_dir* dir = d;
	dir_add_del_block b;
	char* buf = 0;


	buf = kmalloc(BLOCK_SECTOR_SIZE);
	memset(buf, 0, BLOCK_SECTOR_SIZE);
	b.mode = mode;
	b.name = name;
	b.ok = 0;
	ffs_enum_dentry(&dir->self, buf, 0, dir_add_empty_ino_callback, dir_add_dentry_hole_found, &b);

	if (b.ok){
		dir->self.meta.len++;
		ffs_update_node_meta(&dir->self, buf);
	}

	kfree(buf);


	return;
}

/**
 * remove an entry in dir
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param d dir which want to remove an entry
 * @param name entry name that needs remove
 */
static void ffs_del_dir_entry(DIR d, char* name)
{
	struct ffs_dir* dir = d;
	dir_add_del_block b;
	char* buf = 0;


	buf = kmalloc(BLOCK_SECTOR_SIZE);
	memset(buf, 0, BLOCK_SECTOR_SIZE);
	b.name = name;
	b.ok = 0;
	ffs_enum_dentry(&dir->self, buf, dir_delete_dentry_callback, 0, 0, &b);
	if (b.ok){
		dir->self.meta.len--;
		ffs_update_node_meta(&dir->self, buf);
	}
	kfree(buf);
	return;
}

/**
 * close dir and it's reading stream
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param dir 
 */
static void ffs_close_dir(DIR dir)
{
	kfree(dir);
	return;
}

/**
 * get name of INODE
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node 
 * 
 * @return char* 
 */
static char* ffs_get_name(INODE node)
{
	struct ffs_inode* n = node;
	return n->meta.name;
}

/**
 * get size of INODE
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node 
 * 
 * @return unsigned int 
 */
static unsigned int ffs_get_size(INODE node)
{
	struct ffs_inode* n = node;
	return n->meta.len;
}

/**
 * copy struct stat of an INODE
 * 
 * @author ejzheng (6/4/2014)
 * 
 * @param node 
 * @param s 
 * 
 * @return int 
 */
static int ffs_copy_stat(INODE node, struct stat* s, int is_dir)
{
	struct ffs_inode* n = 0;
	struct ffs_super_node* super = 0;
	struct ffs_dir* dir = 0;

	if (is_dir) {
		dir = (struct ffs_dir*)node;
		n = &dir->self;
		super = dir->type->sb;
	}else{
		n = node;
		super = n->type->sb;
	}

	s->st_atime = n->meta.mt_access;
	s->st_mode = n->meta.mode;
	if (!S_ISDIR(s->st_mode)) {
		s->st_mode |= S_IFREG;
	}
	s->st_size = n->meta.len;
	s->st_blksize = BLOCK_SECTOR_SIZE;
	s->st_blocks = super->total_size;
	s->st_ctime = n->meta.mt_create;
	s->st_dev = 0;
	s->st_gid = 0;
	s->st_ino = n->meta_sector;
	s->st_mtime = n->meta.mt_modify;
	s->st_uid = 0;

	return 1;
}

