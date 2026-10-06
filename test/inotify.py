#!/usr/bin/env python3
"""Validate filesystem notifications, watch lifetime, queues, and proc controls."""

import argparse
import array
import ctypes
import errno
import fcntl
import os
from pathlib import Path
import select
import signal
import struct
import tempfile
import time

ACCESS, MODIFY, ATTRIB, CLOSE_WRITE, CLOSE_NOWRITE, OPEN = 1, 2, 4, 8, 16, 32
MOVED_FROM, MOVED_TO, CREATE, DELETE, DELETE_SELF, MOVE_SELF = 64, 128, 256, 512, 1024, 2048
UNMOUNT, Q_OVERFLOW, IGNORED = 0x2000, 0x4000, 0x8000
ONLYDIR, DONT_FOLLOW, EXCL_UNLINK = 0x1000000, 0x2000000, 0x4000000
MASK_CREATE, MASK_ADD, ISDIR, ONESHOT = 0x10000000, 0x20000000, 0x40000000, 0x80000000
ALL = 0xfff
FIONREAD = 0x541b
HEADER = struct.Struct('=iIII')
libc = ctypes.CDLL(None, use_errno=True)
libc.inotify_init1.argtypes = [ctypes.c_int]
libc.inotify_init1.restype = ctypes.c_int
libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
libc.inotify_add_watch.restype = ctypes.c_int
libc.inotify_rm_watch.argtypes = [ctypes.c_int, ctypes.c_int]
libc.inotify_rm_watch.restype = ctypes.c_int
libc.syscall.restype = ctypes.c_long


def checked(value):
    if value < 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    return value


def create(flags=os.O_NONBLOCK | os.O_CLOEXEC):
    return checked(libc.inotify_init1(flags))


def watch(fd, path, mask=ALL):
    return checked(libc.inotify_add_watch(fd, os.fsencode(path), mask))


def remove(fd, wd):
    checked(libc.inotify_rm_watch(fd, wd))


def error(expected, call):
    try:
        call()
    except OSError as exc:
        assert exc.errno == expected, (exc, expected)
    else:
        raise AssertionError(f'Expected errno {expected}')


def decode(data):
    result = []
    offset = 0
    while offset < len(data):
        assert len(data) - offset >= HEADER.size, data
        wd, mask, cookie, length = HEADER.unpack_from(data, offset)
        assert length % HEADER.size == 0
        end = offset + HEADER.size + length
        assert end <= len(data), data
        name = data[offset + HEADER.size:end].split(b'\0', 1)[0]
        result.append((wd, mask, cookie, os.fsdecode(name)))
        offset = end
    return result


def drain(fd):
    events = []
    while True:
        try:
            events.extend(decode(os.read(fd, 65536)))
        except BlockingIOError:
            return events


def contains(events, wd, mask, name=''):
    assert any(w == wd and m & mask == mask and n == name
               for w, m, c, n in events), (wd, hex(mask), name, events)


def queued(fd):
    value = array.array('i', [0])
    fcntl.ioctl(fd, FIONREAD, value, True)
    return value[0]


