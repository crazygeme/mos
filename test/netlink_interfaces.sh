#!/bin/sh
# Validate route netlink snapshots and libc interface enumeration.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-netlink_interfaces.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import ctypes
import socket
import struct

for message in (18, 22):
    with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 0) as sock:
        sock.settimeout(5)
        sock.bind((0, 0))
        port = sock.getsockname()[0]
        assert port
        sequence = message + 100
        request = struct.pack('=IHHIIB', 17, message, 0x301, sequence, 0, 0)
        sock.sendto(request, (0, 0))
        payload = sock.recv(65536, socket.MSG_PEEK)
        assert sock.recv(65536) == payload
        count = 0
        done = False
        while not done:
            if not payload:
                payload = sock.recv(65536)
            length, kind, flags, seq, pid = struct.unpack_from('=IHHII', payload)
            assert length >= 16 and seq == sequence and pid == port
            if kind == 3:
                assert struct.unpack_from('=i', payload, 16)[0] == 0
                done = True
                break
            assert kind == message - 2 and flags & 2
            count += 1
            payload = payload[(length + 3) & ~3:]
        assert count

class Ifaddrs(ctypes.Structure):
    pass
Ifaddrs._fields_ = [('next', ctypes.POINTER(Ifaddrs)), ('name', ctypes.c_char_p),
                   ('flags', ctypes.c_uint), ('address', ctypes.c_void_p),
                   ('netmask', ctypes.c_void_p), ('broadcast', ctypes.c_void_p),
                   ('data', ctypes.c_void_p)]
libc = ctypes.CDLL(None, use_errno=True)
libc.getifaddrs.argtypes = [ctypes.POINTER(ctypes.POINTER(Ifaddrs))]
libc.freeifaddrs.argtypes = [ctypes.POINTER(Ifaddrs)]
head = ctypes.POINTER(Ifaddrs)()
assert libc.getifaddrs(ctypes.byref(head)) == 0, ctypes.get_errno()
try:
    names = set()
    current = head
    while current:
        entry = current.contents
        names.add(entry.name.decode())
        current = entry.next
    assert 'lo' in names and len(names) >= 2, names
finally:
    libc.freeifaddrs(head)
print('Route netlink interface checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
