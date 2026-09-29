#!/usr/bin/env python3
"""Validate syscall tracing across startup, exec, and process exit."""
import re
import subprocess
import tempfile
from pathlib import Path


with tempfile.TemporaryDirectory(prefix="strace-check-") as directory:
    trace = Path(directory) / "trace"
    for command, output in ((["/bin/true"], ""),
                            (["/bin/echo", "trace-ok"], "trace-ok\n")):
        result = subprocess.run(
            ["strace", "-o", str(trace), *command],
            capture_output=True, text=True, timeout=30,
        )
        assert result.returncode == 0, result
        assert result.stdout == output, result.stdout
        assert "Stray" not in result.stderr, result.stderr
        log = trace.read_text()
        assert re.search(r"execve\(.*\)\s+= 0", log), log
        assert "+++ exited with 0 +++" in log, log
        assert re.search(r"(?:openat|mmap2|brk)\(", log), log
        if output:
            assert re.search(r'write\(1, "trace-ok\\n", 9\)\s+= 9', log), log
print("strace startup, exec, and exit: PASS")
