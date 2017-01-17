#include <elf.h>
#include <mm.h>
#include <ps.h>
#include <klib.h>


static int elf_read(filep fp, unsigned off, void* buf, int len)
{
    size_t wcnt;
    int ret = -1;
    ret = ext4_fseek(fp->inode, off, SEEK_SET);
    if (ret != EOK)
        return -1;
    ret = ext4_fread(fp->inode, buf, len, &wcnt);
    if (ret != EOK)
        return -1;
    return (int)wcnt;
}

static unsigned elf_map_section(filep fp, Elf32_Phdr* phdr, mos_binfmt* fmt)
{
    unsigned file_off = phdr->p_offset;
    unsigned va_begin = phdr->p_vaddr & PAGE_SIZE_MASK;
    unsigned fileSiz = phdr->p_filesz;
    unsigned va_end = (phdr->p_vaddr + phdr->p_memsz - 1) & PAGE_SIZE_MASK;
    unsigned i = 0;

    if (va_begin == phdr->p_vaddr){
        do_mmap_kernel(va_begin, (va_end - va_begin + PAGE_SIZE), 0, 0, fp, file_off);
    }else{
        do_mmap_kernel(va_begin, (va_end - va_begin + PAGE_SIZE), 0, 0, 0, 0);
        elf_read(fp, file_off, phdr->p_vaddr, fileSiz);
    }
    return 1;
}

static int elf_read_interp(filep fp, Elf32_Phdr* phdr, char* path)
{
    unsigned off = phdr->p_offset;
    unsigned size = phdr->p_filesz;
    return (elf_read(fp, off, path, size) >= 0);
}

static int elf_find_interp(filep fp, unsigned table_offset, unsigned size, unsigned num, char* path)
{
    unsigned offset = 0;
    int i = 0;

    for (i = 0; i < num; i++)
    {
        unsigned head_offset = table_offset + i * size;
        Elf32_Phdr phdr;
        elf_read(fp, head_offset, &phdr, sizeof(phdr));
        if (phdr.p_type == PT_INTERP)
        {
            return elf_read_interp(fp, &phdr, path);
        }
        else
        {
            continue;
        }

    }

    return 0;
}


static unsigned elf_map_programs(filep fp, unsigned table_offset, unsigned size, unsigned num)
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
        elf_read(fp, head_offset, &phdr, sizeof(phdr));
        if (/*phdr.p_type == PT_DYNAMIC ||*/
            phdr.p_type == PT_LOAD)
        {
            elf_map_section(fp, &phdr, 0);
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
        }
        REFRESH_CACHE();
    }

    return 1;

}
static unsigned elf_map_elf_hdr(filep fp, unsigned table_offset, unsigned size, unsigned num,
    mos_binfmt* fmt)
{
    unsigned offset = 0;
    int i = 0;

    for (i = 0; i < num; i++)
    {
        unsigned head_offset = table_offset + i * size;
        Elf32_Phdr phdr;
        elf_read(fp, head_offset, &phdr, sizeof(phdr));

        if (phdr.p_type == PT_PHDR)
        {
            fmt->e_phnum = num;
            fmt->e_phoff = (phdr.p_vaddr & ~PAGE_SIZE_MASK);
            fmt->elf_load_addr = (phdr.p_vaddr & PAGE_SIZE_MASK);
        }
        else if (phdr.p_type == PT_LOAD/* || phdr.p_type == PT_DYNAMIC */)
        {
            elf_map_section(fp, &phdr, 0);
        }
        else
        {
            continue;
        }

    }

    return 1;
}


static unsigned elf_map_section_at(filep fp, Elf32_Phdr* phdr, unsigned bias)
{
    unsigned file_off = phdr->p_offset;
    unsigned va_begin = phdr->p_vaddr & PAGE_SIZE_MASK;
    unsigned fileSiz = phdr->p_filesz;
    unsigned va_end = (phdr->p_vaddr + phdr->p_memsz - 1) & PAGE_SIZE_MASK;
    unsigned i = 0;

    do_mmap(bias+ va_begin, (va_end - va_begin + PAGE_SIZE), 0, 0, -1, 0);
    elf_read(fp, file_off, phdr->p_vaddr + bias, fileSiz);
    return 1;
}

