#include <test/test.h>
#include <fs/syslog.h>
#include <fs/fcntl.h>
#include <errno.h>


static int check_IndependentRecords(char *scratch)
{
	file *a = syslog_open(S_IFCHR | 0600, 0);
	file *b = syslog_open(S_IFCHR | 0600, 0);
	char *first = scratch, *second = scratch + 256;
	ssize_t n, m;
	ASSERT_NONNULL(a);
	ASSERT_NONNULL(b);
	a->f_flag = b->f_flag = O_NONBLOCK;
	EXPECT_EQ(a->f_fop->llseek(a, 0, SEEK_END), 0);
	EXPECT_EQ(b->f_fop->llseek(b, 0, SEEK_END), 0);
	EXPECT_EQ(a->f_fop->read(a, first, 256, NULL), -EAGAIN);
	EXPECT_EQ(a->f_fop->poll(a, FS_POLL_READ, NULL), 0);
	syslog_emit(6, "record\t\\\n", 9);
	EXPECT_EQ(a->f_fop->poll(a, FS_POLL_READ, NULL), FS_POLL_READ);
	EXPECT_EQ(a->f_fop->read(a, first, 1, NULL), -EINVAL);
	n = a->f_fop->read(a, first, 256 - 1, NULL);
	m = b->f_fop->read(b, second, 256 - 1, NULL);
	EXPECT_GT(n, 0);
	EXPECT_EQ(n, m);
	if (n > 0 && m == n) {
		first[n] = second[m] = 0;
		EXPECT_EQ(strcmp(first, second), 0);
		EXPECT_NONNULL(strstr(first, ",-;record\\x09\\x5c\n"));
	}
	EXPECT_EQ(a->f_fop->read(a, first, 256, NULL), -EAGAIN);
	fs_put_file(a);
	fs_put_file(b);
	return 0;
}

KTEST(SyslogTest, IndependentRecords)
{
	char *scratch = zalloc(512);
	int result;
	ASSERT_NONNULL(scratch);
	result = check_IndependentRecords(scratch);
	free(scratch);
	return result;
}

static int check_PrintkAndDeviceWrite(char *scratch)
{
	file *fp = syslog_open(S_IFCHR | 0600, 0);
	char *buf = scratch;
	ssize_t n;
	ASSERT_NONNULL(fp);
	fp->f_flag = O_NONBLOCK;
	EXPECT_EQ(fp->f_fop->llseek(fp, 0, SEEK_END), 0);
	printk("syslog printk test\n");
	n = fp->f_fop->read(fp, buf, 256 - 1, NULL);
	EXPECT_GT(n, 0);
	if (n > 0) {
		buf[n] = 0;
		EXPECT_EQ(strncmp(buf, "6,", 2), 0);
		EXPECT_NONNULL(strstr(buf, ";syslog printk test\n"));
	}
	EXPECT_EQ(fp->f_fop->write(fp, "<6>user test", 12, NULL), 12);
	n = fp->f_fop->read(fp, buf, 256 - 1, NULL);
	EXPECT_GT(n, 0);
	if (n > 0) {
		buf[n] = 0;
		EXPECT_EQ(strncmp(buf, "14,", 3), 0);
		EXPECT_NONNULL(strstr(buf, ";user test\n"));
	}
	fs_put_file(fp);
	return 0;
}

KTEST(SyslogTest, PrintkAndDeviceWrite)
{
	char *scratch = zalloc(512);
	int result;
	ASSERT_NONNULL(scratch);
	result = check_PrintkAndDeviceWrite(scratch);
	free(scratch);
	return result;
}

