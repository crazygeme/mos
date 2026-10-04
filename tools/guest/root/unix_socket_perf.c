/*
 * Unix-domain SOCK_STREAM performance benchmark for LFS and Red Hat 9.
 * Build: gcc -O2 -Wall -W -o unix_socket_perf unix_socket_perf.c
 * Run:   ./unix_socket_perf
 * Help:  ./unix_socket_perf --help
 *
 * Separate processes measure one-way throughput and request/reply latency.
 * Socket establishment, allocation, warmup, and process teardown are excluded
 * from measurements. Each throughput repetition transfers for at least two
 * seconds and includes a final receiver acknowledgement. Rates use the actual
 * transferred byte count and elapsed time. MB/s uses 1000000 bytes; MiB/s
 * uses 1048576 bytes. Timing is checked between transfer batches, so a sample
 * can exceed the configured duration by a batch and acknowledgement.
 * Latency is the mean round-trip time, not an estimate of one-way latency.
 * Timing uses gettimeofday(); system time must remain unchanged during a run.
 * Guest scheduling, virtual-machine scheduling, and syscall logging affect
 * results. Small samples may be below the guest timer's effective resolution.
 * No payload comparisons or functional test cases are performed.
 */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long count_t;
static count_t byte_count = 16ULL * 1024 * 1024;
static count_t roundtrips = 10000;
static count_t warmup_roundtrips = 1000;
static size_t block_size = 65536;
static size_t message_size = 64;
static unsigned int repetitions = 3, timeout_seconds = 120;
static unsigned int throughput_seconds = 2;
static pid_t owner_pid;
static volatile sig_atomic_t child_pid;
static char directory[] = "/tmp/unix-socket-perf.XXXXXX";
static char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static int directory_created;

static void cleanup(void)
{
    if (getpid() != owner_pid)
        return;
    if (child_pid > 0)
        kill((pid_t)child_pid, SIGKILL);
    if (socket_path[0])
        unlink(socket_path);
    if (directory_created)
        rmdir(directory);
}

static void fail(const char *operation)
{
    perror(operation);
    cleanup();
    _exit(1);
}

static void stopped(int sig)
{
    static const char timeout_text[] = "Benchmark sample timed out.\n";
    static const char signal_text[] = "Benchmark interrupted.\n";
    if (sig == SIGALRM)
        (void)write(STDERR_FILENO, timeout_text, sizeof(timeout_text) - 1);
    else
        (void)write(STDERR_FILENO, signal_text, sizeof(signal_text) - 1);
    cleanup();
    _exit(128 + sig);
}

static void usage(const char *name)
{
    printf("Usage: %s [options]\n"
           "  --transport both|named|pair  Socket establishment (default both)\n"
           "  --bytes N                   Minimum throughput bytes (16777216)\n"
           "  --seconds N                 Minimum throughput seconds, at least 2 (2)\n"
           "  --block N                   Throughput block bytes (65536)\n"
           "  --roundtrips N              Measured request/reply exchanges (10000)\n"
           "  --message N                 Bytes in each request and reply (64)\n"
           "  --warmup N                  Latency warmup exchanges (1000)\n"
           "  --repeats N                 Samples per benchmark and transport (3)\n"
           "  --timeout N                 Seconds allowed per sample (120)\n"
           "  --help                      Display usage\n"
           "Numeric arguments are decimal integers; block and message sizes\n"
           "must be between 1 and 1048576 bytes. Throughput warmup transfers\n"
           "up to 262144 bytes. Each sample uses a new connection and process.\n"
           "Output is CSV; MB uses 1000000 bytes and MiB uses 1048576 bytes.\n"
           "Throughput runs until both --seconds and --bytes are satisfied.\n"
           "Setup and warmup are excluded. Round-trip time includes both\n"
           "transfers and scheduling.\n"
           "A timeout terminates the sample and benchmark with nonzero status.\n",
           name);
}

static count_t number(const char *text, count_t maximum)
{
    char *end;
    count_t value;
    errno = 0;
    if (!*text || strspn(text, "0123456789") != strlen(text))
        goto invalid;
    value = strtoull(text, &end, 10);
    if (errno || *end || value > maximum)
        goto invalid;
    return value;
invalid:
    fprintf(stderr, "Invalid decimal argument: %s\n", text);
    exit(2);
}

static void transfer(int fd, char *buffer, size_t length, int sending)
{
    ssize_t result;
    while (length) {
        if (sending)
            result = write(fd, buffer, length);
        else
            result = read(fd, buffer, length);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            fail(sending ? "write" : "read");
        }
        if (result == 0) {
            errno = sending ? EIO : ECONNRESET;
            fail(sending ? "zero-length write" : "unexpected end of stream");
        }
        buffer += result;
        length -= (size_t)result;
    }
}

