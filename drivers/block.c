#include <drivers/block.h>
#include <ps/lock.h>
#include <lib/klib.h>

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



#ifdef TEST_BLOCK
static void block_print_all(block_type type)
{
    unsigned count = 0;
    block** blocks = 0;
    unsigned i = 0;

    count = block_get_by_type(type, 0);
    if (!count)
    {
        return;
    }

    blocks = kmalloc(count * sizeof(block*));
    block_get_by_type(type, blocks);

    for (i = 0; i < count; i++)
    {
        printk("block type %s\n", block_type_name(blocks[i]));
        printk("\t\tname: %s\n", blocks[i]->name);
        printk("\t\tid: %d\n", blocks[i]->id);
        printk("\t\tsector_size: %d\n", blocks[i]->sector_size);
        printk("\t\tread:  [%x]\n", blocks[i]->read);
        printk("\t\twrite: [%x]\n", blocks[i]->write);
    }

    kfree(blocks);
}

void test_block_process()
{
    unsigned i = 0;

    for (i = 0; i < BLOCK_MAX; i++)
    {
        block_print_all(i);
    }
}

#endif
