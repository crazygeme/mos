#!/bin/sh
set -e
BASE=/tmp/posix_cache_growth_$$
mkdir -p "$BASE"
trap 'rm -rf "$BASE"' EXIT
cat > "$BASE/probe.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int ok, const char *what)
{
    if (!ok) { fprintf(stderr, "cache_growth: %s\n", what); exit(1); }
}

static unsigned long long block_budget(void)
{
    FILE *f = fopen("/proc/mos", "r");
    char line[256], metric[64], formatted[32];
    unsigned long long value = 0;
    int block = 0;
    check(f != NULL, "open kernel cache statistics");
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "Block cache", 11)) block = 1;
        if (block && sscanf(line, "| %63s | %31s | %llu", metric,
                            formatted, &value) == 3 && !strcmp(metric, "budget")) {
            fclose(f);
            return value;
        }
    }
    fclose(f);
    check(0, "find block cache budget");
    return 0;
}

static unsigned long long memory(const char *name)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128], key[64];
    unsigned long long kb = 0, value;
    check(f != NULL, "open memory statistics");
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "%63s %llu", key, &value) == 2 && !strcmp(key, name))
            kb = value;
    fclose(f);
    return kb;
}

int main(int argc, char **argv)
{
    const unsigned chunk = 1024 * 1024;
    const unsigned chunks = 256;
    unsigned char *buf;
    unsigned pass, i;
    FILE *f;
    unsigned long long buffers, cached;
    if (memory("MemTotal:") < 1024ULL * 1024) {
        puts("cache_growth: SKIP (at least 1 GiB usable RAM required)");
        return 0;
    }
    check(argc == 2, "data path");
    check(block_budget() > 64ULL * 1024 * 1024, "block budget exceeds old 64 MiB cap");
    buf = malloc(chunk);
    check(buf != NULL, "allocate transfer buffer");
    memset(buf, 0xa5, chunk);
    f = fopen(argv[1], "wb");
    check(f != NULL, "create temporary file");
    for (i = 0; i < chunks; i++)
        check(fwrite(buf, 1, chunk, f) == chunk, "write file");
    check(fclose(f) == 0, "close written file");
    sync();
    buffers = memory("Buffers:");
    check(buffers >= 128 * 1024, "block cache must grow beyond 64 MiB");
    for (pass = 0; pass < 2; pass++) {
        f = fopen(argv[1], "rb");
        check(f != NULL, "open file for repeated read");
        for (i = 0; i < chunks; i++) {
            unsigned j;
            check(fread(buf, 1, chunk, f) == chunk, "read file");
            for (j = 0; j < chunk; j++)
                check(buf[j] == 0xa5, "cached contents");
        }
        check(fclose(f) == 0, "close read file");
    }
    cached = memory("Cached:");
    check(cached >= 128 * 1024, "filesystem cache growth");
    printf("cache_growth: PASS Buffers=%llu kB Cached=%llu kB\n", buffers, cached);
    free(buf);
    return 0;
}
EOF
gcc -O2 -o "$BASE/probe" "$BASE/probe.c"
"$BASE/probe" "$BASE/data"
