#!/usr/bin/env python3
"""Validate AMD64 pathname flags, descriptor waits, and libc sleep calls."""

import ctypes as C
import errno
import fcntl
import mmap
import os
from pathlib import Path
import select
import signal
import socket
import stat
import tempfile
import time


class Time(C.Structure):
    _fields_ = [("sec", C.c_longlong), ("fraction", C.c_longlong)]


class MaskArg(C.Structure):
    _fields_ = [("mask", C.POINTER(C.c_ulonglong)), ("size", C.c_ulonglong)]


class PollFd(C.Structure):
    _fields_ = [("fd", C.c_int), ("events", C.c_short), ("revents", C.c_short)]


libc = C.CDLL(None, use_errno=True)
libc.syscall.restype = C.c_long
libc.nanosleep.argtypes = [C.POINTER(Time), C.POINTER(Time)]
libc.nanosleep.restype = C.c_int
libc.clock_nanosleep.argtypes = [C.c_int, C.c_int, C.POINTER(Time), C.POINTER(Time)]
libc.clock_nanosleep.restype = C.c_int


def call(number, *args, error=0):
    C.set_errno(0)
    converted = [C.c_long(arg) if isinstance(arg, int) else arg for arg in args]
    result = libc.syscall(C.c_long(number), *converted)
    if error:
        assert result == -1 and C.get_errno() == error, (number, result, C.get_errno())
    else:
        assert result >= 0, (number, result, C.get_errno())
    return result


def pathname_flags(base):
    target = base / "file"
    target.write_bytes(b"x")
    link = base / "link"
    link.symlink_to("file")
    dangling = base / "dangling"
    dangling.symlink_to("missing")
    assert link.is_symlink() and dangling.is_symlink()
    assert stat.S_ISREG(link.stat().st_mode)
    assert stat.S_ISLNK(dangling.lstat().st_mode)
    raw = (C.c_ubyte * 144)()
    fd = os.open(base, os.O_RDONLY | os.O_DIRECTORY)
    try:
        call(262, fd, b"dangling", raw, 0x100)
        assert int.from_bytes(bytes(raw)[24:28], "little") & 0o170000 == stat.S_IFLNK
        call(262, fd, b"dangling", raw, 0, error=errno.ENOENT)
        call(262, fd, b"", raw, 0x1000)
        assert int.from_bytes(bytes(raw)[24:28], "little") & 0o170000 == stat.S_IFDIR
        call(262, fd, b"", raw, 0, error=errno.ENOENT)
        call(262, fd, b"file", raw, 0x40000000, error=errno.EINVAL)
    finally:
        os.close(fd)