static int check_OverrunAndClear(char *scratch)
{
	file *fp = syslog_open(S_IFCHR | 0600, 0);
	char *buf = scratch;
	unsigned i;
	ASSERT_NONNULL(fp);
	fp->f_flag = O_NONBLOCK;
	EXPECT_EQ(fp->f_fop->llseek(fp, 0, SEEK_END), 0);
	for (i = 0; i < 65; i++)
		syslog_emit(6, "overrun", 7);
	EXPECT_EQ(fp->f_fop->read(fp, buf, 256, NULL), -EPIPE);
	EXPECT_GT(fp->f_fop->read(fp, buf, 256, NULL), 0);
	EXPECT_EQ(sys_syslog(5, NULL, 0), 0);
	EXPECT_EQ(sys_syslog(3, buf, 256), 0);
	EXPECT_EQ(fp->f_fop->llseek(fp, 0, 3), 0);
	EXPECT_EQ(fp->f_fop->read(fp, buf, 256, NULL), -EAGAIN);
	/* Clearing snapshots retains records for independent device readers. */
	EXPECT_EQ(fp->f_fop->llseek(fp, 0, SEEK_SET), 0);
	EXPECT_GT(fp->f_fop->read(fp, buf, 256, NULL), 0);
	fs_put_file(fp);
	return 0;
}

KTEST(SyslogTest, OverrunAndClear)
{
	char *scratch = zalloc(512);
	int result;
	ASSERT_NONNULL(scratch);
	result = check_OverrunAndClear(scratch);
	free(scratch);
	return result;
}

static int check_SharedLegacyStream(char *scratch)
{
	file *fp = syslog_open(S_IFREG | 0400, 0);
	char *buf = scratch, *snapshot = scratch + 256;
	int n, m;
	ASSERT_NONNULL(fp);
	fp->f_flag = O_NONBLOCK;
	while (fp->f_fop->read(fp, buf, 256, NULL) > 0)
		;
	EXPECT_EQ(sys_syslog(5, NULL, 0), 0);
	syslog_emit(6, "shared", 6);
	n = sys_syslog(3, snapshot, 256);
	m = sys_syslog(3, buf, 256);
	EXPECT_EQ(n, 10);
	EXPECT_EQ(m, n);
	if (n == 10 && m == n)
		EXPECT_EQ(memcmp(buf, snapshot, n), 0);
	EXPECT_EQ(fp->f_fop->read(fp, buf, 3, NULL), 3);
	EXPECT_EQ(sys_syslog(9, NULL, 0), 7);
	/* The syscall continues at the byte following the proc read. */
	n = sys_syslog(2, buf, 7);
	EXPECT_EQ(n, 7);
	if (n == 7)
		EXPECT_EQ(memcmp(buf, "shared\n", 7), 0);
	EXPECT_EQ(fp->f_fop->read(fp, buf, 256, NULL), -EAGAIN);
	fs_put_file(fp);
	return 0;
}

KTEST(SyslogTest, SharedLegacyStream)
{
	char *scratch = zalloc(512);
	int result;
	ASSERT_NONNULL(scratch);
	result = check_SharedLegacyStream(scratch);
	free(scratch);
	return result;
}

static int check_long_escaped_record(char *scratch)
{
	char *text = scratch, *output = scratch + 512;
	file *fp = syslog_open(S_IFCHR | 0600, 0);
	ssize_t length;
	char *payload;
	unsigned i;
	ASSERT_NONNULL(fp);
	fp->f_flag = O_NONBLOCK;
	EXPECT_EQ(fp->f_fop->llseek(fp, 0, SEEK_END), 0);
	memset(text, '\\', 511);
	text[511] = 0;
	printk("%s", text);
	length = fp->f_fop->read(fp, output, 3583, NULL);
	EXPECT_GT(length, 2044);
	if (length > 0 && length < 3584) {
		output[length] = 0;
		payload = strchr(output, ';');
		EXPECT_NONNULL(payload);
		if (payload && output + length - payload == 2046) {
			payload++;
			for (i = 0; i < 511; i++)
				EXPECT_EQ(memcmp(payload + i * 4, "\\x5c", 4),
					  0);
			EXPECT_EQ(payload[2044], '\n');
		} else {
			EXPECT_TRUE(0);
		}
	}
	fs_put_file(fp);
	return 0;
}

KTEST(SyslogTest, LongEscapedRecord)
{
	char *scratch = zalloc(4096);
	int result;
	ASSERT_NONNULL(scratch);
	result = check_long_escaped_record(scratch);
	free(scratch);
	return result;
}
