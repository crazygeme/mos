/*
 * src/proc/entries/test.c — /proc/tests/ kernel test .runner.
 *
 * Provides three virtual filesystem objects:
 *
 *   /proc/tests/             - directory listing all suites, scripts, ".runner"
 *   /proc/tests/.runner      - write a filter to run matching tests
 *   /proc/tests/<name>       - read a runnable shell script
 *
 * Filter syntax for /proc/tests/.runner (same as old /proc/test):
 *
 *   echo all        > /proc/tests/.runner   # run everything  (also: "1")
 *   echo MallocTest > /proc/tests/.runner   # exact suite name
 *   echo Malloc*    > /proc/tests/.runner   # glob pattern
 *
 * Each suite file contains the shell command that would run it. Extra shell
 * scripts can also be registered via KTEST_SCRIPT and appear here directly.
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
#include <hw/tty.h>
#include <macro.h>
#include <errno.h>
#include <ext4.h>
#include "common.h"

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

static ktest_script_t *find_script(const char *name)
{
	ktest_script_t *script;

	for (script = __ktest_script_start; script < __ktest_script_end;
	     script++) {
		if (strcmp(script->name, name) == 0)
			return script;
	}

	return NULL;
}

/* ── Test .runner ─────────────────────────────────────────────────────────── */

static void run_all_tests(const char *suite_pat)
{
	ktest_t *t;
	int pass = 0, fail = 0, total, num_suites;
	const char **fail_suite = NULL;
	const char **fail_name = NULL;
	int nfail = 0;

	total = count_tests(suite_pat);
	num_suites = count_suites(suite_pat);
	if (total > 0) {
		fail_suite = kmalloc((size_t)total * sizeof(*fail_suite));
		fail_name = kmalloc((size_t)total * sizeof(*fail_name));
		if (!fail_suite || !fail_name) {
			kfree(fail_suite);
			kfree(fail_name);
			fail_suite = NULL;
			fail_name = NULL;
		}
	}

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
				if (fail_suite && fail_name) {
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
	kfree(fail_suite);
	kfree(fail_name);
}

/* ── Shared getattr ──────────────────────────────────────────────────────── */

static int tests_getattr(inode *node, struct stat *s)
{
	memset(s, 0, sizeof(*s));
	s->st_mode = node->i_mode;
	s->st_size = (loff_t)node->i_size;
	s->st_blksize = PAGE_SIZE;
	s->st_ino = PROC_INODE;
	s->st_atime = time_now_sec();
	s->st_ctime = time_now_sec();
	s->st_mtime = time_now_sec();
	s->st_nlink = S_ISDIR(node->i_mode) ? 2 : 1;
	return 0;
}

static const inode_operations tests_iops = {
	.getattr = tests_getattr,
};

/* ── /proc/tests/.runner — write-only ────────────────────────────────────── */

static ssize_t runner_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	char *filter;
	unsigned len = (unsigned)size;
	unsigned i;
	const char *suite_pat;

	(void)fp;
	(void)pos;

	filter = kmalloc((size_t)len + 1);
	if (!filter)
		return -ENOMEM;

	for (i = 0; i < len; i++)
		filter[i] = ((const char *)buf)[i];
	while (len > 0 && (filter[len - 1] == '\n' || filter[len - 1] == '\r' ||
			   filter[len - 1] == ' ' || filter[len - 1] == '/'))
		len--;
	filter[len] = '\0';

	if (len == 0)
		goto out;

	suite_pat = (strcmp(filter, "1") == 0 || strcmp(filter, "all") == 0) ?
			    "*" :
			    filter;
	run_all_tests(suite_pat);

out:
	kfree(filter);
	return (ssize_t)size;
}

