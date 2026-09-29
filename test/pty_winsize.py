#!/usr/bin/env python3
"""Validate PTY size updates and SIGWINCH delivery to the foreground group."""

import fcntl
import os
import select
import signal
import struct
import termios

master, slave = os.openpty()
ready_r, ready_w = os.pipe()
stop_r, stop_w = os.pipe()
pid = os.fork()
if pid == 0:
    try:
        os.close(master)
        os.close(ready_r)
        os.close(stop_w)
        os.setsid()
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
        os.tcsetpgrp(slave, os.getpgrp())
        signal.signal(signal.SIGWINCH, lambda *_: os.write(ready_w, b'W'))
        os.write(ready_w, b'R')
        os.read(stop_r, 1)
        os._exit(0)
    except BaseException:
        os.write(ready_w, b'E')
        os._exit(1)

os.close(ready_w)
os.close(stop_r)


def expect_event(expected):
    assert select.select([ready_r], [], [], 3)[0], 'PTY signal timeout'
    event = os.read(ready_r, 1)
    assert event == expected, (event, expected)


try:
    expect_event(b'R')
    for fd, dims in (
        (master, (30, 120, 800, 600)),
        (slave, (40, 160, 1024, 768)),
        (master, (40, 160, 1280, 960)),
    ):
        size = struct.pack('HHHH', *dims)
        fcntl.ioctl(fd, termios.TIOCSWINSZ, size)
        expect_event(b'W')
        for endpoint in (master, slave):
            actual = fcntl.ioctl(endpoint, termios.TIOCGWINSZ, bytes(8))
            assert actual == size, (struct.unpack('HHHH', actual), dims)
            fcntl.ioctl(endpoint, termios.TIOCSWINSZ, size)
            assert not select.select([ready_r], [], [], 0.15)[0], (
                'Identical PTY dimensions generated SIGWINCH'
            )
    print('PTY dimensions and SIGWINCH: PASS')
finally:
    os.write(stop_w, b'Q')
    os.close(stop_w)
    _, status = os.waitpid(pid, 0)
    os.close(ready_r)
    os.close(slave)
    os.close(master)
    assert status == 0, status
