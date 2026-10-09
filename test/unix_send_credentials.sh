#!/bin/sh
# Validate explicit Unix credentials with SO_PASSCRED disabled.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-unix_send_credentials.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import errno
import os
import socket
import struct

a, b = socket.socketpair()
try:
    cred = struct.pack("iII", os.getpid(), os.getuid(), os.getgid())
    assert a.sendmsg([b"\0"], [(socket.SOL_SOCKET, 2, cred)]) == 1
    data, controls, flags, _ = b.recvmsg(1, 128)
    assert data == b"\0" and controls == []
    try:
        a.sendmsg([b"x"], [(socket.SOL_SOCKET, 2, cred[:4])])
    except OSError as error:
        assert error.errno == errno.EINVAL
    else:
        raise AssertionError("malformed credentials accepted")
finally:
    a.close()
    b.close()
print("Unix credential send checks passed.")
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
