#!/bin/sh

set -e
CASE_NAME=posix_pthread
CASE_MTIME=@CASE_MTIME@
BASE=/root/tests/$CASE_NAME
WORKDIR=/root/tests/$CASE_NAME
SRC="$BASE/pthread_test.c"
BIN="$BASE/pthread_test"
STAMP="$BASE/.case_timestamp"

fail()
{
	echo "posix_pthread: $1" >&2
	exit 1
}

expect_success()
{
	set +e
	output=$("$@" 2>&1)
	rc=$?
	set -e
	[ "$rc" -eq 0 ] && return 0
	fail "Expected: success from '$*'
Actual: exit status $rc
Output: ${output:-<none>}"
}

require_cmd()
{
	command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
}

prepare_embedded_c()
{
	mkdir -p "$BASE" "$WORKDIR" >/dev/null 2>&1 || fail "mkdir failed"
	if [ -f "$STAMP" ] && [ -f "$SRC" ] && [ -f "$BIN" ]; then
		stamp_mtime=$(cat "$STAMP" 2>/dev/null || printf '')
		if [ "$stamp_mtime" = "$CASE_MTIME" ]; then
			return 1
		fi
	fi
	return 0
}

update_case_timestamp()
{
	printf '%s\n' "$CASE_MTIME" > "$STAMP" || fail "timestamp write failed"
}

finish()
{
	sync >/dev/null 2>&1 || true
}

trap finish EXIT

require_cmd gcc

if prepare_embedded_c; then
cat > "$SRC" <<'EOF'
#define _XOPEN_SOURCE 600
#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static void fail(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

static void fail_pthread(const char *what, int rc)
{
	fprintf(stderr, "%s: %s\n", what, strerror(rc));
	exit(1);
}

static void check(int cond, const char *msg)
{
	if (!cond)
		fail(msg);
}

static void check_pthread(int rc, const char *what)
{
	if (rc != 0)
		fail_pthread(what, rc);
}

static int counter = 0;
static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;

static void *counter_worker(void *arg)
{
	int id = (int)(long)arg;
	int rc;

	rc = pthread_mutex_lock(&counter_lock);
	if (rc != 0) {
		fprintf(stderr, "pthread_mutex_lock: %s\n", strerror(rc));
		return (void *)1;
	}
	counter += id;
	rc = pthread_mutex_unlock(&counter_lock);
	if (rc != 0) {
		fprintf(stderr, "pthread_mutex_unlock: %s\n", strerror(rc));
		return (void *)2;
	}

	return (void *)(long)(id + 10);
}

static void test_create_join_and_exit(void)
{
	pthread_t th[3];
	void *ret[3] = {NULL, NULL, NULL};
	int rc;

	counter = 0;

	rc = pthread_create(&th[0], NULL, counter_worker, (void *)1);
	check_pthread(rc, "pthread_create #0");
	rc = pthread_create(&th[1], NULL, counter_worker, (void *)2);
	check_pthread(rc, "pthread_create #1");
	rc = pthread_create(&th[2], NULL, counter_worker, (void *)3);
	check_pthread(rc, "pthread_create #2");

	rc = pthread_join(th[0], &ret[0]);
	check_pthread(rc, "pthread_join #0");
	rc = pthread_join(th[1], &ret[1]);
	check_pthread(rc, "pthread_join #1");
	rc = pthread_join(th[2], &ret[2]);
	check_pthread(rc, "pthread_join #2");

	check((long)ret[0] == 11, "unexpected return from thread #0");
	check((long)ret[1] == 12, "unexpected return from thread #1");
	check((long)ret[2] == 13, "unexpected return from thread #2");
	check(counter == 6, "counter mismatch after joins");
}

struct self_equal_arg {
	pthread_t self;
};

static void *self_equal_worker(void *arg)
{
	struct self_equal_arg *ctx = arg;

	ctx->self = pthread_self();
	return 0;
}