static void bulk(int fd, char *buffer, count_t bytes, int sending)
{
    size_t chunk;
    while (bytes) {
        chunk = bytes < (count_t)block_size ? (size_t)bytes : block_size;
        transfer(fd, buffer, chunk, sending);
        bytes -= chunk;
    }
}

/* Drain the measured stream before acknowledging completion. */
static void drain(int fd, char *buffer)
{
    ssize_t received;
    for (;;) {
        received = read(fd, buffer, block_size);
        if (received > 0)
            continue;
        if (received == 0)
            return;
        if (errno != EINTR)
            fail("read throughput stream");
    }
}

static void timestamp(struct timeval *value)
{
    if (gettimeofday(value, NULL) < 0)
        fail("gettimeofday");
}

static double interval(const struct timeval *start, const struct timeval *end)
{
    return (double)(end->tv_sec - start->tv_sec)
         + (double)(end->tv_usec - start->tv_usec) / 1000000.0;
}

static double elapsed(const struct timeval *start, const struct timeval *end)
{
    double seconds = interval(start, end);
    if (seconds <= 0.0) {
        fputs("No positive elapsed time; increase the workload and check the clock.\n",
              stderr);
        cleanup();
        exit(1);
    }
    return seconds;
}

static void peer(int fd, char *buffer, int latency)
{
    count_t i, warm_bytes;
    char acknowledgement = 0;
    alarm(timeout_seconds);
    if (latency) {
        for (i = 0; i < warmup_roundtrips + roundtrips; ++i) {
            transfer(fd, buffer, message_size, 0);
            transfer(fd, buffer, message_size, 1);
        }
    } else {
        warm_bytes = byte_count < 262144 ? byte_count : 262144;
        bulk(fd, buffer, warm_bytes, 0);
        transfer(fd, &acknowledgement, 1, 1);
        drain(fd, buffer);
        transfer(fd, &acknowledgement, 1, 1);
    }
    close(fd);
    _exit(0);
}

static int connection(int named, char *buffer, int latency)
{
    int pair[2], listener = -1, fd;
    pid_t pid;
    struct sockaddr_un address;
    socklen_t address_length;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    address_length = sizeof(address);
    if (named) {
        strcpy(address.sun_path, socket_path);
        listener = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listener < 0)
            fail("socket listener");
        if (bind(listener, (struct sockaddr *)&address, address_length) < 0)
            fail("bind");
        if (listen(listener, 1) < 0)
            fail("listen");
    } else if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        fail("socketpair");
    }
    pid = fork();
    if (pid < 0)
        fail("fork");
    if (pid == 0) {
        alarm(timeout_seconds);
        if (named) {
            close(listener);
            fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0)
                fail("socket client");
            if (connect(fd, (struct sockaddr *)&address, address_length) < 0)
                fail("connect");
        } else {
            close(pair[0]);
            fd = pair[1];
        }
        peer(fd, buffer, latency);
    }
    child_pid = pid;
    if (named) {
        do {
            fd = accept(listener, NULL, NULL);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0)
            fail("accept");
        close(listener);
        if (unlink(socket_path) < 0)
            fail("unlink socket");
    } else {
        close(pair[1]);
        fd = pair[0];
    }
    return fd;
}

