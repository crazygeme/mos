#!/bin/sh
# Validate protection changes across previously split mapping ranges.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-mprotect_ranges.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import ctypes
import mmap

libc = ctypes.CDLL(None, use_errno=True)
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
SIZE = 8 * 1024 * 1024
with mmap.mmap(-1, SIZE, flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS) as view:
    address = ctypes.addressof(ctypes.c_char.from_buffer(view))
    assert libc.mprotect(address, SIZE, 0) == 0
    for offset, length in ((2 * 1024 * 1024, mmap.PAGESIZE),
                           (4 * 1024 * 1024, mmap.PAGESIZE),
                           (0, 6 * 1024 * 1024)):
        assert libc.mprotect(address + offset, length, 3) == 0
    for offset in range(0, 6 * 1024 * 1024, mmap.PAGESIZE):
        view[offset] = 19
        assert view[offset] == 19
print('Split mapping protection checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
