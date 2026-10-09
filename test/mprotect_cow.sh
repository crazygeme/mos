#!/bin/sh
# Verify mapping isolation and shared writes after mprotect transitions.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-mprotect_cow.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import contextlib
import ctypes
import mmap
import os
import tempfile
import unittest


PAGE = mmap.PAGESIZE
READ = mmap.PROT_READ
WRITE = mmap.PROT_WRITE
libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long]
libc.mmap.restype = ctypes.c_void_p
libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
libc.mprotect.restype = ctypes.c_int
libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
libc.munmap.restype = ctypes.c_int


def protect(address, prot):
    if libc.mprotect(address, PAGE, prot) != 0:
        raise OSError(ctypes.get_errno(), 'mprotect')


@contextlib.contextmanager
def mapping(fd=-1, flags=mmap.MAP_PRIVATE, prot=READ):
    if fd == -1:
        flags |= mmap.MAP_ANONYMOUS
    address = libc.mmap(None, PAGE, prot, flags, fd, 0)
    if address == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_errno(), 'mmap')
    try:
        yield address
    finally:
        libc.munmap(address, PAGE)


def byte(address):
    return ctypes.c_ubyte.from_address(address)


class MprotectCow(unittest.TestCase):
    def test_private_file_keeps_cache_and_other_mappings_unchanged(self):
        with tempfile.TemporaryFile() as source:
            source.write(b'A' * PAGE)
            source.flush()
            with mapping(source.fileno()) as private, \
                    mapping(source.fileno()) as observer:
                self.assertEqual(byte(private).value, ord('A'))
                self.assertEqual(byte(observer).value, ord('A'))
                protect(private, READ | WRITE)
                byte(private).value = ord('B')
                protect(private, READ)
                self.assertEqual(byte(observer).value, ord('A'))
                self.assertEqual(os.pread(source.fileno(), 1, 0), b'A')
                with mapping(source.fileno()) as fresh:
                    self.assertEqual(byte(fresh).value, ord('A'))

    def test_anonymous_zero_page_remains_zero(self):
        with mapping() as private, mapping() as observer:
            self.assertEqual(byte(private).value, 0)
            self.assertEqual(byte(observer).value, 0)
            protect(private, READ | WRITE)
            byte(private).value = 123
            self.assertEqual(byte(observer).value, 0)
            with mapping() as fresh:
                self.assertEqual(byte(fresh).value, 0)

    def test_fork_private_page_survives_protection_changes(self):
        with mapping(prot=READ | WRITE) as private:
            byte(private).value = 45
            child = os.fork()
            if child == 0:
                try:
                    protect(private, 0)
                    protect(private, READ | WRITE)
                    byte(private).value = 67
                    os._exit(0 if byte(private).value == 67 else 1)
                except BaseException:
                    os._exit(2)
            self.assertEqual(os.waitpid(child, 0)[1], 0)
            self.assertEqual(byte(private).value, 45)

    def test_shared_mapping_propagates_writes(self):
        with tempfile.TemporaryFile() as source:
            source.write(b'A' * PAGE)
            source.flush()
            with mapping(source.fileno(), mmap.MAP_SHARED) as writer, \
                    mapping(source.fileno(), mmap.MAP_SHARED) as observer:
                self.assertEqual(byte(writer).value, ord('A'))
                self.assertEqual(byte(observer).value, ord('A'))
                protect(writer, READ | WRITE)
                byte(writer).value = ord('B')
                self.assertEqual(byte(observer).value, ord('B'))


if __name__ == '__main__':
    unittest.main()
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
