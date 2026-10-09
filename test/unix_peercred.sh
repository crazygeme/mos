#!/bin/sh
# Validate Unix peer credential snapshots and short option buffers.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-unix_peercred.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import os
from pathlib import Path
import socket
import struct
import tempfile

SO_PEERCRED = 17

def credentials(sock):
    return struct.unpack("iII", sock.getsockopt(socket.SOL_SOCKET, SO_PEERCRED, 12))

expected = (os.getpid(), os.geteuid(), os.getegid())
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as unconnected:
    assert credentials(unconnected) == (0, 0xffffffff, 0xffffffff)
for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
    a, b = socket.socketpair(socket.AF_UNIX, kind)
    try:
        assert credentials(a) == expected
        assert credentials(b) == expected
        assert a.getsockopt(socket.SOL_SOCKET, SO_PEERCRED, 4) == struct.pack("i", os.getpid())
        b.close()
        assert credentials(a) == expected
    finally:
        a.close()
        b.close()

with tempfile.TemporaryDirectory(prefix="peercred-") as directory:
    address = str(Path(directory) / "socket")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(address)
        server.listen(1)
        ready_r, ready_w = os.pipe()
        child = os.fork()
        if child == 0:
            try:
                os.close(ready_r)
                server.close()
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                    client.connect(address)
                    assert credentials(client) == expected
                    if os.geteuid() == 0:
                        os.setgid(65534)
                        os.setuid(65534)
                    os.write(ready_w, b"ready")
                    assert client.recv(2) == b"ok"
                os._exit(0)
            except BaseException:
                os._exit(1)
        os.close(ready_w)
        try:
            connection, _ = server.accept()
            with connection:
                assert os.read(ready_r, 5) == b"ready"
                assert credentials(connection) == (child, expected[1], expected[2])
                connection.sendall(b"ok")
                assert connection.recv(1) == b""
                assert credentials(connection) == (child, expected[1], expected[2])
            assert os.waitpid(child, 0)[1] == 0
        finally:
            os.close(ready_r)
print("Unix peer credential checks passed.")
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
