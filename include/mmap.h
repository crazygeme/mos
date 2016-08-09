#ifndef _MMAP_H_
#define _MMAP_H_
typedef void* vm_struct_t;

typedef struct _vm_region
{
    unsigned begin;
    unsigned end;
    void* node;
    int offset;
}vm_region;

vm_struct_t vm_create();

void vm_destroy(vm_struct_t vm);

/**
 * map begin <= addr < end to virtual, with related fd fd can be
 * -1, which means it's not related with file when map to an 
 * already maped reagion, it will split regions 
 *  
 * @author zhengender (8/3/2014)
 * 
 * @param begin 
 * @param end 
 * @param fd 
 */
void vm_add_map(vm_struct_t vm, unsigned begin, unsigned end, void* node, int offset);

/**
 * delete a mapped region which contains addr
 * 
 * @author zhengender (8/3/2014)
 * 
 * @param addr 
 */
void vm_del_map(vm_struct_t vm, unsigned addr);

/**
 * find the region that contains addr
 * 
 * @author zhengender (8/3/2014)
 * 
 * @param addr 
 * 
 * @return vm_region* 
 */
vm_region* vm_find_map(vm_struct_t vm, unsigned addr);


/**
 * find a region large enough to contain @size 
 * return the begin address 
 * 
 * @author zhengender (8/3/2014)
 * 
 * @param size 
 * 
 * @return unsigned 
 */
unsigned vm_disc_map(vm_struct_t vm, int size);


/**
 * dup vm maps from cur into new
 * 
 * @author zhengender (9/7/2014)
 * 
 * @param cur 
 */
void vm_dup(vm_struct_t cur, vm_struct_t new);


#endif
