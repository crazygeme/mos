#!/bin/sh
# Validate parent-death signals inside a MOS guest without compilation.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-pdeathsig.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import ctypes
import errno
import os
import select
import signal
import sys

libc = ctypes.CDLL(None, use_errno=True)
libc.prctl.restype = ctypes.c_int


def set_signal(value):
    assert libc.prctl(1, value, 0, 0, 0) == 0, ctypes.get_errno()


def get_signal():
    value = ctypes.c_int(-1)
    assert libc.prctl(2, ctypes.byref(value), 0, 0, 0) == 0
    return value.value


if len(sys.argv) > 1 and sys.argv[1] == '--exec':
    assert get_signal() == signal.SIGUSR1
    sys.exit(0)

set_signal(signal.SIGUSR1)
assert get_signal() == signal.SIGUSR1
assert libc.prctl(1, -1, 0, 0, 0) == -1
assert ctypes.get_errno() == errno.EINVAL
assert get_signal() == signal.SIGUSR1
assert libc.prctl(2, None, 0, 0, 0) == -1
assert ctypes.get_errno() == errno.EFAULT

child = os.fork()
if child == 0:
    assert get_signal() == 0
    set_signal(signal.SIGUSR1)
    os.execv(sys.executable, [sys.executable, __file__, '--exec'])
assert os.waitpid(child, 0)[1] == 0
assert get_signal() == signal.SIGUSR1
set_signal(0)
assert get_signal() == 0

# Keep the observer alive while the monitored parent exits.
read_fd, write_fd = os.pipe()
parent = os.fork()
if parent == 0:
    os.close(read_fd)
    ready_read, ready_write = os.pipe()
    child = os.fork()
    if child == 0:
        os.close(ready_read)

        def received(signum, frame):
            os.write(write_fd, b'D')
            os._exit(0)

        signal.signal(signal.SIGUSR1, received)
        signal.alarm(5)
        set_signal(signal.SIGUSR1)
        os.write(ready_write, b'R')
        os.close(ready_write)
        while True:
            signal.pause()
    os.close(ready_write)
    assert os.read(ready_read, 1) == b'R'
    os._exit(0)
os.close(write_fd)
assert os.waitpid(parent, 0)[1] == 0
assert select.select([read_fd], [], [], 7)[0], 'Parent-death signal timed out'
assert os.read(read_fd, 1) == b'D'
os.close(read_fd)

if os.geteuid() == 0:
    child = os.fork()
    if child == 0:
        set_signal(signal.SIGUSR1)
        os.setgid(65534)
        assert get_signal() == 0
        set_signal(signal.SIGUSR1)
        os.setuid(65534)
        assert get_signal() == 0
        os._exit(0)
    assert os.waitpid(child, 0)[1] == 0

print('Parent-death signal checks passed.')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
