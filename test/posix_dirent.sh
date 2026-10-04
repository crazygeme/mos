#!/bin/sh
set -eu
BASE=/root/tests/posix_dirent
mkdir -p "$BASE/work"
trap 'rm -rf "$BASE"' EXIT
cat > "$BASE/dirent.c" <<'EOF'
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COUNT 160

static void check(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "posix_dirent: %s (errno=%d)\n", message, errno);
        exit(1);
    }
}

static void name_for(int i, char *name)
{
    sprintf(name, "entry_%03d_", i);
    memset(name + 10, 'x', 120);
    name[130] = 0;
}

static void enumerate(DIR *directory)
{
    unsigned char seen[COUNT];
    struct dirent *entry;
    int count = 0, i;
    long cookie;
    char expected[131];

    memset(seen, 0, sizeof(seen));
    for (;;) {
        errno = 0;
        entry = readdir(directory);
        if (!entry) {
            check(errno == 0, "complete directory enumeration");
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        check(sscanf(entry->d_name, "entry_%d_", &i) == 1 &&
              i >= 0 && i < COUNT, "directory entry index");
        name_for(i, expected);
        check(!strcmp(entry->d_name, expected), "complete directory entry name");
        check(!seen[i], "directory entry returned once");
        seen[i] = 1;
        ++count;
    }
    check(count == COUNT, "all entries including the final entry");
    cookie = telldir(directory);
    check(cookie >= 0, "nonnegative terminal directory cookie");
    seekdir(directory, cookie);
    errno = 0;
    check(readdir(directory) == NULL && errno == 0, "seek to directory end");
}

int main(int argc, char **argv)
{
    DIR *directory;
    struct dirent *entry;
    char path[512], name[131], following[256];
    long cookie;
    int i, fd;

    check(argc == 2, "directory argument");
    alarm(30);
    directory = opendir(argv[1]);
    check(directory != NULL, "open fixture directory");
    for (i = 0; i < COUNT; ++i) {
        name_for(i, name);
        check(snprintf(path, sizeof(path), "%s/%s", argv[1], name) <
              (int)sizeof(path), "fixture pathname length");
        fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        check(fd >= 0 && close(fd) == 0, "create directory fixture");
    }
    rewinddir(directory);
    enumerate(directory);
    rewinddir(directory);
    enumerate(directory);
    rewinddir(directory);
    check(readdir(directory) != NULL, "first directory entry");
    cookie = telldir(directory);
    entry = readdir(directory);
    check(entry != NULL, "following directory entry");
    strcpy(following, entry->d_name);
    seekdir(directory, cookie);
    entry = readdir(directory);
    check(entry != NULL && !strcmp(entry->d_name, following),
          "seek resumes at the following entry");
    check(closedir(directory) == 0, "close fixture directory");
    puts("posix_dirent: PASS");
    return 0;
}
EOF
gcc -std=gnu89 -Wall -Werror -O2 "$BASE/dirent.c" -o "$BASE/dirent"
"$BASE/dirent" "$BASE/work"
