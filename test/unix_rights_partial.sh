#!/bin/sh
# Validate descriptor delivery with the first partial stream read.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-unix_rights_partial.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import array
import fcntl
import mmap
import os
import socket
import tempfile

left, right = socket.socketpair()
try:
    with tempfile.TemporaryFile(dir='/dev/shm') as stream:
        stream.truncate(mmap.PAGESIZE)
        with mmap.mmap(stream.fileno(), mmap.PAGESIZE) as original:
            original[:] = b'P' * mmap.PAGESIZE
            left.sendmsg([b'header', b'payload'],
                         [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                           array.array('i', [stream.fileno()]))])
            data, control, flags, address = right.recvmsg(
                3, 128, socket.MSG_CMSG_CLOEXEC)
            assert data == b'hea' and len(control) == 1, (data, control)
            descriptors = array.array('i')
            descriptors.frombytes(control[0][2])
            assert len(descriptors) == 1
            fd = descriptors[0]
            try:
                assert fcntl.fcntl(fd, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
                with mmap.mmap(fd, mmap.PAGESIZE) as shared:
                    assert shared[:] == b'P' * mmap.PAGESIZE
                    shared[0] = 7
                    assert original[0] == 7
            finally:
                os.close(fd)
            remainder, control, flags, address = right.recvmsg(128, 128)
            assert remainder == b'derpayload' and not control
finally:
    left.close()
    right.close()
print('Partial stream descriptor delivery checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
