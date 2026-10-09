#!/bin/sh
# Validate MOS rejection of unavailable clone namespace isolation.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-clone_namespaces.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'

import ctypes
import errno
import os
import signal

libc = ctypes.CDLL(None, use_errno=True)
libc.syscall.restype = ctypes.c_long
number = 56 if ctypes.sizeof(ctypes.c_void_p) == 8 else 120
for namespace in (0x00020000, 0x02000000, 0x04000000, 0x08000000,
                  0x10000000, 0x20000000, 0x40000000):
    ctypes.set_errno(0)
    result = libc.syscall(ctypes.c_long(number),
                          ctypes.c_ulong(namespace | signal.SIGCHLD),
                          ctypes.c_void_p(0), ctypes.c_void_p(0),
                          ctypes.c_void_p(0), ctypes.c_void_p(0))
    if result == 0:
        os._exit(1)
    if result > 0:
        os.waitpid(result, 0)
    assert result == -1 and ctypes.get_errno() == errno.EINVAL, (
        hex(namespace), result, ctypes.get_errno())
print('Unavailable clone namespace checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
