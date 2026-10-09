#!/bin/sh
# Validate procfs thread-group directories and live thread metadata.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc_tasks.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'

import os
from pathlib import Path
import stat
import threading


def status(path):
    with open(path) as stream:
        return dict(line.split(':', 1) for line in stream if ':' in line)


def check_directory(path, tids):
    info = os.stat(path)
    assert stat.S_ISDIR(info.st_mode), path
    assert info.st_nlink == len(tids) + 2, (path, info.st_nlink, tids)
    assert set(os.listdir(path)) == {str(tid) for tid in tids}, path


pid = os.getpid()
main_tid = threading.get_native_id()
task_path = f'/proc/{pid}/task'
baseline = {int(name) for name in os.listdir(task_path)}
assert main_tid in baseline
check_directory('/proc/self/task', baseline)
assert 'task' in os.listdir('/proc/self')
proc_fd = os.open('/proc', os.O_RDONLY | os.O_DIRECTORY)
task_fd = os.open(task_path, os.O_RDONLY | os.O_DIRECTORY)
ready = threading.Event()
finish = threading.Event()
result = {}


def worker():
    result['tid'] = threading.get_native_id()
    try:
        fields = status('/proc/self/status')
        assert int(fields['Pid']) == pid
        assert int(fields['Tgid']) == pid
    except BaseException as exc:
        result['error'] = exc
    finally:
        ready.set()
    finish.wait()


thread = threading.Thread(target=worker)
thread.start()
try:
    assert ready.wait(5), 'Thread initialization timed out'
    assert 'error' not in result, result
    tid = result['tid']
    expected = baseline | {tid}
    check_directory('/proc/self/task', expected)
    check_directory(task_path, expected)
    # Chromium's thread helper uses this directory-relative stat operation.
    assert os.stat('self/task/', dir_fd=proc_fd).st_nlink == len(expected) + 2
    assert os.fstat(task_fd).st_nlink == len(expected) + 2
    entry = Path(task_path) / str(tid)
    assert stat.S_ISDIR(os.stat(entry).st_mode)
    assert 'task' not in os.listdir(entry)
    fields = status(entry / 'status')
    assert int(fields['Pid']) == tid and int(fields['Tgid']) == pid, fields
    assert int(fields['Threads']) == len(expected), fields
    assert os.readlink(entry / 'exe') == os.readlink('/proc/self/exe')
    fields = status('/proc/self/status')
    assert int(fields['Threads']) == len(expected), fields
finally:
    finish.set()
    thread.join(5)
    os.close(proc_fd)
    assert not thread.is_alive(), 'Thread termination timed out'

try:
    check_directory(task_path, baseline)
    assert os.fstat(task_fd).st_nlink == len(baseline) + 2
    assert not os.path.exists(f'{task_path}/{result["tid"]}')
finally:
    os.close(task_fd)

ready_r, ready_w = os.pipe()
done_r, done_w = os.pipe()
child = os.fork()
if child == 0:
    try:
        os.close(ready_r)
        os.close(done_w)
        check_directory('/proc/self/task', {os.getpid()})
        os.write(ready_w, b'R')
        os.read(done_r, 1)
        os._exit(0)
    except BaseException:
        os._exit(1)
os.close(ready_w)
os.close(done_r)
try:
    assert os.read(ready_r, 1) == b'R'
    assert not os.path.exists(f'{task_path}/{child}')
    check_directory(f'/proc/{child}/task', {child})
finally:
    os.close(ready_r)
    os.close(done_w)
    assert os.waitpid(child, 0)[1] == 0
print('Procfs thread-group checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
