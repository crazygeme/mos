#!/usr/bin/env python3
"""Run Xorg kernel-interface regression checks inside a MOS guest."""

import ctypes
import errno
import fcntl
import mmap
import os
from pathlib import Path
import socket
import tempfile

libc = ctypes.CDLL(None, use_errno=True)
libc.getauxval.argtypes = [ctypes.c_ulong]
libc.getauxval.restype = ctypes.c_ulong
libc.unlinkat.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
libc.unlinkat.restype = ctypes.c_int

for key, expected in ((11, os.getuid()), (12, os.geteuid()),
                      (13, os.getgid()), (14, os.getegid())):
    assert libc.getauxval(key) == expected, (key, expected)
assert libc.getauxval(23) == int(os.getuid() != os.geteuid() or os.getgid() != os.getegid())

with tempfile.TemporaryDirectory(prefix="mos-xorg-") as directory:
    root = Path(directory)
    fd = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
    try:
        target = root / "target"
        target.write_text("payload")
        os.symlink("target", root / "link")
        assert libc.unlinkat(fd, b"link", 0) == 0
        assert target.read_text() == "payload"
        assert libc.unlinkat(fd, b"target", 0x4000) == -1
        assert ctypes.get_errno() == errno.EINVAL
        assert target.exists()
        assert libc.unlinkat(fd, b"target", 0) == 0
        (root / "empty").mkdir()
        assert libc.unlinkat(fd, b"empty", 0) == -1
        assert ctypes.get_errno() == errno.EISDIR
        assert libc.unlinkat(fd, b"empty", 0x200) == 0
        assert libc.unlinkat(-1, b"missing", 0) == -1
        assert ctypes.get_errno() == errno.EBADF
    finally:
        os.close(fd)

    names = [str(root / "socket"), b"\0mos-xorg-" + str(os.getpid()).encode() + b"\0name"]
    for name in names:
        with socket.socket(socket.AF_UNIX) as listener:
            listener.bind(name)
            listener.listen(1)
            assert listener.getsockname() == name
            with socket.socket(socket.AF_UNIX) as duplicate:
                try:
                    duplicate.bind(name)
                except OSError as error:
                    assert error.errno == errno.EADDRINUSE
                else:
                    raise AssertionError("duplicate Unix socket binding succeeded")
            with socket.socket(socket.AF_UNIX) as client:
                client.connect(name)
                accepted, _ = listener.accept()
                with accepted:
                    assert client.getpeername() == name
                    assert accepted.getsockname() == name
                    client.sendall(b"X11")
                    assert accepted.recv(3) == b"X11"
        if isinstance(name, bytes):
            with socket.socket(socket.AF_UNIX) as rebound:
                rebound.bind(name)
        elif os.path.exists(name):
            os.unlink(name)

# Shared fence storage must remain valid after removal of its directory entry.
shm_fd, shm_path = tempfile.mkstemp(prefix="mos-fence-", dir="/dev/shm")
try:
    os.unlink(shm_path)
    assert not os.path.exists(shm_path)
    os.ftruncate(shm_fd, 4096)
    with mmap.mmap(shm_fd, 4096) as first, mmap.mmap(shm_fd, 4096) as second:
        first[:4] = b"DRI3"
        assert second[:4] == b"DRI3"
finally:
    os.close(shm_fd)
    if os.path.exists(shm_path):
        os.unlink(shm_path)

try:
    anonymous_fd = os.open("/dev/shm", os.O_TMPFILE | os.O_RDWR, 0o600)
except OSError as error:
    assert error.errno == errno.EOPNOTSUPP
else:
    os.close(anonymous_fd)
    raise AssertionError("O_TMPFILE requires anonymous inode support")

fences = ctypes.CDLL("libxshmfence.so.1", use_errno=True)
fences.xshmfence_alloc_shm.restype = ctypes.c_int
fences.xshmfence_map_shm.argtypes = [ctypes.c_int]
fences.xshmfence_map_shm.restype = ctypes.c_void_p
for function in ("xshmfence_query", "xshmfence_trigger", "xshmfence_reset",
                 "xshmfence_unmap_shm"):
    getattr(fences, function).argtypes = [ctypes.c_void_p]
