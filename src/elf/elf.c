#include <hw/time.h>
#include <elf/elf.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <macro.h>
#include <ext4.h>

/* Round x up to the nearest page boundary. */
#define PAGE_ALIGN_UP(x) (((x) + PAGE_SIZE - 1) & PAGE_SIZE_MASK)

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
 * elf_load_segment - map one PT_LOAD segment into the current address space.
 *
 * @bias is added to p_vaddr before mapping; pass 0 for ET_EXEC executables or
 * the load bias returned by vm_disc_map for shared objects.
 *
 * BSS handling (memsz > filesz):
 * --------------------------------
 * A naive file-backed mapping over the full memsz range is wrong for static
 * binaries: the page fault handler reads a full PAGE_SIZE from the file, but
 * bytes beyond p_filesz may contain section headers, debug info, etc., which
 * would silently corrupt the BSS.  The correct approach splits the segment:
 *
 *  Region 1  [va_begin, file_page_end)
 *    Whole pages fully covered by file data.  Mapped lazily file-backed;
 *    the page fault handler loads them on demand.
 *
 *  Region 2  [file_page_end, file_pages_end)  — the "boundary page"
 *    Exists only when filesz is not page-aligned.  The page contains file
 *    data in [file_page_end, elf_bss) and BSS in [elf_bss, file_pages_end).
 *    Mapped anonymous+writable; the file portion is read eagerly; the BSS
 *    tail is left zero from the anonymous zero-fill.
 *
 *  Region 3  [file_pages_end, mem_pages_end)
 *    Whole pages fully in the BSS.  Mapped anonymous; the page fault handler
 *    zero-fills them on demand.
 *
 * No-BSS fast path (filesz == memsz):
 * ------------------------------------
 * The entire segment is mapped lazily file-backed.  The page fault handler
 * already zeros any sub-page trailing bytes when the file read comes up short,
 * which is correct because there is no BSS to protect.
 *
 * Unaligned vaddr (uncommon):
 * ----------------------------
 * The whole page range is mapped anonymous and file data is copied eagerly.
 * BSS bytes are left zero by the anonymous page-fault zero-fill.
 */