def filesystem_checks(root):
    directory = root / 'basic'
    directory.mkdir()
    fd = create()
    try:
        assert fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_NONBLOCK
        assert fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
        assert os.lseek(fd, 1, os.SEEK_SET) == 0
        assert queued(fd) == 0
        error(errno.EAGAIN, lambda: os.read(fd, 65536))
        wd = watch(fd, directory)
        target = directory / 'file'
        file_fd = os.open(target, os.O_CREAT | os.O_RDWR, 0o600)
        events = drain(fd)
        assert not any(e[1] & ATTRIB for e in events), events
        os.write(file_fd, b'abc')
        os.lseek(file_fd, 0, os.SEEK_SET)
        assert os.read(file_fd, 3) == b'abc'
        os.fchmod(file_fd, 0o640)
        os.ftruncate(file_fd, 1)
        os.close(file_fd)
        events.extend(drain(fd))
        for mask in (CREATE, OPEN, MODIFY, ACCESS, ATTRIB, CLOSE_WRITE):
            contains(events, wd, mask, 'file')
        with open(target, 'rb') as stream:
            stream.read()
        contains(drain(fd), wd, CLOSE_NOWRITE, 'file')
        readonly = os.open(target, os.O_RDONLY)
        drain(fd)
        try:
            error(errno.EINVAL, lambda: os.ftruncate(readonly, 0))
            assert not drain(fd), 'Rejected truncation must not emit events'
        finally:
            os.close(readonly)
        drain(fd)
        child = directory / 'dir'
        child.mkdir()
        contains(drain(fd), wd, CREATE | ISDIR, 'dir')
        (child / 'nested').touch()
        assert not drain(fd), 'Directory watches must not recurse'
        (child / 'nested').unlink()
        child.rmdir()
        contains(drain(fd), wd, DELETE | ISDIR, 'dir')

        file_wd = watch(fd, target)
        renamed = directory / 'renamed'
        file_fd = os.open(target, os.O_RDWR)
        drain(fd)
        target.rename(renamed)
        events = drain(fd)
        contains(events, wd, MOVED_FROM, 'file')
        contains(events, wd, MOVED_TO, 'renamed')
        contains(events, file_wd, MOVE_SELF)
        first = next(e for e in events if e[0] == wd and e[1] & MOVED_FROM)
        second = next(e for e in events if e[0] == wd and e[1] & MOVED_TO)
        assert first[2] and first[2] == second[2], events
        os.write(file_fd, b'x')
        os.close(file_fd)
        events = drain(fd)
        contains(events, file_wd, MODIFY)
        contains(events, wd, CLOSE_WRITE, 'renamed')

        alias = directory / 'alias'
        os.link(renamed, alias)
        contains(drain(fd), file_wd, ATTRIB)
        assert watch(fd, alias, MODIFY | DELETE_SELF | ATTRIB | CLOSE_WRITE) == file_wd
        error(errno.EEXIST, lambda: watch(fd, alias, MODIFY | MASK_CREATE))
        renamed.unlink()
        events = drain(fd)
        contains(events, file_wd, ATTRIB)
        assert not any(e[0] == file_wd and e[1] & IGNORED for e in events)
        file_fd = os.open(alias, os.O_RDWR)
        drain(fd)
        alias.unlink()
        events = drain(fd)
        contains(events, file_wd, ATTRIB)
        assert not any(e[0] == file_wd and e[1] & (DELETE_SELF | IGNORED) for e in events)
        os.close(file_fd)
        events = drain(fd)
        contains(events, file_wd, CLOSE_WRITE)
        contains(events, file_wd, DELETE_SELF)
        contains(events, file_wd, IGNORED)
        error(errno.EINVAL, lambda: remove(fd, file_wd))

        # A rename replacement keeps its inode watch while open references exist.
        old = directory / 'old'
        replacement = directory / 'replacement'
        old.touch()
        replacement.touch()
        replacement_wd = watch(fd, replacement)
        held = os.open(replacement, os.O_RDONLY)
        drain(fd)
        old.rename(replacement)
        events = drain(fd)
        contains(events, replacement_wd, ATTRIB)
        assert not any(e[0] == replacement_wd and e[1] & IGNORED for e in events)
        os.close(held)
        events = drain(fd)
        contains(events, replacement_wd, DELETE_SELF)
        contains(events, replacement_wd, IGNORED)
        remove(fd, wd)
        contains(drain(fd), wd, IGNORED)
    finally:
        os.close(fd)


def path_reference_checks(root):
    item = root / 'path-reference'
    item.touch()
    fd = create()
    held = -1
    try:
        wd = watch(fd, item)
        held = os.open(item, os.O_PATH)
        assert not drain(fd), 'O_PATH must not produce open notifications'
        item.unlink()
        events = drain(fd)
        contains(events, wd, ATTRIB)
        assert not any(e[1] & (DELETE_SELF | IGNORED) for e in events), events
        os.close(held)
        held = -1
        events = drain(fd)
        contains(events, wd, DELETE_SELF)
        contains(events, wd, IGNORED)
        assert not any(e[1] & (CLOSE_WRITE | CLOSE_NOWRITE) for e in events), events
    finally:
        if held >= 0:
            os.close(held)
        os.close(fd)


