/*
 * src/proc/entries/test.c — /proc/test kernel test runner.
 *
 * Write "1" to trigger all registered KTEST() tests.
 * Results are printed to the kernel log (klog / serial).
 *
 *   echo 1 > /proc/test
 *
 * Output mirrors Google Test format:
 *
 *   [==========] Running 9 tests from 1 test suite.
 *   [----------] 9 tests from MallocTest
 *   [ RUN      ] MallocTest.Basic
 *   [       OK ] MallocTest.Basic (0 ms)
 *   ...
 *   [----------] 9 tests from MallocTest (2 ms total)
 *   [==========] 9 tests ran.
 *   [  PASSED  ] 9 tests.
 *   [  FAILED  ] 0 tests.
 */

#include <fs/fs.h>
#include <fs/vfs.h>
#include <proc/proc.h>
#include <test/test.h>
#include <lib/klib.h>
#include <hw/time.h>
#include <macro.h>
#include <errno.h>
#include <ext4.h>

/* ── Non-fatal failure accumulator ──────────────────────────────────────── */

/* Reset before each test; set by EXPECT_* macros in test files. */
int _ktest_expect_failed = 0;

/* ── Runner ──────────────────────────────────────────────────────────────── */

/*
 * Count how many consecutive entries starting at 't' share the same suite.
 */
static int count_suite(ktest_t *t)
{
	ktest_t *u = t;
	while (u < __ktest_end && strcmp(u->suite, t->suite) == 0)
		u++;
	return (int)(u - t);
}

/*
 * Count distinct suites across all registered tests.
 */
static int count_suites(void)
{
	ktest_t *t = __ktest_start;
	int n = 0;
	while (t < __ktest_end) {
		n++;
		t += count_suite(t);
	}
	return n;
}

static void run_all_tests(void)
{
	ktest_t *t;
	int pass = 0, fail = 0, total = 0, num_suites;

	/* failed-test list: collect Suite.Name pairs to reprint at the end */
#define MAX_FAIL 64
	const char *fail_suite[MAX_FAIL];
	const char *fail_name[MAX_FAIL];
	int nfail = 0;

	for (t = __ktest_start; t < __ktest_end; t++)
		total++;
	num_suites = count_suites();

	klog("[==========] Running %d %s from %d test %s.\n",
	     total, total == 1 ? "test" : "tests",
	     num_suites, num_suites == 1 ? "suite" : "suites");

	/* ── Iterate suites ── */
	t = __ktest_start;
	while (t < __ktest_end) {
		int in_suite = count_suite(t);
		const char *suite_name = t->suite;
		unsigned suite_start = time_now_ms();

		klog("[----------] %d %s from %s\n",
		     in_suite, in_suite == 1 ? "test" : "tests", suite_name);

		ktest_t *end_of_suite = t + in_suite;
		while (t < end_of_suite) {
			unsigned t_start = time_now_ms();

			klog("[ RUN      ] %s.%s\n", t->suite, t->name);

			_ktest_expect_failed = 0;
			int ret = t->fn();
			int failed = (ret != 0) || (_ktest_expect_failed != 0);
			unsigned t_ms = time_now_ms() - t_start;

			if (!failed) {
				klog("[       OK ] %s.%s (%u ms)\n",
				     t->suite, t->name, t_ms);
				pass++;
			} else {
				klog("[  FAILED  ] %s.%s (%u ms)\n",
				     t->suite, t->name, t_ms);
				if (nfail < MAX_FAIL) {
					fail_suite[nfail] = t->suite;
					fail_name[nfail] = t->name;
					nfail++;
				}
				fail++;
			}
			t++;
		}

		unsigned suite_ms = time_now_ms() - suite_start;
		klog("[----------] %d %s from %s (%u ms total)\n\n",
		     in_suite, in_suite == 1 ? "test" : "tests",
		     suite_name, suite_ms);
	}

	/* ── Summary ── */
	klog("[==========] %d %s ran.\n",
	     total, total == 1 ? "test" : "tests");
	klog("[  PASSED  ] %d %s.\n",
	     pass, pass == 1 ? "test" : "tests");

	if (fail > 0) {
		int i;
		klog("[  FAILED  ] %d %s, listed below:\n",
		     fail, fail == 1 ? "test" : "tests");
		for (i = 0; i < nfail; i++)
			klog("[  FAILED  ] %s.%s\n", fail_suite[i], fail_name[i]);
		klog("\n %d FAILED %s\n", fail, fail == 1 ? "TEST" : "TESTS");
	}

#undef MAX_FAIL
}

/* ── VFS file operations ─────────────────────────────────────────────────── */

static ssize_t test_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	const char *p = (const char *)buf;
	unsigned i;

	for (i = 0; i < (unsigned)size; i++) {
		if (p[i] == '1') {
			run_all_tests();
			break;
		}
	}
	return (ssize_t)size;
}

static int test_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_size = 0;
	s->st_blksize = PAGE_SIZE;
	s->st_ino = PROC_INODE;
	s->st_atime = time_now_ms();
	s->st_ctime = time_now_ms();
	return 0;
}

static int test_release(file *fp)
{
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const inode_operations test_iops = {
	.getattr = test_getattr,
};

static const file_operations test_fops = {
	.write = test_write,
	.release = test_release,
};

/* ── Superblock and registration ─────────────────────────────────────────── */

static file *test_open_root(super_block *sb)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFREG | S_IWUSR | S_IWGRP | S_IWOTH;
	node->i_op = &test_iops;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &test_fops;
	return fp;
}

static super_operations test_sops = {
	.open_root = test_open_root,
};

static void test_proc_register(super_block *proc_sb)
{
	vfs_mount(proc_sb, "/test", sget(&test_sops));
}

PROC_INIT(test_proc_register);
