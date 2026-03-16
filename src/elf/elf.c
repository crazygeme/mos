#include <hw/time.h>
#include <elf/elf.h>
#include <mm/mm.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <macro.h>
#include <ext4.h>

/* Cumulative time (microseconds) spent in elf_read(), used when profiling is
 * enabled via TestControl.profiling. */
unsigned long long elf_read_time = 0;

/*
 * elf_read - read bytes from an ELF file at a given offset
 *
 * Seeks to @off in the underlying ext4 file and reads @len bytes into @buf.
 * When profiling is enabled the elapsed time is accumulated into elf_read_time.
 *
 * Returns the number of bytes actually read, or a negative value on error.
 */
static int elf_read(file *fp, unsigned off, void *buf, int len)
{
	size_t rcnt;
	int ret = -1;
	unsigned long long begin = TestControl.profiling ? time_now_us() : 0;

	ret = ext4_fseek(fp->f_inode->i_private, off, SEEK_SET);
	if (ret != EOK)
		goto DONE;

	ret = ext4_fread(fp->f_inode->i_private, buf, len, &rcnt);
	if (ret != EOK)
		goto DONE;

	ret = (int)rcnt;

DONE:
	if (TestControl.profiling)
		elf_read_time += time_now_us() - begin;

	return ret;
}

/*
 * elf_map_section - map one PT_LOAD segment of the main executable
 *
 * If the segment's virtual address is already page-aligned, create a
 * file-backed mapping so pages are loaded lazily on fault.  Otherwise fall
 * back to an anonymous read-write mapping and eagerly copy the segment data.
 *
 * @fmt is currently unused here (reserved for future bookkeeping).
 */
/* Translate ELF segment permission flags (PF_R/PF_W/PF_X) to mmap PROT_*. */
static int elf_pflags_to_prot(unsigned p_flags)
{
	int prot = 0;
	if (p_flags & PF_R)
		prot |= PROT_READ;
	if (p_flags & PF_W)
		prot |= PROT_WRITE;
	if (p_flags & PF_X)
		prot |= PROT_EXEC;
	return prot;
}

/*
 * elf_map_bss - zero-fill and map pages for the BSS region
 *
 * @elf_bss:  max(p_vaddr + p_filesz) across all PT_LOAD segments — the first
 *            virtual address past the last byte of file data (unaligned).
 * @last_bss: max(p_vaddr + p_memsz)  across all PT_LOAD segments — the first
 *            virtual address past the last byte of the memory image.
 *
 * Two distinct regions need handling:
 *
 *   1. Partial tail of the last file-backed page [elf_bss, round_up(elf_bss)):
 *      The file-backed mapping covers up to elf_bss; the remaining bytes on
 *      that page are not read from disk and must be explicitly zeroed here.
 *
 *   2. Full BSS pages [round_up(elf_bss), last_bss):
 *      These pages lie entirely beyond the file data and are not covered by
 *      any mapping yet.  Anonymous zero-filled pages are allocated for them,
 *      the TLB is flushed, then the region is zeroed via memset.
 *
 * Correctness assumption: only the PT_LOAD segment with the highest virtual
 * address is expected to have p_memsz > p_filesz (i.e. contain BSS).  This
 * holds for every standard compiler/linker output.  If multiple PT_LOAD
 * segments had BSS, the earlier segments' BSS regions would not be handled
 * by this function.
 */
static void elf_map_bss(unsigned elf_bss, unsigned last_bss)
{
	unsigned elf_bss_raw = elf_bss;
	elf_bss = (elf_bss + PAGE_SIZE - 1) & PAGE_SIZE_MASK;
	if (elf_bss_raw < elf_bss)
		memset((void *)elf_bss_raw, 0, elf_bss - elf_bss_raw);
	if (last_bss > elf_bss) {
		unsigned page_count =
			(last_bss - elf_bss + PAGE_SIZE - 1) / PAGE_SIZE;
		unsigned i;
		for (i = 0; i < page_count; i++) {
			mm_add_dynamic_map(elf_bss + i * PAGE_SIZE, 0,
					   PAGE_ENTRY_USER_DATA);
		}
		RELOAD_CR3();
		memset((void *)elf_bss, 0, last_bss - elf_bss);
	}
}