def readiness():
    reader, writer = os.pipe()
    try:
        os.write(writer, b"x")
        for minimum in (0, 40, 70):
            fd = fcntl.fcntl(reader, fcntl.F_DUPFD, minimum)
            try:
                nfds = fd + 1
                words = (nfds + 63) // 64
                for number in (23, 270):
                    bits = (C.c_ulonglong * (words + 1))()
                    bits[fd // 64] = 1 << (fd % 64)
                    if nfds % 64:
                        bits[words - 1] |= ((1 << 64) - 1) ^ ((1 << (nfds % 64)) - 1)
                    bits[words] = 0x123456789ABCDEF0
                    timeout = Time(0, 0)
                    args = [nfds, bits, None, None, C.byref(timeout)]
                    if number == 270:
                        args.append(None)
                    assert call(number, *args) == 1
                    assert list(bits)[:words] == [
                        1 << (fd % 64) if word == fd // 64 else 0
                        for word in range(words)
                    ], (number, fd, list(bits))
                    assert bits[words] == 0x123456789ABCDEF0
            finally:
                os.close(fd)
        descriptor = PollFd(reader, 1, 0)
        timeout = Time(0, 0)
        assert call(271, C.byref(descriptor), 1, C.byref(timeout), None, 8) == 1
        assert descriptor.revents & 1
        assert os.read(reader, 1) == b"x"
    finally:
        os.close(reader)
        os.close(writer)


def timeouts_and_masks():
    for number, units in ((23, 1000000), (270, 1000000000), (271, 1000000000)):
        timeout = Time(0, units // 20)
        start = time.monotonic()
        args = [0, None, None, None, C.byref(timeout)]
        if number == 270:
            args.append(None)
        elif number == 271:
            args = [None, 0, C.byref(timeout), None, 8]
        assert call(number, *args) == 0
        assert 0.035 <= time.monotonic() - start < 2
        assert timeout.sec == timeout.fraction == 0
    mask = C.c_ulonglong(0)
    timeout = Time(0, 0)
    argument = MaskArg(C.pointer(mask), 8)
    call(270, 0, None, None, None, C.byref(timeout), C.byref(argument))
    for size in (4, (1 << 32) + 8):
        argument.size = size
        call(270, 0, None, None, None, C.byref(timeout), C.byref(argument), error=errno.EINVAL)
    argument = MaskArg(None, 0)
    call(270, 0, None, None, None, C.byref(timeout), C.byref(argument))
    timeout = Time(0, 1000000000)
    call(270, 0, None, None, None, C.byref(timeout), None, error=errno.EINVAL)
    call(270, -1, None, None, None, None, None, error=errno.EINVAL)
    call(271, None, 0, None, C.byref(mask), 4, error=errno.EINVAL)


def interrupted_wait(number):
    received = []
    previous_handler = signal.signal(signal.SIGALRM, lambda *_: received.append(True))
    previous_mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGALRM})
    timeout = Time(3, 0)
    temporary = C.c_ulonglong((1 << (signal.SIGKILL - 1)) | (1 << (signal.SIGSTOP - 1)))
    argument = MaskArg(C.pointer(temporary), 8)
    try:
        signal.alarm(1)
        if number == 270:
            call(270, 0, None, None, None, C.byref(timeout), C.byref(argument), error=errno.EINTR)
        else:
            call(271, None, 0, C.byref(timeout), C.byref(temporary), 8, error=errno.EINTR)
        assert received and 0 < timeout.sec * 1000000000 + timeout.fraction < 3000000000
        assert signal.SIGALRM in signal.pthread_sigmask(signal.SIG_BLOCK, set())
    finally:
        signal.alarm(0)
        signal.pthread_sigmask(signal.SIG_SETMASK, previous_mask)
        signal.signal(signal.SIGALRM, previous_handler)


def libc_sleep():
    request = Time(0, 50000000)
    start = time.monotonic()
    assert libc.nanosleep(C.byref(request), None) == 0, C.get_errno()
    assert 0.035 <= time.monotonic() - start < 2
    for clockid in (0, 1):
        now = Time()
        call(228, clockid, C.byref(now))
        deadline = Time(now.sec, now.fraction + 50000000)
        if deadline.fraction >= 1000000000:
            deadline.sec += 1
            deadline.fraction -= 1000000000
        remainder = Time(123, 456)
        start = time.monotonic()
        assert libc.clock_nanosleep(clockid, 1, C.byref(deadline), C.byref(remainder)) == 0
        assert 0.035 <= time.monotonic() - start < 2
        assert (remainder.sec, remainder.fraction) == (123, 456)
    call(230, 99, 0, C.byref(request), None, error=errno.EINVAL)
    call(230, 1, 0, None, None, error=errno.EFAULT)
    start = time.monotonic()
    time.sleep(0.05)
    assert 0.035 <= time.monotonic() - start < 2
    start = time.monotonic()
    assert select.select([], [], [], 0.05) == ([], [], [])
    assert 0.035 <= time.monotonic() - start < 2


