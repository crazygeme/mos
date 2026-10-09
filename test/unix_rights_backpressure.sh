#!/bin/sh
# Validate stream descriptor queue backpressure and partial sends.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-unix_rights_backpressure.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import array
import errno
import os
import select
import socket
import tempfile
import threading


def receive(sock, value):
    data, control, flags, _ = sock.recvmsg(1, socket.CMSG_SPACE(4))
    assert data == value and len(control) == 1 and not flags, (data, control, flags)
    descriptors = array.array('i')
    descriptors.frombytes(control[0][2])
    assert len(descriptors) == 1
    for fd in descriptors:
        assert os.fstat(fd).st_size == 4096
        os.close(fd)


with tempfile.TemporaryFile(dir='/dev/shm') as backing:
    backing.truncate(4096)
    ancillary = [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                  array.array('i', [backing.fileno()]))]
    left, right = socket.socketpair()
    try:
        left.setblocking(False)
        count = 0
        while count < 10000:
            try:
                assert left.sendmsg([b'A'], ancillary) == 1
                count += 1
            except BlockingIOError as error:
                assert error.errno == errno.EAGAIN
                break
        else:
            raise AssertionError('Descriptor queue did not apply backpressure')
        assert count > 0 and not select.select([], [left], [], 0)[1]
        receive(right, b'A')
        assert select.select([], [left], [], 1)[1]
        assert left.sendmsg([b'B'], ancillary) == 1
        left.setblocking(True)
        completed = threading.Event()
        errors = []

        def sender():
            try:
                assert left.sendmsg([b'C'], ancillary) == 1
                completed.set()
            except BaseException as error:
                errors.append(error)

        thread = threading.Thread(target=sender, daemon=True)
        thread.start()
        assert not completed.wait(.05)
        receive(right, b'A')
        assert completed.wait(2), errors
        thread.join(2)
        assert not thread.is_alive() and not errors, errors
        for _ in range(count - 2):
            receive(right, b'A')
        receive(right, b'B')
        receive(right, b'C')
        left.setblocking(False)
        payload = b'P' * (4 * 1024 * 1024)
        sent = left.sendmsg([payload], ancillary)
        assert 0 < sent < len(payload), sent
        receive(right, b'P')
        remaining = sent - 1
        while remaining:
            data, control, flags, _ = right.recvmsg(min(32768, remaining), 128)
            assert data and data == b'P' * len(data) and not control
            remaining -= len(data)
    finally:
        left.close()
        right.close()
print('Stream descriptor backpressure checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
