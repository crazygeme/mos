#include <test/test.h>
#include <proc/proc.h>
#include <fs/mount.h>
#include <fs/fcntl.h>
#include <ps/ps.h>

KTEST(ProcEntriesTest, ProvidersSurviveIndependentMountViews)
{
	vfs_entry_node *root = procfs_entries();
	vfs_entry_node *uptime = vfs_entry_child(root, "uptime");
	file *first, *second, *process;
	char buffer[64];
	loff_t position = 0;
	ASSERT_NONNULL(root);
	ASSERT_NONNULL(uptime);
	ASSERT_EQ(fs_do_mount("proc", "/.mos-proc-test-a", "proc", 0, NULL), 0);
	ASSERT_EQ(fs_do_mount("proc", "/.mos-proc-test-b", "proc", 0, NULL), 0);
	first = vfs_open(CURRENT_TASK()->fs->root, "/.mos-proc-test-a/uptime",
			 O_RDONLY);
	second = vfs_open(CURRENT_TASK()->fs->root, "/.mos-proc-test-b/uptime",
			  O_RDONLY);
	ASSERT_NONNULL(first);
	ASSERT_NONNULL(second);
	EXPECT_GT(first->f_fop->read(first, buffer, sizeof(buffer), &position),
		  0);
	process = vfs_open(CURRENT_TASK()->fs->root,
			   "/.mos-proc-test-b/self/status", O_RDONLY);
	ASSERT_NONNULL(process);
	fs_put_file(process);
	EXPECT_EQ(fs_do_umount("/.mos-proc-test-a", 0), 0);
	position = 0;
	EXPECT_GT(first->f_fop->read(first, buffer, sizeof(buffer), &position),
		  0);
	fs_put_file(first);
	EXPECT_EQ(fs_do_umount("/.mos-proc-test-b", 0), 0);
	position = 0;
	EXPECT_GT(second->f_fop->read(second, buffer, sizeof(buffer),
				      &position),
		  0);
	fs_put_file(second);
	EXPECT_TRUE(root == procfs_entries());
	EXPECT_TRUE(uptime == vfs_entry_child(root, "uptime"));
	return 0;
}
