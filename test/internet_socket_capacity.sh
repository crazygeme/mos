#!/bin/sh
# Validate simultaneous IPv4 socket allocation and reuse.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-internet_socket_capacity.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import socket

for attempt in range(3):
    sockets = []
    try:
        for kind, count in ((socket.SOCK_STREAM, 64), (socket.SOCK_DGRAM, 32)):
            for _ in range(count):
                sock = socket.socket(socket.AF_INET, kind)
                sockets.append(sock)
                assert sock.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) == kind
    finally:
        for sock in sockets:
            sock.close()
print('IPv4 socket capacity checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
