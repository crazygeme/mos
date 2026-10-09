#!/bin/sh
# Validate page faults concurrent with neighboring VMA protection changes.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-mapping_fault_race.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import ctypes
import os
import threading

libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long]
libc.mmap.restype = ctypes.c_void_p
for name in ['madvise', 'mprotect']:
    getattr(libc, name).argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
libc.memset.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_size_t]
page = os.sysconf('SC_PAGE_SIZE')
base = libc.mmap(None, page * 5, 3, 0x22, -1, 0)
assert base != ctypes.c_void_p(-1).value, ctypes.get_errno()
errors = []
start = threading.Barrier(5)

def fault(index):
    try:
        start.wait()
        for _ in range(2000):
            address = base + index * page
            assert libc.madvise(address, page, 4) == 0
            libc.memset(address, index + 1, page)
            assert ctypes.c_ubyte.from_address(address).value == index + 1
    except BaseException as error:
        errors.append(error)

def protect():
    try:
        start.wait()
        for _ in range(2000):
            assert libc.mprotect(base + 4 * page, page, 1) == 0
            assert libc.mprotect(base + 4 * page, page, 3) == 0
    except BaseException as error:
        errors.append(error)

threads = [threading.Thread(target=fault, args=(i,)) for i in range(4)]
threads.append(threading.Thread(target=protect))
for thread in threads:
    thread.start()
for thread in threads:
    thread.join()
assert not errors, errors
print('PASS: page faults remain valid during concurrent VMA replacement')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