def late_watch_checks(root):
    path = root / 'late-watch'
    path.mkdir()
    for action in ('rename', 'unlink'):
        item = path / action
        item.touch()
        held = os.open(item, os.O_RDWR)
        fd = -1
        try:
            name = action
            if action == 'rename':
                name = 'renamed'
                item.rename(path / name)
            else:
                item.unlink()
            fd = create()
            wd = watch(fd, path, MODIFY | CLOSE_WRITE)
            assert not drain(fd), 'Watch registration must not emit earlier events'
            os.write(held, b'x')
            os.close(held)
            held = -1
            events = drain(fd)
            contains(events, wd, MODIFY, name)
            contains(events, wd, CLOSE_WRITE, name)
            assert not any(e[1] & Q_OVERFLOW for e in events), events
        finally:
            if held >= 0:
                os.close(held)
            if fd >= 0:
                os.close(fd)


def async_checks(root):
    path = root / 'async'
    path.mkdir()
    fd = create()
    received = []
    previous = signal.signal(signal.SIGIO, lambda signo, frame: received.append(signo))
    try:
        wd = watch(fd, path, CREATE)
        fcntl.fcntl(fd, fcntl.F_SETOWN, os.getpid())
        fcntl.fcntl(fd, fcntl.F_SETFL, os.O_NONBLOCK | os.O_ASYNC)
        (path / 'signal').touch()
        deadline = time.monotonic() + 2
        while not received and time.monotonic() < deadline:
            time.sleep(0.01)
        assert received, 'Asynchronous notification timed out'
        contains(drain(fd), wd, CREATE, 'signal')
    finally:
        os.close(fd)
        signal.signal(signal.SIGIO, previous)


def mask_checks(root):
    path = root / 'masks'
    path.mkdir()
    fd = create()
    try:
        error(errno.EINVAL, lambda: watch(fd, path, 0))
        error(errno.EINVAL, lambda: watch(fd, path, 0x1000))
        error(errno.EINVAL, lambda: watch(fd, path, CREATE | MASK_ADD | MASK_CREATE))
        error(errno.EBADF, lambda: watch(-1, path))
        error(errno.ENOENT, lambda: watch(fd, path / 'missing'))
        error(errno.ENOENT, lambda: watch(fd, ''))
        error(errno.EFAULT, lambda: checked(libc.inotify_add_watch(fd, None, ALL)))
        ordinary = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        try:
            error(errno.EINVAL, lambda: watch(ordinary, path))
        finally:
            os.close(ordinary)
        wd = watch(fd, path, CREATE)
        assert watch(fd, path, DELETE | MASK_ADD) == wd
        item = path / 'item'
        item.touch()
        item.unlink()
        events = drain(fd)
        contains(events, wd, CREATE, 'item')
        contains(events, wd, DELETE, 'item')
        assert watch(fd, path, OPEN) == wd
        item.touch()
        events = drain(fd)
        assert events and all(e[1] & OPEN for e in events), events
        error(errno.ENOTDIR, lambda: watch(fd, item, MODIFY | ONLYDIR))
        link = path / 'link'
        link.symlink_to(item.name)
        item_wd = watch(fd, item)
        assert watch(fd, link) == item_wd
        link_wd = watch(fd, link, ALL | DONT_FOLLOW)
        assert link_wd != item_wd
        drain(fd)
        link.unlink()
        contains(drain(fd), link_wd, IGNORED)
        remove(fd, wd)
        drain(fd)
        wd = watch(fd, path, CREATE | ONESHOT)
        (path / 'once').touch()
        (path / 'twice').touch()
        events = drain(fd)
        contains(events, wd, CREATE, 'once')
        contains(events, wd, IGNORED)
        assert not any(e[0] == wd and e[3] == 'twice' for e in events)
        error(errno.EINVAL, lambda: remove(fd, wd))
    finally:
        os.close(fd)

    # EXCL_UNLINK suppresses subsequent events for the deleted directory entry.
    for exclude in (False, True):
        fd = create()
        item = path / 'unlinked'
        item.touch()
        try:
            wd = watch(fd, path, CLOSE_WRITE | (EXCL_UNLINK if exclude else 0))
            held = os.open(item, os.O_RDWR)
            item.unlink()
            os.close(held)
            events = drain(fd)
            assert any(e[0] == wd and e[1] & CLOSE_WRITE for e in events) != exclude, events
        finally:
            os.close(fd)