static unsigned elf_map_programs_at(filep fp, unsigned table_offset, unsigned size, unsigned num, unsigned bias)
{
    unsigned offset = 0;
    int i = 0;
    for (i = 0; i < num; i++)
    {
        unsigned head_offset = table_offset + i * size;
        Elf32_Phdr phdr;
        elf_read(fp, head_offset, &phdr, sizeof(phdr));
        if (/*phdr.p_type == PT_DYNAMIC ||*/
            phdr.p_type == PT_LOAD)
        {
            elf_map_section_at(fp, &phdr, bias);
        }
        else
        {
            continue;
        }
    }
    return 1;

}

static unsigned elf_map_get_dynamic_pages(filep fp, unsigned table_offset, unsigned size, unsigned num)
{
    unsigned va_page_start = 0xffffffff;
    unsigned va_page_end = 0;
    unsigned offset = 0;
    int i = 0;
    for (i = 0; i < num; i++)
    {
        unsigned head_offset = table_offset + i * size;
        Elf32_Phdr phdr;
        elf_read(fp, head_offset, &phdr, sizeof(phdr));
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
    filep fp = fs_open_file(path, 0, "r", 1);
    unsigned entry_point = 0;
    unsigned head_len = 0;
    Elf32_Ehdr elf;

    if (fp == NULL)
    {
        klog("!!!!!!!!!!!\n");
        return 0;
    }
    // elf header will at the beginning of va anyway
    elf_read(fp, 0, &elf, sizeof(Elf32_Ehdr));
    entry_point = elf.e_entry;
    head_len = elf.e_ehsize;


    if (elf.e_ident[0] != 0x7f)
    {
        fs_destroy(fp);
        return 0;
    }

    // now we only support ia32, sorry..
    if (elf.e_ident[4] != ELFCLASS32)
    {
        fs_destroy(fp);
        return 0;
    }

    if (elf.e_type != ET_DYN)
    {
        fs_destroy(fp);
        return 0;
    }

    // interop must be the first dynamic library
    fmt->interp_bias = vm_get_usr_zone(elf_map_get_dynamic_pages(fp, elf.e_phoff, elf.e_phentsize, elf.e_phnum));
    fmt->interp_load_addr = fmt->interp_bias + elf.e_entry;
    elf_map_programs_at(fp, elf.e_phoff, elf.e_phentsize, elf.e_phnum, fmt->interp_bias);

    fs_destroy(fp);

    return 1;
}

unsigned elf_map(char* path, mos_binfmt* fmt)
{
    filep fp = fs_open_file(path, 0, "r", 1);
    unsigned entry_point = 0;
    unsigned head_len = 0;
    Elf32_Ehdr elf;
    char* interp = name_get();
    memset(interp, 0, MAX_PATH);

    if (fp == NULL)
    {
        klog("!!!!!!!!!!!\n");
        name_put(interp);
        return 0;
    }
    // elf header will at the beginning of va anyway
    elf_read(fp, 0, &elf, sizeof(Elf32_Ehdr));
    entry_point = elf.e_entry;
    head_len = elf.e_ehsize;


    if (elf.e_ident[0] != 0x7f)
    {
        fs_destroy(fp);
        name_put(interp);
        return 0;
    }

    // now we only support ia32, sorry..
    if (elf.e_ident[4] != ELFCLASS32)
    {
        fs_destroy(fp);
        name_put(interp);
        return 0;
    }

    if (elf.e_type != ET_EXEC)
    {
        fs_destroy(fp);
        name_put(interp);
        return 0;
    }

    fmt->e_entry = elf.e_entry;

    if (elf_find_interp(fp, elf.e_phoff, elf.e_phentsize, elf.e_phnum, interp))
    {
        if (!strcmp(path, interp))
        {
            elf_map_programs(fp, elf.e_phoff, elf.e_phentsize, elf.e_phnum);
            fmt->interp_load_addr = entry_point;
        }
        else
        {
            elf_map_elf_hdr(fp, elf.e_phoff, elf.e_phentsize, elf.e_phnum, fmt);
            elf_map_dynamic(interp, fmt);
            //fmt->interp_load_addr = entry_point;
        }
    }
    else
    {
        elf_map_programs(fp, elf.e_phoff, elf.e_phentsize, elf.e_phnum);
        fmt->interp_load_addr = entry_point;
    }

    fs_destroy(fp);
    name_put(interp);

    return entry_point;

}