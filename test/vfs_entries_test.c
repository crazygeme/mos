#include <test/test.h>
#include <fs/entries.h>
#include <fs/fcntl.h>
#include <errno.h>

static int check_mounts_and_aliases(char *buf, size_t capacity)
{
	vfs_entry_tree *tree = vfs_entry_tree_create();
	vfs_entry_node *root, *device, *class;
	super_block *sb, *host;
	file *fp;
	size_t count = 0;
	loff_t pos = 0;
	ASSERT_NONNULL(tree);
	root = vfs_entry_root(tree);
	device = vfs_entry_directory(root, "device");
	class = vfs_entry_directory(root, "class");
	ASSERT_NONNULL(device);
	ASSERT_NONNULL(class);
	ASSERT_NONNULL(vfs_entry_text(device, "vendor", "0x1234\n"));
	ASSERT_NONNULL(vfs_entry_link(device, "subsystem", class));
	ASSERT_NONNULL(vfs_entry_link(class, "card0", device));
	ASSERT_EQ(vfs_entry_tree_error(tree), 0);
	sb = vfs_entry_tree_super(tree);
	host = sget(NULL);
	ASSERT_NONNULL(host);
	ASSERT_EQ(vfs_mount(host, "/alternate", sb), 0);
	fp = vfs_open(host, "/alternate/class/card0/vendor", O_RDONLY);
	ASSERT_NONNULL(fp);
	EXPECT_EQ(fp->f_fop->read(fp, buf, capacity, &pos), 7);
	EXPECT_EQ(memcmp(buf, "0x1234\n", 7), 0);
	fs_put_file(fp);
	EXPECT_NULL(vfs_open(host, "/alternate/missing", O_RDONLY));
	EXPECT_NULL(vfs_open(host, "/alternate/device/vendor/child", O_RDONLY));
	EXPECT_NULL(vfs_open(host, "/alternate/class/card0/missing", O_RDONLY));
	fp = vfs_open(host, "/alternate/class/card0", O_PATH | O_NOFOLLOW);
	ASSERT_NONNULL(fp);
	EXPECT_TRUE(S_ISLNK(fp->f_inode->i_mode));
	fs_put_file(fp);
	EXPECT_EQ(vfs_readlink(host, "/alternate/class/card0", buf, capacity,
			       &count),
		  0);
	EXPECT_EQ(count, strlen("../device"));
	EXPECT_EQ(memcmp(buf, "../device", count), 0);
	EXPECT_EQ(vfs_readlink(host, "/alternate/class/card0/subsystem", buf,
			       capacity, &count),
		  0);
	EXPECT_EQ(count, strlen("../class"));
	EXPECT_EQ(memcmp(buf, "../class", count), 0);
	EXPECT_EQ(vfs_readlink(host, "/alternate/class/card0/vendor", buf,
			       capacity, &count),
		  -EINVAL);
	EXPECT_EQ(vfs_readlink(host, "/alternate/class/card0/absent", buf,
			       capacity, &count),
		  -ENOENT);
	fp = vfs_open(host, "/alternate/class", O_RDONLY);
	ASSERT_NONNULL(fp);
	pos = 0;
	{
		ssize_t length = fp->f_fop->read(fp, buf, capacity, &pos);
		unsigned offset = 0, found = 0;
		EXPECT_GT(length, 0);
		while (length > 0 && offset < (unsigned)length) {
			struct linux_dirent *entry = (void *)(buf + offset);
			if (!strcmp(entry->d_name, "card0"))
				found++;
			ASSERT_GT(entry->d_reclen, 0);
			offset += entry->d_reclen;
		}
		EXPECT_EQ(found, 1);
	}
	fs_put_file(fp);
	sb_put(host);
	return 0;
}

KTEST(VfsEntriesTest, MountsAndAliases)
{
	char *buf = zalloc(256);
	int result;
	ASSERT_NONNULL(buf);
	/* The helper may return early through a fatal test assertion. */
	result = check_mounts_and_aliases(buf, 256);
	free(buf);
	return result;
}

KTEST(VfsEntriesTest, OpenFileSurvivesUnmount)
{
	vfs_entry_tree *a = vfs_entry_tree_create();
	vfs_entry_tree *b = vfs_entry_tree_create();
	file *fp;
	loff_t pos = 0;
	char buf[8];
	ASSERT_NONNULL(a);
	ASSERT_NONNULL(b);
	ASSERT_NONNULL(vfs_entry_text(vfs_entry_root(a), "value", "first"));
	ASSERT_NONNULL(vfs_entry_text(vfs_entry_root(b), "value", "second"));
	fp = vfs_open(vfs_entry_tree_super(a), "/value", O_RDONLY);
	ASSERT_NONNULL(fp);
	sb_put(vfs_entry_tree_super(a));
	EXPECT_EQ(fp->f_fop->read(fp, buf, sizeof(buf), &pos), 5);
	EXPECT_EQ(memcmp(buf, "first", 5), 0);
	fs_put_file(fp);
	fp = vfs_open(vfs_entry_tree_super(b), "/value", O_RDONLY);
	ASSERT_NONNULL(fp);
	pos = 0;
	EXPECT_EQ(fp->f_fop->read(fp, buf, sizeof(buf), &pos), 6);
	EXPECT_EQ(memcmp(buf, "second", 6), 0);
	fs_put_file(fp);
	sb_put(vfs_entry_tree_super(b));
	return 0;
}

