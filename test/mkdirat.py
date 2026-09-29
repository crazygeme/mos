#!/usr/bin/env python3
"""Validate the i386 mkdirat interface on persistent and runtime filesystems."""
import ctypes
import errno
import os
from pathlib import Path
import stat
import tempfile


def main():
    assert ctypes.sizeof(ctypes.c_void_p) == 4, "An i386 userspace is required."
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long

    def mkdirat(fd, path, mode, expected=0):
        result = libc.syscall(296, fd, path, mode)
        assert result == (0 if expected == 0 else -1), (result, ctypes.get_errno())
        if expected:
            assert ctypes.get_errno() == expected, ctypes.get_errno()

    for root in ("/root", "/run"):
        with tempfile.TemporaryDirectory(dir=root) as directory:
            base = Path(directory)
            fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
            mask = os.umask(0o027)
            try:
                mkdirat(fd, b"child", 0o777)
                assert stat.S_IMODE((base / "child").stat().st_mode) == 0o750
                mkdirat(fd, b"child", 0o700, errno.EEXIST)
                mkdirat(-1, b"relative", 0o700, errno.EBADF)
                mkdirat(fd, b"", 0o700, errno.ENOENT)
                mkdirat(fd, b"missing/child", 0o700, errno.ENOENT)
                mkdirat(-1, os.fsencode(base / "absolute"), 0o700)
                with (base / "file").open("w") as file:
                    mkdirat(file.fileno(), b"child", 0o700, errno.ENOTDIR)
                cwd = os.open(".", os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.chdir(directory)
                    mkdirat(-100, b"cwd", 0o700)
                    assert (base / "cwd").is_dir()
                finally:
                    os.fchdir(cwd)
                    os.close(cwd)
            finally:
                os.umask(mask)
                os.close(fd)
    print("mkdirat: PASS")


if __name__ == "__main__":
    main()
