#!/usr/bin/env python3
"""Validate IPC bulk transfers, ring wrapping, readiness, and shutdown."""

import array
import errno
import fcntl
import os
import select
import signal
import socket
import struct
import tempfile


def set_blocking(fd, blocking):
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    flags = flags & ~os.O_NONBLOCK if blocking else flags | os.O_NONBLOCK
    fcntl.fcntl(fd, fcntl.F_SETFL, flags)


def transfer(kind, fragment):
    """Compare the complete stream across independently fragmented I/O."""
    payload = bytes(range(251)) * (17 if fragment == 1 else 8357)
    endpoints = []
    with tempfile.TemporaryDirectory(prefix="ipc-buffers-") as directory:
        if kind == "pipe":
            reader, writer = os.pipe()
        elif kind == "fifo":
            path = os.path.join(directory, "fifo")
            os.mkfifo(path)
            reader = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            writer = os.open(path, os.O_WRONLY)
            set_blocking(reader, True)
        elif kind == "pair":
            endpoints = list(socket.socketpair())
            reader, writer = (endpoint.fileno() for endpoint in endpoints)
        else:
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            path = os.path.join(directory, "socket")
            listener.bind(path)
            listener.listen(1)
            sender = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sender.connect(path)
            receiver, _ = listener.accept()
            listener.close()
            endpoints = [receiver, sender]
            reader, writer = receiver.fileno(), sender.fileno()

        child = os.fork()
        if child == 0:
            try:
                os.close(reader)
                offset = 0
                while offset < len(payload):
                    written = os.write(writer, payload[offset:offset + fragment])
                    assert written > 0
                    offset += written
                os.close(writer)
                os._exit(0)
            except BaseException:
                os._exit(1)

        if endpoints:
            endpoints[1].close()
        else:
            os.close(writer)
        try:
            offset = 0
            while True:
                data = os.read(reader, 65521)
                if not data:
                    break
                assert data == payload[offset:offset + len(data)], (kind, fragment, offset)
                offset += len(data)
            assert offset == len(payload), (kind, fragment, offset)
            assert os.waitpid(child, 0)[1] == 0, (kind, fragment)
        finally:
            if endpoints:
                endpoints[0].close()
            else:
                os.close(reader)


def readiness(kind):
    endpoints = []
    if kind == "pipe":
        reader, writer = os.pipe()
    else:
        endpoints = list(socket.socketpair())
        reader, writer = (endpoint.fileno() for endpoint in endpoints)
    try:
        set_blocking(reader, False)
        set_blocking(writer, False)
        try:
            os.read(reader, 1)
        except BlockingIOError:
            pass
        else:
            raise AssertionError("Empty IPC stream must return EAGAIN.")
        block = bytes(range(256)) * 256
        filled = 0
        while True:
            try:
                filled += os.write(writer, block)
            except BlockingIOError:
                break
        assert filled > 0
        poller = select.poll()
        poller.register(writer, select.POLLOUT)
        assert not poller.poll(0)
        offset = 0
        while offset < filled:
            data = os.read(reader, min(4093, filled - offset))
            assert data == bytes((offset + i) % 256 for i in range(len(data)))
            offset += len(data)
        assert poller.poll(0)[0][1] & select.POLLOUT
        if endpoints:
            endpoints[0].close()
        else:
            os.close(reader)
        reader = -1
        try:
            os.write(writer, b"x")
        except BrokenPipeError:
            pass
        else:
            raise AssertionError("Closed IPC reader must return EPIPE.")
    finally:
        if endpoints:
            for endpoint in endpoints:
                endpoint.close()
        else:
            if reader >= 0:
                os.close(reader)
            os.close(writer)


def datagrams():
    sender, receiver = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        # A truncated record must not contaminate the following record.
        for _ in range(10):
            assert sender.send(b"a" * 3001) == 3001
            assert receiver.recv(7) == b"a" * 7
            assert sender.send(b"next") == 4
            assert receiver.recv(100) == b"next"
        try:
            sender.send(b"x" * 1048576)
        except OSError as error:
            assert error.errno in (errno.ENOBUFS, errno.EMSGSIZE)
        else:
            raise AssertionError("Oversized Unix datagram accepted.")
    finally:
        sender.close()
        receiver.close()


def socket_timeouts():
    sender, receiver = socket.socketpair()
    try:
        timeout = struct.pack("@ll", 0, 20000)
        receiver.setsockopt(socket.SOL_SOCKET, socket.SO_RCVTIMEO, timeout)
        try:
            os.read(receiver.fileno(), 1)
        except BlockingIOError:
            pass
        else:
            raise AssertionError("Socket receive timeout must return EAGAIN.")
        sender.setblocking(False)
        while True:
            try:
                sender.send(b"x" * 65536)
            except BlockingIOError:
                break
        sender.setblocking(True)
        sender.setsockopt(socket.SOL_SOCKET, socket.SO_SNDTIMEO, timeout)
        try:
            os.write(sender.fileno(), b"x")
        except BlockingIOError:
            pass
        else:
            raise AssertionError("Socket send timeout must return EAGAIN.")
        receiver.setsockopt(socket.SOL_SOCKET, socket.SO_RCVTIMEO,
                            struct.pack("@ll", 0, 0))
        sender.shutdown(socket.SHUT_WR)
        while receiver.recv(65536):
            pass
    finally:
        sender.close()
        receiver.close()


def descriptor_transfer():
    sender, receiver = socket.socketpair()
    reader, writer = os.pipe()
    payload = bytes(range(251)) * 399
    try:
        for _ in range(6):
            rights = array.array("i", [reader])
            assert sender.sendmsg([payload[:4093], payload[4093:]],
                                  [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                    rights)]) == len(payload)
            received = bytearray()
            descriptors = array.array("i")
            while len(received) < len(payload):
                data, controls, flags, _ = receiver.recvmsg(
                    len(payload) - len(received), socket.CMSG_SPACE(rights.itemsize))
                assert data and not flags & socket.MSG_CTRUNC
                received.extend(data)
                for level, kind, value in controls:
                    if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                        descriptors.frombytes(value)
            try:
                assert received == payload and len(descriptors) == 1
                os.write(writer, b"fd")
                assert os.read(descriptors[0], 2) == b"fd"
            finally:
                for fd in descriptors:
                    os.close(fd)
    finally:
        sender.close()
        receiver.close()
        os.close(reader)
        os.close(writer)


signal.alarm(60)
for kind in ("pipe", "fifo", "pair", "named"):
    for fragment in (1, 4093, 65536, 262147):
        transfer(kind, fragment)
    print(kind + " transfer checks passed.", flush=True)
for kind in ("pipe", "pair"):
    readiness(kind)
datagrams()
socket_timeouts()
descriptor_transfer()
signal.alarm(0)
print("IPC buffer checks passed.")
