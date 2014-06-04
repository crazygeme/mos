#include <mm/mm.h>
#include <lib/klib.h>
#include <boot/multiboot.h>
#include <lib/list.h>
#include <ps/lock.h>
#include <ps/ps.h>

#define GDT_ADDRESS			0x1C0000
#define PG0_ADDRESS			0x1C1000
#define PG1_ADDRESS			0x1C2000
#define PG_CACHE_ADDRESS	0x1C3000

_START static void mm_dump_phy(multiboot_info_t* mb);
_START static void mm_get_phy_mem_bound(multiboot_info_t* mb);
_START static void mm_setup_beginning_8m();

static unsigned long long phy_mem_low;
static unsigned long long phy_mem_high;

_STARTDATA static unsigned long long _phy_mem_low;
_STARTDATA static unsigned long long _phy_mem_high;

// this can present 256*1024*4k = 1G
#define PAGE_MASK_TABLE_SIZE (256*1024)
static unsigned char free_phy_page_mask[PAGE_MASK_TABLE_SIZE];

_START void mm_init(multiboot_info_t* mb)
{
    // mm_dump_phy(mb);
    mm_get_phy_mem_bound(mb);
    mm_setup_beginning_8m();
}



_START void mm_dump_phy(multiboot_info_t* mb)
{
    unsigned int count = mb->mmap_length;
    unsigned int map_addr = mb->mmap_addr;
    unsigned int i = 0;
    memory_map_t *map;
    klib_info("flags: ", mb->flags, "\n");

    klib_info("mem_lower: ", mb->mem_lower, "\t");

    klib_info("mem_upper: ", mb->mem_upper, "\t");

    klib_info("boot_device: ", mb->boot_device, "\n");

    klib_info("cmdline: ", mb->cmdline, "\t");

    klib_info("mods_count: ", mb->mods_count, "\t");

    klib_info("mods_addr: ", mb->mods_addr, "\n");

    klib_info("mmap_length: ", mb->mmap_length, "\t");

    klib_info("mmap_addr: ", mb->mmap_addr, "\n");

    if (mb->flags & 0x40) {
        
    map = mb->mmap_addr; 
        while((unsigned int)map < mb->mmap_addr + mb->mmap_length) {

            klib_info("base_addr: ", map->base_addr_low, ":");
            klib_info("", map->base_addr_high, "\t");
            klib_info("length: ", map->length_low, ":");
            klib_info("", map->length_high, "\t");
            klib_info("type: ", map->type, "\n");

    		map = (memory_map_t*) ( (unsigned int)map + map->size + sizeof(unsigned int) );
    	}
    }

    /* 
    dump info when set -m 256 
    flags: 0x4F                                                                     
    mem_lower: 0x27F    mem_upper: 0x3FBF8  boot_device: 0x8000FFFF                 
    cmdline: 0x109000   mods_count: 0x0 mods_addr: 0x109000                         
    mmap_length: 0x90   mmap_addr: 0x9000                                           
    base_addr: 0x0:0x0          length: 0x9FC00:0x0     type: 0x1  ; first 640K reserved for booting                            
    base_addr: 0x9FC00:0x0      length: 0x400:0x0       type: 0x2                           
    base_addr: 0xF0000:0x0      length: 0x10000:0x0     type: 0x2                           
    base_addr: 0x100000:0x0     length: 0xFEFE000:0x0   type: 0x1  ; usable memory                     
    base_addr: 0xFFFE000:0x0    length: 0x2000:0x0      type: 0x2                       
    base_addr: 0xFFFC0000:0x0   length: 0x40000:0x0     type: 0x2 
    */
}

_START static void mm_get_phy_mem_bound(multiboot_info_t* mb)
{  
    unsigned int map_addr = mb->mmap_addr;
    memory_map_t *map;

    if (mb->flags & 0x40) {
        
        map = (memory_map_t*)mb->mmap_addr; 
        while((unsigned int)map < mb->mmap_addr + mb->mmap_length) {

            if (map->type == 0x1 && map->base_addr_low != 0) {
                _phy_mem_low = map->base_addr_low + map->base_addr_high << 32;
                _phy_mem_high = phy_mem_low + (map->length_low + map->length_high << 32);
                break;
            }
    		map = (memory_map_t*) ( (unsigned int)map + map->size + sizeof(unsigned int) );
    	}
    }

     // klib_info("static address: ", &phy_mem_low, "\n");
     // klib_info("local address: ", &map_addr, "\n");
     // klib_info("code address: ", &mm_get_phy_mem_bound, "\n");
}



