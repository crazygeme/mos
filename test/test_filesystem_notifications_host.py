#!/usr/bin/env python3
"""Exercise production VFS, virtual entries, and inotify with host services."""
from pathlib import Path
import os
import unittest
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SHIMS = {
    'config.h': '#pragma once\n',
    'mm/mm.h': '''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;
typedef struct _file file;
#define PAGE_SIZE 4096
''',
    'macro.h': '''#pragma once
#define MAX_PATH 4096
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define ROUND_UP(x) (((x) + sizeof(long) - 1) & ~(sizeof(long) - 1))
#define KERNEL_INIT(index, fn) static void __attribute__((constructor)) init_##fn(void) { fn(); }
''',
    'lib/lock.h': '''#pragma once
#include <assert.h>
#include <lib/list.h>
typedef struct { int held; } mutex_t;
typedef struct { int held; } spinlock_t;
static inline void mutex_init(mutex_t *lock) { lock->held = 0; }
static inline void mutex_lock(mutex_t *lock) { assert(!lock->held); lock->held = 1; }
static inline void mutex_unlock(mutex_t *lock) { assert(lock->held); lock->held = 0; }
static inline void spinlock_init(spinlock_t *lock) { lock->held = 0; }
static inline void spinlock_lock(spinlock_t *lock, int *irq) { assert(!lock->held); lock->held = 1; *irq = 0; }
static inline void spinlock_unlock(spinlock_t *lock, int irq) { (void)irq; assert(lock->held); lock->held = 0; }
''',
    'lib/klib.h': '''#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
void *malloc(size_t);
void free(void *);
int sprintf(char *, const char *, ...);
int puts(const char *);
extern unsigned allocations;
static inline void *test_malloc(size_t size) { allocations++; return malloc(size); }
static inline void *test_zalloc(size_t size) { void *p = test_malloc(size); if (p) memset(p, 0, size); return p; }
static inline char *test_strdup(const char *s) { size_t n = strlen(s) + 1; char *p = test_malloc(n); if (p) memcpy(p, s, n); return p; }
#define malloc test_malloc
#define zalloc test_zalloc
#define strdup test_strdup
#define kmalloc malloc
#define kfree free
#define printk(...) ((void)0)
#define klog(...) ((void)0)
char *name_get(void);
void name_put(char *);
''',
    'device/time.h': '''#pragma once
struct timespec { int tv_sec, tv_nsec; };
''',
    'ps/signal.h': '''#pragma once
typedef unsigned long sigset_t;
#define SIGIO 29
''',
    'ps/ps.h': '''#pragma once
#include <fs/vfs.h>
#include <ps/signal.h>
typedef struct { unsigned uid, euid, gid, egid; } test_user;
struct _task_struct {
    super_block *root;
    struct { mutex_t lock; } *files;
    file *fds[MAX_FD];
    test_user *user;
    void *io_wait;
    void (*cancel_io_wait)(void *);
};
extern task_struct *current;
#define CURRENT_TASK() current
int ps_read_process_memory(task_struct *, const void *, void *, unsigned);
int ps_write_process_memory(task_struct *, void *, const void *, unsigned);
int ps_send_signal_owner(int, int);
''',
    'syscall/syscall.h': '''#pragma once
int sys_inotify_init1(int);
int sys_inotify_init(void);
int sys_inotify_add_watch(int, const char *, unsigned);
int sys_inotify_rm_watch(int, int);
''',
    'ext4_oflags.h': '#pragma once\n#include <fs/fcntl.h>\n',
}