static unsigned elf_map_section(file *fp, Elf32_Phdr *phdr, mos_binfmt *fmt)
{
	unsigned file_off = phdr->p_offset;
	unsigned va_begin = phdr->p_vaddr & PAGE_SIZE_MASK;
	unsigned va_diff = phdr->p_vaddr - va_begin;
	unsigned fileSiz = phdr->p_filesz;
	unsigned va_end = (phdr->p_vaddr + phdr->p_memsz - 1) & PAGE_SIZE_MASK;
	unsigned map_size = va_end - va_begin + PAGE_SIZE;
	int prot = elf_pflags_to_prot(phdr->p_flags);

	if (va_begin == phdr->p_vaddr)
		/* Page-aligned: use a file-backed mapping (lazy load). */
		do_mmap_kernel(va_begin, map_size, prot, 0, fp, file_off);
	else {
		/*
		 * Unaligned: allocate anonymous pages and eagerly copy data.
		 * PROT_WRITE is required here so the elf_read below can
		 * populate the mapping; the final prot still reflects the
		 * segment flags.
		 */
		unsigned mapped = do_mmap_kernel(va_begin, map_size,
						 prot | PROT_WRITE, 0, 0, 0);
		elf_read(fp, file_off, phdr->p_vaddr, fileSiz);
		do_mmap_update(mapped, prot, 0);
	}
	return 1;
}

/*
 * elf_read_interp - read the PT_INTERP segment into @path
 *
 * The PT_INTERP segment contains the NUL-terminated path of the dynamic
 * linker (e.g. "/lib/ld-linux.so.2").  Returns non-zero on success.
 */
static int elf_read_interp(file *fp, Elf32_Phdr *phdr, char *path)
{
	unsigned off = phdr->p_offset;
	unsigned size = phdr->p_filesz;
	return (elf_read(fp, off, path, size) >= 0);
}

/*
 * elf_find_interp - scan the program header table for a PT_INTERP entry
 *
 * Iterates over all @num program headers starting at file offset
 * @table_offset (each @size bytes wide).  When PT_INTERP is found its
 * content is read into @path.
 *
 * Returns 1 if an interpreter path was found and read, 0 otherwise.
 */
static int elf_find_interp(file *fp, unsigned table_offset, unsigned size,
			   unsigned num, char *path)
{
	unsigned offset = 0;
	int i = 0;

	for (i = 0; i < num; i++) {
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
		elf_read(fp, head_offset, &phdr, sizeof(phdr));
		if (phdr.p_type == PT_INTERP) {
			return elf_read_interp(fp, &phdr, path);
		}
	}

	return 0;
}

/*
 * elf_map_programs - map all PT_LOAD segments of the main executable
 *
 * In addition to mapping each loadable segment via elf_map_section(), this
 * function takes care of the BSS region that lies beyond the end of the
 * file image:
 *
 *   elf_bss  — first virtual address past the last byte of file data
 *              (aligned up to the next page boundary)
 *   last_bss — first virtual address past the last byte of memory image
 *              (memsz, which includes .bss)
 *
 * Any pages in [elf_bss, last_bss) are not covered by the file-backed
 * mapping, so they are mapped as anonymous zero-filled user-data pages and
 * the TLB is flushed with RELOAD_CR3().
 */
