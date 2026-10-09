#!/bin/sh
# Validate i386 filesystem statistics and cross-mount symbolic links.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-statfs64.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import ctypes
import errno
import os
from pathlib import Path
import subprocess
import tempfile


class Statfs64(ctypes.Structure):
    _pack_ = 4
    _fields_ = [
        ("type", ctypes.c_uint32), ("bsize", ctypes.c_uint32),
        ("blocks", ctypes.c_uint64), ("bfree", ctypes.c_uint64),
        ("bavail", ctypes.c_uint64), ("files", ctypes.c_uint64),
        ("ffree", ctypes.c_uint64), ("fsid", ctypes.c_int32 * 2),
        ("namelen", ctypes.c_uint32), ("frsize", ctypes.c_uint32),
        ("flags", ctypes.c_uint32), ("spare", ctypes.c_uint32 * 4),
    ]


assert ctypes.sizeof(Statfs64) == 84
assert Statfs64.fsid.offset == 48
assert Statfs64.flags.offset == 64


def main():
    assert ctypes.sizeof(ctypes.c_void_p) == 4, "An i386 userspace is required."
    assert os.readlink("/bin/sh") == os.readlink("/usr/bin/sh")
    subprocess.run(["/bin/sh", "-c", "test -r /proc/cpuinfo"], check=True)
    assert b"processor" in Path("/proc/cpuinfo").read_bytes()
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long
    stats = Statfs64()
    assert libc.syscall(268, b"/", 84, ctypes.byref(stats)) == 0
    assert stats.bsize > 0 and stats.blocks > 0
    assert stats.bavail <= stats.bfree <= stats.blocks
    assert stats.flags & 0x20
    descriptor = os.open("/", os.O_RDONLY)
    try:
        by_fd = Statfs64()
        assert libc.syscall(269, descriptor, 84, ctypes.byref(by_fd)) == 0
        assert (by_fd.type, by_fd.blocks, by_fd.bsize) == (stats.type, stats.blocks, stats.bsize)
    finally:
        os.close(descriptor)
    assert libc.syscall(268, b"/", 83, ctypes.byref(stats)) == -1
    assert ctypes.get_errno() == errno.EINVAL
    assert libc.syscall(269, -1, 84, ctypes.byref(stats)) == -1
    assert ctypes.get_errno() == errno.EBADF
    assert Path("/etc/mtab").read_bytes() == Path("/proc/mounts").read_bytes()
    with tempfile.TemporaryDirectory(dir="/root") as directory:
        base = Path(directory)
        absolute = base / "absolute"
        relative = base / "relative"
        directory_alias = base / "directory"
        absolute.symlink_to("/proc/mounts")
        relative.symlink_to("absolute")
        directory_alias.symlink_to(base, target_is_directory=True)
        expected = Path("/proc/mounts").read_bytes()
        assert absolute.read_bytes() == expected
        assert relative.read_bytes() == expected
        assert os.readlink(directory_alias / "absolute") == "/proc/mounts"
        assert (directory_alias / "relative").read_bytes() == expected
        assert relative.is_symlink()
        assert relative.stat().st_size == absolute.stat().st_size
    for command in (["df", "-h"], ["df", "-h", "/"]):
        subprocess.run(command, check=True)
    print("statfs64 and cross-mount symlink checks passed")


if __name__ == "__main__":
    main()
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
