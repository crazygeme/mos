#!/bin/sh
# Validate metadata access through retained proc descriptor links.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc_fd_stat.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import os
import socket
import tempfile

def check(fd):
    direct = os.fstat(fd)
    linked = os.stat(f'/proc/self/fd/{fd}')
    assert (direct.st_mode, direct.st_ino, direct.st_dev) == (
        linked.st_mode, linked.st_ino, linked.st_dev)

read, write = os.pipe()
try:
    check(read)
    check(write)
finally:
    os.close(read)
    os.close(write)
with socket.socket() as sock:
    check(sock.fileno())
with tempfile.TemporaryDirectory() as directory:
    path = directory + '/deleted'
    with open(path, 'w+') as stream:
        os.unlink(path)
        check(stream.fileno())
print('Proc descriptor metadata checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
