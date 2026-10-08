#!/bin/sh
set -eu
BASE=$(mktemp -d /tmp/mos-fifo-jobserver.XXXXXX)
trap 'rm -rf "$BASE"' EXIT
cat > "$BASE/probe.c" <<'EOF'
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FIFO/getcwd check failed at line %d: %s (errno=%d)\n", \
            __LINE__, #expr, errno); exit(1); } } while (0)

int main(int argc, char **argv)
{
    char cwd[4096], small[1] = {'X'}, token;
    struct stat st;
    long n;
    int reader, writer;

    CHECK(argc == 2);
    if (geteuid() == 0) {
        CHECK(chown(argv[1], 65534, 65534) == 0);
        CHECK(setgid(65534) == 0);
        CHECK(setuid(65534) == 0);
    }
    CHECK(geteuid() != 0);
    CHECK(chdir(argv[1]) == 0);
    n = syscall(SYS_getcwd, cwd, sizeof(cwd));
    CHECK(n > 0 && n == (long)strlen(argv[1]) + 1);
    CHECK(strcmp(cwd, argv[1]) == 0);
    CHECK(getcwd(cwd, sizeof(cwd)) == cwd);
    CHECK(strcmp(cwd, argv[1]) == 0);
    errno = 0;
    CHECK(syscall(SYS_getcwd, small, sizeof(small)) == -1);
    CHECK(errno == ERANGE && small[0] == 'X');
    errno = 0;
    CHECK(syscall(SYS_getcwd, small, 0) == -1 && errno == EINVAL);

    CHECK(mkfifo("fifo", 0600) == 0);
    CHECK(stat("fifo", &st) == 0 && S_ISFIFO(st.st_mode));
    CHECK(st.st_uid == geteuid() && st.st_gid == getegid());
    CHECK((st.st_mode & 0777) == 0600);
    reader = open("fifo", O_RDONLY | O_NONBLOCK);
    CHECK(reader >= 0);
    CHECK(fstat(reader, &st) == 0);
    CHECK(st.st_uid == geteuid() && st.st_gid == getegid());
    writer = open("fifo", O_WRONLY);
    CHECK(writer >= 0 && write(writer, "+", 1) == 1);
    CHECK(read(reader, &token, 1) == 1 && token == '+');
    CHECK(unlink("fifo") == 0);
    errno = 0;
    CHECK(stat("fifo", &st) == -1 && errno == ENOENT);
    CHECK(write(writer, "-", 1) == 1);
    CHECK(read(reader, &token, 1) == 1 && token == '-');
    CHECK(close(writer) == 0 && close(reader) == 0);
    puts("FIFO jobserver and getcwd checks passed.");
    return 0;
}
EOF
gcc -Wall -O2 "$BASE/probe.c" -o "$BASE/probe"
"$BASE/probe" "$BASE"
