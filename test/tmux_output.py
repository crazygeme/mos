#!/usr/bin/env python3
"""Validate tmux output through a slow PTY without keyboard input."""

import errno
import fcntl
import os
import pty
import select
import shutil
import signal
import struct
import subprocess
import tempfile
import termios
import time


def check(rows, cols):
    tmux = shutil.which("tmux")
    assert tmux, "tmux is required"
    marker = b"TMUX_OUTPUT_COMPLETE"
    with tempfile.TemporaryDirectory(prefix="tmux-output-") as directory:
        socket_path = os.path.join(directory, "server")
        child, master = pty.fork()
        if child == 0:
            fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
            environment = os.environ.copy()
            environment.pop("TMUX", None)
            environment["TERM"] = "xterm-256color"
            os.execve(tmux, [tmux, "-S", socket_path, "-f", "/dev/null",
                            "new-session", "sleep 1; ls -alh /usr/lib; "
                            "printf '\\nTMUX_OUTPUT_COMPLETE\\n'; sleep 30"],
                      environment)
        output = bytearray()
        started = time.monotonic()
        try:
            deadline = started + 15
            while time.monotonic() < deadline:
                if not select.select([master], [], [], 0.1)[0]:
                    continue
                try:
                    chunk = os.read(master, 512)
                except OSError as error:
                    if error.errno == errno.EIO:
                        break
                    raise
                if not chunk:
                    break
                output.extend(chunk)
                if marker in output:
                    print(f"tmux {cols}x{rows}: PASS without input "
                          f"({time.monotonic() - started:.2f}s)", flush=True)
                    return
                # Apply backpressure while retaining continuous output progress.
                select.select([], [], [], 0.01)
            raise AssertionError(f"tmux output stalled: {bytes(output[-512:])!r}")
        finally:
            subprocess.run([tmux, "-S", socket_path, "kill-server"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           check=False)
            os.close(master)
            try:
                os.kill(child, signal.SIGKILL)
            except ProcessLookupError:
                pass
            os.waitpid(child, 0)


if __name__ == "__main__":
    for dimensions in ((24, 80), (36, 100), (80, 240)):
        check(*dimensions)