static unsigned elf_map_programs(file *fp, unsigned table_offset, unsigned size,
				 unsigned num)
{
	unsigned offset = 0;
	int i = 0;
	unsigned elf_bss = 0; /* end of file-backed data (unaligned) */
	unsigned last_bss = 0; /* end of memory image (.bss included) */
	unsigned k;
	for (i = 0; i < num; i++) {
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
		elf_read(fp, head_offset, &phdr, sizeof(phdr));
		if (phdr.p_type != PT_LOAD) {
			continue;
		}

		elf_map_section(fp, &phdr, 0);

		/* Track the highest virtual address covered by file data. */
		k = phdr.p_filesz + phdr.p_vaddr;
		if (k > elf_bss) {
			elf_bss = k;
		}
		/* Track the highest virtual address covered by memory image. */
		k = phdr.p_memsz + phdr.p_vaddr;
		if (k > last_bss) {
			last_bss = k;
		}
	}

	elf_map_bss(elf_bss, last_bss);

	return 1;
}
/*
 * elf_map_elf_hdr - map segments and record auxiliary info for the dynamic case
 *
 * When the executable has a dynamic linker (PT_INTERP), the kernel must pass
 * certain ELF metadata to the dynamic linker via the auxiliary vector.  This
 * function:
 *   - Records the program-header count (e_phnum), the within-page offset of
 *     the phdr table (e_phoff), and the load address of the first page
 *     (elf_load_addr) into @fmt — the dynamic linker reads these via AT_PHDR,
 *     AT_PHNUM, etc.
 *   - Maps every PT_LOAD segment via elf_map_section().
 *
 * PT_PHDR, when present, describes where the program header table itself is
 * mapped, which is how we derive the base load address.
 */
static unsigned elf_map_elf_hdr(file *fp, unsigned table_offset, unsigned size,
				unsigned num, mos_binfmt *fmt)
{
	unsigned offset = 0;
	int i = 0;
	unsigned elf_bss = 0;
	unsigned last_bss = 0;

	for (i = 0; i < num; i++) {
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
		elf_read(fp, head_offset, &phdr, sizeof(phdr));

		if (phdr.p_type == PT_PHDR) {
			/* Save phdr metadata for the auxiliary vector. */
			fmt->e_phnum = num;
			fmt->e_phoff = (phdr.p_vaddr & ~PAGE_SIZE_MASK);
			fmt->elf_load_addr = (phdr.p_vaddr & PAGE_SIZE_MASK);
		} else if (phdr.p_type ==
			   PT_LOAD /* || phdr.p_type == PT_DYNAMIC */) {
			elf_map_section(fp, &phdr, 0);

			unsigned k = phdr.p_filesz + phdr.p_vaddr;
			if (k > elf_bss)
				elf_bss = k;
			k = phdr.p_memsz + phdr.p_vaddr;
			if (k > last_bss)
				last_bss = k;
		}
	}

	elf_map_bss(elf_bss, last_bss);

	return 1;
}

static unsigned elf_map_section_at(file *fp, Elf32_Phdr *phdr, unsigned bias)
{
	unsigned file_off = phdr->p_offset;
	unsigned va_begin = phdr->p_vaddr & PAGE_SIZE_MASK;
	unsigned fileSiz = phdr->p_filesz;
	unsigned va_end = (phdr->p_vaddr + phdr->p_memsz - 1) & PAGE_SIZE_MASK;
	unsigned map_size = va_end - va_begin + PAGE_SIZE;

	int prot = elf_pflags_to_prot(phdr->p_flags);

	if (va_begin == phdr->p_vaddr) {
		/*
		 * Page-aligned section: map file-backed with lazy page fault
		 * loading.  The mmap cache in the page fault handler shares
		 * physical pages across processes loading the same .so,
		 * eliminating redundant disk reads on every execve.
		 * BSS (memsz > filesz) is zero-filled automatically by the
		 * page fault handler for pages that fall beyond the file end.
		 */
		do_mmap_kernel(bias + va_begin, map_size, prot, 0, fp,
			       file_off);
	} else {
		/*
		 * Unaligned section: page boundary does not align with a file
		 * block boundary, so fall back to eager read into a private
		 * anonymous mapping.  PROT_WRITE is added temporarily so the
		 * elf_read below can populate the pages.
		 */
		unsigned mapped = do_mmap_kernel(bias + va_begin, map_size,
						 prot | PROT_WRITE, 0, 0, 0);
		elf_read(fp, file_off, phdr->p_vaddr + bias, fileSiz);
		do_mmap_update(mapped, prot, 0);
	}

	return 1;
}

