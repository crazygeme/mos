#!/usr/bin/env python3
"""Check fork/COW behavior and measure region-count costs on an isolated disk."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile

from epoll_qemu import disk_image

ROOT = Path(__file__).resolve().parents[1]


def run(kernel, probe, directory, output, args):
    disk = disk_image(probe, directory, [(probe, "bin/true")])
    command = ['qemu-system-x86_64', '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'max',
               '-smp', str(args.cpus), '-m', '512', '-display', 'none',
               '-serial', 'stdio', '-monitor', 'none',
               '-kernel', str(kernel.resolve()),
               '-drive', f'file={disk},format=raw,if=ide',
               '-append', 'verbose=0', '-net', 'none', '-no-reboot',
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']
    try:
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=args.timeout)
    except subprocess.TimeoutExpired as error:
        output.write_bytes(error.stdout or b'')
        raise SystemExit(f'Guest timed out: {output}')
    output.write_text(result.stdout + result.stderr)
    for line in result.stdout.splitlines():
        if 'BENCH ' in line or '_PASS' in line or 'FAIL' in line:
            print(line)
    if (result.returncode != 1 or 'FORK_SCALE_PASS' not in result.stdout
            or '[  FAILED  ]' in result.stdout):
        raise SystemExit(f'Guest failed: {output}')
    if args.kernel_tests and not all(
            f'[       OK ] mmap.{name}' in result.stdout for name in
            ('brk_contiguous_growth', 'brk_preserves_protected_tail')):
        raise SystemExit(f'Heap growth tests missing: {output}')
    rows = re.findall(r'BENCH (mmap|brk) regions=(\d+) pages=(\d+) '
                      r'forks=(\d+) elapsed_us=(\d+)', result.stdout)
    if len(rows) != 6 or any(int(row[-1]) <= 0 for row in rows):
        raise SystemExit(f'Benchmark rows missing or invalid: {output}')
    return [{'mapping': kind, 'regions_requested': int(regions),
             'pages': int(pages), 'forks': int(forks), 'elapsed_us': int(us)}
            for kind, regions, pages, forks, us in rows]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=('x86', 'x64'), default='x86')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--cpus', type=int, choices=(1, 2, 4), default=2)
    parser.add_argument('--kernel-tests', action='store_true')
    parser.add_argument('--timeout', type=int, default=90)
    args = parser.parse_args()
    destination = ROOT / 'out' / args.arch / 'release'
    destination.mkdir(parents=True, exist_ok=True)
    stem = 'kernel-test' if args.kernel_tests else 'kernel'
    kernel = args.kernel or destination / (stem + ('.boot' if args.arch == 'x64' else ''))
    log = destination / f'fork-scale-{args.cpus}.log'
    with tempfile.TemporaryDirectory(prefix='mos-fork-scale-') as raw:
        directory = Path(raw)
        probe = directory / 'probe'
        command = ['gcc', '-m32' if args.arch == 'x86' else '-m64', '-O2',
                   '-fno-pie', '-no-pie', '-fno-stack-protector', '-fno-builtin',
                   '-nostdlib', '-static', '-mno-red-zone', '-mgeneral-regs-only',
                   '-Wall', '-Werror', '-Wl,-e,_start', '-o', str(probe),
                   str(ROOT / 'tools/user/fork_scale_probe.c')]
        if args.kernel_tests:
            command.append('-DFORK_SCALE_KERNEL_TESTS')
        subprocess.run(command, check=True)
        measured = run(kernel, probe, directory, log, args)
    log.with_suffix('.json').write_text(json.dumps(
        {'arch': args.arch, 'cpus': args.cpus, 'acceleration': args.accel,
         'benchmarks': measured}, indent=2) + '\n')


if __name__ == '__main__':
    main()
