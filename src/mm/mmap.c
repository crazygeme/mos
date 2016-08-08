#include <mmap.h>
#include <rbtree.h>
#include <klib.h>
#include <config.h>

typedef struct _vm_key
{
    unsigned begin;
    unsigned end;
}vm_key;

// case 1
// ---| region1 | --------------
// ---------------| region2 | --
// case 2
// ---------------| region1 | --
// ---| region2 | --------------
// case 3
// -------| region1 | ----------
// -----------| region2 | ------
// case 4
// -----------| region1 | ------
// -------| region2 | ----------
// case 5
// -------|   region1   | ------
// ---------| region2 | --------
// case 6
// ---------| region1 | --------
// -------|   region2   | ------ 
static INLINE int vm_region_compair(void* region1, void* region2)
{
    vm_key* key1 = region1;
    vm_key* key2 = region2;

    if (key1->end <= key2->begin)
    {
        // case 1
        return -1;
    }
    else if (key1->begin >= key2->end)
    {
        // case 2
        return 1;
    }
    else
    {
        // conflict!
        // case 3, 4, 5, 6
        return 0;
    }
}

vm_struct_t vm_create()
{
    hash_table* table = hash_create(vm_region_compair);
    return table;
}

void vm_destroy(vm_struct_t vm)
{
    hash_table* table = vm;
    struct rb_node* node = rb_first(&table->root);

    while (node)
    {
        key_value_pair* pair = rb_entry(node, key_value_pair, node);
        vm_region* region = pair->val;
        vm_key* key = pair->key;
        unsigned vir = 0;

        hash_remove(table, key);

        kfree(key);

        // no need do this...
        /*
        // close all opened fd anyway because it's not useless after
        // thia call, process's virtual memory is destroy!!
        if (region->fd >= 0 && region->fd < MAX_FD) {
            fs_close(region->fd);
        }

        for (vir = region->begin; vir < region->end; vir += PAGE_SIZE) {
            mm_del_dynamic_map(vir);
        }

        */

        kfree(region);

        node = rb_first(&table->root);
    }

    hash_destroy(table);
}

//static void dump_mem(unsigned addr, unsigned len)
//{
//    unsigned char* tmp = (unsigned char*)addr;
//    unsigned size = 0;
//    klog_printf("%x: ", addr);
//    while (size < len) {
//        klog_printf("%b ", *tmp);
//        if ( (size+1) % 16 == 0) {
//            klog_printf("\n%x: ", addr + size+1);
//        }
//
//        if (size % 1000 == 0) {
//            printk("%d of %d finish\n", size, len);
//        }
//
//        tmp++;
//        size++;
//    }
//}

void vm_add_map(vm_struct_t vm, unsigned begin, unsigned end, void* fd, int offset)
{
    hash_table* table = vm;
    vm_key* key = kmalloc(sizeof(*key));
    key->begin = begin;
    key->end = end;
    key_value_pair* pair = hash_find(table, key);

    if (!pair)
    {
        vm_region* region = kmalloc(sizeof(*region));
        region->begin = begin;
        region->end = end;
        region->node = fd;
        if (fd)
        {
            vfs_refrence(fd);
        }
        region->offset = offset;
        hash_insert(table, key, region);
    }
    else
    {

        // cases 3, 4, 5, 6
        vm_key* conflict_key = pair->key;
        vm_region* conflict_region = pair->val;


        if (key->end < conflict_key->end && key->begin <= conflict_key->begin &&
            key->end > conflict_key->begin)
        {
            // case 3
            // -------| region1 | ----------
            // -----------| region2 | ------
            unsigned new_begin = key->end;
            unsigned new_offset = conflict_region->offset + (key->end - conflict_key->begin);
            unsigned new_end = conflict_key->end;
            void* new_fd = conflict_region->node;
            vm_del_map(vm, conflict_key->begin);
            vm_add_map(vm, begin, end, fd, offset);
            vm_add_map(vm, new_begin, new_end, new_fd, new_offset);
        }
        else if (key->begin > conflict_key->begin && key->begin < conflict_key->end &&
            key->end >= conflict_key->end)
        {
            // case 4
            // -----------| region1 | ------
            // -------| region2 | ----------
            unsigned new_begin = conflict_key->begin;
            unsigned new_offset = conflict_region->offset;
            unsigned new_end = key->begin;
            unsigned new_fd = conflict_region->node;
            vm_del_map(vm, conflict_key->begin);
            vm_add_map(vm, begin, end, fd, offset);
            vm_add_map(vm, new_begin, new_end, new_fd, new_offset);
        }
        else if (key->begin <= conflict_key->begin && key->end >= conflict_key->end)
        {
            // case 5
            // -------|   region1   | ------
            // ---------| region2 | -------- 
            vm_del_map(vm, conflict_key->begin);
            vm_add_map(vm, begin, end, fd, offset);
        }
        else if (key->begin > conflict_key->begin && key->end < conflict_key->end)
        {
            // case 6
            // ---------| region1 | --------
            // -------|   region2   | ------ 
            unsigned new_begin1 = conflict_key->begin;
            unsigned new_offset1 = conflict_region->offset;
            unsigned new_end1 = key->begin;
            unsigned new_fd1 = conflict_region->node;
            unsigned new_begin2 = key->end;
            unsigned new_offset2 = conflict_region->offset + (key->end - conflict_key->begin);
            unsigned new_end2 = conflict_key->end;
            unsigned new_fd2 = conflict_region->node;
            vm_del_map(vm, conflict_key->begin);
            vm_add_map(vm, begin, end, fd, offset);
            vm_add_map(vm, new_begin1, new_end1, new_fd1, new_offset1);
            vm_add_map(vm, new_begin2, new_end2, new_fd2, new_offset2);
        }

        kfree(key);
    }


}


