#!/bin/sh
# Validate Unix stream readiness and waiter enrollment during concurrent I/O.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-unix_wait_wakeup.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import os
import socket
import threading

left, right = socket.socketpair()
errors = []

def reply():
    try:
        for _ in range(20000):
            assert os.read(right.fileno(), 1) == b'q'
            assert os.write(right.fileno(), b'r') == 1
    except BaseException as error:
        errors.append(error)

thread = threading.Thread(target=reply, daemon=True)
thread.start()
for _ in range(20000):
    assert os.write(left.fileno(), b'q') == 1
    assert os.read(left.fileno(), 1) == b'r'
thread.join(5)
assert not thread.is_alive(), 'Reply thread did not finish'
assert not errors, errors
left.close()
right.close()
print('PASS: Unix stream request/reply wakeups remain observable')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
