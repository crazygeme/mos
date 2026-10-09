#!/usr/bin/env python3
"""Check the production shebang parser and direct argv builder with sanitizers."""
from pathlib import Path
import os
import unittest
import subprocess
import tempfile

from test_fd_callback_lifetime_host import ROOT, function

PROBE = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static unsigned allocations;
static int fail_allocation;
static void *kmalloc(unsigned bytes)
{ allocations++; return fail_allocation ? NULL : malloc(bytes); }
#define kfree free
@@FUNCTIONS@@

static void parsed(const char *text, int truncated, const char *path,
                   const char *argument, int error)
{
    unsigned n = strlen(text);
    char *line = malloc(n + 2);
    memcpy(line, text, n);
    line[n] = (char)0xa5; /* The parser must provide its own terminator. */
    line[n + 1] = (char)0x5a;
    const char *interp = NULL, *arg = NULL;
    assert(parse_shebang(line, n, truncated, &interp, &arg) == error);
    assert((unsigned char)line[n + 1] == 0x5a);
    if (!error) {
        assert(!strcmp(interp, path));
        assert(argument ? arg && !strcmp(arg, argument) : !arg);
    }
    free(line);
}
static void copied(const char *interp, const char *arg, char **argv,
                   unsigned user_argc, const char **expected, unsigned count)
{
    unsigned argc = 0, before = allocations;
    char script[] = "/root/example";
    char **out = dup_script_argv(interp, arg, script, argv, user_argc, &argc);
    assert(out && argc == count && allocations == before + 1);
    for (unsigned i = 0; i < argc; i++)
        assert(!strcmp(out[i], expected[i]));
    /* The copy must survive mutation of its source strings. */
    script[1] = 'X';
    assert(!strcmp(out[arg ? 2 : 1], "/root/example"));
    free(out);
}
int main(void)
{
    parsed("#!/bin/sh\nbody", 1, "/bin/sh", NULL, 0);
    parsed("#! \t/bin/sh\t-u -O  \r\nbody", 1, "/bin/sh", "-u -O  ", 0);
    parsed("#!/bin/sh", 0, "/bin/sh", NULL, 0);
    parsed("#!/bin/sh   \n", 0, "/bin/sh", NULL, 0);
    parsed("#!   \n", 0, NULL, NULL, -ENOEXEC);
    parsed("#!", 0, NULL, NULL, -ENOEXEC);
    parsed("#!/bin/sh incomplete", 1, NULL, NULL, -ENOEXEC);
    char *argv[] = { "custom argv0", "", "tail value", NULL };
    assert(count_strv(argv) == 3 && count_strv(NULL) == 0);
    const char *plain[] = { "/bin/sh", "/root/example", "", "tail value" };
    const char *argument[] = { "/bin/sh", "-u -O", "/root/example", "", "tail value" };
    const char *empty[] = { "/bin/sh", "/root/example" };
    copied("/bin/sh", NULL, argv, 3, plain, 4);
    copied("/bin/sh", "-u -O", argv, 3, argument, 5);
    copied("/bin/sh", NULL, NULL, 0, empty, 2);
    char **direct = dup_strv(argv, 3);
    assert(direct && !strcmp(direct[0], argv[0]) && !strcmp(direct[1], ""));
    free(direct);
    char *many[40];
    for (unsigned i = 0; i < 40; i++) many[i] = "long vector tail";
    direct = dup_strv(many, 40);
    assert(direct && !strcmp(direct[39], many[39]));
    free(direct);
    assert(!dup_strv(NULL, 0) && !dup_strv(NULL, UINT32_MAX));
    unsigned argc;
    fail_allocation = 1;
    assert(!dup_strv(argv, 3));
    assert(!dup_script_argv("/bin/sh", NULL, "/root/example", argv, 3, &argc));
    fail_allocation = 0;
    assert(!dup_script_argv("/bin/sh", "arg", "/root/example", NULL, UINT32_MAX, &argc));
    /* Bounded fuzzing covers short reads, embedded NULs, tabs, CRs and LFs. */
    const char alphabet[] = "abc /\t\r\n";
    uint32_t seed = 17;
    for (unsigned trial = 0; trial < 10000; trial++) {
        unsigned n = 2 + trial % 63;
        char *line = malloc(n + 1);
        line[0] = '#'; line[1] = '!';
        for (unsigned i = 2; i < n; i++) {
            seed = seed * 1664525U + 1013904223U;
            line[i] = alphabet[seed % sizeof(alphabet)];
        }
        const char *interp, *arg;
        int result = parse_shebang(line, n, trial & 1, &interp, &arg);
        assert(result == 0 || result == -ENOEXEC);
        if (!result) {
            assert(interp >= line + 2 && interp < line + n);
            assert(strlen(interp) <= n - 2);
            if (arg) assert(arg > interp && arg < line + n);
        }
        free(line);
    }
    puts("Shebang bounds, argv ownership, argument order, and allocation checks: PASS");
    return 0;
}
'''


def main():
    source = (ROOT / 'src/elf/impl/exec.c').read_text()
    functions = '\n'.join(function(source, signature) for signature in (
        'static unsigned count_strv(', 'static char **dup_strv(', 'static char **dup_script_argv(',
        'static int parse_shebang('))
    with tempfile.TemporaryDirectory(prefix='mos-shebang-host-') as raw:
        directory = Path(raw)
        probe = directory / 'probe.c'
        probe.write_text(PROBE.replace('@@FUNCTIONS@@', functions))
        executable = directory / 'probe'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-Wall', '-Werror', str(probe), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)


class ShebangHostTest(unittest.TestCase):
    def test_production_contract(self):
        main()


if __name__ == '__main__':
    unittest.main()
