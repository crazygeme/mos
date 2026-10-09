#include <test/test.h>
#include <device/blockdev.h>
#include <errno.h>

static unsigned transfers;
static int transfer_result;

static int test_read(void *data, void *buffer, uint64_t sector, unsigned count)
{
	(void)data;
	(void)buffer;
	(void)sector;
	(void)count;
	transfers++;
	return transfer_result;
}
static int test_write(void *data, const void *buffer, uint64_t sector,
		      unsigned count)
{
	return test_read(data, (void *)buffer, sector, count);
}
static const blockdev_io test_io = { .read = test_read, .write = test_write };

KTEST(BlockDeviceTest, RangeErrorsAndPinnedLifetime)
{
	blockdev_handle *device;
	char buffer[16];
	blockdev_register("test-block-io", 250, 1, 100 * 512,
			  BLOCKDEV_FLAG_MOUNTABLE);
	ASSERT_EQ(blockdev_bind_io("test-block-io", 512, &test_io, NULL), 0);
	device = blockdev_open("test-block-io");
	ASSERT_NONNULL(device);
	EXPECT_EQ(blockdev_sector_count(device), 100);
	transfers = 0;
	transfer_result = 0;
	EXPECT_EQ(blockdev_read(device, buffer, 0x100000001ULL, 1), -EIO);
	EXPECT_EQ(blockdev_write(device, buffer, ~0ULL, 2), -EIO);
	EXPECT_EQ(blockdev_read(device, buffer, 99, 2), -EIO);
	EXPECT_EQ(transfers, 0);
	EXPECT_EQ(blockdev_read(device, buffer, 99, 1), 0);
	EXPECT_EQ(transfers, 1);
	transfer_result = -EIO;
	EXPECT_EQ(blockdev_write(device, buffer, 0, 1), -EIO);
	EXPECT_EQ(blockdev_unbind_io("test-block-io"), -EBUSY);
	EXPECT_EQ(blockdev_bind_io("test-block-io", 512, &test_io, NULL),
		  -EBUSY);
	blockdev_close(device);
	EXPECT_EQ(blockdev_unbind_io("test-block-io"), 0);
	EXPECT_NULL(blockdev_open("test-block-io"));
	blockdev_update("test-block-io", 0, 0);
	return 0;
}
