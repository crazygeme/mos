#!/bin/sh
# Validate Unix sequenced packets, ancillary data, readiness, and shutdown.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-unix_seqpacket.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'

import array
import errno
import fcntl
import os
import select
import socket
import struct
import tempfile


def pair():
    return socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)


def expect_error(error, call):
    try:
        call()
    except OSError as exc:
        assert exc.errno == error, (exc, error)
    else:
        raise AssertionError(f'Expected errno {error}')


def pending(sock):
    result = array.array('i', [0])
    fcntl.ioctl(sock, 0x541b, result, True)
    return result[0]


with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as unconnected:
    expect_error(errno.ENOTCONN, lambda: unconnected.recv(1, socket.MSG_DONTWAIT))

a, b = pair()
try:
    assert a.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) == socket.SOCK_SEQPACKET
    a.sendmsg([b'first', b'-record'])
    a.send(b'second')
    assert pending(b) == len(b'first-recordsecond')
    payload, _, flags, _ = b.recvmsg(5, 0, socket.MSG_PEEK)
    assert payload == b'first' and flags & socket.MSG_TRUNC
    assert pending(b) == len(b'first-recordsecond')
    payload, _, flags, _ = b.recvmsg(5)
    assert payload == b'first' and flags & socket.MSG_TRUNC
    assert b.recv(64) == b'second'
    a.send(b'')
    a.send(b'after-empty')
    assert select.select([b], [], [], 0)[0] == [b]
    assert b.recv(1) == b''
    assert b.recv(64) == b'after-empty'
    a.send(b'large')
    out = bytearray(2)
    assert b.recv_into(out, 2, socket.MSG_TRUNC) == 5
    assert out == b'la'
    expect_error(errno.EAGAIN, lambda: b.recv(1, socket.MSG_DONTWAIT))
    payload = bytes(range(251)) * 300
    assert os.write(a.fileno(), payload) == len(payload)
    assert os.read(b.fileno(), len(payload)) == payload
    expect_error(errno.EMSGSIZE, lambda: a.send(b'x' * (512 * 1024)))
    expect_error(errno.EAGAIN, lambda: b.recv(1, socket.MSG_DONTWAIT))

    b.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 1)
    assert b.getsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED) == 1
    with tempfile.TemporaryFile() as first, tempfile.TemporaryFile() as second:
        first.write(b'one')
        second.write(b'two')
        first.seek(0)
        second.seek(0)
        for label, file in ((b'one', first), (b'two', second)):
            a.sendmsg([label], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                array.array('i', [file.fileno()]))])
        control_size = socket.CMSG_SPACE(12) + socket.CMSG_SPACE(4)
        for label, peek in ((b'one', True), (b'one', False), (b'two', False)):
            payload, ancillary, flags, _ = b.recvmsg(64, control_size,
                                                  socket.MSG_PEEK if peek else 0)
            assert payload == label and flags == 0, (payload, ancillary, flags)
            credentials = [data for level, kind, data in ancillary
                           if level == socket.SOL_SOCKET and kind == socket.SCM_CREDENTIALS]
            assert len(credentials) == 1
            assert struct.unpack('iII', credentials[0]) == (os.getpid(), os.getuid(), os.getgid())
            rights = [data for level, kind, data in ancillary
                      if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS]
            assert len(rights) == 1
            passed = array.array('i')
            passed.frombytes(rights[0])
            assert len(passed) == 1
            try:
                os.lseek(passed[0], 0, os.SEEK_SET)
                assert os.read(passed[0], 3) == label
            finally:
                os.close(passed[0])

    # Credential records identify the sender at send time, including fork.
    child = os.fork()
    if child == 0:
        try:
            b.close()
            a.send(b'child')
            os._exit(0)
        except BaseException:
            os._exit(1)
    payload, ancillary, flags, _ = b.recvmsg(64, socket.CMSG_SPACE(12))
    assert payload == b'child' and flags == 0
    assert struct.unpack('iII', ancillary[0][2]) == (child, os.getuid(), os.getgid())
    assert os.waitpid(child, 0)[1] == 0
    b.setsockopt(socket.SOL_SOCKET, socket.SO_PASSCRED, 0)
    with tempfile.TemporaryFile() as file:
        assert a.sendmsg([b'sixteen'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                         array.array('i', [file.fileno()] * 16))]) == 7
        payload, ancillary, flags, _ = b.recvmsg(64, socket.CMSG_SPACE(16 * 4))
        assert payload == b'sixteen' and flags == 0
        descriptors = array.array('i')
        descriptors.frombytes(ancillary[0][2])
        try:
            assert len(descriptors) == 16
        finally:
            for descriptor in descriptors:
                os.close(descriptor)

    # A full queue must reject a complete record without publishing a prefix.
    a.setblocking(False)
    records = 0
    while True:
        try:
            assert a.send(b'q' * 1024) == 1024
            records += 1
            assert records < 10000
        except BlockingIOError as exc:
            assert exc.errno == errno.EAGAIN
            break
    for _ in range(records):
        assert b.recv(2048) == b'q' * 1024
    expect_error(errno.EAGAIN, lambda: b.recv(1, socket.MSG_DONTWAIT))
    assert a.send(b'after-full') == 10
    assert b.recv(64) == b'after-full'
    a.shutdown(socket.SHUT_WR)
    assert b.recv(1) == b''
    assert b.send(b'reply') == 5
    assert a.recv(64) == b'reply'
    b.close()
    assert a.recv(1) == b''
    expect_error(errno.EPIPE, lambda: a.send(b'x', socket.MSG_NOSIGNAL))
finally:
    a.close()
    b.close()

with tempfile.TemporaryDirectory(prefix='seqpacket-') as directory:
    address = os.path.join(directory, 'socket')
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as listener:
        listener.bind(address)
        listener.listen(1)
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
            client.connect(address)
            connection, _ = listener.accept()
            with connection:
                client.send(b'first')
                client.send(b'second')
                assert connection.recv(64) == b'first'
                assert connection.recv(64) == b'second'

a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET |
                        socket.SOCK_NONBLOCK | socket.SOCK_CLOEXEC)
try:
    for endpoint in (a, b):
        assert fcntl.fcntl(endpoint, fcntl.F_GETFL) & os.O_NONBLOCK
        assert fcntl.fcntl(endpoint, fcntl.F_GETFD) & fcntl.FD_CLOEXEC
finally:
    a.close()
    b.close()
print('Unix sequenced-packet checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
