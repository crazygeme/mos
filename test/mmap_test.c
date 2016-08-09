#include <mmap.h>
#include <rbtree.h>
#include <klib.h>
#include <config.h>

static void vm_print(vm_struct_t vm)
{
    hash_table* table = vm;
    key_value_pair* pair = hash_first(table);
    vm_region* region;

    while (pair)
    {
        region = pair->val;
        printf("%x - %x, off %x\t, node %x\n", region->begin, region->end, region->offset, region->node);

        pair = hash_next(table, pair);
    }
}

static void test_case_1()
{
    vm_struct_t vm = 0;

    printf("test insert case 1\n---------------\n");
    klogquota();

    vm = vm_create();
    vm_add_map(vm, USER_ZONE_BEGIN + 3 * PAGE_SIZE, USER_ZONE_BEGIN + 5 * PAGE_SIZE, 0, 0);
    vm_add_map(vm, USER_ZONE_BEGIN, USER_ZONE_BEGIN + 3 * PAGE_SIZE, 0, 0);
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

    vm_add_map(vm, USER_ZONE_BEGIN, USER_ZONE_BEGIN + 3 * PAGE_SIZE, 0, 0);
    vm_add_map(vm, USER_ZONE_BEGIN + 3 * PAGE_SIZE, USER_ZONE_BEGIN + 5 * PAGE_SIZE, 0, 0);

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

    vm_add_map(vm, USER_ZONE_BEGIN + 3 * PAGE_SIZE, USER_ZONE_BEGIN + 6 * PAGE_SIZE, 0, 0);
    vm_add_map(vm, USER_ZONE_BEGIN + 0 * PAGE_SIZE, USER_ZONE_BEGIN + 5 * PAGE_SIZE, 0, 0);

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

    vm_add_map(vm, USER_ZONE_BEGIN + 0 * PAGE_SIZE, USER_ZONE_BEGIN + 5 * PAGE_SIZE, 0, 0);
    vm_add_map(vm, USER_ZONE_BEGIN + 3 * PAGE_SIZE, USER_ZONE_BEGIN + 6 * PAGE_SIZE, 0, 0);

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

    vm_add_map(vm, USER_ZONE_BEGIN + 2 * PAGE_SIZE, USER_ZONE_BEGIN + 5 * PAGE_SIZE, 0, 0);
    vm_add_map(vm, USER_ZONE_BEGIN + 0 * PAGE_SIZE, USER_ZONE_BEGIN + 6 * PAGE_SIZE, 0, 0);

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

    vm_add_map(vm, USER_ZONE_BEGIN, USER_ZONE_BEGIN + 3 * PAGE_SIZE, 0, 0);
    vm_add_map(vm, USER_ZONE_BEGIN + PAGE_SIZE, USER_ZONE_BEGIN + 2 * PAGE_SIZE, 0, 0);

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
    vm_add_map(vm, USER_ZONE_BEGIN, USER_ZONE_BEGIN + 3 * PAGE_SIZE, 0, 0);

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
    vm_add_map(vm, USER_ZONE_BEGIN, USER_ZONE_BEGIN + 3 * PAGE_SIZE, 0, 0);
    vm_add_map(vm, USER_ZONE_BEGIN + 4 * PAGE_SIZE, USER_ZONE_BEGIN + 6 * PAGE_SIZE, 0, 0);

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
