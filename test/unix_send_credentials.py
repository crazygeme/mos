#!/usr/bin/env python3
"""Validate explicit Unix credentials with SO_PASSCRED disabled."""
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
