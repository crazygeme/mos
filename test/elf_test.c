#include <test/test.h>
#include <elf/elf.h>
#include <elf/format.h>
#include <fs/fs.h>
#include <errno.h>

struct elf_fixture {
	file fp;
	unsigned size;
	union {
		struct {
			Elf32_Ehdr header;
			Elf32_Phdr segment;
		} elf32;
		struct {
			Elf64_Ehdr header;
			Elf64_Phdr segment;
		} elf64;
	} bytes;
};

static int fixture_getattr(file *fp, struct stat *st)
{
	struct elf_fixture *fixture = (void *)fp;
	memset(st, 0, sizeof(*st));
	st->st_size = fixture->size;
	return 0;
}

static ssize_t fixture_read(file *fp, void *buf, size_t size, loff_t *pos)
{
	struct elf_fixture *fixture = (void *)fp;
	if (*pos < 0 || *pos >= fixture->size)
		return 0;
	if (size > fixture->size - *pos)
		size = fixture->size - *pos;
	memcpy(buf, (char *)&fixture->bytes + *pos, size);
	*pos += size;
	return size;
}

static const file_operations fixture_ops = {
	.getattr = fixture_getattr,
	.read = fixture_read,
};

static int prepare_fixture(int elf_class)
{
	struct elf_fixture fixture = { 0 };
	elf_image *image = NULL;
	unsigned char *ident = (void *)&fixture.bytes;
	int ret;

	fixture.fp.f_count = 1;
	fixture.fp.f_fop = &fixture_ops;
	memcpy(ident, "\177ELF", 4);
	ident[EI_CLASS] = elf_class;
	ident[EI_DATA] = ELFDATA2LSB;
	ident[EI_VERSION] = EV_CURRENT;
	if (elf_class == ELFCLASS32) {
		Elf32_Ehdr *h = &fixture.bytes.elf32.header;
		Elf32_Phdr *ph = &fixture.bytes.elf32.segment;
		fixture.size = sizeof(fixture.bytes.elf32);
		h->e_type = ET_EXEC;
		h->e_machine = EM_386;
		h->e_version = EV_CURRENT;
		h->e_entry = 0x08048000;
		h->e_phoff = sizeof(*h);
		h->e_ehsize = sizeof(*h);
		h->e_phentsize = sizeof(*ph);
		h->e_phnum = 1;
		ph->p_type = PT_LOAD;
		ph->p_vaddr = h->e_entry;
		ph->p_filesz = ph->p_memsz = fixture.size;
		ph->p_flags = PF_R | PF_X;
		ph->p_align = 0x1000;
	} else {
		Elf64_Ehdr *h = &fixture.bytes.elf64.header;
		Elf64_Phdr *ph = &fixture.bytes.elf64.segment;
		fixture.size = sizeof(fixture.bytes.elf64);
		h->e_type = ET_EXEC;
		h->e_machine = EM_X86_64;
		h->e_version = EV_CURRENT;
		h->e_entry = 0x00400000;
		h->e_phoff = sizeof(*h);
		h->e_ehsize = sizeof(*h);
		h->e_phentsize = sizeof(*ph);
		h->e_phnum = 1;
		ph->p_type = PT_LOAD;
		ph->p_vaddr = h->e_entry;
		ph->p_filesz = ph->p_memsz = fixture.size;
		ph->p_flags = PF_R | PF_X;
		ph->p_align = 0x1000;
	}

	ret = elf_prepare(&fixture.fp, &image);
	EXPECT_EQ(image != NULL, ret == 0);
	elf_release(image);
	EXPECT_EQ(fixture.fp.f_count, 1);
	return ret;
}

KTEST(ElfTest, Elf32Image)
{
	EXPECT_EQ(prepare_fixture(ELFCLASS32), 0);
	return 0;
}

KTEST(ElfTest, Elf64Image)
{
	EXPECT_EQ(prepare_fixture(ELFCLASS64),
		  arch_elf_format(ELFCLASS64) ? 0 : -ENOEXEC);
	return 0;
}