fence_fd = fences.xshmfence_alloc_shm()
assert fence_fd >= 0, ("xshmfence allocation", ctypes.get_errno())
fence_map = None
try:
    fence_map = fences.xshmfence_map_shm(fence_fd)
    assert fence_map, ("xshmfence mapping", ctypes.get_errno())
    fences.xshmfence_reset(fence_map)
    assert fences.xshmfence_query(fence_map) == 0
    fences.xshmfence_trigger(fence_map)
    assert fences.xshmfence_query(fence_map) != 0
finally:
    if fence_map:
        fences.xshmfence_unmap_shm(fence_map)
        os.close(fence_fd)

# KDGETLED returns one byte and must preserve adjacent caller memory.
keyboard_fd = os.open("/dev/tty0", os.O_RDONLY | os.O_NONBLOCK)
try:
    leds = bytearray([0xff, 0xa5, 0x5a, 0xc3])
    fcntl.ioctl(keyboard_fd, 0x4b31, leds, True)
    assert leds[0] <= 7
    assert leds[1:] == bytearray([0xa5, 0x5a, 0xc3])
finally:
    os.close(keyboard_fd)

ioports = Path("/proc/ioports").read_text()
assert ioports.endswith("\n")
protected_ports = set()
for line in ioports.splitlines():
    interval, name = line.split(":", 1)
    start, end = (int(value, 16) for value in interval.strip().split("-"))
    assert 0 <= start <= end <= 0xffff
    if "keyboard" in name or "timer" in name:
        protected_ports.update(range(start, end + 1))
assert {0x40, 0x41, 0x42, 0x43, 0x60, 0x64} <= protected_ports
assert not protected_ports.intersection(range(0x3b0, 0x3e0))

pci = Path("/sys/bus/pci/devices")
devices = list(pci.iterdir())
assert devices, "PCI sysfs is empty"
displays = 0
for device in devices:
    config_fd = os.open(device / "config", os.O_RDONLY)
    try:
        config = os.pread(config_fd, 64, 0)
        assert len(config) == 64
        assert os.pread(config_fd, 8, 256) == b""
    finally:
        os.close(config_fd)
    assert int((device / "vendor").read_text(), 16) == int.from_bytes(config[:2], "little")
    klass = int((device / "class").read_text(), 16)
    assert klass == int.from_bytes(config[9:12], "little")
    resources = (device / "resource").read_text().splitlines()
    assert len(resources) == 7
    for row in resources:
        start, end, flags = (int(value, 16) for value in row.split())
        assert (start == end == flags == 0) or end >= start
    if klass >> 16 == 3 and os.geteuid() == 0:
        rom_start, rom_end, _ = (int(value, 16) for value in resources[6].split())
        if rom_start and rom_end >= rom_start:
            config_fd = os.open(device / "config", os.O_RDWR)
            mem_fd = os.open("/dev/mem", os.O_RDONLY)
            saved_rom = os.pread(config_fd, 4, 0x30)
            try:
                assert len(saved_rom) == 4
                enabled_rom = (int.from_bytes(saved_rom, "little") | 1).to_bytes(4, "little")
                assert os.pwrite(config_fd, enabled_rom, 0x30) == 4
                assert os.pread(mem_fd, 2, rom_start) == b"\x55\xaa"
                assert os.pread(mem_fd, 1, 1 << 32) == b""
                assert os.pread(mem_fd, 1, (1 << 32) + rom_start) == b""
                try:
                    os.pread(mem_fd, 1, -1)
                except OSError as error:
                    assert error.errno == errno.EINVAL
                else:
                    raise AssertionError("negative physical offset accepted")
                assert os.lseek(mem_fd, 0, os.SEEK_CUR) == 0
                assert os.lseek(mem_fd, rom_start, os.SEEK_SET) == rom_start
            finally:
                os.pwrite(config_fd, saved_rom, 0x30)
                os.close(mem_fd)
                os.close(config_fd)
    if klass >> 16 == 3:
        displays += 1
        assert (device / "boot_vga").read_text().strip() in ("0", "1")
assert displays, "No PCI display controller was enumerated"
print("Xorg kernel-interface regression checks passed.")