static void elf_load_segment(file *fp, Elf32_Phdr *phdr, unsigned bias)
{
	unsigned file_off = phdr->p_offset;
	unsigned vaddr = phdr->p_vaddr + bias;
	unsigned filesz = phdr->p_filesz;
	unsigned memsz = phdr->p_memsz;
	int prot = elf_pflags_to_prot(phdr->p_flags);

	unsigned va_begin = vaddr & PAGE_SIZE_MASK;
	unsigned elf_bss = vaddr + filesz; /* first byte of BSS */
	unsigned last_bss = vaddr + memsz; /* one past last BSS byte */
	unsigned file_pages_end =
		PAGE_ALIGN_UP(elf_bss); /* page ceiling of file data */
	unsigned mem_pages_end =
		PAGE_ALIGN_UP(last_bss); /* page ceiling of memory image */

	if (memsz == 0)
		return;

	if (va_begin == vaddr) {
		if (filesz == memsz) {
			/*
			 * No BSS: map the entire segment lazily from the file.
			 * The page fault handler zeros any sub-page trailing
			 * bytes when the file read comes up short.
			 */
			do_mmap_kernel(va_begin, mem_pages_end - va_begin, prot,
				       MAP_FIXED, fp, file_off);
		} else {
			/*
			 * BSS present (memsz > filesz).
			 *
			 * file_page_end: floor(elf_bss) — start of the boundary
			 *   page (the page that straddles file data and BSS).
			 */
			unsigned file_page_end = elf_bss & PAGE_SIZE_MASK;

			/*
			 * Region 1: file-data pages INCLUDING the boundary page,
			 * all registered as lazy file-backed.
			 *
			 * Unlike the old approach (which stopped at file_page_end
			 * and mapped the boundary page anonymous), we extend the
			 * file-backed region to file_pages_end.  This matches
			 * Linux: /proc/maps shows one contiguous file-backed entry
			 * [va_begin, file_pages_end) instead of stopping one page
			 * short.
			 *
			 * The boundary page's BSS tail is corrected below by
			 * eagerly installing its PTE before writing, which avoids
			 * the kernel-mode page-fault problem (the fault handler
			 * calls sys_exit for kernel faults on unmapped user VAs).
			 */
			if (file_pages_end > va_begin)
				do_mmap_kernel(va_begin,
					       file_pages_end - va_begin, prot,
					       MAP_FIXED, fp, file_off);

			/*
			 * Boundary page — only when filesz is not page-aligned
			 * (elf_bss falls strictly inside the page).
			 *
			 * Install the PTE eagerly so that the user VA is
			 * accessible from kernel mode without triggering a fault.
			 * Then zero the whole page and overwrite the lower
			 * [file_page_end, elf_bss) bytes with the actual file
			 * content.  Result: file data in [file_page_end, elf_bss),
			 * zeros in [elf_bss, file_pages_end) — exactly correct.
			 *
			 * The vm_region stays file-backed (registered above), so
			 * /proc/maps is accurate.  Because the PTE is present the
			 * page-fault handler will never reload this page from file,
			 * so the zeroed BSS tail is permanent.
			 */
			if (elf_bss > file_page_end) {
				unsigned page_file_off =
					file_off + (file_page_end - va_begin);
				unsigned bytes = elf_bss - file_page_end;

				mm_map_page(file_page_end, 0,
					    PAGE_ENTRY_USER_DATA);
				INVLPG(file_page_end);
				memset((void *)file_page_end, 0, PAGE_SIZE);
				elf_read(fp, page_file_off,
					 (void *)file_page_end, bytes);

				/* Restore read-only protection if needed. */
				if (!(prot & PROT_WRITE)) {
					mm_set_map_flag(file_page_end,
							PAGE_ENTRY_USER_CODE);
					INVLPG(file_page_end);
				}
			}

			/*
			 * Region 2: pure BSS pages beyond the boundary page,
			 * anonymous zero-filled on demand.
			 *
			 * PROT_EXEC is added unconditionally: on Linux 2.4 x86
			 * there is no NX bit, so any readable page is implicitly
			 * executable.  The kernel always set VM_EXEC on anonymous
			 * writable regions, which is why /proc/maps shows "rwxp"
			 * for BSS/heap on RH9.  Matching this prot lets the merge
			 * logic coalesce the BSS region with the heap (brk pages)
			 * into a single "rwxp" entry.
			 */
			if (mem_pages_end > file_pages_end)
				do_mmap_kernel(file_pages_end,
					       mem_pages_end - file_pages_end,
					       prot | PROT_WRITE | PROT_EXEC,
					       MAP_FIXED, 0, 0);
		}
	} else {
		/*
		 * vaddr is not page-aligned (uncommon — only produced by some
		 * older or hand-crafted linker scripts).  Fall back to a single
		 * anonymous mapping over the full page range and copy the file
		 * data eagerly.  BSS bytes (vaddr+filesz .. last_bss) are left
		 * zero by the anonymous page-fault zero-fill.
		 */
		unsigned map_size = mem_pages_end - va_begin;
		unsigned mapped = do_mmap_kernel(
			va_begin, map_size, prot | PROT_WRITE, MAP_FIXED, 0, 0);
		if (filesz > 0)
			elf_read(fp, file_off, (void *)vaddr, filesz);
		do_mmap_update(mapped, prot, 0);
	}
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
	int i;
	for (i = 0; i < num; i++) {
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
		elf_read(fp, head_offset, &phdr, sizeof(phdr));
		if (phdr.p_type == PT_INTERP)
			return elf_read_interp(fp, &phdr, path);
	}
	return 0;
}

/*
 * elf_map_programs - map all PT_LOAD segments of a static/plain executable.
 *
 * Each PT_LOAD segment is loaded via elf_load_segment(), which correctly
 * handles the file/BSS split.  fmt->start_brk is set to the page-aligned
 * ceiling of the highest BSS address so the process's initial brk is just
 * past the BSS.
 */
static unsigned elf_map_programs(file *fp, unsigned table_offset, unsigned size,
				 unsigned num, mos_binfmt *fmt)
{
	int i;
	unsigned last_bss = 0;

	for (i = 0; i < num; i++) {
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
		elf_read(fp, head_offset, &phdr, sizeof(phdr));
		if (phdr.p_type != PT_LOAD)
			continue;

		elf_load_segment(fp, &phdr, 0);

		unsigned k = phdr.p_vaddr + phdr.p_memsz;
		if (k > last_bss)
			last_bss = k;
	}

	if (fmt)
		fmt->start_brk = PAGE_ALIGN_UP(last_bss);

	return 1;
}

/*
 * elf_map_elf_hdr - map segments and record auxiliary info for the dynamic case.
 *
 * When the executable has a dynamic linker (PT_INTERP), the kernel must pass
 * certain ELF metadata to the dynamic linker via the auxiliary vector.  This
 * function:
 *   - Records the program-header count (e_phnum), the within-page offset of
 *     the phdr table (e_phoff), and the load address of the first page
 *     (elf_load_addr) into @fmt — the dynamic linker reads these via AT_PHDR,
 *     AT_PHNUM, etc.
 *   - Maps every PT_LOAD segment via elf_load_segment().
 *
 * PT_PHDR, when present, describes where the program header table itself is
 * mapped, which is how we derive the base load address.
 */
