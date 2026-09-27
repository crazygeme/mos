#!/usr/bin/env python3
"""Exercise process exit with threads blocked in socket and poll waits."""
import os
import select
import socket
import threading
import time

for operation in ("poll", "recv"):
    for _ in range(8):
        sender, receiver = socket.socketpair()
        ready_r, ready_w = os.pipe()
        child = os.fork()
        if child == 0:
            os.close(ready_r)
            sender.close()
            def wait():
                if operation == "poll":
                    poller = select.poll()
                    poller.register(receiver, select.POLLIN)
                    os.write(ready_w, b"r")
                    poller.poll(10000)
                else:
                    os.write(ready_w, b"r")
                    receiver.recv(1)
            threading.Thread(target=wait, daemon=True).start()
            time.sleep(0.1)
            os._exit(0)
        os.close(ready_w)
        try:
            assert os.read(ready_r, 1) == b"r"
            assert os.waitpid(child, 0)[1] == 0
            # Retain the endpoint so the send visits its wakeup lists after exit.
            assert sender.send(b"x") == 1
            assert receiver.recv(1) == b"x"
        finally:
            sender.close()
            receiver.close()
            os.close(ready_r)
print("Exit with blocked I/O waiters passed.")
