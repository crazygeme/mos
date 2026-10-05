#!/bin/sh
set -eu
BASE=$(mktemp -d /tmp/mos-unlink-open.XXXXXX)
trap 'rm -rf "$BASE"' EXIT
cat > "$BASE/probe.c" <<'EOF'
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/vfs.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "Open-inode lifetime check failed at line %d: %s\n", \
            __LINE__, #expr); exit(1); } } while (0)

static void links(int fd, unsigned count)
{
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && st.st_nlink == count);
}

static void payload(int fd, const char *expected)
{
    char buf[16] = {0};
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, buf, strlen(expected)) == (ssize_t)strlen(expected));
    CHECK(!memcmp(buf, expected, strlen(expected)));
}

int main(int argc, char **argv)
{
    int a, b, replacement, duplicate, status;
    char *mapping;
    struct stat st;
    struct statfs before, after;
    DIR *dir;
    struct dirent *entry;
    pid_t child;
    CHECK(argc == 2 && chdir(argv[1]) == 0);
    CHECK(statfs(".", &before) == 0);
    a = open("file", O_CREAT | O_TRUNC | O_RDWR, 0600);
    CHECK(a >= 0 && write(a, "original", 8) == 8);
    CHECK(ftruncate(a, 4096) == 0 && link("file", "alias") == 0);
    b = open("alias", O_RDWR);
    CHECK(b >= 0);
    duplicate = dup(b);
    CHECK(duplicate >= 0);
    mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, b, 0);
    CHECK(mapping != MAP_FAILED);
    CHECK(rename("file", "alias") == 0);
    CHECK(rename("file", "file") == 0);
    links(a, 2);
    CHECK(unlink("file") == 0);
    links(a, 1);
    CHECK(unlink("alias") == 0);
    links(a, 0);
    links(b, 0);
    CHECK(access("file", F_OK) != 0 && access("alias", F_OK) != 0);
    dir = opendir(".");
    CHECK(dir != NULL);
    while ((entry = readdir(dir)) != NULL)
        CHECK(strncmp(entry->d_name, ".mos-unlinked-", 14) != 0);
    CHECK(closedir(dir) == 0);
    CHECK(close(a) == 0);
    payload(b, "original");
    CHECK(lseek(b, 0, SEEK_SET) == 0 && write(b, "retained", 8) == 8);
    payload(duplicate, "retained");
    CHECK(close(b) == 0);
    payload(duplicate, "retained");
    CHECK(close(duplicate) == 0);
    CHECK(!memcmp(mapping, "retained", 8));
    memcpy(mapping, "mapped!!", 8);
    replacement = open("file", O_CREAT | O_TRUNC | O_RDWR, 0600);
    CHECK(replacement >= 0 && write(replacement, "new", 3) == 3);
    CHECK(!memcmp(mapping, "mapped!!", 8));
    CHECK(munmap(mapping, 4096) == 0);
    payload(replacement, "new");
    CHECK(close(replacement) == 0 && unlink("file") == 0);

    a = open("target", O_CREAT | O_RDWR, 0600);
    CHECK(a >= 0 && write(a, "target", 6) == 6);
    b = open("target", O_RDONLY);
    CHECK(b >= 0);
    replacement = open("source", O_CREAT | O_RDWR, 0600);
    CHECK(replacement >= 0 && write(replacement, "source", 6) == 6);
    CHECK(rename("source", "target") == 0);
    links(a, 0);
    CHECK(close(a) == 0);
    payload(b, "target");
    CHECK(close(b) == 0);
    CHECK(stat("target", &st) == 0 && fstat(replacement, &st) == 0);
    CHECK(close(replacement) == 0 && unlink("target") == 0);

    a = open("child", O_CREAT | O_RDWR, 0600);
    CHECK(a >= 0 && write(a, "child", 5) == 5);
    CHECK(unlink("child") == 0);
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        links(a, 0);
        payload(a, "child");
        CHECK(close(a) == 0);
        _exit(0);
    }
    CHECK(close(a) == 0);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(statfs(".", &after) == 0);
    CHECK(after.f_ffree == before.f_ffree);
    puts("Open-inode lifetime checks passed.");
    return 0;
}
EOF
gcc -Wall -O2 "$BASE/probe.c" -o "$BASE/probe"
"$BASE/probe" "$BASE"
