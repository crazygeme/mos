#!/bin/sh
# Validate large socket vector I/O without payload-sized kernel buffers.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-socket_vector_large.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import os
import socket
import threading

parts = [bytes([index]) * (1024 * 1024) for index in range(4)]
expected = b''.join(parts)
left, right = socket.socketpair()
received = bytearray()
errors = []

def reader():
    try:
        while len(received) < len(expected):
            buffers = [bytearray(65536), bytearray(65536)]
            count = os.readv(right.fileno(), buffers)
            assert count > 0
            received.extend(b''.join(buffers)[:count])
    except BaseException as error:
        errors.append(error)

thread = threading.Thread(target=reader, daemon=True)
try:
    thread.start()
    pending = [memoryview(part) for part in parts]
    while pending:
        count = os.writev(left.fileno(), pending)
        assert count > 0
        while pending and count >= len(pending[0]):
            count -= len(pending.pop(0))
        if count:
            pending[0] = pending[0][count:]
    thread.join(10)
    assert not thread.is_alive() and not errors, errors
    assert received == expected
finally:
    left.close()
    right.close()
print('Large socket vector I/O checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
