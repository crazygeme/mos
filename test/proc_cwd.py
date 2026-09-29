#!/usr/bin/env python3
"""Validate procfs working-directory links; --tmux also checks pane paths."""

import argparse
import ctypes
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tmux', action='store_true')
args = parser.parse_args()
libc = ctypes.CDLL(None, use_errno=True)
libc.readlink.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t]
libc.readlink.restype = ctypes.c_ssize_t


def check_link(link, expected):
    assert stat.S_ISLNK(os.lstat(link).st_mode), link
    assert os.readlink(link) == expected, (link, os.readlink(link), expected)
    assert os.stat(link).st_ino == os.stat(expected).st_ino, link
    assert 'cwd' in os.listdir(str(Path(link).parent)), link
    buf = ctypes.create_string_buffer(b'XXXX', 4)
    assert libc.readlink(os.fsencode(link), buf, 2) == min(2, len(os.fsencode(expected)))
    n = min(2, len(os.fsencode(expected)))
    assert buf.raw == os.fsencode(expected)[:n] + b'XXXX'[n:], buf.raw


original = os.getcwd()
with tempfile.TemporaryDirectory(prefix='proc-cwd-') as tmp:
    deep = Path(tmp) / 'one' / 'two' / 'three'
    deep.mkdir(parents=True)
    try:
        for path in ('/', tmp, str(deep)):
            os.chdir(path)
            check_link('/proc/self/cwd', path)
            check_link(f'/proc/{os.getpid()}/cwd', path)
    finally:
        os.chdir(original)

    ready_r, ready_w = os.pipe()
    done_r, done_w = os.pipe()
    pid = os.fork()
    if pid == 0:
        try:
            os.close(ready_r)
            os.close(done_w)
            os.chdir(deep)
            os.write(ready_w, b'R')
            os.read(done_r, 1)
            os._exit(0)
        except BaseException:
            os._exit(1)
    os.close(ready_w)
    os.close(done_r)
    try:
        assert os.read(ready_r, 1) == b'R'
        check_link(f'/proc/{pid}/cwd', str(deep))
    finally:
        os.close(done_w)
        os.close(ready_r)
        _, status = os.waitpid(pid, 0)
        assert status == 0, status
    print('procfs cwd links: PASS')

    if args.tmux:
        socket = str(Path(tmp) / 'tmux.sock')
        base = ['tmux', '-S', socket]
        try:
            subprocess.run(base + ['-f', '/dev/null', 'new-session', '-d',
                                   '-s', 'cwd-check', '-c', str(deep),
                                   'sleep 30'], check=True)
            deadline = time.monotonic() + 3
            while True:
                actual = subprocess.check_output(base + ['display-message', '-p',
                    '-t', 'cwd-check:0.0', '#{pane_current_path}'], text=True).strip()
                if actual == str(deep):
                    break
                assert time.monotonic() < deadline, ('pane_current_path', actual)
                time.sleep(0.05)
            print('tmux pane_current_path: PASS')
        finally:
            subprocess.run(base + ['kill-server'], check=False)