PROBE = r'''
#include <fs/entries.h>
#include <fs/inotify.h>
#include <fs/fcntl.h>
#include <fs/poll.h>
#include <syscall/syscall.h>
#include <ps/ps.h>
#include <lib/klib.h>
#include <errno.h>
#include <macro.h>
unsigned allocations;
static test_user user;
static typeof(*((task_struct *)0)->files) files;
static task_struct task = { .user = &user, .files = &files };
task_struct *current = &task;
char *name_get(void) { char *p = malloc(MAX_PATH); if (p) { memset(p, 0xa5, MAX_PATH); p[0] = 0; } return p; }
void name_put(char *name) { free(name); }
int resolve_path(const char *source, char *dest) { assert(source[0] == '/'); strcpy(dest, source); return 0; }
int fs_check_perm(const struct stat *st, int mask) { (void)st; (void)mask; return 0; }
int ps_read_process_memory(task_struct *t, const void *source, void *dest, unsigned n)
{ assert(t == current); if (!source || !dest) return -EFAULT; memcpy(dest, source, n); return 0; }
int ps_write_process_memory(task_struct *t, void *dest, const void *source, unsigned n)
{ return ps_read_process_memory(t, source, dest, n); }
int ps_send_signal_owner(int owner, int signal) { (void)owner; (void)signal; return 0; }
void poll_table_init(poll_table *pt, task_struct *t, poll_table_entry *entries, unsigned cap)
{ memset(pt, 0, sizeof(*pt)); pt->task = t; pt->entries = entries; pt->cap = cap; }
void poll_table_cleanup(poll_table *pt) { pt->nr = 0; }
void poll_subscribe(poll_table *pt, list_entry *head, spinlock_t *lock)
{ (void)pt; (void)head; (void)lock; }
void poll_notify(list_entry *head) { (void)head; }
int poll_wait_loop(const struct poll_ops *ops, void *ctx, int test, int infinite, unsigned long long deadline)
{ (void)infinite; (void)deadline; int result = ops->check(ctx); assert(test || result); return result; }
void epoll_release_file(file *fp) { (void)fp; }
void fs_flock_release(file *fp) { (void)fp; }
int fs_install_fd(file *fp, int flags)
{ (void)flags; for (int fd = 0; fd < MAX_FD; fd++) if (!task.fds[fd]) { task.fds[fd] = fp; return fd; } return -1; }
super_block *devnode_create(unsigned mode, unsigned dev) { (void)mode; (void)dev; assert(0); return NULL; }

@@FS_PUT@@

static void close_fd(int fd) { file *fp = task.fds[fd]; assert(fp); task.fds[fd] = NULL; fs_put_file(fp); }
static unsigned collect(int fd, unsigned expected, int wd, const char *name)
{
    char buffer[4096];
    file *fp = task.fds[fd];
    loff_t pos = 0;
    int length = fp->f_fop->read(fp, buffer, sizeof(buffer), &pos);
    if (!expected) { assert(length == -EAGAIN); return 0; }
    assert(length > 0);
    unsigned seen = 0;
    for (unsigned offset = 0; offset < (unsigned)length;) {
        struct inotify_event event;
        memcpy(&event, buffer + offset, sizeof(event));
        assert(event.wd == wd);
        assert(event.mask != IN_Q_OVERFLOW);
        if (name) { assert(event.len); assert(!strcmp(buffer + offset + sizeof(event), name)); }
        else assert(!event.len);
        seen |= event.mask;
        offset += sizeof(event) + event.len;
        assert(offset <= (unsigned)length);
    }
    assert((seen & expected) == expected);
    return seen;
}
static unsigned metadata_calls;
static file_operations spy_fops;
static const file_operations *original_fops;
static int spy_getattr(file *fp, struct stat *st) { metadata_calls++; return original_fops->getattr(fp, st); }
static file *tracked_open(const char *path)
{
    file *fp = vfs_open(task.root, path, O_RDONLY);
    assert(fp);
    inotify_file_open(fp, task.root);
    return fp;
}
static file *leaf_open(super_block *sb, int flags)
{
    (void)sb;
    file *fp = zalloc(sizeof(*fp));
    fp->f_inode = zalloc(sizeof(*fp->f_inode));
    fp->f_inode->i_mode = S_IFCHR | 0666;
    fp->f_inode->i_ino = 1;
    fp->f_count = 1;
    fp->f_flag = flags;
    return fp;
}
static const super_operations leaf_sops = { .open_root = leaf_open };

int main(void)
{
    super_block *host = sget(NULL);
    task.root = host;
    vfs_entry_tree *tree = vfs_entry_tree_create();
    vfs_entry_node *root = vfs_entry_root(tree);
    vfs_entry_node *device = vfs_entry_directory(root, "device");
    vfs_entry_node *classes = vfs_entry_directory(root, "class");
    assert(vfs_entry_text(device, "vendor", "1234"));
    assert(vfs_entry_link(classes, "card0", device));
    assert(vfs_entry_link(device, "subsystem", classes));
    assert(!vfs_mount(host, "/tree", vfs_entry_tree_super(tree)));
    super_block *leaf = sget(&leaf_sops);
    assert(!vfs_mount(host, "/leaf", leaf));
    file *fp = vfs_open(host, "/leaf", O_PATH);
    assert(fp && fp->f_sb == leaf);
    fs_put_file(fp);
    assert(!vfs_open(host, "/leaf/child", O_RDONLY));
    assert(!vfs_open(host, "/leaf/child/grandchild", O_PATH));
    assert(!vfs_open(host, "/tree/device/vendor/child", O_RDONLY));
    assert(!vfs_open(host, "/tree/class/card0/missing", O_RDONLY));

    /* The zero-watch execution path performs no notification capture work. */
    for (unsigned round = 0; round < 1000; round++) {
        fp = vfs_open(host, "/tree/device/vendor", O_RDONLY);
        assert(fp);
        if (!original_fops) { original_fops = fp->f_fop; spy_fops = *original_fops; spy_fops.getattr = spy_getattr; }
        fp->f_fop = &spy_fops;
        unsigned before = allocations, stat_before = metadata_calls;
        inotify_file_open(fp, host);
        inotify_file_event(fp, IN_ACCESS);
        assert(!fp->f_notify);
        assert(allocations == before && metadata_calls == stat_before);
        fs_put_file(fp);
    }

    /* A watch added after open receives later access and close events. */
    fp = tracked_open("/tree/device/vendor");
    assert(!fp->f_notify);
    int fd = sys_inotify_init1(O_NONBLOCK);
    assert(fd >= 0);
    int wd = sys_inotify_add_watch(fd, "/tree/class/card0/vendor", IN_ACCESS | IN_CLOSE_NOWRITE);
    assert(wd > 0 && fp->f_notify);
    collect(fd, 0, wd, NULL);
    assert(sys_inotify_add_watch(fd, "/tree/device/vendor", IN_ACCESS | IN_CLOSE_NOWRITE) == wd);
    assert(sys_inotify_add_watch(fd, "/tree/device/vendor", IN_ACCESS | IN_MASK_CREATE) == -EEXIST);
    inotify_file_event(fp, IN_ACCESS);
    fs_put_file(fp);
    collect(fd, IN_ACCESS | IN_CLOSE_NOWRITE, wd, NULL);
    close_fd(fd);

    /* Directory watches and nested aliases use the same backend identity. */
    fd = sys_inotify_init1(O_NONBLOCK);
    wd = sys_inotify_add_watch(fd, "/tree/class/card0", IN_OPEN | IN_CLOSE_NOWRITE);
    assert(wd > 0);
    assert(sys_inotify_add_watch(fd, "/tree/device", IN_OPEN | IN_CLOSE_NOWRITE) == wd);
    fp = tracked_open("/tree/class/card0/subsystem/card0/vendor");
    file *direct = vfs_open(host, "/tree/device/vendor", O_PATH);
    assert(fp->f_sb == direct->f_sb && fp->f_inode->i_ino == direct->f_inode->i_ino);
    fs_put_file(direct);
    fs_put_file(fp);
    collect(fd, IN_OPEN | IN_CLOSE_NOWRITE, wd, "vendor");
    close_fd(fd);
    sb_put(host);

    /* Mount views retain a canonical owner after the registry is released. */
    tree = vfs_entry_tree_create();
    root = vfs_entry_root(tree);
    assert(vfs_entry_text(root, "value", "text"));
    super_block *registry = vfs_entry_tree_super(tree);
    super_block *first = vfs_entry_tree_mount(tree);
    super_block *second = vfs_entry_tree_mount(tree);
    host = sget(NULL);
    task.root = host;
    assert(!vfs_mount(host, "/first", first));
    assert(!vfs_mount(host, "/second", second));
    sb_put(registry);
    fd = sys_inotify_init1(O_NONBLOCK);
    wd = sys_inotify_add_watch(fd, "/first", IN_OPEN | IN_CLOSE_NOWRITE);
    assert(wd > 0 && sys_inotify_add_watch(fd, "/second", IN_OPEN | IN_CLOSE_NOWRITE) == wd);
    fp = tracked_open("/second/value");
    fs_put_file(fp);
    collect(fd, IN_OPEN | IN_CLOSE_NOWRITE, wd, "value");
    close_fd(fd);
    fp = vfs_open(host, "/second", O_PATH);
    assert(fp && fp->f_sb == registry);
    sb_put(host);
    struct stat st;
    assert(!fp->f_fop->getattr(fp, &st) && S_ISDIR(st.st_mode));
    fs_put_file(fp);
    puts("Production filesystem notification checks: PASS");
    return 0;
}
'''


