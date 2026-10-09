#!/bin/sh
# Validate proc task enumeration during concurrent thread creation and exit.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-proc_thread_churn.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import os
import threading

done = threading.Event()
errors = []
def scan():
    try:
        while not done.is_set():
            names = os.listdir('/proc/self/task')
            assert names and all(name.isdigit() for name in names), names
            os.stat('/proc/self/task')
    except BaseException as error:
        errors.append(error)

scanner = threading.Thread(target=scan, daemon=True)
scanner.start()
try:
    for batch in range(100):
        workers = [threading.Thread(target=lambda: None) for _ in range(8)]
        for worker in workers:
            worker.start()
        for worker in workers:
            worker.join(5)
            assert not worker.is_alive()
finally:
    done.set()
    scanner.join(5)
assert not scanner.is_alive() and not errors, errors
print('Proc task enumeration churn checks: PASS')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
