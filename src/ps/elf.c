#include <elf.h>
#include <namespace.h>
#if defined WIN32 || defined MACOS
#include <osdep.h>
#else
#include <mm.h>
#include <ps.h>
#include <klib.h>
#endif

static unsigned elf_map_section(unsigned fd, Elf32_Phdr* phdr, mos_binfmt* fmt)
{
    unsigned file_off = phdr->p_offset;
    unsigned va_begin = phdr->p_vaddr & PAGE_SIZE_MASK;
    unsigned fileSiz = phdr->p_filesz;
    unsigned va_end = (phdr->p_vaddr + phdr->p_memsz - 1) & PAGE_SIZE_MASK;
    unsigned i = 0;

    for (i = va_begin; i <= va_end; i += PAGE_SIZE)
    {
        mm_add_dynamic_map(i, 0, PAGE_ENTRY_USER_DATA);
        memset(i, 0, PAGE_SIZE);
    }

    fs_read(fd, file_off, phdr->p_vaddr, fileSiz);
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

    for (i = 0; i < num; i++)
    {
        unsigned head_offset = table_offset + i * size;
        Elf32_Phdr phdr;
        fs_read(fd, head_offset, &phdr, sizeof(phdr));
        if (phdr.p_type == PT_INTERP)
        {
            return elf_read_interp(fd, &phdr, path);
        }
        else
        {
            continue;
        }

    }

    return 0;
}


static unsigned elf_map_programs(unsigned fd, unsigned table_offset, unsigned size, unsigned num)
{
    unsigned offset = 0;
    int i = 0;
    unsigned elf_bss = 0;
    unsigned last_bss = 0;
    unsigned k;
    for (i = 0; i < num; i++)
    {
        unsigned head_offset = table_offset + i * size;
        Elf32_Phdr phdr;
        fs_read(fd, head_offset, &phdr, sizeof(phdr));
        if (/*phdr.p_type == PT_DYNAMIC ||*/
            phdr.p_type == PT_LOAD)
        {
            elf_map_section(fd, &phdr, 0);
            k = phdr.p_filesz + phdr.p_vaddr;
            if (k > elf_bss)
            {
                elf_bss = k;
            }
            k = phdr.p_memsz + phdr.p_vaddr;
            if (k > last_bss)
            {
                last_bss = k;
            }

        }
        else
        {
            continue;
        }
    }

    elf_bss = (elf_bss + PAGE_SIZE - 1) & PAGE_SIZE_MASK;
    if (last_bss > elf_bss)
    {
        unsigned page_count = (last_bss - elf_bss) / PAGE_SIZE + 1;
        unsigned i;
        for (i = 0; i < page_count; i++)
        {
            mm_add_dynamic_map(elf_bss + i*PAGE_SIZE, 0, PAGE_ENTRY_USER_DATA);
            memset(elf_bss + i*PAGE_SIZE, 0, PAGE_SIZE);
        }
    }

    return 1;

}
static unsigned elf_map_elf_hdr(unsigned fd, unsigned table_offset, unsigned size, unsigned num,
    mos_binfmt* fmt)
{
    unsigned offset = 0;
    int i = 0;

    for (i = 0; i < num; i++)
    {
        unsigned head_offset = table_offset + i * size;
        Elf32_Phdr phdr;
        fs_read(fd, head_offset, &phdr, sizeof(phdr));

        if (phdr.p_type == PT_PHDR)
        {
            fmt->e_phnum = num;
            fmt->e_phoff = (phdr.p_vaddr & ~PAGE_SIZE_MASK);
            fmt->elf_load_addr = (phdr.p_vaddr & PAGE_SIZE_MASK);
        }
        else if (phdr.p_type == PT_LOAD/* || phdr.p_type == PT_DYNAMIC */)
        {
            elf_map_section(fd, &phdr, 0);
        }
        else
        {
            continue;
        }

    }

    return 1;
}


static unsigned elf_map_section_at(unsigned fd, Elf32_Phdr* phdr, unsigned bias)
{
    unsigned file_off = phdr->p_offset;
    unsigned va_begin = phdr->p_vaddr & PAGE_SIZE_MASK;
    unsigned fileSiz = phdr->p_filesz;
    unsigned va_end = (phdr->p_vaddr + phdr->p_memsz - 1) & PAGE_SIZE_MASK;
    unsigned i = 0;

    for (i = va_begin; i <= va_end; i += PAGE_SIZE)
    {
        do_mmap(i + bias, PAGE_SIZE, 0, 0, -1, 0);
        // memset(i+bias, 0, PAGE_SIZE);
    }

    fs_read(fd, file_off, phdr->p_vaddr + bias, fileSiz);
    return 1;
}