#define ENABLE_PAGING() \
    __asm__ ("movl %cr0,%eax"); \
    __asm__ ("orl $0x80000000,%eax"); \
    __asm__ ("movl %eax,%cr0");

#define RELOAD_EIP() \
	__asm__ ("jmp 1f \n1:\n\tmovl $1f,%eax\n\tjmp *%eax \n1:\n\tnop");

#define RELOAD_ESP(OFFSET) \
	__asm__ ("movl %esp, %ecx");\
	__asm__ ("addl %0, %%ecx" : : "i" (OFFSET));\
	__asm__ ("movl %ecx, %esp");
	

_START static void simulate_paging(unsigned address)
{
    int *gdt = (int*)GDT_ADDRESS;
    int *pet = 0;
    int gd_index = 0;
    int pe_index = 0;
    int page_offset = 0;
    unsigned phy = 0;


    klib_info("virtual address: ", address, "\n");

    gd_index = ADDR_TO_PGT_OFFSET(address) ;
    pe_index = ADDR_TO_PET_OFFSET(address) ;
    page_offset = ADDR_TO_PAGE_OFFSET(address);

    klib_info("gd_index: ", gd_index, "\n");
    klib_info("pe_index: ", pe_index, "\n");
    klib_info("page_offset: ", page_offset, "\n");

    pet = (int*)(gdt[0] & 0xfffff000);
    klib_info("pet: ", pet, "\n");

    phy = pet[pe_index] & 0xfffff000;
    klib_info("phy: ", phy, "\n");

    phy = phy | page_offset;
    klib_info("physical address: ", phy, "\n");
}

void mm_set_phy_page_mask(unsigned int page_index, unsigned int used)
{
	int mask_index = page_index / 8;
	int mask_offset = page_index % 8;
	int mask = 1 << mask_offset;
	if (used){	
		free_phy_page_mask[mask_index] |= mask;
	}else{
		free_phy_page_mask[mask_index] &= ~mask;
	}
}

static unsigned int mm_get_phy_page_mask(unsigned int page_index)
{
	int mask_index = page_index / 8;
	int mask_offset = page_index % 8;
	int mask = 1 << mask_offset;

	return ((free_phy_page_mask[mask_index] & mask));
}

static void mm_init_free_phy_page_mask()
{
	int i = 0;
	unsigned int phy_page_low = phy_mem_low / PAGE_SIZE;
	unsigned int phy_page_high = phy_mem_high / PAGE_SIZE;
	for (i = 0; i < PAGE_MASK_TABLE_SIZE; i++)
	  free_phy_page_mask[i] = 0;

	// lower then phy_mem_low set to used
	for (i = 0; i < phy_page_low; i++)
	  mm_set_phy_page_mask(i, 1);

	// higher then phy_mem_high set to used
	for (i = phy_page_high; i < (1024*1024); i++)
	  mm_set_phy_page_mask(i, 1);
	
	// first 12M are used
	for (i = phy_page_low; i < (3*1024); i++)
	  mm_set_phy_page_mask(i, 1);

}

unsigned int  mm_get_free_phy_page_index()
{
	unsigned int i = 0;
	unsigned int page_mask_index = 0;
	int mask = 0;
	int offset = 0;

	for (i = 0; i < PAGE_MASK_TABLE_SIZE; i++){
		if (free_phy_page_mask[i] != 0xff){
			page_mask_index = i;
			break;
		}
	}

	if (i == PAGE_MASK_TABLE_SIZE)
	  return -1;

	mask = free_phy_page_mask[page_mask_index];
	mask = ~mask;
	while((mask%2) == 0){
		offset++;
		mask = mask / 2;
	}

	return (page_mask_index * 8 + offset);
}

