#!/bin/sh
# Validate process memory reporting across reservation, faults, and unmap.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc_memory.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'

import mmap
import os


page = os.sysconf("SC_PAGE_SIZE")
reservation = 64 * 1024 * 1024
touched = 4 * 1024 * 1024
tolerance = 1024 * 1024
commands_r, commands_w = os.pipe()
ready_r, ready_w = os.pipe()
pid = os.fork()
if pid == 0:
    try:
        os.close(commands_w)
        os.close(ready_r)
        mapping = None
        os.write(ready_w, b"R")
        while True:
            command = os.read(commands_r, 1)
            if not command:
                break
            if command == b"M":
                mapping = mmap.mmap(-1, reservation,
                                    flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS,
                                    prot=mmap.PROT_READ | mmap.PROT_WRITE)
            elif command == b"T":
                for offset in range(0, touched, page):
                    mapping[offset] = 1
            elif command == b"U":
                mapping.close()
                mapping = None
            os.write(ready_w, b"R")
        os._exit(0)
    except BaseException:
        os._exit(1)


def snapshot():
    with open(f"/proc/{pid}/statm") as stream:
        counts = [int(value) for value in stream.read().split()]
    with open(f"/proc/{pid}/status") as stream:
        status = dict(line.split(":", 1) for line in stream if ":" in line)
    with open(f"/proc/{pid}/stat") as stream:
        fields = stream.read().rsplit(")", 1)[1].split()
    virtual, resident, shared = [value * page for value in counts[:3]]
    assert 0 < resident <= virtual, counts
    assert shared <= resident, counts
    assert int(status["VmSize"].split()[0]) * 1024 == virtual
    assert int(status["VmRSS"].split()[0]) * 1024 == resident
    assert int(fields[20]) == virtual
    assert int(fields[21]) * page == resident
    assert int(status["VmStk"].split()[0]) * 1024 <= virtual
    return virtual, resident


def step(command):
    os.write(commands_w, command)
    assert os.read(ready_r, 1) == b"R"
    return snapshot()


os.close(commands_r)
os.close(ready_w)
try:
    assert os.read(ready_r, 1) == b"R"
    baseline = snapshot()
    reserved = step(b"M")
    assert abs(reserved[0] - baseline[0] - reservation) <= tolerance
    assert abs(reserved[1] - baseline[1]) <= tolerance
    faulted = step(b"T")
    assert abs(faulted[0] - reserved[0]) <= tolerance
    assert abs(faulted[1] - reserved[1] - touched) <= tolerance
    unmapped = step(b"U")
    assert abs(unmapped[0] - baseline[0]) <= tolerance
    assert abs(unmapped[1] - baseline[1]) <= tolerance
finally:
    os.close(commands_w)
    os.close(ready_r)
    _, status = os.waitpid(pid, 0)
    assert status == 0, status

print("process memory reservation, residency, and unmap: PASS")
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
