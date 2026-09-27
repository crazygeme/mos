#!/usr/bin/env python3
"""Check that Unix recv and recvfrom wake a sender blocked in poll."""
import os
import select
import socket
import time

for operation in ("recv", "recvfrom"):
    sender, receiver = socket.socketpair()
    sender.setblocking(False)
    filled = 0
    block = bytes(range(256)) * 256
    while True:
        try:
            filled += sender.send(block)
        except BlockingIOError:
            break
    assert filled > 0
    poller = select.poll()
    poller.register(sender, select.POLLOUT)
    assert not poller.poll(0)
    ready_r, ready_w = os.pipe()
    child = os.fork()
    if child == 0:
        try:
            receiver.close()
            os.close(ready_r)
            os.write(ready_w, b"ready")
            events = poller.poll(2000)
            assert any(mask & select.POLLOUT for _, mask in events)
            os._exit(0)
        except BaseException:
            os._exit(1)
    os.close(ready_w)
    try:
        assert os.read(ready_r, 5) == b"ready"
        time.sleep(0.05)
        received = 0
        while received < filled:
            if operation == "recv":
                data = receiver.recv(min(65536, filled - received))
            else:
                data, _ = receiver.recvfrom(min(65536, filled - received))
            assert data
            assert data == bytes((received + i) % 256 for i in range(len(data)))
            received += len(data)
        assert os.waitpid(child, 0)[1] == 0, operation
    finally:
        sender.close()
        receiver.close()
        os.close(ready_r)
print("Unix receive sender-wakeup checks passed.")
