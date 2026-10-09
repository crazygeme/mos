#!/bin/sh
# Verify creat returns a writable descriptor and truncates relative paths.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-creat_descriptor.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import ctypes
import os
import tempfile

libc = ctypes.CDLL(None, use_errno=True)
libc.creat.argtypes = [ctypes.c_char_p, ctypes.c_uint]
libc.creat.restype = ctypes.c_int
original = os.getcwd()
with tempfile.TemporaryDirectory() as directory:
    os.chdir(directory)
    try:
        held = [os.open('/dev/null', os.O_RDONLY) for _ in range(3)]
        descriptor = libc.creat(b'created.txt', 0o600)
        assert descriptor >= 0, ctypes.get_errno()
        assert descriptor not in held
        assert os.write(descriptor, b'created payload') == 15
        os.close(descriptor)
        assert open('created.txt', 'rb').read() == b'created payload'
        descriptor = libc.creat(b'created.txt', 0o600)
        assert descriptor >= 0, ctypes.get_errno()
        assert os.fstat(descriptor).st_size == 0
        os.close(descriptor)
        for descriptor in held:
            os.fstat(descriptor)
            os.close(descriptor)
    finally:
        os.chdir(original)
print('PASS: creat returns an open writable descriptor and truncates files')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