#ifdef TEST_MM
void mm_test()
{
	unsigned int phy_page_low = phy_mem_low / PAGE_SIZE;
	unsigned int phy_page_high = phy_mem_high / PAGE_SIZE;
	printk("mask of 8M -1: %x\n", mm_get_phy_page_mask(2*1024 - 1));
	printk("mask of 8M: %x \n", mm_get_phy_page_mask(2*1024));
	printk("mask of high bound -1: %x, high bound is %x \n", 
				mm_get_phy_page_mask(phy_page_high-1), 
				phy_page_high);
	printk("mask of high bound: %x, high bound is %x \n", 
				mm_get_phy_page_mask(phy_page_high), 
				phy_page_high);

	printk("mask of high bound +1: %x, high bound is %x \n", 
				mm_get_phy_page_mask(phy_page_high+1), 
				phy_page_high);

	printk("first free page index is %x\n", mm_get_free_phy_page_index());

	if (0)
	{
		int i = 0;
		unsigned int *table1, *table2;	
		for (i = 0; i < 5; i++){
			table1 = mm_alloc_page_table();
			table2 = mm_alloc_page_table();
			printk("alloc page1 %x, page 2 %x\n", table1, table2);
			printk("table1[0] %x, table1[1] %x\n", table1[0], table1[1]);
			printk("table2[0] %x, table2[1] %x\n", table2[0], table2[1]);
			
			mm_free_page_table(table2);
			mm_free_page_table(table1);
		}
	}
	if (1)
	{
		unsigned int vm = vm_alloc(4);
		unsigned int vm2 = vm_alloc(6);
		printk("vm alloc %x, vm2 %x\n", vm, vm2);
		vm_free(vm, 4);
		vm_free(vm2, 6);
	}
}
#endif

static STACK page_table_cache;
// take 8M ~ 12M as page table cache
static void mm_init_page_table_cache()
{
	unsigned int cache_addr = 0xC0800000;
	unsigned int i = 0;
	InitializeStack(&page_table_cache);
	for(i = 0; i < 1024; i++){
		PushStack(&page_table_cache, cache_addr);
		cache_addr += PAGE_SIZE;
	}
}


static spinlock mm_lock;
static void lock_mm()
{
	spinlock_lock(&mm_lock);
}

static void unlock_mm()
{
	spinlock_unlock(&mm_lock);
}

static void mm_high_memory_fun()
{
	// ok now we make eip as virtual address
	RELOAD_EIP();
	phy_mem_high = _phy_mem_high;
	phy_mem_low = _phy_mem_low;
	// ok now we make esp as virtual address
	RELOAD_ESP(KERNEL_OFFSET);

	 mm_init_free_phy_page_mask();
	mm_init_page_table_cache();

	spinlock_init(&mm_lock);
	extern kmain_startup();
	kmain_startup();
}

_START static void mm_setup_beginning_8m()
{
    unsigned int phy = PAGE_ENTRY_USER_DATA;
    int i = 0;
    unsigned int * gdt = (int*)GDT_ADDRESS;
    unsigned int * pg0 = (int*)PG0_ADDRESS;
    unsigned int * pg1 = (int*)PG1_ADDRESS;
	unsigned int * pg2 = (int*)PG_CACHE_ADDRESS;
    for (i = 0; i < PG_TABLE_SIZE; i++) {
        gdt[i] = 0;
    }
    // following two are for user space
    gdt[0] = PG0_ADDRESS | PAGE_ENTRY_USER_DATA; 
    gdt[1] = PG1_ADDRESS | PAGE_ENTRY_USER_DATA;

    // following two are for kernel space
    gdt[768] = PG0_ADDRESS | PAGE_ENTRY_USER_DATA;
    gdt[769] = PG1_ADDRESS | PAGE_ENTRY_USER_DATA;
	gdt[770] = PG_CACHE_ADDRESS | PAGE_ENTRY_USER_DATA;

    for (i = 0; i < PE_TABLE_SIZE; i++) {
        pg0[i] = phy;
        phy += PAGE_SIZE;
    }

    for (i = 0; i < PE_TABLE_SIZE; i++) {
        pg1[i] = phy;
        phy += PAGE_SIZE;
    }

	for (i = 0; i < PE_TABLE_SIZE; i++){
		pg2[i] = phy;
		phy += PAGE_SIZE;
	}

    RELOAD_CR3(GDT_ADDRESS);
    ENABLE_PAGING();
	mm_high_memory_fun();
	return;
    // simulate_paging(0x001012a0);
}



