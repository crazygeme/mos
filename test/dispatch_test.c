#include <test/test.h>
#include <lib/command.h>
#include <lib/slots.h>
#include <fs/vfs.h>
#include <errno.h>

KTEST(DispatchTest, SlotBoundariesAndReuse)
{
	static const unsigned capacities[] = { 1, 63, 64, 65, 128, 2047, 4096 };
	for (unsigned n = 0; n < sizeof(capacities) / sizeof(capacities[0]);
	     n++) {
		slot_pool pool = { 0 };
		unsigned capacity = capacities[n];
		for (unsigned i = 0; i < capacity; i++)
			ASSERT_EQ(slot_take(&pool, capacity), (int)i);
		EXPECT_EQ(slot_take(&pool, capacity), -1);
		slot_return(&pool, capacity - 1);
		EXPECT_EQ(slot_take(&pool, capacity), (int)capacity - 1);
		EXPECT_EQ(slot_take(&pool, capacity), -1);
		for (unsigned i = capacity; i; i--)
			slot_return(&pool, i - 1);
		for (unsigned i = 0; i < capacity; i++)
			EXPECT_EQ(slot_take(&pool, capacity), (int)i);
		EXPECT_EQ(slot_take(&pool, capacity), -1);
	}
	return 0;
}

static int command_test_invoke(void *context, unsigned command, void *argument)
{
	*(unsigned *)argument = command;
	return *(int *)context;
}

KTEST(DispatchTest, CompleteIoctlIdentity)
{
	static const command_operation operations[256] = {
		[0x31] = { 0x40045431, command_test_invoke },
	};
	static const command_operation *const groups[256] = {
		[0x54] = operations
	};
	int result = 23;
	unsigned argument = 0;
	EXPECT_EQ(command_dispatch(groups, &result, 0x40045431, &argument,
				   -ENOTTY),
		  23);
	EXPECT_EQ(argument, 0x40045431U);
	EXPECT_EQ(command_dispatch(groups, &result, 0x80045431, &argument,
				   -ENOTTY),
		  -ENOTTY);
	EXPECT_EQ(command_dispatch(groups, &result, 0x40085431, &argument,
				   -ENOTTY),
		  -ENOTTY);
	EXPECT_EQ(command_dispatch(groups, &result, 0x40045331, &argument,
				   -ENOTTY),
		  -ENOTTY);
	EXPECT_EQ(command_dispatch(groups, &result, 0x40045432, &argument,
				   -ENOTTY),
		  -ENOTTY);
	return 0;
}

static int mount_test_mkdir(super_block *sb, const char *path, unsigned mode)
{
	(void)mode;
	return strcmp(path, "/item") ? -ENOENT : (int)(uintptr_t)sb->s_fs_info;
}

KTEST(DispatchTest, MountComponentBoundaries)
{
	static const super_operations operations = { .mkdir =
							     mount_test_mkdir };
	super_block *host = sget(NULL);
	super_block *a = sget(&operations), *ab = sget(&operations);
	super_block *nested = sget(&operations), *sibling = sget(&operations);
	ASSERT_NONNULL(host);
	ASSERT_NONNULL(a);
	ASSERT_NONNULL(ab);
	ASSERT_NONNULL(nested);
	ASSERT_NONNULL(sibling);
	a->s_fs_info = (void *)11;
	ab->s_fs_info = (void *)12;
	nested->s_fs_info = (void *)13;
	sibling->s_fs_info = (void *)14;
	ASSERT_EQ(vfs_mount(host, "/a/b", nested), 0);
	ASSERT_EQ(vfs_mount(host, "/ab", ab), 0);
	ASSERT_EQ(vfs_mount(host, "/a", a), 0);
	ASSERT_EQ(vfs_mount(host, "/a-c", sibling), 0);
	EXPECT_EQ(vfs_mkdir(host, "/a/item", 0), 11);
	EXPECT_EQ(vfs_mkdir(host, "/ab/item", 0), 12);
	EXPECT_EQ(vfs_mkdir(host, "/a/b/item", 0), 13);
	EXPECT_EQ(vfs_mkdir(host, "/a-c/item", 0), 14);
	EXPECT_EQ(vfs_mkdir(host, "/abc/item", 0), -ENOSYS);
	EXPECT_EQ(vfs_mkdir(host, "/a/bc/item", 0), -ENOENT);
	EXPECT_EQ(vfs_umount(host, "/a/b"), 0);
	EXPECT_EQ(vfs_umount(host, "/a/b"), -ENOENT);
	sb_put(host);
	return 0;
}