static void test_self_and_equal(void)
{
	struct self_equal_arg ctx;
	pthread_t th;
	pthread_t self;
	void *ret = NULL;

	memset(&ctx, 0, sizeof(ctx));
	self = pthread_self();
	check(pthread_equal(self, self) != 0, "pthread_equal(self, self) failed");

	check_pthread(pthread_create(&th, NULL, self_equal_worker, &ctx),
		      "pthread_create self/equal");
	check_pthread(pthread_join(th, &ret), "pthread_join self/equal");
	check(ret == 0, "self/equal worker returned unexpected value");
	check(pthread_equal(th, ctx.self) != 0,
	      "pthread_equal(thread, worker_self) failed");
	check(pthread_equal(self, ctx.self) == 0,
	      "main thread unexpectedly equal to worker thread");
}

static pthread_once_t once_control = PTHREAD_ONCE_INIT;
static int once_counter = 0;
static int once_seen = 0;

static void once_init_routine(void)
{
	once_counter++;
}

static void *once_worker(void *arg)
{
	(void)arg;
	check_pthread(pthread_once(&once_control, once_init_routine),
		      "pthread_once in worker");
	once_seen++;
	return 0;
}

static void test_once(void)
{
	pthread_t th[4];
	void *ret = NULL;
	int i;

	once_control = PTHREAD_ONCE_INIT;
	once_counter = 0;
	once_seen = 0;

	for (i = 0; i < 4; i++)
		check_pthread(pthread_create(&th[i], NULL, once_worker, NULL),
			      "pthread_create once");
	for (i = 0; i < 4; i++) {
		check_pthread(pthread_join(th[i], &ret), "pthread_join once");
		check(ret == 0, "once worker returned unexpected value");
	}

	check(once_counter == 1, "pthread_once ran init routine more than once");
	check(once_seen == 4, "not all pthread_once workers finished");
}

static void test_mutex_basics(void)
{
	pthread_mutex_t lock;
	pthread_mutexattr_t attr;
	int rc;

	check_pthread(pthread_mutex_init(&lock, NULL), "pthread_mutex_init");
	check_pthread(pthread_mutex_lock(&lock), "pthread_mutex_lock");
	rc = pthread_mutex_trylock(&lock);
	check(rc == EBUSY, "pthread_mutex_trylock should report EBUSY");
	check_pthread(pthread_mutex_unlock(&lock), "pthread_mutex_unlock");
	check_pthread(pthread_mutex_destroy(&lock), "pthread_mutex_destroy");

	check_pthread(pthread_mutexattr_init(&attr), "pthread_mutexattr_init");
	check_pthread(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE),
		      "pthread_mutexattr_settype recursive");
	check_pthread(pthread_mutex_init(&lock, &attr),
		      "pthread_mutex_init recursive");
	check_pthread(pthread_mutex_lock(&lock), "recursive mutex first lock");
	check_pthread(pthread_mutex_lock(&lock), "recursive mutex second lock");
	check_pthread(pthread_mutex_unlock(&lock), "recursive mutex first unlock");
	check_pthread(pthread_mutex_unlock(&lock), "recursive mutex second unlock");
	check_pthread(pthread_mutex_destroy(&lock), "recursive pthread_mutex_destroy");
	check_pthread(pthread_mutexattr_destroy(&attr),
		      "pthread_mutexattr_destroy");
}

static pthread_mutex_t cond_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_var = PTHREAD_COND_INITIALIZER;
static int cond_started = 0;
static int cond_ready = 0;

static void *cond_waiter(void *arg)
{
	(void)arg;
	check_pthread(pthread_mutex_lock(&cond_lock), "cond waiter lock");
	cond_started = 1;
	while (!cond_ready)
		check_pthread(pthread_cond_wait(&cond_var, &cond_lock),
			      "pthread_cond_wait");
	check_pthread(pthread_mutex_unlock(&cond_lock), "cond waiter unlock");
	return (void *)7;
}

