/*
 * src/proc/entries/test.c — /proc/tests/ kernel test .runner.
 *
 * Provides three virtual filesystem objects:
 *
 *   /proc/tests/              — directory listing all suites + ".runner"
 *   /proc/tests/.runner        — write a filter to run matching tests
 *   /proc/tests/<SuiteName>   — read to get a one-line runnable script
 *
 * Filter syntax for /proc/tests/.runner (same as old /proc/test):
 *
 *   echo all         > /proc/tests/.runner   # run everything  (also: "1")
 *   echo MallocTest  > /proc/tests/.runner   # exact suite name
 *   echo Malloc*     > /proc/tests/.runner   # glob pattern
 *
 * Each per-suite file contains the shell command that would run it:
 *
 *   $ cat /proc/tests/MallocTest
 *   echo MallocTest > /proc/tests/.runner
 *
 * Output mirrors Google Test format.
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

/* ── Suite helpers ───────────────────────────────────────────────────────── */

/*
 * Count how many consecutive ktest entries starting at @t share the same
 * suite name.  Used to iterate distinct suites in O(n).
 */
static int count_suite(ktest_t *t)
{
	ktest_t *u = t;
	while (u < __ktest_end && strcmp(u->suite, t->suite) == 0)
		u++;
	return (int)(u - t);
}

static int count_suites(const char *suite_pat)
{
	ktest_t *t = __ktest_start;
	int n = 0;
	while (t < __ktest_end) {
		if (strglob(suite_pat, t->suite))
			n++;
		t += count_suite(t);
	}
	return n;
}

static int count_tests(const char *suite_pat)
{
	ktest_t *t;
	int n = 0;
	for (t = __ktest_start; t < __ktest_end; t++)
		if (strglob(suite_pat, t->suite))
			n++;
	return n;
}

/* ── Test .runner ─────────────────────────────────────────────────────────── */

static void run_all_tests(const char *suite_pat)
{
	ktest_t *t;
	int pass = 0, fail = 0, total, num_suites;

#define MAX_FAIL 64
	const char *fail_suite[MAX_FAIL];
	const char *fail_name[MAX_FAIL];
	int nfail = 0;

	total = count_tests(suite_pat);
	num_suites = count_suites(suite_pat);

	klog("[==========] Running %d %s from %d test %s.\n", total,
	     total == 1 ? "test" : "tests", num_suites,
	     num_suites == 1 ? "suite" : "suites");

	t = __ktest_start;
	while (t < __ktest_end) {
		int in_suite = count_suite(t);
		const char *suite_name = t->suite;
		ktest_t *end_of_suite = t + in_suite;

		if (!strglob(suite_pat, suite_name)) {
			t = end_of_suite;
			continue;
		}

		unsigned suite_start = time_now_ms();
		klog("[----------] %d %s from %s\n", in_suite,
		     in_suite == 1 ? "test" : "tests", suite_name);

		while (t < end_of_suite) {
			unsigned t_start = time_now_ms();
			klog("[ RUN      ] %s.%s\n", t->suite, t->name);

			_ktest_expect_failed = 0;
			int ret = t->fn();
			int failed = (ret != 0) || (_ktest_expect_failed != 0);
			unsigned t_ms = time_now_ms() - t_start;

			if (!failed) {
				klog("[       OK ] %s.%s (%u ms)\n", t->suite,
				     t->name, t_ms);
				pass++;
			} else {
				klog("[  FAILED  ] %s.%s (%u ms)\n", t->suite,
				     t->name, t_ms);
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
		klog("[----------] %d %s from %s (%u ms total)\n\n", in_suite,
		     in_suite == 1 ? "test" : "tests", suite_name, suite_ms);
	}

	klog("[==========] %d %s ran.\n", total, total == 1 ? "test" : "tests");
	klog("[  PASSED  ] %d %s.\n", pass, pass == 1 ? "test" : "tests");

	if (fail > 0) {
		int i;
		klog("[  FAILED  ] %d %s, listed below:\n", fail,
		     fail == 1 ? "test" : "tests");
		for (i = 0; i < nfail; i++)
			klog("[  FAILED  ] %s.%s\n", fail_suite[i],
			     fail_name[i]);
		klog("\n %d FAILED %s\n", fail, fail == 1 ? "TEST" : "TESTS");
	}
#undef MAX_FAIL
}

/* ── Shared getattr ──────────────────────────────────────────────────────── */

static int tests_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_size = (loff_t)node->i_size;
	s->st_blksize = PAGE_SIZE;
	s->st_ino = PROC_INODE;
	s->st_atime = time_now_ms();
	s->st_ctime = time_now_ms();
	s->st_nlink = S_ISDIR(node->i_mode) ? 2 : 1;
	return 0;
}

static const inode_operations tests_iops = {
	.getattr = tests_getattr,
};

/* ── /proc/tests/.runner — write-only ────────────────────────────────────── */

static ssize_t runner_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	char filter[128];
	unsigned len = (unsigned)size < sizeof(filter) - 1 ? (unsigned)size :
							     sizeof(filter) - 1;
	unsigned i;
	const char *suite_pat;

	for (i = 0; i < len; i++)
		filter[i] = ((const char *)buf)[i];
	while (len > 0 && (filter[len - 1] == '\n' || filter[len - 1] == '\r' ||
			   filter[len - 1] == ' ' || filter[len - 1] == '/'))
		len--;
	filter[len] = '\0';

	if (len == 0)
		return (ssize_t)size;

	suite_pat = (strcmp(filter, "1") == 0 || strcmp(filter, "all") == 0) ?
			    "*" :
			    filter;
	run_all_tests(suite_pat);
	return (ssize_t)size;
}

