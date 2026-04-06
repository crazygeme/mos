#ifndef _TEST_TEST_H
#define _TEST_TEST_H

#include <lib/klib.h>

/*
 * Kernel unit-test framework — gtest-compatible API.
 *
 * Defining a test:
 *
 *   KTEST(MallocTest, Basic) {
 *       void *p = malloc(64);
 *       ASSERT_NONNULL(p);
 *       free(p);
 *   }
 *
 * The macro both defines the function and registers it in the ".ktest"
 * ELF section.  No manual registration call is needed.
 *
 * Fatal assertions (ASSERT_*) — abort the current test on failure.
 * Non-fatal expectations (EXPECT_*) — record a failure and continue.
 *
 * Run all tests:   echo 1 > /proc/test
 * Read results:    cat /proc/test
 */

/* ── Test record ─────────────────────────────────────────────────────────── */

typedef int (*ktest_fn_t)(void);

typedef struct {
	const char *suite; /* e.g. "MallocTest" */
	const char *name; /* e.g. "Basic"      */
	ktest_fn_t fn;
} ktest_t;

extern ktest_t __ktest_start[];
extern ktest_t __ktest_end[];

typedef struct {
	const char *name;
	const char *content;
} ktest_script_t;

extern ktest_script_t __ktest_script_start[];
extern ktest_script_t __ktest_script_end[];

/* ── KTEST(Suite, Name) — defines and registers a test ───────────────────── *
 *
 * Expands to a forward declaration, a section-placed ktest_t, and the
 * opening of the function definition.  The user's { ... } body follows
 * directly, matching gtest's TEST() syntax.
 *
 * The function must return int: 0 = pass, non-zero = fail line.
 * ASSERT_* macros return __LINE__ automatically; add "return 0;" at the end.
 */
#define KTEST(suite, name)                                        \
	static int _ktest_fn_##suite##_##name(void);              \
	static ktest_t _ktest_entry_##suite##_##name              \
		__attribute__((used, section(".ktest"))) = {      \
			#suite, #name, _ktest_fn_##suite##_##name \
		};                                                \
	static int _ktest_fn_##suite##_##name(void)

#define __KTEST_SCRIPT_CAT2(a, b) a##b
#define __KTEST_SCRIPT_CAT(a, b) __KTEST_SCRIPT_CAT2(a, b)

#define KTEST_SCRIPT(name, content) KTEST_SCRIPT_NAMED(#name, content)

#define KTEST_SCRIPT_NAMED(name_literal, content)                           \
	static const ktest_script_t                                         \
		__KTEST_SCRIPT_CAT(_ktest_script_entry_, __LINE__)           \
			__attribute__((used, section(".ktest_script"))) = { \
				name_literal, content                        \
			}

/* ── Non-fatal failure accumulator (set by EXPECT_*, read by .runner) ─────── */

extern int _ktest_expect_failed; /* defined in test.c */

/* ── Fatal assertions — abort the test on failure ───────────────────────── */

#define ASSERT_TRUE(cond)                                       \
	do {                                                    \
		if (!(cond)) {                                  \
			klog(__FILE__ ":%d: Failure\n"          \
				      "  Value of: " #cond "\n" \
				      "  Actual: false\n"       \
				      "  Expected: true\n",     \
			     __LINE__);                         \
			return __LINE__;                        \
		}                                               \
	} while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))
#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))
#define ASSERT_NULL(p) ASSERT_TRUE((p) == NULL)
#define ASSERT_NONNULL(p) ASSERT_TRUE((p) != NULL)
#define ASSERT_GE(a, b) ASSERT_TRUE((a) >= (b))
#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))
#define ASSERT_LE(a, b) ASSERT_TRUE((a) <= (b))
#define ASSERT_LT(a, b) ASSERT_TRUE((a) < (b))

/* ── Non-fatal expectations — record failure, keep running ──────────────── */

#define EXPECT_TRUE(cond)                                       \
	do {                                                    \
		if (!(cond)) {                                  \
			klog(__FILE__ ":%d: Failure\n"          \
				      "  Value of: " #cond "\n" \
				      "  Actual: false\n"       \
				      "  Expected: true\n",     \
			     __LINE__);                         \
			_ktest_expect_failed = __LINE__;        \
		}                                               \
	} while (0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))
#define EXPECT_EQ(a, b) EXPECT_TRUE((a) == (b))
#define EXPECT_NE(a, b) EXPECT_TRUE((a) != (b))
#define EXPECT_NULL(p) EXPECT_TRUE((p) == NULL)
#define EXPECT_NONNULL(p) EXPECT_TRUE((p) != NULL)
#define EXPECT_GE(a, b) EXPECT_TRUE((a) >= (b))
#define EXPECT_GT(a, b) EXPECT_TRUE((a) > (b))
#define EXPECT_LE(a, b) EXPECT_TRUE((a) <= (b))
#define EXPECT_LT(a, b) EXPECT_TRUE((a) < (b))

#endif /* _TEST_TEST_H */
