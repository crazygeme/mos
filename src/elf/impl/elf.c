#include <device/time.h>
#include <elf/elf.h>
#include <elf/format.h>
#include <mm/mm.h>
#include <mm/mmap.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <macro.h>
#include <mm/mmu.h>
#include <errno.h>
#include <fs/cache.h>
#include <fs/inotify.h>

/* Main PIE images occupy the executable area below the mmap/heap limit. */
#define ELF_PIE_BIAS 0x08000000U

/* Round x up to the nearest page boundary. */
#define PAGE_ALIGN_UP(x) (((x) + PAGE_SIZE - 1) & PAGE_SIZE_MASK)

/* Read file bytes without changing the shared file position. */
int elf_read(file *fp, unsigned off, void *buf, int len)
{
	loff_t pos = off;
	int count;
	if (!fp || !fp->f_fop || !fp->f_fop->read)
		return -ENOEXEC;
	/* Internal ELF reads use cached pages without per-read atime updates. */
	if (fp->f_inode && fp->f_inode->i_pgcache_tag && fp->f_fop->read_page)
		count = fs_page_cache_read(fp, buf, len, &pos);
	else
		count = fp->f_fop->read(fp, buf, len, &pos);
	if (count > 0)
		inotify_file_event(fp, IN_ACCESS);
	return count;
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
static void elf_load_segment(file *fp, Elf64_Phdr *phdr, vaddr_t bias)
{
	unsigned file_off = phdr->p_offset;
	vaddr_t vaddr = phdr->p_vaddr + bias;
	unsigned filesz = phdr->p_filesz;
	size_t memsz = phdr->p_memsz;
	int prot = elf_pflags_to_prot(phdr->p_flags);

	vaddr_t va_begin = vaddr & PAGE_SIZE_MASK;
	vaddr_t elf_bss = vaddr + filesz; /* first byte of BSS */
	vaddr_t last_bss = vaddr + memsz; /* one past last BSS byte */
	vaddr_t file_pages_end =
		PAGE_ALIGN_UP(elf_bss); /* page ceiling of file data */
	vaddr_t mem_pages_end =
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
			vaddr_t file_page_end = elf_bss & PAGE_SIZE_MASK;

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
				arch_mm_invalidate(file_page_end);
				memset((void *)file_page_end, 0, PAGE_SIZE);
				elf_read(fp, page_file_off,
					 (void *)file_page_end, bytes);

				/* Restore read-only protection if needed. */
				if (!(prot & PROT_WRITE)) {
					mm_set_map_flag(file_page_end,
							PAGE_ENTRY_USER_CODE);
					arch_mm_invalidate(file_page_end);
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
		 * data eagerly.
		 *
		 * The aligned file offset must match the aligned virtual address:
		 * bytes in the leading partial page [va_begin, vaddr) still come
		 * from the file page that contains p_offset, even though they sit
		 * below the segment's nominal start address.  Some dynamic
		 * linkers dereference through page-aligned views of this segment,
		 * so dropping that leading file fragment breaks small ET_EXEC
		 * binaries whose RW PT_LOAD begins mid-page.
		 *
		 * BSS bytes beyond the copied file image remain zero because the
		 * backing mapping is anonymous zero-fill.
		 */
		size_t map_size = mem_pages_end - va_begin;
		unsigned file_begin = file_off & PAGE_SIZE_MASK;
		unsigned copy_size = filesz + (vaddr - va_begin);
		vaddr_t mapped;
		vaddr_t page;

		mapped = do_mmap_kernel(va_begin, map_size, prot | PROT_WRITE,
					MAP_FIXED, 0, 0);
		for (page = va_begin; page < va_begin + map_size;
		     page += PAGE_SIZE) {
			mm_map_page(page, 0, PAGE_ENTRY_USER_DATA);
			arch_mm_invalidate(page);
			memset((void *)page, 0, PAGE_SIZE);
		}
		if (copy_size > 0)
			elf_read(fp, file_begin, (void *)va_begin, copy_size);
		do_mmap_update(mapped, prot, 0);
	}
}

/* Metadata and file references are private to one exec operation. */
struct elf_image {
	file *fp;
	const struct elf_format *format;
	Elf64_Ehdr header;
	Elf64_Phdr *phdrs;
	vaddr_t span;
	struct elf_image *interpreter;
};

/* Validate the complete load description before replacing the process image. */
static int elf_validate(elf_image *image, char *interp, const void *header,
			unsigned length, const struct stat *known_stat)
{
	file *fp = image->fp;
	Elf64_Ehdr *elf = &image->header;
	struct stat st;
	unsigned i, table_end;
	int entry_found = 0, headers_mapped = 0;

	Elf64_Ehdr wire;
	const unsigned char *ident;
	if (known_stat) {
		st = *known_stat;
	} else {
		if (!fp || !fp->f_fop || !fp->f_fop->getattr ||
		    fp->f_fop->getattr(fp, &st) != 0)
			return -ENOENT;
		inotify_file_open(fp, current->fs->root);
	}
	if (!header) {
		int n = elf_read(fp, 0, &wire, sizeof(wire));
		if (n < 0 || n > sizeof(wire))
			return -ENOEXEC;
		header = &wire;
		length = n;
	}
	ident = header;
	if (length < EI_NIDENT || memcmp(ident, "\177ELF", 4) ||
	    ident[EI_DATA] != ELFDATA2LSB || ident[EI_VERSION] != EV_CURRENT)
		return -ENOEXEC;
	image->format = arch_elf_format(ident[EI_CLASS]);
	if (!image->format || image->format->decode_header(header, length, elf))
		return -ENOEXEC;
	if (elf->e_version != EV_CURRENT || !elf->e_phnum ||
	    (elf->e_type != ET_EXEC && elf->e_type != ET_DYN) ||
	    (elf->e_type == ET_EXEC && !elf->e_entry) ||
	    elf->e_phoff > 0x7fffffff)
		return -ENOEXEC;
	table_end = elf->e_phoff + elf->e_phnum * elf->e_phentsize;
	if (table_end < elf->e_phoff || table_end > st.st_size)
		return -ENOEXEC;
	image->phdrs = kmalloc(elf->e_phnum * sizeof(Elf64_Phdr));
	if (!image->phdrs)
		return -ENOMEM;
	if (image->format->read_phdrs(fp, elf->e_phoff, elf->e_phnum,
				      image->phdrs))
		return -ENOEXEC;
	vaddr_t limit = image->format->task_size;
	limit -= USER_STACK_PAGES * PAGE_SIZE;
	interp[0] = 0;
	for (i = 0; i < elf->e_phnum; i++) {
		Elf64_Phdr ph = image->phdrs[i];
		if (ph.p_type != PT_LOAD && ph.p_type != PT_INTERP)
			continue;
		if (ph.p_offset > st.st_size ||
		    ph.p_filesz > st.st_size - ph.p_offset)
			return -ENOEXEC;
		if (ph.p_type == PT_INTERP) {
			if (interp[0] || ph.p_filesz < 2 ||
			    ph.p_filesz > MAX_PATH ||
			    elf_read(fp, ph.p_offset, interp, ph.p_filesz) !=
				    ph.p_filesz ||
			    interp[0] != '/' || interp[ph.p_filesz - 1] != 0)
				return -ENOEXEC;
			continue;
		}
		if (!arch_mm_user_range_valid(ph.p_vaddr, ph.p_memsz))
			return -ENOEXEC;
		if (ph.p_filesz > ph.p_memsz || ph.p_vaddr >= limit ||
		    ph.p_memsz > limit - ph.p_vaddr ||
		    (ph.p_vaddr & (PAGE_SIZE - 1)) !=
			    (ph.p_offset & (PAGE_SIZE - 1)) ||
		    (ph.p_align > 1 &&
		     ((ph.p_align & (ph.p_align - 1)) ||
		      (ph.p_vaddr - ph.p_offset) % ph.p_align)))
			return -ENOEXEC;
		if (PAGE_ALIGN_UP(ph.p_vaddr + ph.p_memsz) > image->span)
			image->span = PAGE_ALIGN_UP(ph.p_vaddr + ph.p_memsz);
		if (elf->e_phoff >= ph.p_offset &&
		    table_end - ph.p_offset <= ph.p_filesz)
			headers_mapped = 1;
		if ((ph.p_flags & PF_X) && elf->e_entry >= ph.p_vaddr &&
		    elf->e_entry - ph.p_vaddr < ph.p_memsz)
			entry_found = 1;
	}
	if ((interp[0] || elf->e_type == ET_DYN) && !headers_mapped)
		return -ENOEXEC;
	return entry_found ? 0 : -ENOEXEC;
}

void elf_release(elf_image *image)
{
	if (!image)
		return;
	elf_release(image->interpreter);
	if (image->fp)
		fs_put_file(image->fp);
	kfree(image->phdrs);
	kfree(image);
}

/* Retain each file and read its load description before replacing memory. */
int elf_prepare_header(file *fp, elf_image **result, const void *header,
		       unsigned length, const struct stat *known_stat)
{
	elf_image *image;
	char *interp;
	int ret;
	unsigned i;

	*result = NULL;
	if (!fp)
		return -ENOENT;
	image = kmalloc(sizeof(*image));
	if (!image)
		return -ENOMEM;
	memset(image, 0, sizeof(*image));
	image->fp = fp;
	fs_get_file(fp);
	interp = name_get();
	if (!interp) {
		elf_release(image);
		return -ENOMEM;
	}
	ret = elf_validate(image, interp, header, length, known_stat);
	if (ret)
		goto done;
	if (image->header.e_type == ET_DYN) {
		for (i = 0; i < image->header.e_phnum; i++) {
			Elf64_Phdr *ph = &image->phdrs[i];
			if (ph->p_type == PT_LOAD &&
			    (ph->p_align > ELF_PIE_BIAS ||
			     ph->p_vaddr >= USER_HEAP_END - ELF_PIE_BIAS ||
			     ph->p_memsz >= USER_HEAP_END - ELF_PIE_BIAS -
						    ph->p_vaddr)) {
				ret = -ENOEXEC;
				goto done;
			}
		}
	}
	if (interp[0]) {
		elf_image *ld = kmalloc(sizeof(*ld));
		if (!ld) {
			ret = -ENOMEM;
			goto done;
		}
		memset(ld, 0, sizeof(*ld));
		image->interpreter = ld;
		ld->fp = fs_open_file(interp, 0, 0);
		ret = elf_validate(ld, interp, NULL, 0, NULL);
		if (!ret && (ld->header.e_type != ET_DYN ||
			     ld->format != image->format || interp[0]))
			ret = -ENOEXEC;
	}
done:
	name_put(interp);
	if (ret)
		elf_release(image);
	else
		*result = image;
	return ret;
}

int elf_prepare(file *fp, elf_image **result)
{
	return elf_prepare_header(fp, result, NULL, 0, NULL);
}

/* Map verified segments; only partial-page contents require eager reads. */
vaddr_t elf_map_prepared(elf_image *image, mos_binfmt *fmt)
{
	Elf64_Ehdr *elf = &image->header;
	vaddr_t bias = elf->e_type == ET_DYN ? ELF_PIE_BIAS : 0;
	unsigned table_size = elf->e_phnum * elf->e_phentsize;
	unsigned i;

	memset(fmt, 0, sizeof(*fmt));
	fmt->e_entry = bias + elf->e_entry;
	fmt->e_phnum = elf->e_phnum;
	fmt->e_phoff = 0;
	fmt->e_phent = elf->e_phentsize;
	fmt->start_brk = bias + image->span;
	for (i = 0; i < elf->e_phnum; i++) {
		Elf64_Phdr *ph = &image->phdrs[i];
		if (ph->p_type != PT_LOAD)
			continue;
		elf_load_segment(image->fp, ph, bias);
		/* AT_PHDR is derived from the segment containing the table. */
		if (elf->e_phoff >= ph->p_offset &&
		    elf->e_phoff - ph->p_offset <= ph->p_filesz &&
		    table_size <= ph->p_filesz - (elf->e_phoff - ph->p_offset))
			fmt->elf_load_addr = bias + ph->p_vaddr + elf->e_phoff -
					     ph->p_offset;
	}
	if (image->interpreter) {
		elf_image *ld = image->interpreter;
		vaddr_t base = vm_disc_map(CURRENT_TASK()->memory, ld->span);
		if (!base || base >= CURRENT_TASK()->memory->task_size ||
		    ld->span > CURRENT_TASK()->memory->task_size - base)
			return 0;
		fmt->interp_bias = base;
		for (i = 0; i < ld->header.e_phnum; i++) {
			Elf64_Phdr *ph = &ld->phdrs[i];
			if (ph->p_type == PT_LOAD)
				elf_load_segment(ld->fp, ph, base);
		}
		fmt->interp_load_addr = base + ld->header.e_entry;
	} else {
		fmt->interp_load_addr = fmt->e_entry;
	}
	return fmt->e_entry;
}

int elf_check_file(file *fp)
{
	elf_image *image;
	int ret = elf_prepare(fp, &image);
	elf_release(image);
	return ret;
}

vaddr_t elf_map_file(char *path, mos_binfmt *fmt, file *fp)
{
	elf_image *image;
	vaddr_t entry = 0;
	file *opened = fp ? fp : fs_open_file(path, 0, 0);

	memset(fmt, 0, sizeof(*fmt));
	if (!elf_prepare(opened, &image)) {
		entry = elf_map_prepared(image, fmt);
		elf_release(image);
	}
	if (!fp && opened)
		fs_put_file(opened);
	return entry;
}

vaddr_t elf_map(char *path, mos_binfmt *fmt)
{
	return elf_map_file(path, fmt, NULL);
}

const struct elf_format *elf_image_format(const elf_image *image)
{
	return image->format;
}
