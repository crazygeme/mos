#!/bin/sh
# Validate descriptor-directory reopening and Bash process substitution.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-dev_fd.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'

import errno
import fcntl
import os
import stat
import subprocess
import tempfile


for directory in ('/dev/fd', '/proc/self/fd'):
    reader, writer = os.pipe()
    reopened = None
    try:
        link = f'{directory}/{writer}'
        assert stat.S_ISLNK(os.lstat(link).st_mode)
        assert stat.S_ISFIFO(os.stat(link).st_mode)
        assert str(writer) in os.listdir(directory)
        reopened = os.open(link, os.O_WRONLY | os.O_NONBLOCK)
        assert not fcntl.fcntl(writer, fcntl.F_GETFL) & os.O_NONBLOCK
        assert fcntl.fcntl(reopened, fcntl.F_GETFL) & os.O_NONBLOCK
        assert os.fstat(reopened).st_uid == os.geteuid()
        os.close(writer)
        writer = None
        os.write(reopened, b'pipe')
        assert os.read(reader, 4) == b'pipe'
        os.set_blocking(reader, False)
        try:
            os.read(reader, 1)
        except BlockingIOError as exc:
            assert exc.errno == errno.EAGAIN
        else:
            raise AssertionError('Pipe reached EOF with a reopened writer')
        os.close(reopened)
        reopened = None
        assert os.read(reader, 1) == b''
    finally:
        for descriptor in (reader, writer, reopened):
            if descriptor is not None:
                os.close(descriptor)

    reader, writer = os.pipe()
    reopened = os.open(f'{directory}/{reader}', os.O_RDONLY)
    os.close(reader)
    try:
        os.write(writer, b'reader')
        assert os.read(reopened, 6) == b'reader'
    finally:
        os.close(writer)
        os.close(reopened)

    reader, writer = os.pipe()
    reopened = os.open(f'{directory}/{reader}', os.O_RDWR)
    os.close(reader)
    os.close(writer)
    try:
        os.write(reopened, b'both')
        assert os.read(reopened, 4) == b'both'
    finally:
        os.close(reopened)

    with tempfile.TemporaryDirectory(prefix='dev-fd-') as tmp:
        file = open(os.path.join(tmp, 'file'), 'w+b')
        try:
            file.write(b'file')
            file.flush()
            reopened = os.open(f'{directory}/{file.fileno()}', os.O_RDONLY)
            try:
                assert os.read(reopened, 4) == b'file'
                assert file.tell() == 4
            finally:
                os.close(reopened)
        finally:
            file.close()

# Exercise the stdout/stderr redirections used by the browser launcher.
result = subprocess.run(['/bin/bash', '-c',
    'exec > >(exec cat); exec 2> >(exec cat >&2); '
    'printf "stdout\\n"; printf "stderr\\n" >&2'],
    capture_output=True, text=True, timeout=10, check=True)
assert result.stdout == 'stdout\n', result
assert result.stderr == 'stderr\n', result
result = subprocess.run(['/bin/bash', '-c', 'cat <(printf "input\\n")'],
    capture_output=True, text=True, timeout=10, check=True)
assert result.stdout == 'input\n' and result.stderr == '', result
print('Descriptor-directory and process-substitution checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
