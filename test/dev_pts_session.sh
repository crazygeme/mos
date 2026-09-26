#!/bin/sh
set -eu
BASE=/root/tests/dev_pts_session
mkdir -p "$BASE"
trap 'rm -rf "$BASE"' EXIT
cat > "$BASE/session.c" <<'EOF'
#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define PT_GET2 0x802c542aUL
#define PT_SET2 0x402c542bUL
#define PT_SETW2 0x402c542cUL
#define PT_SETF2 0x402c542dUL
#define PT_PEER 0x5441UL
#define PT_BOTHER 0x1000U
#define PT_CBAUD 0x100fU
#define PT_CIBAUD 0x100f0000U
struct linux_termios2 {
    unsigned iflag, oflag, cflag, lflag;
    unsigned char line, cc[19];
    unsigned ispeed, ospeed;
};
typedef char termios2_size_check[sizeof(struct linux_termios2) == 44 ? 1 : -1];

static void check(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "dev_pts_session: %s (errno=%d)\n", message, errno);
        exit(1);
    }
}

int main(void)
{
    int master, slave, status, found = 0;
    pid_t child;
    char path[64], link[64], fdpath[64], output[128];
    ssize_t length;
    const char *name;
    size_t used = 0;
    struct termios tc;
    struct stat by_fd, by_path;
    struct linux_termios2 extended, expected;
    struct dirent *entry;
    DIR *directory;
    unsigned long commands[] = { PT_SET2, PT_SETW2, PT_SETF2 };
    unsigned i;

    alarm(15);
    master = posix_openpt(O_RDWR | O_NOCTTY);
    check(master >= 0, "posix_openpt");
    check(grantpt(master) == 0 && unlockpt(master) == 0, "unlock slave");
    check(ptsname_r(master, path, sizeof(path)) == 0, "ptsname_r");
    slave = ioctl(master, PT_PEER, O_RDWR | O_NOCTTY);
    check(slave >= 0, "TIOCGPTPEER");
    check(tcgetattr(slave, &tc) == 0, "tcgetattr on peer descriptor");
    check(isatty(slave), "isatty on peer descriptor");
    name = ttyname(slave);
    check(name != NULL && strcmp(name, path) == 0, "ttyname on peer descriptor");
    check(ttyname_r(slave, link, sizeof(link)) == 0, "ttyname_r");
    check(strcmp(path, link) == 0, "ttyname path");
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", slave);
    length = readlink(fdpath, link, sizeof(link) - 1);
    check(length >= 0, "readlink peer descriptor");
    link[length] = 0;
    check(strcmp(path, link) == 0, "proc peer path");
    check(fstat(slave, &by_fd) == 0 && stat(path, &by_path) == 0, "PTY stat");
    check(by_fd.st_ino == by_path.st_ino && by_fd.st_dev == by_path.st_dev &&
          by_fd.st_rdev == by_path.st_rdev, "PTY identity");
    directory = opendir("/dev/pts");
    check(directory != NULL, "opendir devpts");
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, strrchr(path, '/') + 1) == 0) {
            check(entry->d_ino == by_fd.st_ino, "devpts directory inode");
            found = 1;
        }
    }
    closedir(directory);
    check(found, "devpts slave directory entry");

    memset(&extended, 0xa5, sizeof(extended));
    check(ioctl(slave, PT_GET2, &extended) == 0, "TCGETS2");
    check(extended.ispeed == 38400 && extended.ospeed == 38400, "default speeds");
    extended.cflag = (extended.cflag & ~(PT_CBAUD | PT_CIBAUD)) |
                    PT_BOTHER | (PT_BOTHER << 16);
    extended.ispeed = 12345;
    extended.ospeed = 23456;
    extended.cc[17] = 21;
    extended.cc[18] = 22;
    expected = extended;
    for (i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        check(ioctl(slave, commands[i], &expected) == 0, "set termios2");
        memset(&extended, 0xa5, sizeof(extended));
        check(ioctl(master, PT_GET2, &extended) == 0, "read master termios2");
        check(memcmp(&extended, &expected, sizeof(extended)) == 0,
              "complete termios2 round trip");
    }
    cfmakeraw(&tc);
    check(cfsetispeed(&tc, B38400) == 0 && cfsetospeed(&tc, B38400) == 0,
          "set libc speeds");
    check(tcsetattr(slave, TCSANOW, &tc) == 0, "tcsetattr TCSANOW");
    check(tcsetattr(slave, TCSADRAIN, &tc) == 0, "tcsetattr TCSADRAIN");
    check(write(master, "discard", 7) == 7, "queue input");
    check(tcsetattr(slave, TCSAFLUSH, &tc) == 0, "tcsetattr TCSAFLUSH");
    check(fcntl(slave, F_SETFL, O_NONBLOCK) == 0, "set nonblocking");
    errno = 0;
    check(read(slave, output, sizeof(output)) == -1 && errno == EAGAIN,
          "TCSAFLUSH removes queued input");
    check(fcntl(slave, F_SETFL, 0) == 0, "set blocking");

    child = fork();
    check(child >= 0, "fork shell");
    if (child == 0) {
        close(master);
        check(setsid() >= 0, "setsid");
        check(ioctl(slave, TIOCSCTTY, 0) == 0, "TIOCSCTTY");
        check(dup2(slave, 0) == 0 && dup2(slave, 1) == 1 && dup2(slave, 2) == 2,
              "attach shell descriptors");
        if (slave > 2)
            close(slave);
        check(isatty(0), "shell stdin is a terminal");
        sleep(1);
        execl("/bin/sh", "sh", "-c",
              "printf 'ready\\n'; IFS= read -r line; printf '%s\\n' \"$line\"",
              (char *)NULL);
        _exit(127);
    }
    close(slave);
    check(write(master, "roundtrip\n", 10) == 10, "send shell input");
    while (used < sizeof("ready\nroundtrip\n") - 1) {
        struct pollfd watch = { master, POLLIN, 0 };
        check(poll(&watch, 1, 3000) == 1, "wait for shell output");
        check(!(watch.revents & (POLLERR | POLLNVAL)), "PTY poll status");
        length = read(master, output + used, sizeof(output) - 1 - used);
        check(length > 0, "read shell output");
        used += length;
        check(used < sizeof(output) - 1, "shell output length");
    }
    output[used] = 0;
    check(strcmp(output, "ready\nroundtrip\n") == 0, "shell output contents");
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0, "shell exit status");
    close(master);
    return 0;
}
EOF
gcc -Wall -Wextra -o "$BASE/session" "$BASE/session.c"
"$BASE/session"
