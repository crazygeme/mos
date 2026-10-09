#!/bin/sh
# Validate page discard, mapping preservation, and native-width lengths.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-madvise.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import ctypes
import errno
import mmap
import os
import tempfile

PAGE = mmap.PAGESIZE
libc = ctypes.CDLL(None, use_errno=True)
libc.madvise.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
libc.madvise.restype = ctypes.c_int


def discard(address, length):
    assert libc.madvise(address, length, 4) == 0, ctypes.get_errno()


with mmap.mmap(-1, 3 * PAGE, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS) as view:
    address = ctypes.addressof(ctypes.c_char.from_buffer(view))
    view[:] = b'A' * len(view)
    discard(address + PAGE, 1)
    assert view[:] == b'A' * PAGE + b'\0' * PAGE + b'A' * PAGE
    view[PAGE] = 7
    child = os.fork()
    if child == 0:
        discard(address + PAGE, PAGE)
        os._exit(0 if view[PAGE] == 0 else 1)
    assert os.waitpid(child, 0)[1] == 0
    assert view[PAGE] == 7
    ctypes.set_errno(0)
    assert libc.madvise(address + 1, PAGE, 4) == -1
    assert ctypes.get_errno() == errno.EINVAL

with tempfile.TemporaryFile() as stream:
    stream.write(b'F' * PAGE)
    stream.flush()
    with mmap.mmap(stream.fileno(), PAGE, access=mmap.ACCESS_COPY) as view:
        address = ctypes.addressof(ctypes.c_char.from_buffer(view))
        view[0] = 9
        discard(address, PAGE)
        assert view[:] == b'F' * PAGE

if ctypes.sizeof(ctypes.c_void_p) == 8:
    length = (1 << 32) + PAGE
    with mmap.mmap(-1, length, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS) as view:
        address = ctypes.addressof(ctypes.c_char.from_buffer(view))
        view[0] = view[-1] = 11
        discard(address, length)
        assert view[0] == view[-1] == 0

print('Memory advice checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
