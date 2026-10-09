#!/bin/sh
# Validate PTY write readiness and wakeups after draining or flushing.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-pty_poll_write.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'

import fcntl
import os
import select
import signal
import termios
import time
import tty


def fill(writer):
    poller = select.poll()
    poller.register(writer, select.POLLOUT)
    total = 0
    for _ in range(512):
        try:
            total += os.write(writer, b"x" * 65536)
            assert total < 4 * 1024 * 1024, "PTY did not apply backpressure"
        except BlockingIOError:
            # A host line discipline may still be moving queued input.
            if not poller.poll(0) and not select.select([], [writer], [], 0)[1]:
                return
            select.select([], [], [], 0.001)
    raise AssertionError("PTY reports writable after writes return EAGAIN")


def check(direction, operation, api):
    master, slave = os.openpty()
    gate_r, gate_w = os.pipe()
    child = None
    reaped = False
    try:
        tty.setraw(slave)
        for fd in (master, slave):
            flags = fcntl.fcntl(fd, fcntl.F_GETFL)
            fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        writer, reader = (slave, master) if direction == "slave" else (master, slave)
        fill(writer)
        child = os.fork()
        if child == 0:
            try:
                os.close(gate_w)
                os.read(gate_r, 1)
                select.select([], [], [], 0.1)
                if operation == "flush":
                    termios.tcflush(slave, termios.TCIOFLUSH)
                else:
                    while True:
                        try:
                            if not os.read(reader, 65536):
                                break
                        except BlockingIOError:
                            break
                # Keep SIGCHLD from acting as the write-readiness wakeup.
                os.read(gate_r, 1)
                os._exit(0)
            except BaseException:
                os._exit(1)
        os.close(gate_r)
        gate_r = -1
        started = time.monotonic()
        os.write(gate_w, b"R")
        if api == "select":
            ready = select.select([], [writer], [], 2)[1]
        else:
            poller = select.poll()
            events = select.POLLOUT
            if api == "poll-both":
                events |= select.POLLIN
            poller.register(writer, events)
            ready = any(fd == writer and flags & select.POLLOUT
                        for fd, flags in poller.poll(2000))
        assert ready, "PTY write waiter was not woken"
        if operation == "drain":
            assert time.monotonic() - started < 1, "PTY became ready only at timeout"
        assert os.write(writer, b"Y") == 1, "Writable PTY rejected output"
        os.write(gate_w, b"D")
        _, status = os.waitpid(child, 0)
        reaped = True
        assert status == 0, ("PTY reader failed", status)
        print(f"{direction} {operation} {api}: PASS", flush=True)
    finally:
        if child is not None and not reaped:
            try:
                os.kill(child, signal.SIGKILL)
            except ProcessLookupError:
                pass
            os.waitpid(child, 0)
        for fd in (gate_r, gate_w, slave, master):
            if fd >= 0:
                os.close(fd)


if __name__ == "__main__":
    for direction in ("slave", "master"):
        for operation in ("drain", "flush"):
            for api in ("poll", "poll-both", "select"):
                check(direction, operation, api)
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