def pipe_wakeup():
    reader, writer = os.pipe()
    child = os.fork()
    if child == 0:
        try:
            os.close(reader)
            request = Time(0, 100000000)
            if libc.nanosleep(C.byref(request), None):
                os._exit(1)
            os.write(writer, b"w")
            os._exit(0)
        except BaseException:
            os._exit(1)
    os.close(writer)
    try:
        bits = (C.c_ulonglong * 16)()
        bits[reader // 64] = 1 << (reader % 64)
        timeout = Time(3, 0)
        assert call(270, reader + 1, bits, None, None, C.byref(timeout), None) == 1
        assert os.read(reader, 1) == b"w"
    finally:
        os.close(reader)
        _, status = os.waitpid(child, 0)
    assert status == 0, status


def socket_calls(base):
    path = str(base / "log.socket")
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as receiver:
        receiver.bind(path)
        assert receiver.getsockname() == path
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sender:
            sender.connect(path)
            assert sender.getpeername() == path
            assert sender.send(b"connected") == 9
            assert receiver.recv(32) == b"connected"
            assert sender.sendto(b"addressed", path) == 9
            assert receiver.recvfrom(32)[0] == b"addressed"
            sender.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
            assert sender.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF) > 0
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        assert udp.getsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR) == 1
        udp.bind(("127.0.0.1", 0))
        assert udp.getsockname()[1] > 0
    left, right = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    with left, right:
        assert left.send(b"pair") == 4
        assert right.recv(4) == b"pair"
        left.shutdown(socket.SHUT_WR)
        assert right.recv(4) == b""
    pair = (C.c_int * 2)()
    call(53, socket.AF_UNIX, socket.SOCK_STREAM | 0x800 | 0x80000, 0, pair)
    try:
        for fd in pair:
            assert fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_NONBLOCK
            assert fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
    finally:
        for fd in pair:
            os.close(fd)
    listener_path = str(base / "listener.socket")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        listener.bind(listener_path)
        listener.listen(1)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.connect(listener_path)
            accepted = call(288, listener.fileno(), None, None, 0x800 | 0x80000)
            try:
                assert fcntl.fcntl(accepted, fcntl.F_GETFL) & os.O_NONBLOCK
                assert fcntl.fcntl(accepted, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
            finally:
                os.close(accepted)


def socket_timeouts():
    left, right = socket.socketpair()
    signal.alarm(5)
    try:
        timeout = Time(0, 20000)
        returned = Time(-1, -1)
        length = C.c_uint(C.sizeof(returned))
        for endpoint, option in ((left, socket.SO_RCVTIMEO),
                                 (right, socket.SO_SNDTIMEO)):
            call(54, endpoint.fileno(), socket.SOL_SOCKET, option,
                 C.byref(timeout), C.sizeof(timeout))
            call(55, endpoint.fileno(), socket.SOL_SOCKET, option,
                 C.byref(returned), C.byref(length))
            assert length.value == C.sizeof(returned)
            assert (returned.sec, returned.fraction) == (0, 20000)
        buffer = C.create_string_buffer(65536)
        call(0, left.fileno(), buffer, 1, error=errno.EAGAIN)
        right.setblocking(False)
        while True:
            try:
                os.write(right.fileno(), buffer.raw)
            except BlockingIOError:
                break
        right.setblocking(True)
        call(1, right.fileno(), buffer, 1, error=errno.EAGAIN)
        invalid = Time(0, 1000000)
        call(54, left.fileno(), socket.SOL_SOCKET, socket.SO_RCVTIMEO,
             C.byref(invalid), C.sizeof(invalid), error=errno.EINVAL)
        call(54, left.fileno(), socket.SOL_SOCKET, socket.SO_RCVTIMEO,
             C.byref(timeout), 8, error=errno.EINVAL)
        short = C.c_uint(8)
        returned = Time(-1, -1)
        call(55, left.fileno(), socket.SOL_SOCKET, socket.SO_RCVTIMEO,
             C.byref(returned), C.byref(short))
        assert short.value == C.sizeof(returned)
        assert returned.sec == 0 and returned.fraction == -1
    finally:
        signal.alarm(0)
        left.close()
        right.close()


def timed_futex():
    word = C.c_int(0)
    call(202, C.byref(word), 128, 1, None, None, 0, error=errno.EAGAIN)
    timeout = Time(0, 20000000)
    call(202, C.byref(word), 128, 0, C.byref(timeout), None, 0,
         error=errno.ETIMEDOUT)
    deadline = time.monotonic_ns() + 20000000
    timeout = Time(deadline // 1000000000, deadline % 1000000000)
    call(202, C.byref(word), 137, 0, C.byref(timeout), None, 0xffffffff,
         error=errno.ETIMEDOUT)


def shared_render_fence(base):
    path = base / "render-fence"
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        os.unlink(path)
        os.ftruncate(fd, 4)
        assert os.fstat(fd).st_size == 4
        with mmap.mmap(fd, 4, flags=mmap.MAP_SHARED) as producer:
            with mmap.mmap(fd, 4, flags=mmap.MAP_SHARED) as consumer:
                producer[:] = b"sync"
                assert consumer[:] == b"sync"
        call(77, -1, 4, error=errno.EBADF)
    finally:
        os.close(fd)


if __name__ == "__main__":
    assert C.sizeof(C.c_void_p) == 8 and os.uname().machine == "x86_64", "AMD64 userspace is required."
    with tempfile.TemporaryDirectory(prefix="x64-console-abi-") as directory:
        pathname_flags(Path(directory))
        socket_calls(Path(directory))
        shared_render_fence(Path(directory))
    readiness()
    timeouts_and_masks()
    interrupted_wait(270)
    interrupted_wait(271)
    libc_sleep()
    pipe_wakeup()
    timed_futex()
    socket_timeouts()
    print("AMD64 console ABI: PASS")