static ssize_t result_write(file *fp, const void *buf, size_t size, loff_t *pos)
{
	char *line;
	unsigned len = (unsigned)size;
	unsigned i;

	(void)fp;
	(void)pos;

	line = kmalloc((size_t)len + 1);
	if (!line)
		return -ENOMEM;

	for (i = 0; i < len; i++)
		line[i] = ((const char *)buf)[i];
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		len--;
	line[len] = '\0';

	if (len > 0)
		klog("%s\n", line);

	kfree(line);
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

static const file_operations result_fops = {
	.write = result_write,
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

static file *make_result_file(void)
{
	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFREG | S_IWUSR | S_IWGRP | S_IWOTH;
	node->i_op = &tests_iops;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &result_fops;
	return fp;
}

static file *make_static_file(const char *content);

static file *make_tty_state_file(void)
{
	char *content;
	unsigned length;
	file *fp;

	content = tty_test_snapshot(&length);
	if (!content)
		content = strdup("meta unavailable=1\n");
	if (!content)
		return NULL;
	if (length == 0)
		length = strlen(content);

	fp = make_static_file(content);
	kfree(content);
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

/* make_static_file — file whose content is a read-only string literal */
static file *make_static_file(const char *content)
{
	unsigned length = strlen(content);
	suite_script_t *ss = zalloc(sizeof(*ss));
	ss->content = kmalloc(length + 1);
	memcpy(ss->content, content, length + 1);
	ss->length = length;

	inode *node = zalloc(sizeof(*node));
	node->i_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH | S_IXUSR |
		       S_IXGRP | S_IXOTH;
	node->i_size = length;
	node->i_op = &tests_iops;
	node->i_private = ss;

	file *fp = zalloc(sizeof(*fp));
	fp->f_inode = node;
	fp->f_count = 1;
	fp->f_fop = &suite_fops;
	return fp;
}

static file *make_proc_buf_file(proc_buf_t *pb)
{
	file *fp = make_static_file(pb->buf);
	proc_buf_free(pb);
	return fp;
}

static file *make_wrapped_script_file(const char *script_name, const char *body)
{
	proc_buf_t *pb = proc_buf_new();

	proc_buf_printf(
		pb,
		"#!/bin/sh\n"
		"__ktest_name='%s'\n"
		"__ktest_result(){ printf '%%s\n' \"$1\" > /proc/tests/.result; }\n"
		"read __ktest_up _ < /proc/uptime\n"
		"__ktest_start_s=${__ktest_up%%.*}\n"
		"__ktest_start_cs=${__ktest_up#*.}\n"
		"__ktest_log=/tmp/ktest_${__ktest_name}_$$.log\n"
		"__ktest_result \"[ RUN      ] script.${__ktest_name}\"\n"
		"(\n",
		script_name);
	proc_buf_printf(pb, "%s", body);
	proc_buf_printf(
		pb,
		"\n"
		") > \"$__ktest_log\" 2>&1\n"
		"__ktest_rc=$?\n"
		"read __ktest_up _ < /proc/uptime\n"
		"__ktest_end_s=${__ktest_up%%.*}\n"
		"__ktest_end_cs=${__ktest_up#*.}\n"
		"__ktest_elapsed_ms=$((((__ktest_end_s * 100) + __ktest_end_cs - "
		"((__ktest_start_s * 100) + __ktest_start_cs)) * 10))\n"
		"if [ \"$__ktest_rc\" -eq 0 ]; then\n"
		"  __ktest_result \"[       OK ] script.${__ktest_name} "
		"(${__ktest_elapsed_ms} ms)\"\n"
		"else\n"
		"  __ktest_result \"script.${__ktest_name}: Failure\"\n"
		"  if [ -s \"$__ktest_log\" ]; then\n"
		"    while IFS= read -r __ktest_line; do\n"
		"      __ktest_result \"  $__ktest_line\"\n"
		"    done < \"$__ktest_log\"\n"
		"  else\n"
		"    __ktest_result \"  script exited with status $__ktest_rc\"\n"
		"  fi\n"
		"  __ktest_result \"[  FAILED  ] script.${__ktest_name} "
		"(${__ktest_elapsed_ms} ms)\"\n"
		"fi\n"
		"rm -f \"$__ktest_log\"\n"
		"exit $__ktest_rc\n");
	return make_proc_buf_file(pb);
}

static file *make_suite_file(const char *suite_name)
{
	proc_buf_t *pb = proc_buf_new();

	proc_buf_printf(pb, "#!/bin/sh\n");
	proc_buf_printf(pb, "echo %s > /proc/tests/.runner\n", suite_name);
	return make_proc_buf_file(pb);
}

static file *make_all_script_file(void)
{
	ktest_script_t *script;
	unsigned script_count = 0;
	proc_buf_t *pb = proc_buf_new();

	for (script = __ktest_script_start; script < __ktest_script_end;
	     script++)
		script_count++;

	proc_buf_printf(
		pb,
		"#!/bin/sh\n"
		"__ktest_result(){ printf '%%s\n' \"$1\" > /proc/tests/.result; }\n"
		"__ktest_fail_count=0\n");

	for (script = __ktest_script_start; script < __ktest_script_end;
	     script++) {
		proc_buf_printf(
			pb,
			"if /proc/tests/%s; then\n"
			":\n"
			"else\n"
			"__ktest_rc=$?\n"
			"__ktest_result \"[  FAILED  ] script.%s "
			"(exit ${__ktest_rc})\"\n"
			"__ktest_fail_count=$((__ktest_fail_count + 1))\n"
			"fi\n",
			script->name, script->name);
	}
	proc_buf_printf(
		pb,
		"__ktest_result \"Total %u Cases, ${__ktest_fail_count} Failed\"\n"
		"if [ \"$__ktest_fail_count\" -eq 0 ]; then\n"
		"  __ktest_result \"[==========] all_script finished: PASS\"\n"
		"else\n"
		"  __ktest_result \"[==========] all_script finished: FAIL\"\n"
		"fi\n"
		"exit $__ktest_fail_count\n",
		script_count);
	return make_proc_buf_file(pb);
}

/* ── /proc/tests/ — directory ────────────────────────────────────────────── */

static ssize_t tests_dir_read(file *fp, void *buf, size_t count, loff_t *pos)
{
	memory_dir *td = fp->f_inode->i_private;
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
	memory_dir *td = fp->f_inode->i_private;
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

static file *tests_open_root(super_block *sb, int flag)
{
	ktest_t *t;
	unsigned size = 0;
	char *buf, *p;
	const char *begin;
	struct linux_dirent *dirp;
	memory_dir *td;
	inode *node;
	file *fp;

	/* ── Size calculation ── */
	size += ROUND_UP(NAME_OFFSET() + 2); /* "."       (1 + NUL) */
	size += ROUND_UP(NAME_OFFSET() + 3); /* ".."      (2 + NUL) */
	size += ROUND_UP(NAME_OFFSET() + 7); /* ".runner" (6 + NUL) */
	size += ROUND_UP(NAME_OFFSET() + 7); /* ".result" (6 + NUL) */
	size += ROUND_UP(NAME_OFFSET() + 11); /* ".tty_state" (10 + NUL) */
	size += ROUND_UP(NAME_OFFSET() + 4); /* "all"     (3 + NUL) */
	size += ROUND_UP(NAME_OFFSET() + 11); /* "all_script" (10 + NUL) */

	t = __ktest_start;
	while (t < __ktest_end) {
		size += ROUND_UP(NAME_OFFSET() + strlen(t->suite) + 1);
		t += count_suite(t);
	}

	{
		ktest_script_t *script;

		for (script = __ktest_script_start; script < __ktest_script_end;
		     script++)
			size += ROUND_UP(NAME_OFFSET() + strlen(script->name) +
					 1);
	}

	/* ── Allocate and fill ── */
	buf = p = kmalloc(size);
	begin = buf;
	memset(buf, 0, size);

	FILL_ENTRY(".", PROC_INODE);
	FILL_ENTRY("..", PROC_INODE);
	FILL_ENTRY(".runner", PROC_INODE);
	FILL_ENTRY(".result", PROC_INODE);
	FILL_ENTRY(".tty_state", PROC_INODE);
	FILL_ENTRY("all", PROC_INODE);
	FILL_ENTRY("all_script", PROC_INODE);

	t = __ktest_start;
	while (t < __ktest_end) {
		FILL_ENTRY(t->suite, PROC_INODE);
		t += count_suite(t);
	}

	{
		ktest_script_t *script;

		for (script = __ktest_script_start; script < __ktest_script_end;
		     script++)
			FILL_ENTRY(script->name, PROC_INODE);
	}

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

	if (strcmp(name, ".result") == 0)
		return make_result_file();

	if (strcmp(name, ".tty_state") == 0)
		return make_tty_state_file();

	if (strcmp(name, "all") == 0)
		return make_suite_file("all");

	if (strcmp(name, "all_script") == 0)
		return make_all_script_file();

	{
		ktest_script_t *script = find_script(name);

		if (script != NULL)
			return make_wrapped_script_file(script->name,
							script->content);
	}

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
	if ((unsigned)__ktest_start != (unsigned)__ktest_end ||
	    (unsigned)__ktest_script_start != (unsigned)__ktest_script_end)
		vfs_mount(proc_sb, "/tests", sget(&tests_sops));
}

PROC_INIT(tests_proc_register);
