#!/bin/sh
# Validate sendfile offset handling and buffered copying.
# Requires Python 3 and the modules imported by the embedded guest probe.
set -eu
probe_dir=$(mktemp -d /tmp/mos-sendfile_copy.XXXXXX)
trap 'rm -rf "$probe_dir"' 0
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cat > "$probe_dir/probe.py" <<'MOS_GUEST_PYTHON'
import os
import tempfile

with tempfile.TemporaryDirectory() as directory:
    payload = bytes(range(256)) * 4096
    source = os.open(directory + '/source', os.O_CREAT | os.O_RDWR, 0o600)
    target = os.open(directory + '/target', os.O_CREAT | os.O_RDWR, 0o600)
    os.write(source, payload)
    os.lseek(source, 17, os.SEEK_SET)
    assert os.sendfile(target, source, 3, 65537) == 65537
    assert os.lseek(source, 0, os.SEEK_CUR) == 17
    assert os.lseek(target, 0, os.SEEK_CUR) == 65537
    assert os.pread(target, 65537, 0) == payload[3:65540]
    os.lseek(target, 0, os.SEEK_SET)
    assert os.sendfile(target, source, None, 32769) == 32769
    assert os.lseek(source, 0, os.SEEK_CUR) == 32786
    assert os.pread(target, 32769, 0) == payload[17:32786]
    assert os.sendfile(target, source, len(payload), 4096) == 0
    assert os.sendfile(target, source, 0, 0) == 0
    os.close(target)
    os.close(source)
print('PASS: sendfile copies data and preserves explicit and implicit offsets')
MOS_GUEST_PYTHON
python3 "$probe_dir/probe.py" "$@"