static unsigned elf_map_programs_at(unsigned fd, unsigned table_offset, unsigned size, unsigned num, unsigned bias)
{
    unsigned offset = 0;
    int i = 0;
    for (i = 0; i < num; i++)
    {
        unsigned head_offset = table_offset + i * size;
        Elf32_Phdr phdr;
        fs_read(fd, head_offset, &phdr, sizeof(phdr));
        if (/*phdr.p_type == PT_DYNAMIC ||*/
            phdr.p_type == PT_LOAD)
        {
            elf_map_section_at(fd, &phdr, bias);
        }
        else
        {
            continue;
        }
    }
    return 1;

}

static unsigned elf_map_get_dynamic_pages(unsigned fd, unsigned table_offset, unsigned size, unsigned num)
{
    unsigned va_page_start = 0xffffffff;
    unsigned va_page_end = 0;
    unsigned offset = 0;
    int i = 0;
    for (i = 0; i < num; i++)
    {
        unsigned head_offset = table_offset + i * size;
        Elf32_Phdr phdr;
        fs_read(fd, head_offset, &phdr, sizeof(phdr));
        if (phdr.p_type == PT_LOAD)
        {
            unsigned va_begin = phdr.p_vaddr & PAGE_SIZE_MASK;
            unsigned va_end = (phdr.p_vaddr + phdr.p_memsz - 1) & PAGE_SIZE_MASK;
            if (va_begin < va_page_start)
            {
                va_page_start = va_begin;
            }
            if (va_end > va_page_end)
            {
                va_page_end = va_end;
            }
        }
        else
        {
            continue;
        }
    }

    return (va_page_end - va_page_start) / PAGE_SIZE + 1;
}

static unsigned elf_map_dynamic(char* path, mos_binfmt* fmt)
{
    unsigned fd = fs_open(path);
    unsigned entry_point = 0;
    unsigned head_len = 0;
    Elf32_Ehdr elf;

    if (fd == MAX_FD)
    {
        return 0;
    }
    // elf header will at the beginning of va anyway
    fs_read(fd, 0, &elf, sizeof(Elf32_Ehdr));
    entry_point = elf.e_entry;
    head_len = elf.e_ehsize;


    if (elf.e_ident[0] != 0x7f)
    {
        return 0;
    }

    // now we only support ia32, sorry..
    if (elf.e_ident[4] != ELFCLASS32)
    {
        return 0;
    }

    if (elf.e_type != ET_DYN)
    {
        return 0;
    }

    // interop must be the first dynamic library
    fmt->interp_bias = vm_get_usr_zone(elf_map_get_dynamic_pages(fd, elf.e_phoff, elf.e_phentsize, elf.e_phnum));
    fmt->interp_load_addr = fmt->interp_bias + elf.e_entry;
    elf_map_programs_at(fd, elf.e_phoff, elf.e_phentsize, elf.e_phnum, fmt->interp_bias);

    fs_close(fd);

    return 1;
}

unsigned elf_map(char* path, mos_binfmt* fmt)
{
    unsigned fd = fs_open(path);
    unsigned entry_point = 0;
    unsigned head_len = 0;
    Elf32_Ehdr elf;
    char* interp = kmalloc(64);
    memset(interp, 0, 64);

    if (fd == MAX_FD)
    {
        kfree(interp);
        return 0;
    }
    // elf header will at the beginning of va anyway
    fs_read(fd, 0, &elf, sizeof(Elf32_Ehdr));
    entry_point = elf.e_entry;
    head_len = elf.e_ehsize;


    if (elf.e_ident[0] != 0x7f)
    {
        kfree(interp);
        return 0;
    }

    // now we only support ia32, sorry..
    if (elf.e_ident[4] != ELFCLASS32)
    {
        kfree(interp);
        return 0;
    }

    if (elf.e_type != ET_EXEC)
    {
        kfree(interp);
        return 0;
    }

    fmt->e_entry = elf.e_entry;

    if (elf_find_interp(fd, elf.e_phoff, elf.e_phentsize, elf.e_phnum, interp))
    {
        if (!strcmp(path, interp))
        {
            elf_map_programs(fd, elf.e_phoff, elf.e_phentsize, elf.e_phnum);
            fmt->interp_load_addr = entry_point;
        }
        else
        {
            elf_map_elf_hdr(fd, elf.e_phoff, elf.e_phentsize, elf.e_phnum, fmt);
            elf_map_dynamic(interp, fmt);
            //fmt->interp_load_addr = entry_point;
        }
    }
    else
    {
        elf_map_programs(fd, elf.e_phoff, elf.e_phentsize, elf.e_phnum);
        fmt->interp_load_addr = entry_point;
    }

    fs_close(fd);
    kfree(interp);

    return entry_point;

}