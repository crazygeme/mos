#include <test/test.h>
#include <elf/elf.h>
#include <elf/format.h>
#include <fs/fs.h>
#include <errno.h>

struct elf_fixture {
	file fp;
	unsigned size;
	unsigned reads;
	union {
		struct {
			Elf32_Ehdr header;
			Elf32_Phdr segments[3];
		} elf32;
		struct {
			Elf64_Ehdr header;
			Elf64_Phdr segments[3];
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
	fixture->reads++;
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
		Elf32_Phdr *ph = &fixture.bytes.elf32.segments[0];
		fixture.size = sizeof(fixture.bytes.elf32);
		h->e_type = ET_EXEC;
		h->e_machine = EM_386;
		h->e_version = EV_CURRENT;
		h->e_entry = 0x08048000;
		h->e_phoff = sizeof(*h);
		h->e_ehsize = sizeof(*h);
		h->e_phentsize = sizeof(*ph);
		h->e_phnum = 3;
		ph->p_type = PT_LOAD;
		ph->p_vaddr = h->e_entry;
		ph->p_filesz = ph->p_memsz = fixture.size;
		ph->p_flags = PF_R | PF_X;
		ph->p_align = 0x1000;
	} else {
		Elf64_Ehdr *h = &fixture.bytes.elf64.header;
		Elf64_Phdr *ph = &fixture.bytes.elf64.segments[0];
		fixture.size = sizeof(fixture.bytes.elf64);
		h->e_type = ET_EXEC;
		h->e_machine = EM_X86_64;
		h->e_version = EV_CURRENT;
		h->e_entry = 0x00400000;
		h->e_phoff = sizeof(*h);
		h->e_ehsize = sizeof(*h);
		h->e_phentsize = sizeof(*ph);
		h->e_phnum = 3;
		ph->p_type = PT_LOAD;
		ph->p_vaddr = h->e_entry;
		ph->p_filesz = ph->p_memsz = fixture.size;
		ph->p_flags = PF_R | PF_X;
		ph->p_align = 0x1000;
	}

	ret = elf_prepare(&fixture.fp, &image);
	EXPECT_EQ(image != NULL, ret == 0);
	if (ret == 0)
		EXPECT_EQ(fixture.reads, 2);
	elf_release(image);
	EXPECT_EQ(fixture.fp.f_count, 1);
	if (ret == 0) {
		struct stat st;
		fixture_getattr(&fixture.fp, &st);
		fixture.reads = 0;
		EXPECT_EQ(elf_prepare_header(&fixture.fp, &image, &fixture.bytes,
					    sizeof(Elf64_Ehdr), &st), 0);
		EXPECT_EQ(fixture.reads, 1);
		elf_release(image);
		EXPECT_EQ(fixture.fp.f_count, 1);
	}
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

static int phdr_table_fixture(unsigned elf_class)
{
	struct elf_fixture fixture = { 0 };
	const struct elf_format *format = arch_elf_format(elf_class);
	Elf64_Phdr expected[3] = { 0 }, output[3];
	unsigned offset, i;

	if (!format)
		return 0;
	fixture.fp.f_fop = &fixture_ops;
	offset = elf_class == ELFCLASS32 ? sizeof(Elf32_Ehdr) :
					   sizeof(Elf64_Ehdr);
	fixture.size = elf_class == ELFCLASS32 ? sizeof(fixture.bytes.elf32) :
						 sizeof(fixture.bytes.elf64);
	for (i = 0; i < 3; i++) {
		Elf64_Phdr *ph = &expected[i];

		ph->p_type = PT_LOAD + i;
		ph->p_flags = PF_R | (i == 1 ? PF_W : PF_X);
		ph->p_offset = 0x12345678U + i;
		ph->p_vaddr = (elf_class == ELFCLASS32 ?
				       0x87654321ULL :
				       0x1234567887654321ULL) +
			      i;
		ph->p_paddr = ph->p_vaddr + 0x100;
		ph->p_filesz = 0x333 + i;
		ph->p_memsz = ph->p_filesz + 0x200;
		ph->p_align = 0x1000;
		if (elf_class == ELFCLASS32) {
			Elf32_Phdr *wire = &fixture.bytes.elf32.segments[i];
#define COPY(field) wire->field = ph->field
			COPY(p_type);
			COPY(p_flags);
			COPY(p_offset);
			COPY(p_vaddr);
			COPY(p_paddr);
			COPY(p_filesz);
			COPY(p_memsz);
			COPY(p_align);
#undef COPY
		} else {
			fixture.bytes.elf64.segments[i] = *ph;
		}
	}
	EXPECT_EQ(format->read_phdrs(&fixture.fp, offset, 3, output), 0);
	EXPECT_EQ(fixture.reads, 1);
	EXPECT_EQ(memcmp(output, expected, sizeof(expected)), 0);
	fixture.size--;
	fixture.reads = 0;
	EXPECT_EQ(format->read_phdrs(&fixture.fp, offset, 3, output), -ENOEXEC);
	EXPECT_EQ(fixture.reads, 1);
	return 0;
}

KTEST(ElfTest, Elf32ProgramHeaders)
{
	return phdr_table_fixture(ELFCLASS32);
}

KTEST(ElfTest, Elf64ProgramHeaders)
{
	return phdr_table_fixture(ELFCLASS64);
}