int mm_add_direct_map(unsigned int vir)
{
	unsigned int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
	unsigned int page_table_offset = ADDR_TO_PET_OFFSET(vir);
	unsigned int *page_dir = (unsigned int*)(GDT_ADDRESS+KERNEL_OFFSET);
	unsigned int page_busy = 0;
	unsigned int page_index = 0;
	// only kernel space has direct map
	if (vir < KERNEL_OFFSET)
	  return -1;

	if (vir >= 0xC0000000 && vir < 0xC0C00000)
	  return 1;

	page_index = (vir - KERNEL_OFFSET) & PAGE_SIZE_MASK;
	page_index = page_index / PAGE_SIZE;
	page_busy = mm_get_phy_page_mask(page_index);
	if (page_busy){
		return -1;
	}

	if ( (page_index * PAGE_SIZE) > KERNEL_SIZE)
	  return -1;

	if ((page_dir[page_dir_offset]&PAGE_SIZE_MASK) == 0){
		unsigned int table_addr = mm_alloc_page_table();
		if (table_addr == 0){
			return -1;
		}

		page_dir[page_dir_offset] = (table_addr - KERNEL_OFFSET) | PAGE_ENTRY_KERNEL_DATA;
	}
	
	{
		unsigned int *table = (unsigned int*)((page_dir[page_dir_offset]&PAGE_SIZE_MASK)+KERNEL_OFFSET);
		unsigned int phy_page = (vir - KERNEL_OFFSET) & PAGE_SIZE_MASK;
		table[page_table_offset] = phy_page | PAGE_ENTRY_KERNEL_DATA;
		mm_set_phy_page_mask(  phy_page / PAGE_SIZE, 1);
		if (vir == ((page_dir[page_dir_offset]&PAGE_SIZE_MASK) + KERNEL_OFFSET))
		  return 1;
		else
		  return 0;
	}
}

void mm_del_direct_map(unsigned int vir)
{
	int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
	int page_table_offset = ADDR_TO_PET_OFFSET(vir);
	unsigned int *page_dir = (unsigned int*)(GDT_ADDRESS+KERNEL_OFFSET);
	unsigned int *page_table;
	unsigned int phy_addr = (vir >= KERNEL_OFFSET) ? 
		( (vir&PAGE_SIZE_MASK)-KERNEL_OFFSET) : (vir&PAGE_SIZE_MASK);
	int empty = 1;
	int i = 0;

	// 0xC0000000 ~ 0xC0C00000 is always mapped
	if (vir >= 0xC0000000 && vir < 0xC0C00000)
	  return;


	page_table = (unsigned int*)((page_dir[page_dir_offset]&PAGE_SIZE_MASK) + KERNEL_OFFSET);

	page_table[page_table_offset] = 0;
	mm_set_phy_page_mask( phy_addr / PAGE_SIZE, 0);

	for (i = 0; i < 1024; i++){
		if (page_table[i] !=0){
			empty = 0;
			break;
		}
	}

	if (empty){
		mm_free_page_table( (unsigned int)page_table + KERNEL_OFFSET);
		page_dir[page_dir_offset] = 0;
	}
}


void mm_del_user_map()
{
	unsigned int *page_dir = (unsigned int*)(GDT_ADDRESS+KERNEL_OFFSET); 
	page_dir[0] = 0;
	page_dir[1] = 0;
}

unsigned int mm_alloc_page_table()
{
	unsigned int *ret = PopStack(&page_table_cache);
	
	if (ret == 0)
	  return ret;
	//printk("alloc page table %x\n", ret);
	memset(ret, 0, PAGE_SIZE);
	return ret;
}

void mm_free_page_table(unsigned int vir)
{	
	int ret = PushStack(&page_table_cache, vir);

	// printk("free page table %x\n", vir);

	if (!ret){
		printk("fatal error: stack overflow\n");
	}
}

