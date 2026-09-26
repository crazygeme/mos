#!/bin/sh
set -eu
BASE=/root/tests/posix_realpath
mkdir -p "$BASE/dir"
trap 'rm -rf "$BASE"' EXIT
printf 'payload\n' > "$BASE/dir/target"
ln -sf target "$BASE/dir/link"
ln -sf absent "$BASE/dir/dangling"
cat > "$BASE/check.c" <<'EOF'
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "posix_realpath: %s (errno=%d)\n", message, errno);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    char path[512], expected[512], buf[512];
    char *resolved;
    ssize_t count;
    check(argc == 2, "test directory argument");
    alarm(10);
    snprintf(path, sizeof(path), "%s/dir", argv[1]);
    errno = 0;
    check(readlink(path, buf, sizeof(buf)) == -1 && errno == EINVAL,
          "readlink directory must report EINVAL");
    resolved = canonicalize_file_name(path);
    check(resolved != NULL && strcmp(resolved, path) == 0,
          "canonicalize existing directory");
    free(resolved);

    snprintf(path, sizeof(path), "%s/dir/target", argv[1]);
    errno = 0;
    check(readlink(path, buf, sizeof(buf)) == -1 && errno == EINVAL,
          "readlink regular file must report EINVAL");
    check(realpath(path, buf) != NULL && strcmp(buf, path) == 0,
          "realpath regular file");

    snprintf(expected, sizeof(expected), "%s/dir/target", argv[1]);
    snprintf(path, sizeof(path), "%s/dir/link", argv[1]);
    count = readlink(path, buf, sizeof(buf));
    check(count == 6 && memcmp(buf, "target", 6) == 0, "readlink symlink");
    check(realpath(path, buf) != NULL && strcmp(buf, expected) == 0,
          "realpath symlink");
    count = readlink(path, buf, 3);
    check(count == 3 && memcmp(buf, "tar", 3) == 0, "readlink truncation");

    snprintf(path, sizeof(path), "%s/dir/dangling", argv[1]);
    count = readlink(path, buf, sizeof(buf));
    check(count == 6 && memcmp(buf, "absent", 6) == 0, "readlink dangling link");
    errno = 0;
    check(realpath(path, buf) == NULL && errno == ENOENT,
          "realpath dangling link must report ENOENT");
    snprintf(path, sizeof(path), "%s/dir/absent", argv[1]);
    errno = 0;
    check(readlink(path, buf, sizeof(buf)) == -1 && errno == ENOENT,
          "readlink missing path must report ENOENT");
    return 0;
}
EOF
gcc -Wall -Wextra -o "$BASE/check" "$BASE/check.c"
"$BASE/check" "$BASE"
