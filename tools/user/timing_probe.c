#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#define CHECK(x)                                                       \
	do {                                                           \
		if (!(x)) {                                            \
			fprintf(stderr, "FAIL line %d: %s errno=%d\n", \
				__LINE__, #x, errno);                  \
			exit(1);                                       \
		}                                                      \
	} while (0)
static volatile sig_atomic_t alarm_seen;
/* Exercise the RH9 settimeofday ABI directly rather than a libc shim that
 * may choose a newer clock_settime syscall. */
static int set_wall(const struct timeval *tv)
{
	return syscall(SYS_settimeofday, tv, NULL);
}
static void alarm_handler(int sig)
{
	(void)sig;
	alarm_seen++;
}
static uint64_t monotonic_us(void)
{
	struct timespec ts;
	CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void pause_ms(unsigned ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
	while (nanosleep(&ts, &ts) < 0)
		CHECK(errno == EINTR);
}
static void restore_wall(const struct timeval *saved, uint64_t start)
{
	uint64_t us = (uint64_t)saved->tv_sec * 1000000 + saved->tv_usec +
		      monotonic_us() - start;
	struct timeval tv = { us / 1000000, us % 1000000 };
	CHECK(set_wall(&tv) == 0);
}
static void reap(pid_t pid)
{
	int status;
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static pid_t jump_wall(int direction)
{
	pid_t pid = fork();
	CHECK(pid >= 0);
	if (!pid) {
		struct timeval tv;
		pause_ms(20);
		CHECK(gettimeofday(&tv, NULL) == 0);
		tv.tv_sec += direction * 3600;
		CHECK(set_wall(&tv) == 0);
		_exit(0);
	}
	return pid;
}
static void check_elapsed(uint64_t start, unsigned ms)
{
	uint64_t elapsed = monotonic_us() - start;
	CHECK(elapsed >= (uint64_t)ms * 1000);
	CHECK(elapsed < (uint64_t)ms * 1000 + 300000);
}
static void clock_reads(void)
{
	uint64_t read_start = monotonic_us(), previous = read_start;
	for (unsigned i = 0; i < 100000; i++) {
		uint64_t now = monotonic_us();
		CHECK(now >= previous);
		previous = now;
	}
	printf("CLOCK_READ average_ns=%llu count=100000\n",
	       (unsigned long long)((previous - read_start) * 1000 / 100000));
	struct timespec tiny = { 0, 1 };
	uint64_t start = monotonic_us();
	CHECK(nanosleep(&tiny, NULL) == 0);
	CHECK(monotonic_us() > start);
	struct sysinfo si;
	struct tms tm;
	CHECK(sysinfo(&si) == 0 && si.uptime >= 0 && si.uptime < 86400);
	CHECK(times(&tm) >= 0 && times(&tm) < 8640000);
	puts("PASS monotonic reads, nonzero short sleep, boot-relative uptime/times");
}
static void deadline_tests(int direction)
{
	struct timeval saved;
	uint64_t start;
	pid_t child;
	CHECK(gettimeofday(&saved, NULL) == 0);
	start = monotonic_us();
	child = jump_wall(direction);
	CHECK(poll(NULL, 0, 80) == 0);
	check_elapsed(start, 80);
	reap(child);
	restore_wall(&saved, start);

	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(fd >= 0);
	struct sockaddr_in address = { .sin_family = AF_INET,
				       .sin_addr.s_addr =
					       htonl(INADDR_LOOPBACK),
				       .sin_port = 0 };
	CHECK(bind(fd, (void *)&address, sizeof(address)) == 0);
	struct timeval timeout = { 0, 80000 };
	CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
			 sizeof(timeout)) == 0);
	CHECK(gettimeofday(&saved, NULL) == 0);
	start = monotonic_us();
	child = jump_wall(direction);
	char buffer;
	CHECK(recv(fd, &buffer, 1, 0) == -1 && errno == EAGAIN);
	check_elapsed(start, 80);
	reap(child);
	restore_wall(&saved, start);
	close(fd);

	CHECK(gettimeofday(&saved, NULL) == 0);
	start = monotonic_us();
	child = jump_wall(direction);
	pause_ms(80);
	check_elapsed(start, 80);
	reap(child);
	restore_wall(&saved, start);

	struct sigaction sa = { .sa_handler = alarm_handler };
	sigemptyset(&sa.sa_mask);
	CHECK(sigaction(SIGALRM, &sa, NULL) == 0);
	struct itimerval timer = { .it_value = { 0, 80000 } };
	alarm_seen = 0;
	CHECK(gettimeofday(&saved, NULL) == 0);
	start = monotonic_us();
	CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
	child = jump_wall(direction);
	CHECK(poll(NULL, 0, 500) == -1 && errno == EINTR);
	CHECK(alarm_seen == 1);
	check_elapsed(start, 80);
	reap(child);
	restore_wall(&saved, start);
	printf("PASS wall jump %s: poll, socket EAGAIN, nanosleep, ITIMER_REAL\n",
	       direction > 0 ? "forward" : "backward");
}
static void posix_timer(int clockid, int direction)
{
	struct sigevent ev = { .sigev_notify = SIGEV_SIGNAL,
			       .sigev_signo = SIGALRM };
	timer_t id;
	CHECK(timer_create(clockid, &ev, &id) == 0);
	struct itimerspec value = { .it_value = { 0, 80000000 } };
	alarm_seen = 0;
	struct timeval saved;
	CHECK(gettimeofday(&saved, NULL) == 0);
	uint64_t start = monotonic_us();
	CHECK(timer_settime(id, 0, &value, NULL) == 0);
	pid_t child = jump_wall(direction);
	CHECK(poll(NULL, 0, 500) == -1 && errno == EINTR);
	CHECK(alarm_seen == 1);
	check_elapsed(start, 80);
	CHECK(timer_delete(id) == 0);
	reap(child);
	restore_wall(&saved, start);
	printf("PASS POSIX relative timer clock=%d wall_jump=%d\n", clockid,
	       direction);
}
static void absolute_timer(void)
{
	struct timeval saved;
	CHECK(gettimeofday(&saved, NULL) == 0);
	struct sigevent event = { .sigev_notify = SIGEV_SIGNAL,
				  .sigev_signo = SIGALRM };
	timer_t id;
	CHECK(timer_create(CLOCK_REALTIME, &event, &id) == 0);
	uint64_t start = monotonic_us();
	struct timespec wall;
	CHECK(clock_gettime(CLOCK_REALTIME, &wall) == 0);
	struct itimerspec value = { .it_value = wall };
	value.it_value.tv_sec += 3600;
	value.it_value.tv_nsec += 80000000;
	if (value.it_value.tv_nsec >= 1000000000) {
		value.it_value.tv_sec++;
		value.it_value.tv_nsec -= 1000000000;
	}
	alarm_seen = 0;
	CHECK(timer_settime(id, TIMER_ABSTIME, &value, NULL) == 0);
	pid_t child = jump_wall(1);
	CHECK(poll(NULL, 0, 500) == -1 && errno == EINTR);
	CHECK(alarm_seen == 1);
	check_elapsed(start, 80);
	CHECK(timer_delete(id) == 0);
	reap(child);
	restore_wall(&saved, start);
	puts("PASS absolute REALTIME timer follows wall adjustment");
}
static void busy_alarm(void)
{
	struct itimerval timer = { .it_value = { 0, 80000 } };
	alarm_seen = 0;
	uint64_t start = monotonic_us();
	CHECK(setitimer(ITIMER_REAL, &timer, NULL) == 0);
	while (!alarm_seen)
		__asm__ volatile("" ::: "memory");
	check_elapsed(start, 80);
	puts("PASS ITIMER_REAL delivery during a userspace CPU loop");
}
static void ping_test(const char *address, int expect_success)
{
	pid_t child = fork();
	CHECK(child >= 0);
	if (!child) {
		execl("/bin/ping", "ping", "-n", "-c", "5", "-i", "0.2", "-w",
		      "3", address, NULL);
		_exit(127);
	}
	int status;
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
	CHECK(WEXITSTATUS(status) == (expect_success ? 0 : 1));
	printf("PASS RH9 ping %s: %s\n", address,
	       expect_success ? "five replies" : "bounded timeout");
}
static void network_ready(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(fd >= 0);
	uint64_t start = monotonic_us();
	for (;;) {
		struct ifreq interfaces[8];
		struct ifconf config = { .ifc_len = sizeof(interfaces),
					 .ifc_req = interfaces };
		CHECK(ioctl(fd, SIOCGIFCONF, &config) == 0);
		for (unsigned i = 0;
		     i < (unsigned)config.ifc_len / sizeof(struct ifreq); i++) {
			struct sockaddr_in *addr =
				(void *)&interfaces[i].ifr_addr;
			if (addr->sin_family == AF_INET &&
			    addr->sin_addr.s_addr &&
			    addr->sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
				printf("NETWORK_READY elapsed_ms=%llu\n",
				       (unsigned long long)((monotonic_us() -
							     start) /
							    1000));
				close(fd);
				return;
			}
		}
		CHECK(monotonic_us() - start < 15000000);
		pause_ms(20);
	}
}
int main(void)
{
	if (getpid() == 1) {
		mount("proc", "/proc", "proc", 0, NULL);
		int fd = open("/proc/tests/.result", O_WRONLY);
		CHECK(fd >= 0);
		dup2(fd, 1);
		dup2(fd, 2);
		close(fd);
		fd = open("/proc/tests/.runner", O_WRONLY);
		CHECK(fd >= 0);
		CHECK(write(fd, "Timekeeping", 11) == 11);
		close(fd);
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	clock_reads();
	deadline_tests(1);
	deadline_tests(-1);
	posix_timer(CLOCK_MONOTONIC, 1);
	posix_timer(CLOCK_REALTIME, 1);
	posix_timer(CLOCK_REALTIME, -1);
	absolute_timer();
	busy_alarm();
	if (access("/bin/ping", X_OK) == 0) {
		ping_test("127.0.0.1", 1);
		network_ready();
		ping_test("10.0.2.2", 1);
		ping_test("10.0.2.99", 0);
	}
	puts("TIMING_PROBE_COMPLETE PASS");
	if (getpid() == 1) {
		sync();
		reboot(RB_POWER_OFF);
		for (;;)
			pause();
	}
	return 0;
}
