#include <ps/elf.h>
#include <fs/namespace.h>
#if defined WIN32 || defined MACOS
#include <osdep.h>
#else
#include <mm/mm.h>
#include <ps/ps.h>
#endif

static unsigned elf_map_section(unsigned fd, Elf32_Phdr* phdr)
{
	int writable = (phdr->p_flags & PF_W) != 0;
	unsigned file_page = phdr->p_offset & PAGE_SIZE_MASK;
	unsigned mem_page = phdr->p_vaddr & PAGE_SIZE_MASK;
	unsigned page_offset = phdr->p_vaddr & ~PAGE_SIZE_MASK;
	unsigned read_bytes, zero_bytes;
    unsigned copy_page;
    unsigned total_pages = ROUND_UP(page_offset + phdr->p_memsz, PAGE_SIZE);

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
	//mmap(mem_page, read_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, file_page);

    return 1;
}

static unsigned elf_map_programs(unsigned fd, unsigned table_offset, unsigned size, unsigned num)
{
	unsigned offset = 0;
	int i = 0;

	for (i = 0; i < num; i++){
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
		fs_read(fd, head_offset, &phdr, sizeof(phdr));
		if (phdr.p_type != PT_LOAD)
			continue;
		elf_map_section(fd, &phdr);
	}

    return 1;

}

unsigned elf_map(char* path)
{
	unsigned fd = fs_open(path);
	unsigned entry_point = 0;
	unsigned head_len = 0;
	Elf32_Ehdr elf;

    if (fd == MAX_FD) {
        return 0;
    }
    // elf header will at the beginning of va anyway
	fs_read(fd, 0, &elf, sizeof(Elf32_Ehdr));
	entry_point = elf.e_entry;
	head_len = elf.e_ehsize;


	if (elf.e_ident[0] != 0x7f)
		return 0;

	// now we only support ia32, sorry..
	if (elf.e_ident[4] != ELFCLASS32)
		return 0;

	// now we only support executable file
	if (elf.e_type != ET_EXEC)
		return 0;

	elf_map_programs(fd, elf.e_phoff, elf.e_phentsize, elf.e_phnum);
	fs_close(fd);

	return entry_point;

}