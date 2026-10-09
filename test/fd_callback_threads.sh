#!/bin/sh
# Exercise concurrent descriptor reception, socket polling, and socket ioctls.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-fd_callback_threads.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import array
import fcntl
import os
import select
import socket
import threading
import time

ITERATIONS = 5000
sender, receiver = socket.socketpair()
sender.settimeout(5)
receiver.settimeout(5)
passed = os.open('/dev/null', os.O_RDONLY)
errors = []
stop = threading.Event()
started = threading.Event()


def watch():
    poller = select.poll()
    poller.register(receiver, select.POLLIN | select.POLLOUT)
    started.set()
    try:
        while not stop.is_set():
            poller.poll(0)
            fcntl.ioctl(receiver, 0x541b, array.array('i', [0]), True)
            time.sleep(0.0001)
    except BaseException as error:
        errors.append(error)


def send():
    try:
        for _ in range(ITERATIONS):
            assert sender.sendmsg([b'x'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                          array.array('i', [passed]))]) == 1
            assert sender.recv(1) == b'a'
    except BaseException as error:
        errors.append(error)


watcher = threading.Thread(target=watch, daemon=True)
writer = threading.Thread(target=send, daemon=True)
watcher.start()
assert started.wait(2)
writer.start()
for _ in range(ITERATIONS):
    data, ancillary, flags, _ = receiver.recvmsg(1, socket.CMSG_SPACE(4))
    assert data == b'x' and not (flags & socket.MSG_CTRUNC)
    received = []
    for level, kind, payload in ancillary:
        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
            descriptors = array.array('i')
            descriptors.frombytes(payload)
            received.extend(descriptors)
    assert len(received) == 1, received
    os.close(received[0])
    assert receiver.send(b'a') == 1
stop.set()
watcher.join(3)
writer.join(3)
assert not watcher.is_alive() and not writer.is_alive() and not errors, errors
sender.close()
receiver.close()
os.close(passed)

# A subscription must retain its queue until deregistration after fd closure.
for _ in range(64):
    sender, receiver = socket.socketpair()
    fd = receiver.detach()
    started = threading.Event()
    errors = []

    def wait():
        try:
            poller = select.poll()
            poller.register(fd, select.POLLIN)
            started.set()
            poller.poll(200)
        except BaseException as error:
            errors.append(error)

    waiter = threading.Thread(target=wait, daemon=True)
    waiter.start()
    assert started.wait(1)
    time.sleep(0.01)
    os.close(fd)
    try:
        sender.send(b'x')
    except BrokenPipeError:
        pass
    waiter.join(1)
    assert not waiter.is_alive() and not errors, errors
    sender.close()
print(f'Descriptor reception/poll/ioctl stress: PASS ({ITERATIONS} transfers, 64 close races).')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
