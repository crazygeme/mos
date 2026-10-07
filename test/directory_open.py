#!/usr/bin/env python3
"""Check directory-only opens and copy destination classification."""

import errno
import os
from pathlib import Path
import stat
import subprocess
import tempfile


with tempfile.TemporaryDirectory(prefix='mos-directory-open-') as temporary:
    base = Path(temporary)
    source = base / 'source'
    destination = base / 'destination'
    source.write_bytes(b'new payload\n')
    destination.write_bytes(b'old payload\n')
    directory = base / 'directory'
    directory.mkdir()
    file_link = base / 'file-link'
    file_link.symlink_to('destination')
    directory_link = base / 'directory-link'
    directory_link.symlink_to('directory')
    fifo = base / 'fifo'
    os.mkfifo(fifo, 0o600)

    for access in (os.O_RDONLY | os.O_NONBLOCK, os.O_PATH):
        flags = access | os.O_DIRECTORY
        for path in (destination, file_link, fifo):
            try:
                fd = os.open(path, flags)
            except OSError as error:
                assert error.errno == errno.ENOTDIR, (path, flags, error)
            else:
                os.close(fd)
                raise AssertionError(('directory-only open succeeded', path, flags))
        for path in (directory, directory_link):
            fd = os.open(path, flags)
            try:
                assert stat.S_ISDIR(os.fstat(fd).st_mode), path
            finally:
                os.close(fd)

    subprocess.run(['cp', str(source), str(destination)], check=True)
    assert destination.read_bytes() == source.read_bytes()
    subprocess.run(['cp', str(source), str(directory)], check=True)
    assert (directory / source.name).read_bytes() == source.read_bytes()

print('Directory-only opens and copy destination checks: PASS')
