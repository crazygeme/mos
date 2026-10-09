#!/bin/sh
# Validate procfs executable links and readlinkat in the running system.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc_exe.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'

import ctypes
import errno
import os
from pathlib import Path
import stat
import sys
import tempfile

libc = ctypes.CDLL(None, use_errno=True)
libc.readlink.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t]
libc.readlink.restype = ctypes.c_ssize_t
libc.readlinkat.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p,
                          ctypes.c_size_t]
libc.readlinkat.restype = ctypes.c_ssize_t


def check_link(link, expected):
    assert stat.S_ISLNK(os.lstat(link).st_mode), link
    assert os.readlink(link) == expected, (link, os.readlink(link), expected)
    assert os.stat(link).st_ino == os.stat(expected).st_ino, link
    assert 'exe' in os.listdir(str(Path(link).parent)), link
    encoded = os.fsencode(expected)
    for capacity in (2, 4096):
        buffer = ctypes.create_string_buffer(b'X' * (capacity + 1))
        length = libc.readlink(os.fsencode(link), buffer, capacity)
        count = min(capacity, len(encoded))
        assert length == count, (link, length, ctypes.get_errno())
        assert buffer.raw[:count] == encoded[:count], buffer.raw
        assert buffer.raw[count:capacity + 1] == b'X' * (capacity + 1 - count)
        length = libc.readlinkat(-100, os.fsencode(link), buffer, capacity)
        assert length == count, (link, length, ctypes.get_errno())
        assert buffer.raw[:count] == encoded[:count], buffer.raw
    fd = os.open(str(Path(link).parent), os.O_RDONLY | os.O_DIRECTORY)
    try:
        buffer = ctypes.create_string_buffer(4096)
        length = libc.readlinkat(fd, b'exe', buffer, len(buffer))
        assert length == len(encoded), (length, ctypes.get_errno())
        assert buffer.raw[:length] == encoded, buffer.raw
    finally:
        os.close(fd)


def check_self(expected):
    check_link('/proc/self/exe', expected)
    check_link(f'/proc/{os.getpid()}/exe', expected)


if len(sys.argv) == 3 and sys.argv[1] == '--exec-check':
    os.chdir('/')
    check_self(sys.argv[2])
    print('procfs executable links: PASS')
    sys.exit(0)

expected = str(Path(sys.executable).resolve())
check_self(expected)
script = str(Path(__file__).resolve())
with tempfile.TemporaryDirectory(prefix='proc-exe-') as tmp:
    invalid = Path(tmp) / 'invalid'
    invalid.write_text('invalid executable\n')
    invalid.chmod(0o700)
    for path, error in ((str(invalid), errno.ENOEXEC),
                        (str(Path(tmp) / 'missing'), errno.ENOENT)):
        try:
            os.execve(path, ['unrelated-name'], os.environ.copy())
        except OSError as exc:
            assert exc.errno == error, exc
        else:
            raise AssertionError('Invalid executable accepted')
        check_self(expected)

    alias = Path(tmp) / 'python-alias'
    alias.symlink_to(expected)
    ready_r, ready_w = os.pipe()
    done_r, done_w = os.pipe()
    pid = os.fork()
    if pid == 0:
        try:
            os.close(ready_r)
            os.close(done_w)
            os.chdir(tmp)
            check_self(expected)
            os.write(ready_w, b'R')
            os.read(done_r, 1)
            os.close(ready_w)
            os.close(done_r)
            os.execve('./python-alias', ['unrelated-name', script,
                       '--exec-check', expected], os.environ.copy())
        except BaseException:
            import traceback
            traceback.print_exc()
            os._exit(1)
    os.close(ready_w)
    os.close(done_r)
    try:
        assert os.read(ready_r, 1) == b'R'
        check_link(f'/proc/{pid}/exe', expected)
    finally:
        os.close(ready_r)
        os.close(done_w)
        _, status = os.waitpid(pid, 0)
        assert status == 0, status
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