static void test_cond_wait_and_signal(void)
{
	pthread_t th;
	void *ret = NULL;
	int spins = 0;

	cond_started = 0;
	cond_ready = 0;

	check_pthread(pthread_create(&th, NULL, cond_waiter, NULL),
		      "pthread_create cond waiter");

	while (!cond_started) {
		check(spins++ < 5000, "condition waiter never started");
		sched_yield();
		usleep(1000);
	}

	check_pthread(pthread_mutex_lock(&cond_lock), "cond signaler lock");
	cond_ready = 1;
	check_pthread(pthread_cond_signal(&cond_var), "pthread_cond_signal");
	check_pthread(pthread_mutex_unlock(&cond_lock), "cond signaler unlock");

	check_pthread(pthread_join(th, &ret), "pthread_join cond waiter");
	check((long)ret == 7, "condition waiter returned unexpected value");
}

static void test_cond_timedwait_timeout(void)
{
	pthread_mutex_t lock;
	pthread_cond_t cond;
	struct timeval tv;
	struct timespec ts;
	int rc;

	check_pthread(pthread_mutex_init(&lock, NULL), "timedwait mutex init");
	check_pthread(pthread_cond_init(&cond, NULL), "timedwait cond init");
	check_pthread(pthread_mutex_lock(&lock), "timedwait mutex lock");

	check(gettimeofday(&tv, NULL) == 0, "gettimeofday failed");
	ts.tv_sec = tv.tv_sec + 1;
	ts.tv_nsec = tv.tv_usec * 1000;
	rc = pthread_cond_timedwait(&cond, &lock, &ts);
	check(rc == ETIMEDOUT,
	      "pthread_cond_timedwait should time out with ETIMEDOUT");

	check_pthread(pthread_mutex_unlock(&lock), "timedwait mutex unlock");
	check_pthread(pthread_cond_destroy(&cond), "timedwait cond destroy");
	check_pthread(pthread_mutex_destroy(&lock), "timedwait mutex destroy");
}

static volatile int detached_done = 0;

static void *detached_worker(void *arg)
{
	(void)arg;
	detached_done = 1;
	return (void *)99;
}

static void test_thread_attr_and_detach(void)
{
	pthread_attr_t attr;
	pthread_t th;
	int detachstate = -1;
	size_t stacksize = 0;
	int spins = 0;

	check_pthread(pthread_attr_init(&attr), "pthread_attr_init");
	check_pthread(pthread_attr_getdetachstate(&attr, &detachstate),
		      "pthread_attr_getdetachstate");
	check(detachstate == PTHREAD_CREATE_JOINABLE,
	      "default detach state is not joinable");

	check_pthread(pthread_attr_getstacksize(&attr, &stacksize),
		      "pthread_attr_getstacksize");
#ifdef PTHREAD_STACK_MIN
	check(stacksize >= PTHREAD_STACK_MIN,
	      "default stack size smaller than PTHREAD_STACK_MIN");
#else
	check(stacksize > 0, "default stack size should be positive");
#endif

	check_pthread(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED),
		      "pthread_attr_setdetachstate detached");
	check_pthread(pthread_create(&th, &attr, detached_worker, NULL),
		      "pthread_create detached");
	check_pthread(pthread_attr_destroy(&attr), "pthread_attr_destroy");

	while (!detached_done) {
		check(spins++ < 5000, "detached thread never completed");
		sched_yield();
		usleep(1000);
	}
}

static pthread_key_t tls_key;
static int tls_destructor_calls = 0;
static long tls_destructor_value = 0;

static void tls_destructor(void *ptr)
{
	tls_destructor_calls++;
	tls_destructor_value = (long)ptr;
}

static void *tls_worker(void *arg)
{
	(void)arg;
	check_pthread(pthread_setspecific(tls_key, (void *)1234),
		      "pthread_setspecific");
	check((long)pthread_getspecific(tls_key) == 1234,
	      "pthread_getspecific returned wrong value");
	return 0;
}

