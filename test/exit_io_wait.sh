#!/bin/sh
# Exercise process exit with active threads and blocked I/O waiters.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-exit_io_wait.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import ctypes
import os
import select
import socket
import threading
import time

libc = ctypes.CDLL(None)
for _ in range(32):
    ready_r, ready_w = os.pipe()
    child = os.fork()
    if child == 0:
        os.close(ready_r)

        def active():
            os.write(ready_w, b"r")
            while True:
                libc.getpid()

        threading.Thread(target=active, daemon=True).start()
        time.sleep(0.02)
        os._exit(0)
    os.close(ready_w)
    try:
        assert os.read(ready_r, 1) == b"r"
        assert os.waitpid(child, 0)[1] == 0
    finally:
        os.close(ready_r)

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
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
