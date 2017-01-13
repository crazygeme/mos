#include <block.h>
#include <lock.h>
#include <klib.h>
#include <include/fs.h>

static struct _block_control
{
    LIST_ENTRY block_lists[BLOCK_MAX];
    unsigned int block_count[BLOCK_MAX];
    spinlock lock;
}control;

static unsigned int id;

static void block_add_to_list(block* b, block_type type)
{
    LIST_ENTRY* head = &control.block_lists[type];

    spinlock_lock(&control.lock);
    InsertTailList(head, &(b->block_list));
    control.block_count[type] = control.block_count[type] + 1;
    spinlock_unlock(&control.lock);
}

static block* block_get_first(block_type type)
{
    LIST_ENTRY* head = &control.block_lists[type];
    PLIST_ENTRY node = 0;
    block* ret = 0;

    if (IsListEmpty(head))
        return 0;

    spinlock_lock(&control.lock);

    node = head->Flink;

    spinlock_unlock(&control.lock);

    if (node == head)
        return 0;
    else
        return (CONTAINER_OF(node, block, block_list));

}

static block* block_get_next(block* b)
{
    LIST_ENTRY* head = &control.block_lists[b->type];
    PLIST_ENTRY node = 0;

    spinlock_lock(&control.lock);

    node = b->block_list.Flink;

    spinlock_unlock(&control.lock);

    if (node == head)
        return 0;
    else
        return (CONTAINER_OF(node, block, block_list));
}

static unsigned int block_id_gen()
{
    unsigned int ret;
    spinlock_lock(&control.lock);
    ret = id;
    id++;
    spinlock_unlock(&control.lock);
    return ret;
}

void block_init()
{
    int i = 0;

    for (i = 0; i < BLOCK_MAX; i++)
    {
        InitializeListHead(&control.block_lists[i]);
        control.block_count[i] = 0;
    }

    spinlock_init(&control.lock);

    id = 0;
}


block* block_register(void* aux, char* name, fpblock_read read, fpblock_write write, fpblock_close close,
    block_type type, unsigned int sector_size)
{
    block* b = kmalloc(sizeof(*b));

    memset(b, 0, sizeof(*b));
    b->aux = aux;
    if (name && *name)
        strcpy(b->name, name);
    else
        *b->name = '\0';

    b->read = read;
    b->write = write;
    b->close = close;
    b->type = type;
    b->sector_size = sector_size;
    b->id = block_id_gen();

    block_add_to_list(b, type);

    return b;
}

block* block_get_by_id(unsigned int id)
{
    int i = 0;
    block* b = 0;

    for (i = 0; i < BLOCK_MAX; i++)
    {
        for (b = block_get_first((block_type)i); b; b = block_get_next(b))
        {
            if (b->id == id)
                return b;
        }
    }

    return 0;
}

unsigned int block_get_by_type(block_type type, block** blocks)
{
    block* b = 0;
    int i = 0;
    int count = control.block_count[type];

    if (!blocks)
        return count;

    for (b = block_get_first(type), i = 0;
        (b) && (i < count);
        b = block_get_next(b), i++)
    {
        blocks[i] = b;
    }

    return count;
}

void block_close()
{
    int i = 0;
    block* b = 0;

    for (i = 0; i < BLOCK_MAX; i++)
    {
        for (b = block_get_first((block_type)i); b; b = block_get_next(b))
        {
            if (b->close)
            {
                b->close(b->aux);
            }
        }
    }
}

const char* block_type_name(block* b)
{
    switch (b->type)
    {
    case BLOCK_KERNEL:
        return "BLOCK_KERNEL";
    case BLOCK_LINUX:
        return "BLOCK_LINUX";
    case BLOCK_FILESYS:
        return "BLOCK_FILESYS";
    case BLOCK_SCRATCH:
        return "BLOCK_SCRATCH";
    case BLOCK_SWAP:
        return "BLOCK_SWAP";
    case BLOCK_RAW:
        return "BLOCK_RAW";
    case BLOCK_UNKNOW:
    default:
        return "BLOCK_UNKNOW";
    }
}

static int file_close(void* inode)
{
    int ret;
    ext4_file* file = inode;
    ret = ext4_fclose(file);
    free(inode);

    if (ret != EOK)
        return -1;
    return 0;
}

int file_seek(ext4_file *f, uint64_t offset, uint32_t origin)
{
    int ret = ext4_fseek(f, offset, origin);
    if (ret != EOK)
        return ret;
    
    return ext4_ftell(f);
}

static int dir_close(void* inode)
{
    int ret;
    ext4_dir* dir = inode;
    ret = ext4_dir_close(dir);
    free(inode);
    if (ret != EOK)
        return -1;
    return 0;
}

static int dir_read(ext4_dir *dir, void *buf, size_t size, size_t *rcnt)
{
    int ret = 0;
    ext4_direntry* entry = ext4_dir_entry_next(dir);
    if (!entry){
        ret =0;
    }else{
        // ret = offsetof(ext4_direntry, name) + entry->name_length;
        ret = sizeof(ext4_direntry);
        memcpy(buf, entry, ret);
    }
done:
    if (rcnt)
        *rcnt = ret;
    return 0;
}

static int ext4_select(void* inode, unsigned type)
{
    if (type == FS_SELECT_EXCEPT)
        return -1;
    
    return 0;
}

static int dir_stat(ext4_dir *dir, struct stat* s)
{
    return ext4_fstat(&dir->f, s);
}

static int dir_seek(ext4_dir *dir, uint64_t offset, uint32_t origin)
{
    ext4_direntry* entry = NULL;
    int len;
    int ret = 0;
    int cur_pos = 0;
    struct linux_dirent *dirp;
    int count = offset;
    int loops = 0;
    int i;
    if (origin != SEEK_SET)
        return -EACCES;

    if (offset < sizeof(struct linux_dirent))
    {
        return 0;
    }

    ext4_dir_entry_rewind(dir);
    while (count > 0)
    {
        entry = ext4_dir_entry_next(dir);
        if (entry == NULL){
            break;
        }
        len = ROUND_UP(NAME_OFFSET(dirp) + strlen(entry->name) + 1);
        if (count < len){
            return (cur_pos+len);
        }
        loops++;
        cur_pos += len;
        count -= len;
    }
    return cur_pos;
}

static fileop file_op = {
    .read = ext4_fread,
    .write = ext4_fwrite,
    .close = file_close,
    .seek = file_seek,
    .stat = ext4_fstat,
    .tell = ext4_ftell,
    .select = ext4_select,
};

static fileop dir_op = {
    .read = dir_read,
    .close = dir_close,
    .seek = dir_seek,
    .stat = dir_stat,
    .select = ext4_select,
};

filep fs_alloc_filep_normal(void* content)
{
    filep fp = calloc(1, sizeof(*fp));
    fp->file_type = FILE_TYPE_NORMAL;
    fp->inode = content;
    fp->ref_cnt = 0;
    fp->op = file_op;
    fp->istty = 0;
    return fp;
}



filep fs_alloc_filep_dir(void* content)
{
    filep fp = calloc(1, sizeof(*fp));
    fp->file_type = FILE_TYPE_DIR;
    fp->inode = content;
    fp->ref_cnt = 0;
    fp->op = dir_op;
    return fp;
}