def queue_checks(root):
    path = root / 'queues'
    path.mkdir()
    fd = create()
    duplicate = os.dup(fd)
    try:
        wd = watch(fd, path, CREATE)
        with select.epoll() as epoll:
            epoll.register(fd, select.EPOLLIN)
            assert epoll.poll(0) == []
            (path / 'a').touch()
            assert epoll.poll(1) == [(fd, select.EPOLLIN)]
            assert select.select([fd], [], [], 0)[0] == [fd]
            count = queued(fd)
            assert count == 32, count
            error(errno.EINVAL, lambda: os.read(fd, 16))
            assert queued(fd) == count
            # Legacy inotify reads require a complete record in each iovec.
            error(errno.EINVAL, lambda: os.readv(duplicate, [bytearray(7), bytearray(count)]))
            assert queued(fd) == count
            buffers = [bytearray(count), bytearray(0)]
            assert os.readv(duplicate, buffers) == count
            contains(decode(b''.join(buffers)), wd, CREATE, 'a')
            assert queued(fd) == 0 and epoll.poll(0) == []
            (path / 'b').touch()
            (path / 'c').touch()
            events = decode(os.read(fd, 32))
            contains(events, wd, CREATE, 'b')
            contains(drain(duplicate), wd, CREATE, 'c')
        os.close(fd)
        fd = -1
        (path / 'd').touch()
        contains(drain(duplicate), wd, CREATE, 'd')
    finally:
        os.close(duplicate)
        if fd >= 0:
            os.close(fd)

    # Identical consecutive events are coalesced until consumed.
    item = path / 'coalesced'
    item.touch()
    fd = create()
    held = os.open(item, os.O_WRONLY)
    try:
        wd = watch(fd, item, MODIFY)
        os.write(held, b'a')
        os.write(held, b'b')
        events = drain(fd)
        assert len(events) == 1, events
        contains(events, wd, MODIFY)
    finally:
        os.close(held)
        os.close(fd)


def wait_checks(root):
    path = root / 'waits'
    path.mkdir()
    number = 253 if ctypes.sizeof(ctypes.c_void_p) == 8 else 291
    fd = checked(libc.syscall(ctypes.c_long(number)))
    try:
        assert not fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_NONBLOCK
        assert not fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
        wd = watch(fd, path, CREATE)
        child = os.fork()
        if child == 0:
            try:
                time.sleep(0.05)
                (path / 'wake').touch()
                os._exit(0)
            except BaseException:
                os._exit(1)
        contains(decode(os.read(fd, 65536)), wd, CREATE, 'wake')
        assert os.waitpid(child, 0)[1] == 0
        child = os.fork()
        if child == 0:
            os.read(fd, 65536)
            os._exit(1)
        time.sleep(0.05)
        os.kill(child, signal.SIGKILL)
        assert os.WIFSIGNALED(os.waitpid(child, 0)[1])
    finally:
        os.close(fd)


