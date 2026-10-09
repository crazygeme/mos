#!/usr/bin/env python3
"""Exercise the common resource head, including concurrent acquire/release."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROBE = r'''
#include <lib/ref_count.h>
#include <pthread.h>
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
struct resource { ref_count_t ref; unsigned releases, value; };
static void release(ref_count_t *head) {
    struct resource *resource = (struct resource *)head;
    assert(ref_count_read(head) == 0);
    assert(resource->value == 42);
    resource->releases++;
}
static void *worker(void *opaque) {
    struct resource *resource = opaque;
    for (unsigned i = 0; i < 20000; i++) {
        assert(ref_count_get(resource) == resource);
        assert(resource->value == 42);
        ref_count_put(&resource->ref);
    }
    ref_count_put(&resource->ref);
    return NULL;
}
int main(void) {
    _Static_assert(offsetof(struct resource, ref) == 0, "common head");
    struct resource resource = { .value = 42 };
    ref_count_init(&resource.ref, release);
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; i++) {
        assert(ref_count_get(&resource) == &resource);
        assert(!pthread_create(&threads[i], NULL, worker, &resource));
    }
    for (unsigned i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    assert(ref_count_read(&resource.ref) == 1 && resource.releases == 0);
    ref_count_put(&resource.ref);
    assert(resource.releases == 1);
    assert(ref_count_get(NULL) == NULL); ref_count_put(NULL);
    assert(ref_count_read(NULL) == 0);
    puts("Common resource refcounts: PASS");
}
'''

class RefCountTests(unittest.TestCase):
    def test_concurrent_lifetime(self):
        with tempfile.TemporaryDirectory(prefix='mos-ref-count-') as directory:
            work = Path(directory)
            probe = work / 'probe.c'
            probe.write_text(PROBE)
            executable = work / 'probe'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-O2',
                            '-Wall', '-Wextra', '-Werror', '-pthread',
                            '-I', str(ROOT / 'src'), str(probe),
                            str(ROOT / 'src/lib/impl/ref_count.c'),
                            '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

if __name__ == '__main__':
    unittest.main()