static void test_thread_specific_data(void)
{
	pthread_t th;
	void *ret = NULL;

	tls_destructor_calls = 0;
	tls_destructor_value = 0;

	check_pthread(pthread_key_create(&tls_key, tls_destructor),
		      "pthread_key_create");
	check(pthread_getspecific(tls_key) == NULL,
	      "new pthread key should read as NULL in main thread");
	check_pthread(pthread_create(&th, NULL, tls_worker, NULL),
		      "pthread_create tls");
	check_pthread(pthread_join(th, &ret), "pthread_join tls");
	check(ret == 0, "tls worker returned unexpected value");
	check(tls_destructor_calls == 1,
	      "thread specific data destructor call count mismatch");
	check(tls_destructor_value == 1234,
	      "thread specific data destructor saw wrong value");
	check_pthread(pthread_key_delete(tls_key), "pthread_key_delete");
}

static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
static volatile int rw_writer_blocked = 0;
static volatile int rw_reader_blocked = 0;

static void *rw_try_writer(void *arg)
{
	int rc;

	(void)arg;
	rc = pthread_rwlock_trywrlock(&rwlock);
	if (rc == 0) {
		pthread_rwlock_unlock(&rwlock);
		return (void *)1;
	}
	if (rc != EBUSY)
		return (void *)2;
	rw_writer_blocked = 1;
	return 0;
}

static void *rw_try_reader(void *arg)
{
	int rc;

	(void)arg;
	rc = pthread_rwlock_tryrdlock(&rwlock);
	if (rc == 0) {
		pthread_rwlock_unlock(&rwlock);
		return (void *)1;
	}
	if (rc != EBUSY)
		return (void *)2;
	rw_reader_blocked = 1;
	return 0;
}

static void test_rwlock(void)
{
	pthread_t th;
	void *ret = NULL;

	rw_writer_blocked = 0;
	rw_reader_blocked = 0;

	check_pthread(pthread_rwlock_rdlock(&rwlock), "pthread_rwlock_rdlock");
	check_pthread(pthread_create(&th, NULL, rw_try_writer, NULL),
		      "pthread_create rw writer");
	check_pthread(pthread_join(th, &ret), "pthread_join rw writer");
	check(ret == 0, "rw writer probe returned unexpected value");
	check(rw_writer_blocked == 1, "rw writer was not blocked by read lock");
	check_pthread(pthread_rwlock_unlock(&rwlock), "pthread_rwlock_unlock read");

	check_pthread(pthread_rwlock_wrlock(&rwlock), "pthread_rwlock_wrlock");
	check_pthread(pthread_create(&th, NULL, rw_try_reader, NULL),
		      "pthread_create rw reader");
	check_pthread(pthread_join(th, &ret), "pthread_join rw reader");
	check(ret == 0, "rw reader probe returned unexpected value");
	check(rw_reader_blocked == 1, "rw reader was not blocked by write lock");
	check_pthread(pthread_rwlock_unlock(&rwlock), "pthread_rwlock_unlock write");
}

int main(void)
{
	test_create_join_and_exit();
	test_self_and_equal();
	test_once();
	test_mutex_basics();
	test_cond_wait_and_signal();
	test_cond_timedwait_timeout();
	test_thread_attr_and_detach();
	test_thread_specific_data();
	test_rwlock();

	check_pthread(pthread_rwlock_destroy(&rwlock), "pthread_rwlock_destroy");
	check_pthread(pthread_mutex_destroy(&cond_lock), "pthread_mutex_destroy cond");
	check_pthread(pthread_cond_destroy(&cond_var), "pthread_cond_destroy cond");
	check_pthread(pthread_mutex_destroy(&counter_lock),
		      "pthread_mutex_destroy counter");

	puts("pthread ok");
	return 0;
}
EOF

expect_success gcc -Wall -Werror -pthread -o "$BIN" "$SRC"
update_case_timestamp
fi
[ -x "$BIN" ] || fail "compiled binary missing"

output=$("$BIN") || fail "pthread helper exited non-zero"
[ "$output" = "pthread ok" ] || fail "Expected output 'pthread ok'
Actual: ${output:-<none>}"