/*
 * elf_map_programs_at - map all PT_LOAD segments of a shared object at @bias
 *
 * Works like elf_map_programs() but applies a load-address @bias so the
 * shared object can be placed at an arbitrary virtual address chosen by
 * vm_get_usr_zone().  Used when loading the dynamic linker (interpreter).
 */
static unsigned elf_map_programs_at(file *fp, unsigned table_offset,
				    unsigned size, unsigned num, unsigned bias)
{
	unsigned offset = 0;
	int i = 0;
	for (i = 0; i < num; i++) {
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
		elf_read(fp, head_offset, &phdr, sizeof(phdr));
		if (phdr.p_type == PT_LOAD) {
			elf_map_section_at(fp, &phdr, bias);
		}
	}
	return 1;
}

/*
 * elf_map_get_dynamic_pages - calculate the total page span of a shared object
 *
 * Scans all PT_LOAD segments and returns the number of pages spanned from the
 * lowest to the highest virtual address.  This is used to reserve a
 * contiguous virtual-address region via vm_get_usr_zone() before mapping the
 * interpreter, so that all its segments land in a single coherent window.
 */
static unsigned elf_map_get_dynamic_pages(file *fp, unsigned table_offset,
					  unsigned size, unsigned num)
{
	unsigned va_page_start = 0xffffffff;
	unsigned va_page_end = 0;
	unsigned offset = 0;
	int i = 0;
	for (i = 0; i < num; i++) {
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
		elf_read(fp, head_offset, &phdr, sizeof(phdr));
		if (phdr.p_type != PT_LOAD) {
			continue;
		}

		unsigned va_begin = phdr.p_vaddr & PAGE_SIZE_MASK;
		unsigned va_end = (phdr.p_vaddr + phdr.p_memsz - 1) &
				  PAGE_SIZE_MASK;
		if (va_begin < va_page_start) {
			va_page_start = va_begin;
		}

		if (va_end > va_page_end) {
			va_page_end = va_end;
		}
	}

	return (va_page_end - va_page_start) / PAGE_SIZE + 1;
}

/*
 * elf_map_dynamic - load the dynamic linker (interpreter) into user space
 *
 * Opens the ELF shared object at @path (typically "/lib/ld-linux.so.2"),
 * validates its header (must be a 32-bit ET_DYN), then:
 *   1. Computes the total page span of its PT_LOAD segments.
 *   2. Reserves a contiguous virtual-address window via vm_get_usr_zone()
 *      and stores the base in fmt->interp_bias.
 *   3. Maps all PT_LOAD segments relative to interp_bias.
 *   4. Records the interpreter's entry point as interp_bias + e_entry so
 *      the kernel can jump to it instead of the executable's entry point.
 *
 * The interpreter then takes over, resolves symbol relocations, and
 * eventually jumps to the executable's actual entry point.
 *
 * Returns 1 on success, 0 on any error.
 */
static unsigned elf_map_dynamic(char *path, mos_binfmt *fmt)
{
	file *fp = fs_open_file(path, 0, "r", 1);
	unsigned entry_point = 0;
	unsigned head_len = 0;
	Elf32_Ehdr elf;

	if (fp == NULL) {
		return 0;
	}
	/* ELF header is always at offset 0. */
	elf_read(fp, 0, &elf, sizeof(Elf32_Ehdr));
	entry_point = elf.e_entry;
	head_len = elf.e_ehsize;

	/* Validate ELF magic number (0x7f 'E' 'L' 'F'). */
	if (elf.e_ident[0] != 0x7f) {
		fs_put_file(fp);
		return 0;
	}

	/* Only IA-32 (32-bit) ELF is supported. */
	if (elf.e_ident[4] != ELFCLASS32) {
		fs_put_file(fp);
		return 0;
	}

	/* Must be a shared object (ET_DYN), not an executable. */
	if (elf.e_type != ET_DYN) {
		fs_put_file(fp);
		return 0;
	}

	/* Reserve virtual address space and map interpreter segments. */
	fmt->interp_bias = vm_get_usr_zone(elf_map_get_dynamic_pages(
		fp, elf.e_phoff, elf.e_phentsize, elf.e_phnum));
	fmt->interp_load_addr = fmt->interp_bias + elf.e_entry;
	elf_map_programs_at(fp, elf.e_phoff, elf.e_phentsize, elf.e_phnum,
			    fmt->interp_bias);

	fs_put_file(fp);

	return 1;
}