KTEST(VfsEntriesTest, DevicePublicationAndMountViews)
{
	vfs_entry_tree *tree = vfs_entry_tree_create();
	vfs_entry_node *device;
	super_block *registry, *first, *second;
	file *fp;
	char buf[8];
	loff_t pos = 0;
	ASSERT_NONNULL(tree);
	registry = vfs_entry_tree_super(tree);
	device = vfs_entry_directory_create(vfs_entry_root(tree), "device");
	ASSERT_NONNULL(device);
	ASSERT_NONNULL(vfs_entry_text(device, "vendor", "1234"));
	EXPECT_NULL(vfs_open(registry, "/device/vendor", O_RDONLY));
	ASSERT_EQ(vfs_mount(registry, "/device", vfs_entry_super(device)), 0);
	first = vfs_entry_tree_mount(tree);
	second = vfs_entry_tree_mount(tree);
	ASSERT_NONNULL(first);
	ASSERT_NONNULL(second);
	EXPECT_NE(first, second);
	sb_put(first);
	fp = vfs_open(second, "/device/vendor", O_RDONLY);
	ASSERT_NONNULL(fp);
	sb_put(second);
	EXPECT_EQ(fp->f_fop->read(fp, buf, sizeof(buf), &pos), 4);
	EXPECT_EQ(memcmp(buf, "1234", 4), 0);
	fs_put_file(fp);
	fp = vfs_open(registry, "/device/vendor", O_RDONLY);
	ASSERT_NONNULL(fp);
	fs_put_file(fp);
	sb_put(registry);
	return 0;
}

static int device_test_release(file *fp)
{
	free(fp->f_inode);
	free(fp);
	return 0;
}
static int device_test_stat(file *fp, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFCHR | 0600;
	st->st_rdev = (250 << 8) | 2;
	return 0;
}
static const file_operations device_test_fops = {
	.getattr = device_test_stat,
	.release = device_test_release,
};
static file *device_test_open(super_block *sb, unsigned devno, int flags)
{
	file *fp = zalloc(sizeof(*fp));
	(void)sb;
	(void)devno;
	(void)flags;
	fp->f_inode = zalloc(sizeof(*fp->f_inode));
	fp->f_inode->i_mode = S_IFCHR | 0600;
	fp->f_count = 1;
	fp->f_fop = &device_test_fops;
	return fp;
}

KTEST(VfsEntriesTest, DeviceNodesAndOrdinaryMknodShareDispatch)
{
	vfs_entry_tree *tree = vfs_entry_tree_create();
	super_block *sb;
	file *fp;
	struct stat st;
	ASSERT_NONNULL(tree);
	ASSERT_NONNULL(vfs_entry_device(vfs_entry_root(tree), "input/test",
					S_IFCHR | 0600, (250 << 8) | 2,
					"test-entries", device_test_open));
	sb = vfs_entry_tree_super(tree);
	fp = vfs_open(sb, "/input/test", O_RDWR);
	ASSERT_NONNULL(fp);
	EXPECT_TRUE(fp->f_fop == &device_test_fops);
	fs_put_file(fp);
	ASSERT_EQ(vfs_mknod(sb, "/alias", S_IFCHR | 0600, (250 << 8) | 2), 0);
	fp = vfs_open(sb, "/alias", O_RDONLY);
	ASSERT_NONNULL(fp);
	EXPECT_TRUE(fp->f_fop == &device_test_fops);
	fs_put_file(fp);
	ASSERT_NONNULL(vfs_entry_device(vfs_entry_root(tree), "pipe",
					S_IFIFO | 0600, 0, NULL, NULL));
	fp = vfs_open(sb, "/pipe", O_RDONLY | O_NONBLOCK);
	ASSERT_NONNULL(fp);
	EXPECT_EQ(fp->f_fop->getattr(fp, &st), 0);
	EXPECT_TRUE(S_ISFIFO(st.st_mode));
	fs_put_file(fp);
	sb_put(sb);
	return 0;
}

static char *large_snapshot(void *data, unsigned tag, unsigned *length)
{
	char *buffer = malloc(6000);
	(void)data;
	(void)tag;
	if (buffer)
		memset(buffer, 'x', 6000);
	*length = 6000;
	return buffer;
}
static const vfs_entry_attribute_ops large_snapshot_ops = {
	.snapshot = large_snapshot
};

KTEST(VfsEntriesTest, SnapshotsCanExceedOnePage)
{
	vfs_entry_tree *tree = vfs_entry_tree_create();
	file *fp;
	char bytes[16];
	loff_t pos = 5990;
	ASSERT_NONNULL(tree);
	ASSERT_NONNULL(vfs_entry_attribute(vfs_entry_root(tree), "snapshot",
					   0444, &large_snapshot_ops, NULL, 0));
	fp = vfs_open(vfs_entry_tree_super(tree), "/snapshot", O_RDONLY);
	ASSERT_NONNULL(fp);
	EXPECT_EQ(fp->f_inode->i_size, 6000);
	EXPECT_EQ(fp->f_fop->read(fp, bytes, sizeof(bytes), &pos), 10);
	EXPECT_EQ(bytes[0], 'x');
	fs_put_file(fp);
	sb_put(vfs_entry_tree_super(tree));
	return 0;
}