static int runner_release(file *fp)
{
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations runner_fops = {
	.write = runner_write,
	.release = runner_release,
};

static file *make_runner_file(void)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFREG | S_IWUSR | S_IWGRP | S_IWOTH;
	node->i_op = &tests_iops;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &runner_fops;
	return fp;
}

/* ── /proc/tests/<SuiteName> — read-only script ──────────────────────────── */

/*
 * Each suite file contains a single line that, when executed by a shell,
 * runs that suite via the .runner:
 *
 *   echo MallocTest > /proc/tests/.runner
 */

typedef struct {
	char *content;
	unsigned length;
} suite_script_t;

static ssize_t suite_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	suite_script_t *ss = fp->f_inode->i_private;
	loff_t off = *pos;
	ssize_t left = (ssize_t)ss->length - (ssize_t)off;
	ssize_t n = (ssize_t)count < left ? (ssize_t)count : left;

	if (n <= 0)
		return 0;
	memcpy(buf, ss->content + off, (size_t)n);
	*pos = off + n;
	return n;
}

static int suite_release(file *fp)
{
	suite_script_t *ss = fp->f_inode->i_private;
	kfree(ss->content);
	free(ss);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations suite_fops = {
	.read = suite_read,
	.release = suite_release,
};

static file *make_suite_file(const char *suite_name)
{
	/* "#!/bin/sh\necho <name> > /proc/tests/.runner\n" */
	static const char shebang[] = "#!/bin/sh\n";
	static const char prefix[] = "echo ";
	static const char suffix[] = " > /proc/tests/.runner\n";
	unsigned name_len = strlen(suite_name);
	unsigned total = sizeof(shebang) - 1 + sizeof(prefix) - 1 + name_len +
			 sizeof(suffix) - 1;

	char *content = kmalloc(total + 1);
	strcpy(content, shebang);
	strcat(content, prefix);
	strcat(content, suite_name);
	strcat(content, suffix);

	suite_script_t *ss = zalloc(sizeof(*ss));
	ss->content = content;
	ss->length = total;

	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH | S_IXUSR |
		       S_IXGRP | S_IXOTH;
	node->i_size = total;
	node->i_op = &tests_iops;
	node->i_private = ss;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &suite_fops;
	return fp;
}

/* ── /proc/tests/ — directory ────────────────────────────────────────────── */

typedef struct {
	struct linux_dirent *buf;
	unsigned length;
} tests_dir_t;

static ssize_t tests_dir_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	tests_dir_t *td = fp->f_inode->i_private;
	loff_t off = *pos;
	ssize_t left = (ssize_t)td->length - (ssize_t)off;
	ssize_t n = (ssize_t)count < left ? (ssize_t)count : left;

	if (n <= 0)
		return 0;
	memcpy(buf, (char *)td->buf + off, (size_t)n);
	*pos = off + n;
	return n;
}

