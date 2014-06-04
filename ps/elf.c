#include <ps/elf.h>
#include <fs/namespace.h>
#if defined WIN32 || defined MACOS
#include <osdep.h>
#else
#include <mm/mm.h>
#include <ps/ps.h>
#include <lib/klib.h>
#endif

static unsigned elf_map_section(unsigned fd, Elf32_Phdr* phdr, mos_binfmt* fmt)
{
	int writable = (phdr->p_flags & PF_W) != 0;
	unsigned file_page = phdr->p_offset & PAGE_SIZE_MASK;
	unsigned mem_page = phdr->p_vaddr & PAGE_SIZE_MASK;
	unsigned page_offset = phdr->p_vaddr & ~PAGE_SIZE_MASK;
	unsigned read_bytes, zero_bytes;
    unsigned copy_page;
    unsigned total_pages = ROUND_UP(page_offset + phdr->p_memsz, PAGE_SIZE);

    if (fmt) {
        fmt->e_phoff = page_offset;
        fmt->elf_load_addr = mem_page;
    }

	if (phdr->p_filesz > 0)
	{
		/* Normal segment.
		Read initial part from disk and zero the rest. */
		read_bytes = page_offset + phdr->p_filesz;
		zero_bytes = (ROUND_UP(page_offset + phdr->p_memsz, PAGE_SIZE)
			- read_bytes);
	}
	else
	{
		/* Entirely zero.
				   Don't read anything from disk. */
		read_bytes = 0;
		zero_bytes = ROUND_UP(page_offset + phdr->p_memsz, PAGE_SIZE);
	}

    copy_page = mem_page;
    while (total_pages) {
        mm_add_dynamic_map(copy_page, 0, PAGE_ENTRY_USER_DATA); 
        if (read_bytes >= PAGE_SIZE) {
            fs_read(fd, file_page+(copy_page-mem_page), copy_page, PAGE_SIZE);
            read_bytes -= PAGE_SIZE;
        }else if(read_bytes){
            memset(copy_page+read_bytes, 0, PAGE_SIZE-read_bytes);
            fs_read(fd, file_page+(copy_page-mem_page), copy_page, read_bytes);
            read_bytes = 0;
        }else{
            memset(copy_page, 0, PAGE_SIZE);
        }

        copy_page += PAGE_SIZE;
        total_pages -= PAGE_SIZE;
    }

    return 1;
}

static int elf_read_interp(unsigned fd, Elf32_Phdr* phdr, char* path)
{
    unsigned off = phdr->p_offset;
    unsigned size = phdr->p_filesz;

    return (fs_read(fd, off, path, size) != 0xffffffff);
 

}

static int elf_find_interp(unsigned fd, unsigned table_offset, unsigned size, unsigned num, char* path)
{
    unsigned offset = 0;
	int i = 0;

	for (i = 0; i < num; i++){
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
		fs_read(fd, head_offset, &phdr, sizeof(phdr));
		if (phdr.p_type == PT_INTERP){
            return elf_read_interp(fd, &phdr, path);
        } else {
            continue;
        }
        
    }

    return 0;
}

static unsigned elf_map_programs(unsigned fd, unsigned table_offset, unsigned size, unsigned num)
{
	unsigned offset = 0;
	int i = 0;

	for (i = 0; i < num; i++){
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
        char path[256] = {0};
		fs_read(fd, head_offset, &phdr, sizeof(phdr));
        printf("[%d:%x - %x] ", phdr.p_type, phdr.p_vaddr, phdr.p_memsz);
        if (/*phdr.p_type == PT_DYNAMIC ||*/
            phdr.p_type == PT_LOAD) {
            elf_map_section(fd, &phdr, 0);
       
        } else {
            continue;
        }
        
    }

    return 1;

}
static unsigned elf_map_elf_hdr(unsigned fd, unsigned table_offset, unsigned size, unsigned num,
                                mos_binfmt* fmt)
{
	unsigned offset = 0;
	int i = 0;

	for (i = 0; i < num; i++){
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
        char path[256] = {0};
		fs_read(fd, head_offset, &phdr, sizeof(phdr));

        if (phdr.p_type == PT_PHDR) {
            fmt->e_phnum = num;
            elf_map_section(fd, &phdr, fmt);
        } else if(phdr.p_type == PT_LOAD){
            elf_map_section(fd, &phdr, 0);
        } else {
            continue;
        }
        
    }

    return 1;
}

unsigned elf_map(char* path, mos_binfmt* fmt)
{
	unsigned fd = fs_open(path);
	unsigned entry_point = 0;
	unsigned head_len = 0;
	Elf32_Ehdr elf;
    char* interp = kmalloc(256);
    memset(interp, 0, 256);

    if (fd == MAX_FD) {
        kfree(interp);
        return 0;
    }
    // elf header will at the beginning of va anyway
	fs_read(fd, 0, &elf, sizeof(Elf32_Ehdr));
	entry_point = elf.e_entry;
	head_len = elf.e_ehsize;


	if (elf.e_ident[0] != 0x7f){
        kfree(interp);
		return 0;
    }

	// now we only support ia32, sorry..
	if (elf.e_ident[4] != ELFCLASS32){
        kfree(interp);
		return 0;
    }

	if (elf.e_type != ET_EXEC &&
        elf.e_type != ET_DYN){
        kfree(interp);
		return 0;
    }

    printf("map_elf %s , ", path);
    if( elf_find_interp(fd, elf.e_phoff, elf.e_phentsize, elf.e_phnum, path)){
        fmt->e_entry = elf.e_entry;
        elf_map_elf_hdr(fd,elf.e_phoff, elf.e_phentsize, elf.e_phnum, fmt);
        printf("\n");
        entry_point = elf_map(path, 0);
        fmt->interp_load_addr = entry_point;
    }else{
        elf_map_programs(fd, elf.e_phoff, elf.e_phentsize, elf.e_phnum);
        printf("\n");
    }

    fs_close(fd);
    kfree(interp);

	return entry_point;

}