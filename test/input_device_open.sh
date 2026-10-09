#!/bin/sh
# Validate the published PS/2 mouse endpoint without consuming input data.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-input_device_open.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'

import ctypes
import fcntl
import os
from pathlib import Path
import stat
import struct

IN_OPEN, IN_CLOSE_WRITE, IN_ISDIR = 0x20, 0x08, 0x40000000
libc = ctypes.CDLL(None, use_errno=True)
libc.inotify_init1.argtypes = [ctypes.c_int]
libc.inotify_init1.restype = ctypes.c_int
libc.inotify_add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
libc.inotify_add_watch.restype = ctypes.c_int


def checked(value):
    if value < 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))
    return value


parent = Path('/dev/input')
mouse = parent / 'mice'
assert parent.is_dir(), '/dev/input must be a directory'
assert 'input' in os.listdir('/dev'), '/dev must enumerate the input directory'
assert 'input/mice' not in os.listdir('/dev'), 'Directory entries must be immediate children'
assert 'mice' in os.listdir(parent)
info = mouse.stat()
assert stat.S_ISCHR(info.st_mode), info
assert (os.major(info.st_rdev), os.minor(info.st_rdev)) == (13, 63), info

notifications = checked(libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC))
try:
    wd = checked(libc.inotify_add_watch(notifications, os.fsencode(parent),
                                       IN_OPEN | IN_CLOSE_WRITE))
    # Xorg's xf86OpenSerial opens the endpoint with these access and status flags.
    fd = os.open(mouse, os.O_RDWR | os.O_NONBLOCK)
    try:
        assert stat.S_ISCHR(os.fstat(fd).st_mode)
        flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        assert flags & os.O_ACCMODE == os.O_RDWR
        assert flags & os.O_NONBLOCK
    finally:
        os.close(fd)
    events = []
    data = os.read(notifications, 4096)
    offset = 0
    while offset < len(data):
        event_wd, mask, cookie, length = struct.unpack_from('=iIII', data, offset)
        assert offset + 16 + length <= len(data)
        name = data[offset + 16:offset + 16 + length].split(b'\0', 1)[0]
        events.append((event_wd, mask, name))
        offset += 16 + length
    for required in (IN_OPEN, IN_CLOSE_WRITE):
        assert any(event_wd == wd and mask & required and name == b'mice'
                   for event_wd, mask, name in events), events
    assert not any(mask & IN_ISDIR for event_wd, mask, name in events), events
finally:
    os.close(notifications)
print('Mouse endpoint and notification checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