def file_release_source():
    source = (ROOT / 'src/fs/impl/fs.c').read_text()
    start = source.index('int fs_put_file(file *f)')
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    with tempfile.TemporaryDirectory(prefix='mos-fs-notify-') as directory:
        work = Path(directory)
        for name, content in SHIMS.items():
            target = work / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
        probe = work / 'probe.c'
        probe.write_text(PROBE.replace('@@FS_PUT@@', file_release_source()))
        sources = [ROOT / name for name in (
            'src/fs/impl/vfs.c', 'src/fs/impl/entries.c', 'src/fs/impl/inotify.c',
            'src/lib/impl/rbtree.c', 'src/lib/impl/list.c')]
        executable = work / 'probe'
        command = [os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-Wall', '-Werror', '-Wno-unused-function',
                   '-I' + str(work), '-I' + str(ROOT / 'src'),
                   str(probe), *map(str, sources), '-o', str(executable)]
        subprocess.run(command, check=True)
        env = os.environ.copy()
        env['ASAN_OPTIONS'] = 'detect_leaks=1'
        subprocess.run([str(executable)], check=True, env=env, timeout=30)


class FilesystemNotificationsHostTest(unittest.TestCase):
    def test_production_contract(self):
        main()


if __name__ == '__main__':
    unittest.main()
