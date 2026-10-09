#!/bin/sh
# Validate pipe nonblocking ioctl and descriptor metadata isolation.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-pipe_nonblocking_ioctl.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import array
import errno
import fcntl
import os

read, write = os.pipe()
try:
    name = os.readlink(f'/proc/self/fd/{read}')
    os.stat(f'/proc/self/fd/{read}')
    assert os.readlink(f'/proc/self/fd/{read}') == name
    for fd in (read, write):
        fcntl.ioctl(fd, 0x5421, array.array('i', [1]))
        assert fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_NONBLOCK
    try:
        os.read(read, 1)
    except BlockingIOError as error:
        assert error.errno == errno.EAGAIN
    else:
        raise AssertionError('Empty nonblocking pipe did not report EAGAIN')
    os.write(write, b'pipe')
    available = array.array('i', [0])
    fcntl.ioctl(read, 0x541b, available)
    assert available[0] == 4 and os.read(read, 4) == b'pipe'
    fcntl.ioctl(read, 0x5421, array.array('i', [0]))
    assert not fcntl.fcntl(read, fcntl.F_GETFL) & os.O_NONBLOCK
finally:
    os.close(read)
    os.close(write)
print('Pipe nonblocking ioctl checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