def limit_checks(root):
    controls = Path('/proc/sys/fs/inotify')
    saved = {name: (controls / name).read_text() for name in
             ('max_user_watches', 'max_user_instances', 'max_queued_events')}
    def setting(name, value):
        (controls / name).write_text(str(value) + '\n')
        assert int((controls / name).read_text()) == value
    path = root / 'limits'
    path.mkdir()
    descriptors = []
    try:
        for value in ('-1\n', '2147483648\n', '1x\n', '\n'):
            error(errno.EINVAL, lambda: (controls / 'max_user_watches').write_text(value))
        fd = create()
        descriptors.append(fd)
        wd = watch(fd, path, CREATE)
        setting('max_user_watches', 0)
        assert watch(fd, path, CREATE | MASK_ADD) == wd
        item = path / 'watched'
        item.touch()
        error(errno.ENOSPC, lambda: watch(fd, item))
        setting('max_user_instances', 0)
        error(errno.EMFILE, create)
        for name in ('max_user_watches', 'max_user_instances'):
            (controls / name).write_text(saved[name])
        setting('max_queued_events', 4)
        fd = create()
        descriptors.append(fd)
        wd = watch(fd, path, CREATE)
        # Queue capacity is captured at initialization, independent of later writes.
        setting('max_queued_events', 0)
        for index in range(8):
            (path / str(index)).touch()
        events = drain(fd)
        assert len(events) == 5, events
        assert sum(e[1] == Q_OVERFLOW and e[0] == -1 for e in events) == 1, events
        (path / 'after-overflow').touch()
        contains(drain(fd), wd, CREATE, 'after-overflow')
        zero_fd = create()
        descriptors.append(zero_fd)
        watch(zero_fd, path, CREATE)
        (path / 'zero-limit').touch()
        events = drain(zero_fd)
        assert events == [(-1, Q_OVERFLOW, 0, '')], events
    finally:
        for fd in descriptors:
            os.close(fd)
        for name, value in saved.items():
            (controls / name).write_text(value)


def mount_checks(root):
    path = root / 'mounted'
    path.mkdir()
    libc.mount.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                           ctypes.c_ulong, ctypes.c_void_p]
    libc.umount2.argtypes = [ctypes.c_char_p, ctypes.c_int]
    checked(libc.mount(b'tmpfs', os.fsencode(path), b'tmpfs', 0, None))
    mounted = True
    fd = create()
    try:
        wd = watch(fd, path, ALL)
        checked(libc.umount2(os.fsencode(path), 0))
        mounted = False
        events = drain(fd)
        contains(events, wd, UNMOUNT)
        contains(events, wd, IGNORED)
        error(errno.EINVAL, lambda: remove(fd, wd))
    finally:
        os.close(fd)
        if mounted:
            checked(libc.umount2(os.fsencode(path), 0))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', default='.', help='Filesystem with hard-link and rename support')
    parser.add_argument('--guest', action='store_true', help='Enable explicit guest-only system tests')
    parser.add_argument('--limits', action='store_true', help='Temporarily modify and restore inotify controls')
    parser.add_argument('--mounts', action='store_true', help='Temporarily mount tmpfs to validate unmount notifications')
    args = parser.parse_args()
    if (args.limits or args.mounts) and not args.guest:
        parser.error('--limits and --mounts require --guest')
    if (args.limits or args.mounts) and os.geteuid() != 0:
        parser.error('System tests require root privileges')
    signal.alarm(30)
    error(errno.EINVAL, lambda: create(1))
    controls = Path('/proc/sys/fs/inotify')
    for name in ('max_user_watches', 'max_user_instances', 'max_queued_events'):
        assert int((controls / name).read_text()) >= 0
    with tempfile.TemporaryDirectory(prefix='inotify-', dir=args.directory) as directory:
        root = Path(directory).resolve()
        filesystem_checks(root)
        path_reference_checks(root)
        late_watch_checks(root)
        mask_checks(root)
        queue_checks(root)
        wait_checks(root)
        async_checks(root)
        if args.limits:
            limit_checks(root)
        if args.mounts:
            mount_checks(root)
    signal.alarm(0)
    print('Filesystem notification checks: PASS')


if __name__ == '__main__':
    main()
