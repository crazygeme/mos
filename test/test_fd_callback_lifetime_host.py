#!/usr/bin/env python3
"""Validate descriptor callback lock order and poll subscription lifetimes."""
from pathlib import Path
import os
import unittest
import subprocess
import tempfile

from test_filesystem_notifications_host import SHIMS, ROOT


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PROBE = r'''
#include <fs/vfs.h>
#include <fs/fcntl.h>
#include <fs/ioctl.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <errno.h>
unsigned allocations;
static typeof(*((task_struct *)0)->files) files;
static task_struct task = { .files = &files };
task_struct *current = &task;
static unsigned releases, removals;
static int close_in_callback;
void epoll_release_file(file *fp) { (void)fp; }
void fs_flock_release(file *fp) { (void)fp; }
void fs_posix_lock_release(file *fp, unsigned tgid) { (void)fp; (void)tgid; }
void inotify_file_close(file *fp) { (void)fp; }
void sb_put(super_block *sb) { (void)sb; }
void ps_put_to_ready_queue(task_struct *t) { (void)t; }
struct file_io_scope { file *fp; struct file_io_scope *previous; };
@@FUNCTIONS@@
static int release(file *fp) {
    assert(!files.lock.held);
    releases++;
    free(fp);
    return 0;
}
static void remove_subscription(void *opaque, task_struct *t) {
    file *fp = opaque;
    (void)t;
    assert(fp->f_count > 0);
    removals++;
}
static unsigned poll_callback(file *fp, unsigned events, poll_table *pt) {
    assert(!files.lock.held && fp->f_count >= 2);
    /* Descriptor installation also occurs under socket-core ownership. */
    fs_get_file(fp);
    int installed = fs_install_fd(fp, 0);
    assert(installed >= 0 && !fs_close(installed));
    if (pt) {
        assert(!poll_table_add(pt, fp, remove_subscription));
        assert(!poll_table_add(pt, fp, remove_subscription));
    }
    if (close_in_callback)
        assert(!fs_close(7));
    return events;
}
static int ioctl_callback(file *fp, unsigned cmd, void *buf) {
    assert(!files.lock.held && fp->f_count == 2);
    (void)cmd; (void)buf;
    assert(!fs_close(7));
    assert(fp->f_count == 1);
    return 23;
}
static const file_operations ops = {
    .release = release, .poll = poll_callback, .ioctl = ioctl_callback
};
static file *install(void) {
    file *fp = zalloc(sizeof(*fp));
    fp->f_count = 1; fp->f_fop = &ops;
    assert(!task.fds[7]); task.fds[7] = fp;
    return fp;
}
int main(void) {
    unsigned long cloexec[FD_BITMAP_WORDS] = {0};
    task.fd_cloexec = cloexec;
    mutex_init(&files.lock);
    assert(fs_fd_poll(-1, FS_POLL_READ, NULL) == 0);
    assert(fs_ioctl(MAX_FD, 0, NULL) == -EBADF);
    file *fp = install();
    assert(fs_fd_poll(7, FS_POLL_READ, NULL) == FS_POLL_READ);
    assert(fp->f_count == 1 && !task.io_files);
    assert(!fs_close(7) && releases == 1);

    install();
    assert(fs_ioctl(7, 0, NULL) == 23);
    assert(releases == 2 && !task.io_files);

    poll_table pt;
    poll_table_entry entries[2];
    memset(entries, 0xff, sizeof(entries));
    for (unsigned i = 0; i < 4; i++) {
        fp = install();
        poll_table_init(&pt, &task, entries, 2);
        close_in_callback = 1;
        assert(fs_fd_poll(7, FS_POLL_READ, &pt) == FS_POLL_READ);
        assert(!task.fds[7] && fp->f_count == 1 && pt.nr == 2);
        assert(releases == 2 + i && task.cancel_io_wait);
        if (i % 2)
            task.cancel_io_wait(task.io_wait);
        else
            poll_table_cleanup(&pt);
        assert(releases == 3 + i && removals == 2 * (i + 1));
        assert(!pt.nr && !task.io_wait && !task.cancel_io_wait);
        poll_table_cleanup(&pt);
    }
    close_in_callback = 0;
    install();
    struct file_io_scope scope;
    assert(fs_io_begin(7, &scope));
    assert(!fs_close(7));
    fs_cancel_io(&task);
    assert(releases == 7 && !task.io_files);
    puts("Descriptor callback lock order and lifetime checks: PASS");
    return 0;
}
'''


def main():
    source = (ROOT / 'src/fs/impl/fs.c').read_text()
    signatures = (
        'static int fs_find_empty_fd(', 'static file *fs_io_begin(',
        'static void fs_io_end(', 'void fs_cancel_io(',
        'int fs_install_fd_unsafe(', 'int fs_install_fd(', 'int fs_close(',
        'int fs_put_file(', 'void poll_table_init(', 'void poll_table_cleanup(',
        'static void poll_table_cancel(', 'int poll_table_add(',
        'unsigned fs_fd_poll(', 'int fs_ioctl(',
    )
    with tempfile.TemporaryDirectory(prefix='mos-fd-callback-') as directory:
        work = Path(directory)
        shims = dict(SHIMS)
        shims['ps/ps.h'] = shims['ps/ps.h'].replace(
            'void *io_wait;', 'unsigned tgid;\n    unsigned long *fd_cloexec;\n    struct file_io_scope *io_files;\n    void *io_wait;')
        for name, content in shims.items():
            path = work / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        probe = work / 'probe.c'
        probe.write_text(PROBE.replace('@@FUNCTIONS@@', '\n'.join(
            function(source, signature) for signature in signatures)))
        executable = work / 'probe'
        subprocess.run([
            os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-Wall', '-Werror', '-Wno-unused-function',
            '-I' + str(work), '-I' + str(ROOT / 'src'),
            str(probe), str(ROOT / 'src/lib/impl/list.c'), '-o', str(executable)
        ], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)


class FdCallbackLifetimeHostTest(unittest.TestCase):
    def test_production_contract(self):
        main()


if __name__ == '__main__':
    unittest.main()