static void sample(int named, int latency, unsigned int repetition,
                   char *buffer)
{
    int fd, status;
    pid_t waited;
    count_t i, warm_bytes, transferred = 0, batch_bytes;
    char acknowledgement;
    struct timeval start, end;
    double seconds;

    fflush(stdout);
    alarm(timeout_seconds);
    fd = connection(named, buffer, latency);
    if (latency) {
        for (i = 0; i < warmup_roundtrips; ++i) {
            transfer(fd, buffer, message_size, 1);
            transfer(fd, buffer, message_size, 0);
        }
        timestamp(&start);
        for (i = 0; i < roundtrips; ++i) {
            transfer(fd, buffer, message_size, 1);
            transfer(fd, buffer, message_size, 0);
        }
        timestamp(&end);
    } else {
        warm_bytes = byte_count < 262144 ? byte_count : 262144;
        bulk(fd, buffer, warm_bytes, 1);
        transfer(fd, &acknowledgement, 1, 0);
        timestamp(&start);
        batch_bytes = block_size < 262144 ?
            (262144 / block_size) * block_size : block_size;
        do {
            bulk(fd, buffer, batch_bytes, 1);
            transferred += batch_bytes;
            timestamp(&end);
        } while (interval(&start, &end) < (double)throughput_seconds ||
                 transferred < byte_count);
        if (shutdown(fd, SHUT_WR) < 0)
            fail("shutdown throughput writer");
        transfer(fd, &acknowledgement, 1, 0);
        timestamp(&end);
    }
    close(fd);
    do {
        waited = waitpid((pid_t)child_pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0)
        fail("waitpid");
    child_pid = 0;
    alarm(0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fputs("Peer process did not complete successfully.\n", stderr);
        cleanup();
        exit(1);
    }
    seconds = elapsed(&start, &end);
    if (latency)
        printf("%s,latency,%lu,%llu,%u,%.6f,,,%.3f,%.3f\n",
               named ? "named" : "pair", (unsigned long)message_size,
               roundtrips, repetition, seconds,
               (double)roundtrips / seconds,
               seconds * 1000000.0 / (double)roundtrips);
    else
        printf("%s,throughput,%lu,%llu,%u,%.6f,%.3f,%.3f,,\n",
               named ? "named" : "pair", (unsigned long)block_size,
               transferred, repetition, seconds,
               (double)transferred / 1000000.0 / seconds,
               (double)transferred / 1048576.0 / seconds);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int i, named, transport = 2;
    unsigned int repetition;
    size_t allocation;
    char *buffer;
    const char *option, *value;
    struct sigaction action;

    for (i = 1; i < argc; ++i) {
        option = argv[i];
        if (!strcmp(option, "--help") || !strcmp(option, "-h")) {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 == argc) {
            fprintf(stderr, "Missing value for option: %s\n", option);
            return 2;
        }
        value = argv[++i];
        if (!strcmp(option, "--transport")) {
            if (!strcmp(value, "named")) transport = 1;
            else if (!strcmp(value, "pair")) transport = 0;
            else if (!strcmp(value, "both")) transport = 2;
            else { fputs("Invalid transport.\n", stderr); return 2; }
        } else if (!strcmp(option, "--bytes"))
            byte_count = number(value, 1ULL << 40);
        else if (!strcmp(option, "--seconds"))
            throughput_seconds = (unsigned int)number(value, 86400);
        else if (!strcmp(option, "--block"))
            block_size = (size_t)number(value, 1048576);
        else if (!strcmp(option, "--message"))
            message_size = (size_t)number(value, 1048576);
        else if (!strcmp(option, "--roundtrips"))
            roundtrips = number(value, 1000000000);
        else if (!strcmp(option, "--warmup"))
            warmup_roundtrips = number(value, 1000000000);
        else if (!strcmp(option, "--repeats"))
            repetitions = (unsigned int)number(value, 100);
        else if (!strcmp(option, "--timeout"))
            timeout_seconds = (unsigned int)number(value, 86400);
        else { fprintf(stderr, "Unknown option: %s\n", option); return 2; }
    }
    if (!byte_count || !block_size || !message_size || !roundtrips ||
        !repetitions || !timeout_seconds) {
        fputs("All numeric options except --warmup must be positive.\n", stderr);
        return 2;
    }
    if (throughput_seconds < 2) {
        fputs("--seconds must be at least 2.\n", stderr);
        return 2;
    }
    owner_pid = getpid();
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = stopped;
    if (sigaction(SIGALRM, &action, NULL) < 0 ||
        sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0)
        fail("sigaction");
    action.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &action, NULL) < 0)
        fail("sigaction SIGPIPE");
    allocation = block_size > message_size ? block_size : message_size;
    buffer = malloc(allocation);
    if (!buffer)
        fail("malloc");
    memset(buffer, 0x5a, allocation);
    if (transport != 0) {
        if (!mkdtemp(directory))
            fail("mkdtemp");
        directory_created = 1;
        snprintf(socket_path, sizeof(socket_path), "%s/socket", directory);
    }
    puts("transport,benchmark,payload_bytes,bytes_or_roundtrips,repeat,seconds,MB_per_s,MiB_per_s,roundtrips_per_s,mean_rtt_us");
    for (named = 0; named <= 1; ++named) {
        if (transport != 2 && transport != named)
            continue;
        for (repetition = 1; repetition <= repetitions; ++repetition)
            sample(named, 0, repetition, buffer);
        for (repetition = 1; repetition <= repetitions; ++repetition)
            sample(named, 1, repetition, buffer);
    }
    cleanup();
    free(buffer);
    return 0;
}
