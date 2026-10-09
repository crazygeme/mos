#!/bin/sh
# Validate descriptor disposal by ordinary Unix stream reads.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-unix_read_rights.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import array
import os
import socket

left, right = socket.socketpair()
fd = os.open('/dev/null', os.O_RDONLY)
try:
    left.sendmsg([b'ab'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                           array.array('i', [fd]))])
    left.sendall(b'cd')
    assert os.read(right.fileno(), 1) == b'a'
    payload, ancillary, flags, _ = right.recvmsg(16, socket.CMSG_SPACE(4))
    assert payload == b'bcd', payload
    assert ancillary == [], ancillary
    left.sendall(b'next')
    assert os.read(right.fileno(), 4) == b'next'
finally:
    os.close(fd)
    left.close()
    right.close()
print('PASS: ordinary reads discard consumed descriptor rights without false EOF')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
