#!/bin/sh
# Validate tmpfs mapping coherence across growth, file I/O, and page discard.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-tmpfs_mapping_coherence.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import mmap
import os
import tempfile

PAGE = mmap.PAGESIZE
with tempfile.TemporaryDirectory(dir='/dev/shm') as directory:
    path = directory + '/shared'
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
    try:
        os.ftruncate(fd, PAGE)
        with mmap.mmap(fd, PAGE) as first:
            first[:] = b'P' * PAGE
            os.ftruncate(fd, 2 * PAGE)
            readonly_fd = os.open(path, os.O_RDONLY)
            try:
                with mmap.mmap(fd, 2 * PAGE) as second, \
                     mmap.mmap(readonly_fd, 2 * PAGE, access=mmap.ACCESS_READ) as readonly, \
                     mmap.mmap(fd, PAGE, access=mmap.ACCESS_COPY) as private:
                    assert second[:PAGE] == readonly[:PAGE] == b'P' * PAGE
                    assert second[PAGE:] == b'\0' * PAGE
                    assert os.pread(fd, 8, 0) == b'P' * 8
                    os.pwrite(fd, b'Z', 11)
                    assert first[11] == second[11] == readonly[11] == ord('Z')
                    private[12] = 7
                    assert first[12] == second[12] == ord('P')
                    first[13] = ord('A')
                    assert private[13] == ord('P')
                    private.madvise(mmap.MADV_DONTNEED)
                    assert private[12] == ord('P') and private[13] == ord('A')
                    second.madvise(mmap.MADV_DONTNEED)
                    assert second[11] == ord('Z') and second[13] == ord('A')
                    os.ftruncate(fd, 3 * PAGE)
                    with mmap.mmap(fd, 3 * PAGE) as grown:
                        assert grown[:PAGE] == first[:]
                        assert grown[2 * PAGE:] == b'\0' * PAGE
            finally:
                os.close(readonly_fd)
    finally:
        os.close(fd)
print('Tmpfs mapping coherence checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
