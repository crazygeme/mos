#!/usr/bin/env python3
"""Validate open/close optimizations on a disposable guest filesystem."""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import tempfile

from epoll_qemu import disk_image

ROOT = Path(__file__).resolve().parents[1]


def run(kernel, probe, destination, args, benchmark_only=False):
    with tempfile.TemporaryDirectory(prefix='mos-open-close-') as raw:
        disk = disk_image(probe, Path(raw))
        command = ['qemu-system-x86_64', '-accel', args.accel,
                   '-cpu', 'host' if args.accel == 'kvm' else 'qemu64,+sep',
                   '-smp', '2', '-m', '512', '-display', 'none',
                   '-serial', 'stdio', '-monitor', 'none',
                   '-kernel', str(kernel.resolve()),
                   '-drive', f'file={disk},format=raw,if=ide',
                   '-append', 'verbose=0', '-no-reboot', '-net', 'none',
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']
        try:
            result = subprocess.run(command, capture_output=True, text=True,
                                    timeout=args.timeout)
        except subprocess.TimeoutExpired as error:
            destination.write_bytes(error.stdout or b'')
            raise SystemExit(f'Guest timed out: {destination}')
        destination.write_text(result.stdout + result.stderr)
        for line in result.stdout.splitlines():
            if 'BENCH ' in line or '_PASS' in line or 'FAIL ' in line:
                print(line)
        marker = 'OPEN_CLOSE_BENCHMARK_PASS' if benchmark_only else 'OPEN_CLOSE_PROBE_PASS'
        if result.returncode != 1 or marker not in result.stdout:
            raise SystemExit(f'Guest validation failed: {destination}')
        samples = re.findall(
                    r'BENCH (normal|symlink|mount) pairs=(\d+) elapsed_us=(\d+)',
                    result.stdout)
        rows = {}
        for kind in ('normal', 'symlink', 'mount'):
            elapsed = [int(us) for label, pairs, us in samples
                       if label == kind and int(pairs) == args.pairs]
            if len(elapsed) != args.repeats or any(us <= 0 for us in elapsed):
                raise SystemExit(f'Missing or invalid benchmark samples: {destination}')
            rows[kind] = {'pairs': args.pairs,
                          'elapsed_us': statistics.median(elapsed),
                          'samples_us': elapsed}
        return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=('x86', 'x64'), default='x86')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--baseline-kernel', type=Path)
    parser.add_argument('--accel', choices=('tcg', 'kvm'), default='tcg')
    parser.add_argument('--timeout', type=int, default=90)
    parser.add_argument('--pairs', type=int, default=100000)
    parser.add_argument('--repeats', type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.pairs <= 100000000 or not 1 <= args.repeats <= 100:
        parser.error('--pairs must be 1..100000000 and --repeats must be 1..100')
    destination = ROOT / 'out' / args.arch / 'release'
    destination.mkdir(parents=True, exist_ok=True)
    kernel = args.kernel or destination / ('kernel' if args.arch == 'x86' else 'kernel.boot')
    with tempfile.TemporaryDirectory(prefix='mos-open-close-build-') as raw:
        probe = Path(raw) / 'probe'
        compile_command = ['gcc', '-m32' if args.arch == 'x86' else '-m64', '-O2',
                        '-fno-pie', '-no-pie', '-fno-stack-protector', '-fno-builtin',
                        '-nostdlib', '-static', '-mno-red-zone', '-mgeneral-regs-only',
                        '-Wall', '-Werror', '-Wl,-e,_start', '-o', str(probe),
                        f'-DOPEN_CLOSE_PAIRS={args.pairs}',
                        f'-DOPEN_CLOSE_REPEATS={args.repeats}',
                        str(ROOT / 'tools/user/open_close_probe.c')]
        subprocess.run(compile_command, check=True)
        baseline = None
        if args.baseline_kernel:
            baseline_probe = Path(raw) / 'baseline-probe'
            baseline_command = list(compile_command)
            baseline_command[baseline_command.index('-o') + 1] = str(baseline_probe)
            subprocess.run(baseline_command + ['-DOPEN_CLOSE_BENCH_ONLY'], check=True)
            baseline = run(args.baseline_kernel, baseline_probe,
                           destination / 'open-close-before.log', args, benchmark_only=True)
        measured = run(kernel, probe, destination / 'open-close.log', args)
    output = {'acceleration': args.accel, 'benchmarks': measured}
    if baseline:
        output['baseline'] = baseline
        output['speedup'] = {kind: round(baseline[kind]['elapsed_us'] /
                                         measured[kind]['elapsed_us'], 3)
                             for kind in measured}
        print('Speedups:', output['speedup'])
    (destination / 'open-close.json').write_text(json.dumps(output, indent=2) + '\n')


if __name__ == '__main__':
    main()