static INLINE key_value_pair* vm_find_pair(hash_table* table, unsigned addr)
{
    vm_key* key = kmalloc(sizeof(*key));
    key_value_pair * pair = 0;
    key->begin = (addr & PAGE_SIZE_MASK);
    key->end = key->begin + PAGE_SIZE;

    pair = hash_find(table, key);

    kfree(key);

    return pair;
}

void vm_del_map(vm_struct_t vm, unsigned addr)
{
    hash_table* table = vm;
    vm_region* region = 0;
    key_value_pair* pair = 0;
    unsigned vir = 0;
    vm_key* key = 0;

    // addr has to be 4K aligned, orelse I will do it for you
    addr = (addr & PAGE_SIZE_MASK);

    pair = vm_find_pair(table, addr);
    if (!pair)
    {
        return;
    }

    key = pair->key;
    region = pair->val;
    for (vir = region->begin; vir < region->end; vir += PAGE_SIZE)
    {
        mm_del_dynamic_map(vir);
    }

    if (region->node)
    {
        vfs_free_inode(region->node);
    }
    kfree(region);

    hash_remove(table, key);

    kfree(key);


}

vm_region* vm_find_map(vm_struct_t vm, unsigned addr)
{
    key_value_pair* pair = 0;
    hash_table* table = vm;

    // addr has to be 4K aligned, orelse I will do it for you
    addr = (addr & PAGE_SIZE_MASK);

    pair = vm_find_pair(table, addr);
    if (!pair)
    {
        return 0;
    }
    else
    {
        return pair->val;
    }
}

unsigned vm_disc_map(vm_struct_t vm, int size)
{
    // size has to be 4K or n*4K, I will algin it for you anyway...
    hash_table* table = vm;
    key_value_pair* pair = hash_first(table);
    unsigned begin = USER_ZONE_BEGIN;
    unsigned end;

    if (!pair)
    {
        return begin;
    }

    while (pair)
    {
        vm_key* key = pair->key;
        key_value_pair* next = hash_next(table, pair);
        vm_key* next_key;

        if (!next)
        {
            if ((key->end + size) < KERNEL_OFFSET)
            {
                return key->end;
            }
            else
            {
                return 0;
            }
        }

        next_key = next->key;
        begin = key->end;
        end = begin + size;
        if (end <= next_key->begin)
        {
            return begin;
        }

        pair = next;
    }

    // never to here
    return 0;
}

void vm_dup(vm_struct_t cur, vm_struct_t new)
{
    hash_table* table = cur;
    key_value_pair* pair = hash_first(table);
    vm_region* region;

    while (pair)
    {
        region = pair->val;
        vm_add_map(new, region->begin, region->end, region->node, region->offset);

        pair = hash_next(table, pair);
    }
}


#ifdef TEST_MMAP

static void vm_print(vm_struct_t vm)
{
    hash_table* table = vm;
    key_value_pair* pair = hash_first(table);
    vm_region* region;

    while (pair)
    {
        region = pair->val;
        printf("%x - %x, off %x\t, fd %d\n", region->begin, region->end, region->offset, region->fd);

        pair = hash_next(table, pair);
    }
}

static void test_case_1()
{
    vm_struct_t vm = 0;

    printf("test insert case 1\n---------------\n");
    klogquota();


    vm = vm_create();

    vm_add_map(vm, USER_ZONE_BEGIN + 3 * PAGE_SIZE, USER_ZONE_BEGIN + 5 * PAGE_SIZE, 3, 0);
    vm_add_map(vm, USER_ZONE_BEGIN, USER_ZONE_BEGIN + 3 * PAGE_SIZE, 4, 0);

    vm_print(vm);

    vm_destroy(vm);

    klogquota();
}

