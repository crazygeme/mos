#!/bin/sh
# Validate rlimit output protection and writable copy-on-write pages.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-prlimit_protection.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import ctypes
import errno
import mmap
import os

libc = ctypes.CDLL(None, use_errno=True)
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
libc.prlimit64.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p,
                         ctypes.c_void_p]
with mmap.mmap(-1, mmap.PAGESIZE, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS) as view:
    address = ctypes.addressof(ctypes.c_char.from_buffer(view))
    view[:] = b'Q' * len(view)
    assert libc.mprotect(address, mmap.PAGESIZE, 1) == 0
    ctypes.set_errno(0)
    assert libc.prlimit64(0, 6, None, address) == -1
    assert ctypes.get_errno() == errno.EFAULT and view[:] == b'Q' * len(view)
    assert libc.mprotect(address, mmap.PAGESIZE, 3) == 0
    child = os.fork()
    if child == 0:
        result = libc.prlimit64(0, 6, None, address)
        os._exit(0 if result == 0 and view[:16] != b'Q' * 16 else 1)
    assert os.waitpid(child, 0)[1] == 0 and view[:] == b'Q' * len(view)
    assert libc.prlimit64(0, 6, None, address) == 0
print('Resource limit memory protection checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
