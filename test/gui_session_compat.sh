#!/bin/sh
# Validate GUI session socketpair and futex interfaces inside a MOS guest.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-gui_session_compat.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import ctypes
import errno
import fcntl
import os
import socket
import threading
import time

assert ctypes.sizeof(ctypes.c_void_p) == 4, "Requires the 32-bit MOS guest"
libc = ctypes.CDLL(None, use_errno=True)
libc.syscall.restype = ctypes.c_long
pair = (ctypes.c_int * 2)()
for flags in (0, socket.SOCK_CLOEXEC, socket.SOCK_NONBLOCK,
              socket.SOCK_CLOEXEC | socket.SOCK_NONBLOCK):
    assert libc.socketpair(socket.AF_UNIX, socket.SOCK_STREAM | flags, 0, pair) == 0
    try:
        for fd in pair:
            assert bool(fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC) == bool(flags & socket.SOCK_CLOEXEC)
            assert bool(fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_NONBLOCK) == bool(flags & socket.SOCK_NONBLOCK)
        assert os.write(pair[0], b"request") == 7
        assert os.read(pair[1], 7) == b"request"
        assert os.write(pair[1], b"reply") == 5
        assert os.read(pair[0], 5) == b"reply"
        if flags & socket.SOCK_NONBLOCK:
            try:
                os.read(pair[0], 1)
            except BlockingIOError:
                pass
            else:
                raise AssertionError("empty nonblocking socket did not return EAGAIN")
    finally:
        for fd in pair:
            os.close(fd)

class Timespec64(ctypes.Structure):
    _fields_ = [("sec", ctypes.c_int64), ("nsec", ctypes.c_int64)]

word = ctypes.c_int(0)
def futex(op, value=0, timeout=None, mask=0xffffffff):
    ctypes.set_errno(0)
    result = libc.syscall(422, ctypes.byref(word), op, value,
                          ctypes.byref(timeout) if timeout is not None else None,
                          None, ctypes.c_uint(mask))
    return result, ctypes.get_errno()

assert futex(128, 1) == (-1, errno.EAGAIN)
assert futex(128, timeout=Timespec64(0, 0)) == (-1, errno.ETIMEDOUT)
assert futex(128, timeout=Timespec64(0, 1000000000)) == (-1, errno.EINVAL)
assert futex(128 | 9, mask=0) == (-1, errno.EINVAL)
assert futex(128 | 9, timeout=Timespec64(0, 0)) == (-1, errno.ETIMEDOUT)
assert futex(128 | 9 | 256, timeout=Timespec64(0, 0)) == (-1, errno.ETIMEDOUT)
result = []
ready = threading.Event()
def wait():
    deadline = time.monotonic_ns() + 2_000_000_000
    timeout = Timespec64(deadline // 1_000_000_000, deadline % 1_000_000_000)
    ready.set()
    result.append(futex(128 | 9, timeout=timeout, mask=2))
thread = threading.Thread(target=wait)
thread.start()
assert ready.wait(1)
assert futex(128 | 10, 1, mask=1)[0] == 0
until = time.monotonic() + 1
while time.monotonic() < until:
    if futex(128 | 10, 1, mask=2)[0] == 1:
        break
    time.sleep(0.01)
else:
    raise AssertionError("masked futex waiter was not woken")
thread.join(3)
assert not thread.is_alive() and result == [(0, 0)], result
print("GUI session socketpair and futex checks passed.")
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
