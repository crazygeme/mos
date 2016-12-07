#include <block.h>
#include <lock.h>
#include <klib.h>

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