static unsigned elf_map_elf_hdr(file *fp, unsigned table_offset, unsigned size,
				unsigned num, mos_binfmt *fmt)
{
	int i;
	unsigned last_bss = 0;

	for (i = 0; i < num; i++) {
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
		elf_read(fp, head_offset, &phdr, sizeof(phdr));

		if (phdr.p_type == PT_PHDR) {
			/* Save phdr metadata for the auxiliary vector. */
			fmt->e_phnum = num;
			fmt->e_phoff = phdr.p_vaddr & ~PAGE_SIZE_MASK;
			fmt->elf_load_addr = phdr.p_vaddr & PAGE_SIZE_MASK;
		} else if (phdr.p_type == PT_LOAD) {
			elf_load_segment(fp, &phdr, 0);

			unsigned k = phdr.p_vaddr + phdr.p_memsz;
			if (k > last_bss)
				last_bss = k;
		}
	}

	fmt->start_brk = PAGE_ALIGN_UP(last_bss);
	return 1;
}

/*
 * elf_map_get_dynamic_pages - calculate the total page span of a shared object.
 *
 * Scans all PT_LOAD segments and returns the number of pages spanned from the
 * lowest to the highest virtual address.  This is used to reserve a
 * contiguous virtual-address region via vm_disc_map before mapping the
 * interpreter, so that all its segments land in a single coherent window.
 */
static unsigned elf_map_get_dynamic_pages(file *fp, unsigned table_offset,
					  unsigned size, unsigned num)
{
	unsigned va_page_start = 0xffffffff;
	unsigned va_page_end = 0;
	int i;

	for (i = 0; i < num; i++) {
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
		elf_read(fp, head_offset, &phdr, sizeof(phdr));
		if (phdr.p_type != PT_LOAD)
			continue;

		unsigned va_begin = phdr.p_vaddr & PAGE_SIZE_MASK;
		unsigned va_end = PAGE_ALIGN_UP(phdr.p_vaddr + phdr.p_memsz);

		if (va_begin < va_page_start)
			va_page_start = va_begin;
		if (va_end > va_page_end)
			va_page_end = va_end;
	}

	return (va_page_end - va_page_start) / PAGE_SIZE;
}

/*
 * elf_map_programs_at - map all PT_LOAD segments of a shared object at @bias.
 *
 * Works like elf_map_programs() but applies a load-address @bias so the
 * shared object can be placed at an arbitrary virtual address chosen by
 * vm_disc_map.  Used when loading the dynamic linker (interpreter).
 */
static unsigned elf_map_programs_at(file *fp, unsigned table_offset,
				    unsigned size, unsigned num, unsigned bias)
{
	int i;
	for (i = 0; i < num; i++) {
		unsigned head_offset = table_offset + i * size;
		Elf32_Phdr phdr;
		elf_read(fp, head_offset, &phdr, sizeof(phdr));
		if (phdr.p_type == PT_LOAD)
			elf_load_segment(fp, &phdr, bias);
	}
	return 1;
}

/*
 * elf_map_dynamic - load the dynamic linker (interpreter) into user space.
 *
 * Opens the ELF shared object at @path (typically "/lib/ld-linux.so.2"),
 * validates its header (must be a 32-bit ET_DYN), then:
 *   1. Computes the total page span of its PT_LOAD segments.
 *   2. Reserves a contiguous virtual-address window via vm_disc_map
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
	file *fp = fs_open_file(path, 0, 0, 1);
	Elf32_Ehdr elf;
	task_struct *cur = CURRENT_TASK();
	unsigned pages = 0;

	if (fp == NULL)
		return 0;

	/* ELF header is always at offset 0. */
	elf_read(fp, 0, &elf, sizeof(Elf32_Ehdr));

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
	pages = elf_map_get_dynamic_pages(fp, elf.e_phoff, elf.e_phentsize,
					  elf.e_phnum);
	fmt->interp_bias = vm_disc_map(cur->user->vm, pages * PAGE_SIZE);
	fmt->interp_load_addr = fmt->interp_bias + elf.e_entry;
	elf_map_programs_at(fp, elf.e_phoff, elf.e_phentsize, elf.e_phnum,
			    fmt->interp_bias);

	fs_put_file(fp);
	return 1;
}

/*
 * elf_map - top-level ELF loader: map an executable into the current process.
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
	file *fp = fs_open_file(path, 0, 0, 1);
	unsigned entry_point = 0;
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
					 elf.e_phnum, fmt);
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
		elf_map_programs(fp, elf.e_phoff, elf.e_phentsize, elf.e_phnum,
				 fmt);
		fmt->interp_load_addr = entry_point;
	}

	fs_put_file(fp);
	name_put(interp);

	return entry_point;
}
