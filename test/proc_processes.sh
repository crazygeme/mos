#!/bin/sh
# Validate procfs directory types, ownership, parents, and libgtop selection.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc_processes.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'

import ctypes as C
import os
import stat


class ProcList(C.Structure):
    _fields_ = [(name, C.c_uint64) for name in ("flags", "number", "total", "size")]


libc = C.CDLL(None, use_errno=True)
libc.getdents64.argtypes = [C.c_int, C.c_void_p, C.c_size_t]
libc.getdents64.restype = C.c_ssize_t
gtop = C.CDLL("libgtop-2.0.so.11")
gtop.glibtop_get_proclist.argtypes = [C.POINTER(ProcList), C.c_int64, C.c_int64]
gtop.glibtop_get_proclist.restype = C.POINTER(C.c_int)
glib = C.CDLL("libglib-2.0.so.0")
glib.g_free.argtypes = [C.c_void_p]
glib.g_free.restype = None


def process_list(which, arg=0):
    summary = ProcList()
    pids = gtop.glibtop_get_proclist(C.byref(summary), which, arg)
    try:
        assert summary.flags & 7 == 7, summary.flags
        assert summary.size == C.sizeof(C.c_int), summary.size
        assert summary.total == summary.number * summary.size
        return {pids[i] for i in range(summary.number)}
    finally:
        glib.g_free(pids)


def directory_types(capacity):
    fd = os.open("/proc", os.O_RDONLY | os.O_DIRECTORY)
    seen = set()
    buffer = C.create_string_buffer(capacity)
    try:
        while True:
            length = libc.getdents64(fd, buffer, capacity)
            assert length >= 0, C.get_errno()
            if length == 0:
                break
            data = buffer.raw[:length]
            offset = 0
            while offset < length:
                reclen = int.from_bytes(data[offset + 16:offset + 18], "little")
                assert reclen >= 20 and offset + reclen <= length, reclen
                name = data[offset + 19:offset + reclen].split(b"\0", 1)[0]
                if name.isdigit():
                    assert data[offset + 18] == 4, (name, data[offset + 18])
                    seen.add(int(name))
                offset += reclen
    finally:
        os.close(fd)
    assert os.getpid() in seen, seen


for capacity in (128, 4096):
    directory_types(capacity)

with open("/proc/1/status") as stream:
    fields = dict(line.split(":", 1) for line in stream if ":" in line)
assert int(fields["Pid"]) == 1 and int(fields["PPid"]) == 0, fields
with open("/proc/1/stat") as stream:
    fields = stream.read().rsplit(")", 1)[1].split()
assert int(fields[1]) == 0, fields

ready_r, ready_w = os.pipe()
done_r, done_w = os.pipe()
pid = os.fork()
if pid == 0:
    try:
        os.close(ready_r)
        os.close(done_w)
        if os.geteuid() == 0:
            os.setegid(1000)
            os.seteuid(1000)
        os.write(ready_w, b"R")
        os.read(done_r, 1)
        os._exit(0)
    except BaseException:
        os._exit(1)

os.close(ready_w)
os.close(done_r)
try:
    assert os.read(ready_r, 1) == b"R"
    uid = 1000 if os.geteuid() == 0 else os.geteuid()
    gid = 1000 if os.geteuid() == 0 else os.getegid()
    for name in (str(pid), f"{pid}/status", f"{pid}/fd"):
        info = os.stat(f"/proc/{name}")
        assert (info.st_uid, info.st_gid) == (uid, gid), (name, info)
    assert stat.S_ISDIR(os.stat(f"/proc/{pid}").st_mode)
    assert pid in process_list(0)
    assert process_list(1, pid) == {pid}
    assert pid in process_list(5, uid)
    assert pid not in process_list(5, uid + 1)
finally:
    os.close(ready_r)
    os.close(done_w)
    _, status = os.waitpid(pid, 0)
    assert status == 0, status

print("procfs and libgtop process enumeration: PASS")
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