/*
 * elf_map - top-level ELF loader: map an executable into the current process
 *
 * Opens the ELF at @path, validates it (32-bit ET_EXEC only), and maps its
 * segments.  The behaviour depends on whether the executable has a PT_INTERP
 * (dynamic linker) segment:
 *
 *   No interpreter (static binary):
 *     Map all PT_LOAD segments directly.  fmt->interp_load_addr is set to
 *     the executable's own entry point — the kernel jumps there on exec.
 *
 *   Interpreter path == executable path (the interpreter loads itself):
 *     Same as the static case.  This handles the unusual situation where
 *     the dynamic linker is executed directly.
 *
 *   Interpreter path != executable path (normal dynamically-linked binary):
 *     1. Map the executable's segments via elf_map_elf_hdr(), which also
 *        records PT_PHDR metadata for the auxiliary vector.
 *     2. Load the dynamic linker via elf_map_dynamic(), which sets
 *        fmt->interp_load_addr to the interpreter's biased entry point.
 *        The kernel jumps to the interpreter instead; it will later jump to
 *        the executable's fmt->e_entry after relocation.
 *
 * Returns the executable's raw (unbiased) entry point on success, 0 on error.
 * Callers should use fmt->interp_load_addr as the actual jump target.
 */
unsigned elf_map(char *path, mos_binfmt *fmt)
{
	file *fp = fs_open_file(path, 0, "r", 1);
	unsigned entry_point = 0;
	unsigned head_len = 0;
	Elf32_Ehdr elf;
	char *interp = name_get();
	memset(interp, 0, MAX_PATH);

	if (fp == NULL) {
		name_put(interp);
		return 0;
	}
	/* ELF header is always at offset 0. */
	elf_read(fp, 0, &elf, sizeof(Elf32_Ehdr));
	entry_point = elf.e_entry;
	head_len = elf.e_ehsize;

	/* Validate ELF magic number (0x7f 'E' 'L' 'F'). */
	if (elf.e_ident[0] != 0x7f) {
		fs_put_file(fp);
		name_put(interp);
		return 0;
	}

	/* Only IA-32 (32-bit) ELF is supported. */
	if (elf.e_ident[4] != ELFCLASS32) {
		fs_put_file(fp);
		name_put(interp);
		return 0;
	}

	/* Must be an executable (ET_EXEC), not a shared object. */
	if (elf.e_type != ET_EXEC) {
		fs_put_file(fp);
		name_put(interp);
		return 0;
	}

	fmt->e_entry = elf.e_entry;

	if (elf_find_interp(fp, elf.e_phoff, elf.e_phentsize, elf.e_phnum,
			    interp)) {
		if (!strcmp(path, interp)) {
			/*
			 * The interpreter is being run directly — treat it as a
			 * plain static binary and jump straight to its entry.
			 */
			elf_map_programs(fp, elf.e_phoff, elf.e_phentsize,
					 elf.e_phnum);
			fmt->interp_load_addr = entry_point;
		} else {
			/*
			 * Normal dynamically-linked executable: map the binary
			 * and then load the dynamic linker at a separate bias.
			 * Control will be handed to the interpreter first.
			 */
			elf_map_elf_hdr(fp, elf.e_phoff, elf.e_phentsize,
					elf.e_phnum, fmt);
			elf_map_dynamic(interp, fmt);
			/* fmt->interp_load_addr is set by elf_map_dynamic(). */
		}
	} else {
		/* Static binary: no interpreter, jump directly to e_entry. */
		elf_map_programs(fp, elf.e_phoff, elf.e_phentsize, elf.e_phnum);
		fmt->interp_load_addr = entry_point;
	}

	fs_put_file(fp);
	name_put(interp);

	return entry_point;
}