static int tests_dir_release(file *fp)
{
	tests_dir_t *td = fp->f_inode->i_private;
	kfree(td->buf);
	free(td);
	free(fp->f_inode);
	free(fp);
	return 0;
}

static const file_operations tests_dir_fops = {
	.read = tests_dir_read,
	.release = tests_dir_release,
};

static file *tests_open_root(super_block *sb)
{
	ktest_t *t;
	unsigned size = 0;
	char *buf, *p;
	const char *begin;
	struct linux_dirent *dirp;
	tests_dir_t *td;
	inode *node;
	file *fp;

	/* ── Size calculation ── */
	size += ROUND_UP(NAME_OFFSET() + 2); /* "."      (1 + NUL) */
	size += ROUND_UP(NAME_OFFSET() + 3); /* ".."     (2 + NUL) */
	size += ROUND_UP(NAME_OFFSET() + 7); /* ".runner" (6 + NUL) */
	size += ROUND_UP(NAME_OFFSET() + 4); /* "all"    (3 + NUL) */

	t = __ktest_start;
	while (t < __ktest_end) {
		size += ROUND_UP(NAME_OFFSET() + strlen(t->suite) + 1);
		t += count_suite(t);
	}

	/* ── Allocate and fill ── */
	buf = p = kmalloc(size);
	begin = buf;
	memset(buf, 0, size);

#define FILL_ENTRY(name_str)                                               \
	do {                                                               \
		dirp = (struct linux_dirent *)p;                           \
		dirp->d_ino = PROC_INODE;                                  \
		strcpy(dirp->d_name, (name_str));                          \
		dirp->d_reclen =                                           \
			ROUND_UP(NAME_OFFSET() + strlen(name_str) + 1);    \
		dirp->d_off = (unsigned long)(p + dirp->d_reclen - begin); \
		p += dirp->d_reclen;                                       \
	} while (0)

	FILL_ENTRY(".");
	FILL_ENTRY("..");
	FILL_ENTRY(".runner");
	FILL_ENTRY("all");

	t = __ktest_start;
	while (t < __ktest_end) {
		FILL_ENTRY(t->suite);
		t += count_suite(t);
	}

#undef FILL_ENTRY

	td = zalloc(sizeof(*td));
	td->buf = (struct linux_dirent *)buf;
	td->length = size;

	node = zalloc(sizeof(*node));
	node->i_mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IXUSR |
		       S_IXGRP | S_IXOTH;
	node->i_op = &tests_iops;
	node->i_private = td;

	fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &tests_dir_fops;
	return fp;
}

/* ── open callback for sub-paths under /proc/tests/ ────────────────────── */

/*
 * Called by VFS for paths like "/.runner" or "/MallocTest"
 * (path is relative to the /proc/tests mount root, always starts with '/').
 */
static file *tests_open(super_block *sb, const char *path, int flag)
{
	const char *name;
	ktest_t *t;

	if (*path != '/')
		return NULL;
	name = path + 1;

	if (strcmp(name, ".runner") == 0)
		return make_runner_file();

	if (strcmp(name, "all") == 0)
		return make_suite_file("all");

	t = __ktest_start;
	while (t < __ktest_end) {
		if (strcmp(t->suite, name) == 0)
			return make_suite_file(t->suite);
		t += count_suite(t);
	}

	return NULL;
}

/* ── Superblock and registration ─────────────────────────────────────────── */

static const super_operations tests_sops = {
	.open_root = tests_open_root,
	.open = tests_open,
};

static void tests_proc_register(super_block *proc_sb)
{
	if ((unsigned)__ktest_start != (unsigned)__ktest_end)
		vfs_mount(proc_sb, "/tests", sget(&tests_sops));
}

PROC_INIT(tests_proc_register);
