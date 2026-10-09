#!/bin/sh
# Verify sparse address-space cloning and copy-on-write isolation.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-sparse_fork.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import ctypes
import os
import time

libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long]
libc.mmap.restype = ctypes.c_void_p
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
size = 1 << 40
page = os.sysconf('SC_PAGE_SIZE')
base = libc.mmap(None, size, 0, 0x22, -1, 0)
assert base != ctypes.c_void_p(-1).value, ctypes.get_errno()
addresses = [base, base + size // 2, base + size - page]
for address in addresses:
    assert libc.mprotect(address, page, 3) == 0, ctypes.get_errno()
    ctypes.c_uint32.from_address(address).value = 0x12345678
started = time.monotonic()
pid = os.fork()
if pid == 0:
    for address in addresses:
        if ctypes.c_uint32.from_address(address).value != 0x12345678:
            os._exit(1)
        ctypes.c_uint32.from_address(address).value = 0x87654321
    os._exit(0)
_, status = os.waitpid(pid, 0)
assert status == 0, status
assert time.monotonic() - started < 5, 'Sparse fork exceeded five seconds'
for address in addresses:
    assert ctypes.c_uint32.from_address(address).value == 0x12345678
print('PASS: sparse fork preserves mapped pages and copy-on-write isolation')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