static void test_case_2()
{
    vm_struct_t vm = 0;

    printf("test insert case 2\n---------------\n");
    klogquota();


    vm = vm_create();

    vm_add_map(vm, USER_ZONE_BEGIN, USER_ZONE_BEGIN + 3 * PAGE_SIZE, 3, 0);
    vm_add_map(vm, USER_ZONE_BEGIN + 3 * PAGE_SIZE, USER_ZONE_BEGIN + 5 * PAGE_SIZE, 4, 0);

    vm_print(vm);

    vm_destroy(vm);

    klogquota();
}

static void test_case_3()
{
    vm_struct_t vm = 0;

    printf("test insert case 3\n---------------\n");
    klogquota();


    vm = vm_create();

    vm_add_map(vm, USER_ZONE_BEGIN + 3 * PAGE_SIZE, USER_ZONE_BEGIN + 6 * PAGE_SIZE, 3, 0);
    vm_add_map(vm, USER_ZONE_BEGIN + 0 * PAGE_SIZE, USER_ZONE_BEGIN + 5 * PAGE_SIZE, 4, 0);

    vm_print(vm);

    vm_destroy(vm);

    klogquota();
}

static void test_case_4()
{
    vm_struct_t vm = 0;

    printf("test insert case 4\n---------------\n");
    klogquota();


    vm = vm_create();

    vm_add_map(vm, USER_ZONE_BEGIN + 0 * PAGE_SIZE, USER_ZONE_BEGIN + 5 * PAGE_SIZE, 3, 0);
    vm_add_map(vm, USER_ZONE_BEGIN + 3 * PAGE_SIZE, USER_ZONE_BEGIN + 6 * PAGE_SIZE, 4, 0);

    vm_print(vm);

    vm_destroy(vm);

    klogquota();
}

static void test_case_5()
{
    vm_struct_t vm = 0;

    printf("test insert case 5\n---------------\n");
    klogquota();


    vm = vm_create();

    vm_add_map(vm, USER_ZONE_BEGIN + 2 * PAGE_SIZE, USER_ZONE_BEGIN + 5 * PAGE_SIZE, 3, 0);
    vm_add_map(vm, USER_ZONE_BEGIN + 0 * PAGE_SIZE, USER_ZONE_BEGIN + 6 * PAGE_SIZE, 4, 0);

    vm_print(vm);

    vm_destroy(vm);

    klogquota();
}


static void test_case_6()
{
    vm_struct_t vm = 0;

    printf("test insert case 6\n---------------\n");

    klogquota();


    vm = vm_create();

    vm_add_map(vm, USER_ZONE_BEGIN, USER_ZONE_BEGIN + 3 * PAGE_SIZE, 3, 0);
    vm_add_map(vm, USER_ZONE_BEGIN + PAGE_SIZE, USER_ZONE_BEGIN + 2 * PAGE_SIZE, 4, 0);

    vm_print(vm);

    vm_destroy(vm);

    klogquota();
}


static void test_find_1()
{
    vm_struct_t vm = 0;
    unsigned addr = 0;

    printf("test find case 1\n---------------\n");
    klogquota();
    vm = vm_create();
    vm_add_map(vm, USER_ZONE_BEGIN, USER_ZONE_BEGIN + 3 * PAGE_SIZE, 3, 0);

    addr = vm_disc_map(vm, 2 * PAGE_SIZE);
    printf("found addr %x\n", addr);
    vm_destroy(vm);
    klogquota();
}

static void test_find_2()
{
    vm_struct_t vm = 0;
    unsigned addr = 0;

    printf("test find case 2\n---------------\n");
    klogquota();
    vm = vm_create();
    vm_add_map(vm, USER_ZONE_BEGIN, USER_ZONE_BEGIN + 3 * PAGE_SIZE, 3, 0);
    vm_add_map(vm, USER_ZONE_BEGIN + 4 * PAGE_SIZE, USER_ZONE_BEGIN + 6 * PAGE_SIZE, 3, 0);

    addr = vm_disc_map(vm, 2 * PAGE_SIZE);
    printf("found addr %x\n", addr);
    vm_destroy(vm);
    klogquota();
}


void vm_test()
{
    test_case_1();
    test_case_2();
    test_case_3();
    test_case_4();
    test_case_5();
    test_case_6();

    test_find_1();
    test_find_2();
}
#endif