unsigned int vm_alloc(int page_count)
{
	int page_index = 0;
	int i = 0;
	int valid = 0;
	unsigned int vir = 0;

	lock_mm();

	page_index = mm_get_free_phy_page_index();
	
	do{
		if ((page_index + page_count) >= 0x40000){
			unlock_mm();
			return 0;
		}

		valid = 1;
		for (i = 0; i < page_count; i++){
			if ( mm_get_phy_page_mask( page_index+i) != 0 ){
				valid = 0;
				break;
			}
		}
		if (!valid)
		  page_index++;
	}while(!valid);

	for (i = 0; i < page_count; i++){
		vir = (page_index+i) * PAGE_SIZE + KERNEL_OFFSET; 
		mm_add_direct_map(vir);
        memset(vir, 0, PAGE_SIZE);
	}

	vir = page_index * PAGE_SIZE + KERNEL_OFFSET;
	unlock_mm();
	return vir;

}

void vm_free(unsigned int vm, int page_count)
{
	unsigned int phy = 0;
	int i = 0;
	unsigned int cur;
	lock_mm();
	vm = vm & PAGE_SIZE_MASK;
	for (i = 0; i < page_count; i++){
		cur = vm + i * PAGE_SIZE;
		phy = cur - KERNEL_OFFSET;
		mm_del_direct_map(cur);
	}
	unlock_mm();

}

void mm_add_dynamic_map(unsigned int vir, unsigned int phy, unsigned flag)
{
	unsigned int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
	unsigned int page_table_offset = ADDR_TO_PET_OFFSET(vir);
	unsigned int *page_dir = (unsigned int*)(GDT_ADDRESS+KERNEL_OFFSET);
	unsigned int page_busy = 0;
	unsigned int page_index = 0;
	unsigned int target_phy = 0;

	phy = phy & PAGE_SIZE_MASK;
	page_index = mm_get_free_phy_page_index();// vm_alloc(1) - KERNEL_OFFSET;
	target_phy = page_index*PAGE_SIZE;



	page_index = (target_phy) & PAGE_SIZE_MASK;
	page_index = page_index / PAGE_SIZE;


	if ((page_dir[page_dir_offset]&PAGE_SIZE_MASK) == 0){
		unsigned int table_addr = mm_alloc_page_table();
		if (table_addr == 0){
			return ;
		}

		page_dir[page_dir_offset] = (table_addr - KERNEL_OFFSET) | flag;
	}
	
	{
		unsigned int *table = (unsigned int*)((page_dir[page_dir_offset]&PAGE_SIZE_MASK)+KERNEL_OFFSET);
		unsigned int phy_page = (target_phy) & PAGE_SIZE_MASK;
        unsigned int table_entry = table[page_table_offset] & PAGE_SIZE_MASK;
        if (table_entry) 
            return;

        table[page_table_offset] = phy_page | flag; 
		mm_set_phy_page_mask(  phy_page / PAGE_SIZE, 1);

		if (phy) {
			char* src = (char*)phy + KERNEL_OFFSET;
			char* dst = (char*)vir;
			memcpy(dst,src,PAGE_SIZE);
		}
	}

    if (vir < KERNEL_OFFSET) {
        ps_record_dynamic_map(vir);
        
    }


}

void mm_del_dynamic_map(unsigned int vir)
{
	int page_dir_offset = ADDR_TO_PGT_OFFSET(vir);
	int page_table_offset = ADDR_TO_PET_OFFSET(vir);
	unsigned int *page_dir = (unsigned int*)(GDT_ADDRESS+KERNEL_OFFSET);
	unsigned int *page_table;
	unsigned int phy_addr;
	int empty = 1;
	int i = 0;


	page_table = (unsigned int*)((page_dir[page_dir_offset]&PAGE_SIZE_MASK) + KERNEL_OFFSET);
	phy_addr = page_table[page_table_offset] & PAGE_SIZE_MASK;

	page_table[page_table_offset] = 0;
	mm_set_phy_page_mask( phy_addr / PAGE_SIZE, 0);

	for (i = 0; i < 1024; i++){
		if (page_table[i] !=0){
			empty = 0;
			break;
		}
	}

	if (empty){
		mm_free_page_table( (unsigned int)page_table);
		page_dir[page_dir_offset] = 0;
	}


}

unsigned vm_get_usr_zone(unsigned page_count)
{
    task_struct* cur = CURRENT_TASK();
    unsigned ret = cur->user.zone_top;

    cur->user.zone_top += page_count * PAGE_SIZE;

    return ret;
}


