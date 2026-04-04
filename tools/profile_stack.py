#!/usr/bin/env python3
"""
profile_stack.py — QEMU monitor-based kernel call-stack profiler

Walks the EBP frame chain for each sample to collect full call stacks,
then renders a proper hierarchical flamegraph SVG under out/.

Usage:
    ./tools/profile_stack.py [--delay MS] [--depth N] [--sock PATH] [--kernel PATH]

Requires QEMU running with:
    ./run.sh profile
(which passes -monitor unix:/tmp/qemu-profiler.sock,server,nowait)

Requires -O0 build (default) — GCC keeps frame pointers at -O0.
"""

import socket
import time
import re
import sys
import subprocess
import argparse
import datetime
import os
from collections import defaultdict

DEFAULT_SOCK   = "/tmp/qemu-profiler.sock"
DEFAULT_KERNEL = "out/kernel.dbg"
KERNEL_BASE    = 0xC0000000
KERNEL_END     = 0xFFFFFFFF
OUT_DIR        = "out"
MAX_DEPTH      = 32


# ── Monitor I/O ──────────────────────────────────────────────────────────────

def connect(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    s.settimeout(3.0)
    _recv(s)
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


def read_words(s, addr, n=2):
    """Read n 32-bit words from guest virtual address. Returns list or None."""
    out = cmd(s, f"x /{n}wx {addr:#010x}")
    vals = []
    for line in out.splitlines():
        if ":" in line:
            vals.extend(line.split(":", 1)[1].split())
    if len(vals) < n:
        return None
    try:
        return [int(v, 16) for v in vals[:n]]
    except ValueError:
        return None


def sample_stack(s, syms, max_depth):
    """
    Stop VM, read EIP + EBP, walk frame chain, resume.
    Returns list of symbol names, innermost first (EIP at index 0).
    """
    cmd(s, "stop")
    regs = cmd(s, "info registers")

    m_eip = re.search(r"EIP=([0-9a-f]{8})", regs)
    m_ebp = re.search(r"EBP=([0-9a-f]{8})", regs)
    if not m_eip or not m_ebp:
        cmd(s, "cont")
        return None

    eip = int(m_eip.group(1), 16)
    ebp = int(m_ebp.group(1), 16)

    if eip < KERNEL_BASE:
        cmd(s, "cont")
        return ["(userspace)"]

    stack = [addr_to_func(eip, syms)]

    for _ in range(max_depth - 1):
        # EBP must point into kernel stack space
        if not (KERNEL_BASE <= ebp <= KERNEL_END - 8):
            break
        words = read_words(s, ebp, 2)
        if words is None:
            break
        saved_ebp, ret_addr = words
        # Return address must be in kernel text
        if not (KERNEL_BASE <= ret_addr <= KERNEL_END):
            break
        # Stack grows down: saved EBP must be above current EBP
        if saved_ebp <= ebp:
            break
        stack.append(addr_to_func(ret_addr, syms))
        ebp = saved_ebp

    cmd(s, "cont")
    return stack


# ── Symbol table ─────────────────────────────────────────────────────────────

def load_symbols(kernel_dbg):
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
    return syms


def addr_to_func(addr, syms):
    lo, hi = 0, len(syms) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid - 1
    return syms[hi][1] if hi >= 0 else "(unknown)"


# ── Flamegraph tree ───────────────────────────────────────────────────────────

class FGNode:
    __slots__ = ("name", "count", "children")

    def __init__(self, name):
        self.name     = name
        self.count    = 0
        self.children = {}

    def add(self, stack, pos=0):
        """stack is outermost-first; pos is the current index."""
        self.count += 1
        if pos < len(stack):
            name = stack[pos]
            if name not in self.children:
                self.children[name] = FGNode(name)
            self.children[name].add(stack, pos + 1)

    def max_depth(self):
        if not self.children:
            return 0
        return 1 + max(c.max_depth() for c in self.children.values())


def build_tree(stacks):
    """
    Build a prefix tree from a list of stacks.
    Each stack is [innermost, ..., outermost] (EIP first).
    The tree is keyed outermost-first (caller at root).
    """
    root = FGNode("all")
    for stack in stacks:
        root.add(list(reversed(stack)))  # outermost first
    return root


# ── SVG flamegraph renderer ───────────────────────────────────────────────────

_PALETTE = [
    "#e8603c", "#e8803c", "#e8a03c", "#c86a2c", "#d4502c",
    "#e87a3c", "#f0a04e", "#e06030", "#d87840", "#c85830",
    "#a04428", "#b85c34", "#cc7040", "#e08c4c", "#f0a060",
]


def _color(name):
    return _PALETTE[hash(name) % len(_PALETTE)]


def _esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def write_flamegraph(path, root, ts):
    SVG_W   = 1400
    ROW_H   = 20
    FONT    = 12
    HDR_H   = 66
    PADDING = 8   # bottom padding

    depth   = root.max_depth()   # levels below root
    svg_h   = HDR_H + (depth + 1) * ROW_H + PADDING
    total   = root.count

    lines = []
    lines.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{SVG_W}" height="{svg_h}">')
    lines.append(f'<rect x="0" y="0" width="{SVG_W}" height="{svg_h}" fill="#f8f8f8"/>')
    lines.append('<style>')
    lines.append('  text { font-family: monospace; pointer-events: none; }')
    lines.append('  rect { stroke: rgba(0,0,0,0.08); stroke-width: 0.5; }')
    lines.append('  rect:hover { opacity: 0.75; cursor: default; }')
    lines.append('</style>')

    lines.append(f'<text x="10" y="22" font-size="16" font-weight="bold">'
                 f'MOS Kernel Stack Profile — {_esc(ts)}</text>')
    lines.append(f'<text x="10" y="42" font-size="{FONT}" fill="#555">'
                 f'{total} samples  ·  max stack depth {depth}  ·  root at bottom</text>')
    lines.append(f'<text x="10" y="58" font-size="{FONT}" fill="#888">'
                 f'Wider = more samples.  Hover for details.</text>')

    def render(node, x, w, level):
        """level=0 is root (bottom row)."""
        if w < 0.5:
            return
        y = svg_h - PADDING - (level + 1) * ROW_H
        bw = max(0.5, w - 0.5)
        pct = 100 * node.count / total
        title = _esc(f"{node.name}: {node.count} ({pct:.1f}%)")
        color = _color(node.name)

        lines.append(
            f'<rect x="{x:.2f}" y="{y}" width="{bw:.2f}" height="{ROW_H - 1}" '
            f'fill="{color}"><title>{title}</title></rect>'
        )
        if w > 20:
            chars = max(0, int((w - 6) / 7))
            label = _esc(node.name[:chars])
            if label:
                lines.append(
                    f'<text x="{x + 3:.2f}" y="{y + 14}" '
                    f'font-size="{FONT}" fill="white">{label}</text>'
                )

        cx = x
        for child in sorted(node.children.values(), key=lambda n: (-n.count, n.name)):
            cw = w * child.count / node.count
            render(child, cx, cw, level + 1)
            cx += cw

    render(root, 0, SVG_W, 0)
    lines.append("</svg>")

    os.makedirs(OUT_DIR, exist_ok=True)
    with open(path, "w") as f:
        f.write("\n".join(lines))


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(description="QEMU kernel stack profiler — flamegraph output")
    p.add_argument("--delay",  type=float, default=10.0,      help="ms between samples (default: 10)")
    p.add_argument("--depth",  type=int,   default=MAX_DEPTH, help=f"max frame depth (default: {MAX_DEPTH})")
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
    stacks  = []
    n       = 0

    input("Start your workload in the guest, then press Enter to begin sampling ...")
    print(f"Sampling every {args.delay:.1f} ms (up to {args.depth} frames) — Ctrl-C to stop.\n",
          flush=True)

    try:
        while True:
            stack = sample_stack(s, syms, args.depth)
            if stack:
                stacks.append(stack)
                n += 1
            if n % 50 == 0 and n > 0:
                sys.stdout.write(f"\r  {n} samples ...")
                sys.stdout.flush()
            time.sleep(delay_s)
    except KeyboardInterrupt:
        print(f"\nInterrupted — {n} samples collected.")

    s.close()

    if n == 0:
        print("No samples collected.")
        return

    # ── Console summary ───────────────────────────────────────────────────────
    flat = defaultdict(int)
    for stack in stacks:
        flat[stack[0]] += 1   # count by innermost frame (EIP)

    kernel_n = sum(v for k, v in flat.items() if k != "(userspace)")
    user_n   = flat.get("(userspace)", 0)

    print(f"\n{'='*65}")
    print(f"  Top functions by EIP  ({n} samples)")
    print(f"{'='*65}")
    print(f"  {'Function':<48} {'Samples':>7}  {'%':>6}")
    print(f"  {'-'*48} {'-'*7}  {'-'*6}")
    for func, cnt in sorted(flat.items(), key=lambda x: -x[1])[:30]:
        print(f"  {func:<48} {cnt:>7}  {100*cnt/n:>5.1f}%")
    print(f"\n  kernel: {kernel_n:>6} ({100*kernel_n/n:.1f}%)   "
          f"user: {user_n:>6} ({100*user_n/n:.1f}%)")

    # ── Flamegraph SVG ────────────────────────────────────────────────────────
    root = build_tree(stacks)
    ts   = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    svg  = os.path.join(OUT_DIR, f"profile-{ts}.svg")
    write_flamegraph(svg, root, ts)
    print(f"\n  Flamegraph written to {svg}")


if __name__ == "__main__":
    main()
