#!/usr/bin/env python3
"""Validate event counters, descriptor flags, epoll notifications, and waits."""

import ctypes
import errno
import fcntl
import os
import select
import signal
import struct
import time

libc = ctypes.CDLL(None, use_errno=True)
libc.eventfd.argtypes = [ctypes.c_uint, ctypes.c_int]
libc.eventfd.restype = ctypes.c_int
libc.syscall.restype = ctypes.c_long
EFD_SEMAPHORE = 1
EFD_NONBLOCK = os.O_NONBLOCK
EFD_CLOEXEC = os.O_CLOEXEC
MAX = (1 << 64) - 2
signal.alarm(15)


def create(initial=0, flags=EFD_NONBLOCK):
    fd = libc.eventfd(initial, flags)
    assert fd >= 0, ctypes.get_errno()
    return fd


def read(fd, size=8):
    return struct.unpack('Q', os.read(fd, size))[0]


def write(fd, value):
    assert os.write(fd, struct.pack('Q', value)) == 8


def expect_error(error, call):
    try:
        call()
    except OSError as exc:
        assert exc.errno == error, (exc, error)
    else:
        raise AssertionError(f'Expected errno {error}')


assert libc.eventfd(0, 0x100) == -1 and ctypes.get_errno() == errno.EINVAL
fd = create(7, EFD_NONBLOCK | EFD_CLOEXEC)
try:
    assert fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_NONBLOCK
    assert fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
    assert read(fd, 16) == 7
    expect_error(errno.EAGAIN, lambda: read(fd))
    expect_error(errno.EINVAL, lambda: os.read(fd, 7))
    expect_error(errno.EINVAL, lambda: os.write(fd, b'x' * 7))
    expect_error(errno.EINVAL, lambda: os.write(fd, b'x' * 16))
    expect_error(errno.EINVAL, lambda: write(fd, MAX + 1))
    assert os.lseek(fd, 1, os.SEEK_SET) == 0
    write(fd, 2)
    write(fd, 5)
    duplicate = os.dup(fd)
    try:
        assert read(duplicate) == 7
        expect_error(errno.EAGAIN, lambda: read(fd))
    finally:
        os.close(duplicate)
    with select.epoll() as epoll:
        epoll.register(fd, select.EPOLLIN)
        assert epoll.poll(0) == []
        write(fd, 1)
        assert epoll.poll(0.5) == [(fd, select.EPOLLIN)]
        assert read(fd) == 1
        assert epoll.poll(0) == []
    write(fd, MAX)
    assert select.select([], [fd], [], 0)[1] == []
    expect_error(errno.EAGAIN, lambda: write(fd, 1))
    assert read(fd) == MAX
    assert select.select([], [fd], [], 0)[1] == [fd]
finally:
    os.close(fd)

fd = create(3, EFD_SEMAPHORE | EFD_NONBLOCK)
try:
    assert [read(fd), read(fd), read(fd)] == [1, 1, 1]
    expect_error(errno.EAGAIN, lambda: read(fd))
finally:
    os.close(fd)

# The original eventfd syscall provides a blocking descriptor without flags.
number = 284 if ctypes.sizeof(ctypes.c_void_p) == 8 else 323
fd = libc.syscall(ctypes.c_long(number), ctypes.c_uint(0xffffffff))
assert fd >= 0, ctypes.get_errno()
try:
    assert not fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_NONBLOCK
    assert not fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
    assert read(fd) == 0xffffffff
finally:
    os.close(fd)

# Read and overflow-write waits share the counter across forked processes.
for overflow in (False, True):
    fd = create(0, 0)
    if overflow:
        write(fd, MAX)
    child = os.fork()
    if child == 0:
        try:
            time.sleep(0.05)
            if overflow:
                assert read(fd) == MAX
            else:
                write(fd, 9)
            os.close(fd)
            os._exit(0)
        except BaseException:
            os._exit(1)
    try:
        if overflow:
            write(fd, 4)
            assert read(fd) == 4
        else:
            assert read(fd) == 9
        assert os.waitpid(child, 0)[1] == 0
    finally:
        os.close(fd)
signal.alarm(0)
print('Event counter checks: PASS')
