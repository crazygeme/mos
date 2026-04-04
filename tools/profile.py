#!/usr/bin/env python3
"""
profile.py — QEMU monitor-based kernel profiler (flat EIP histogram)

Samples EIP by pausing the VM through QEMU's HMP monitor, reading registers,
then resuming. Prints a function-level histogram to stdout.

For full call-stack sampling and flamegraph output use profile_stack.py.

Usage:
    ./tools/profile.py [--delay MS] [--sock PATH] [--kernel PATH]

Requires QEMU running with:
    ./run.sh profile
(which passes -monitor unix:/tmp/qemu-profiler.sock,server,nowait)
"""

import socket
import time
import re
import sys
import subprocess
import argparse
from collections import defaultdict

DEFAULT_SOCK   = "/tmp/qemu-profiler.sock"
DEFAULT_KERNEL = "out/kernel.dbg"
KERNEL_BASE    = 0xC0000000


# ── Monitor I/O ──────────────────────────────────────────────────────────────

def connect(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    s.settimeout(3.0)
    _recv(s)   # drain welcome banner
    return s


def _recv(s):
    buf = b""
    while b"(qemu) " not in buf:
        try:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            break
    return buf.decode(errors="replace")


def cmd(s, text):
    s.sendall((text + "\n").encode())
    return _recv(s)


def sample_eip(s):
    """Pause VM, read EIP, resume. Returns EIP as int or None."""
    cmd(s, "stop")
    out = cmd(s, "info registers")
    cmd(s, "cont")
    m = re.search(r"EIP=([0-9a-f]{8})", out)
    return int(m.group(1), 16) if m else None


# ── Symbol table ─────────────────────────────────────────────────────────────

def load_symbols(kernel_dbg):
    """Return sorted list of (addr, name) for text symbols via nm."""
    result = subprocess.run(
        ["nm", "-n", kernel_dbg], capture_output=True, text=True
    )
    syms = []
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ("T", "t", "W", "w"):
            try:
                syms.append((int(parts[0], 16), parts[2]))
            except ValueError:
                pass
    return syms  # already sorted by nm -n


def addr_to_func(addr, syms):
    """Binary search: find the last symbol whose address <= addr."""
    lo, hi = 0, len(syms) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid - 1
    return syms[hi][1] if hi >= 0 else "(unknown)"


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(description="QEMU monitor-based kernel profiler")
    p.add_argument("--delay",  type=float, default=5.0,       help="ms between samples (default: 5)")
    p.add_argument("--sock",   default=DEFAULT_SOCK,           help=f"monitor socket (default: {DEFAULT_SOCK})")
    p.add_argument("--kernel", default=DEFAULT_KERNEL,         help=f"debug binary   (default: {DEFAULT_KERNEL})")
    args = p.parse_args()

    print(f"Loading symbols from {args.kernel} ...", flush=True)
    syms = load_symbols(args.kernel)
    if not syms:
        print("Error: no symbols found. Is the debug binary built?")
        sys.exit(1)
    print(f"  {len(syms)} symbols loaded.")

    print(f"Connecting to QEMU monitor at {args.sock} ...", flush=True)
    try:
        s = connect(args.sock)
    except (FileNotFoundError, ConnectionRefusedError):
        print(f"Error: cannot connect to {args.sock}")
        print("Start the kernel with:  ./run.sh profile")
        sys.exit(1)
    print("  Connected.")

    delay_s = args.delay / 1000.0
    counts  = defaultdict(int)
    n       = 0

    input("Start your workload in the guest, then press Enter to begin sampling ...")
    print(f"Sampling every {args.delay:.1f} ms — Ctrl-C to stop.\n", flush=True)

    try:
        while True:
            eip = sample_eip(s)
            if eip is None:
                continue
            if eip >= KERNEL_BASE:
                counts[addr_to_func(eip, syms)] += 1
            else:
                counts["(userspace)"] += 1
            n += 1
            if n % 100 == 0:
                sys.stdout.write(f"\r  {n} samples ...")
                sys.stdout.flush()
            time.sleep(delay_s)
    except KeyboardInterrupt:
        print(f"\nInterrupted — {n} samples collected.")
        try:
            s.sendall(b"cont\n")
            _recv(s)
        except Exception:
            pass

    s.close()

    if n == 0:
        print("No samples collected.")
        return

    kernel_total = sum(v for k, v in counts.items() if k != "(userspace)")
    user_total   = counts.get("(userspace)", 0)

    # ── Console histogram ─────────────────────────────────────────────────────
    print(f"\n{'='*65}")
    print(f"  Profiling results  ({n} samples)")
    print(f"{'='*65}")
    print(f"  {'Function':<48} {'Samples':>7}  {'%':>6}")
    print(f"  {'-'*48} {'-'*7}  {'-'*6}")

    for func, cnt in sorted(counts.items(), key=lambda x: -x[1])[:40]:
        pct = 100.0 * cnt / n
        print(f"  {func:<48} {cnt:>7}  {pct:>5.1f}%")

    print(f"\n  kernel: {kernel_total:>6} samples  ({100.0 * kernel_total / n:.1f}%)")
    print(f"  user  : {user_total:>6} samples  ({100.0 * user_total   / n:.1f}%)")



if __name__ == "__main__":
    main()
