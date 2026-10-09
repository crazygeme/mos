#!/usr/bin/env python3
"""Sample release-kernel open/close hotspots on a disposable QEMU disk.

Uses HMP stop/register sampling, so results indicate hotspots rather than
precise CPU percentages. Monitor pauses distort elapsed time. One CPU keeps the workload
on the sampled CPU. Frame pointers and debug builds are not required.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

import profile_stack as profiler

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'test'))
from epoll_qemu import disk_image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, default=ROOT / 'out/x86/release/kernel')
    parser.add_argument('--symbols', type=Path, default=ROOT / 'out/x86/release/kernel.dbg')
    parser.add_argument('--output', type=Path, default=ROOT / 'out/x86/release/open-close-profile.json')
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--pairs', type=int, default=5000000)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--delay', type=float, default=0.2, help='Milliseconds between samples')
    args = parser.parse_args()
    if not 1 <= args.pairs <= 100000000 or args.timeout <= 0 or args.delay < 0:
        parser.error('Invalid workload, timeout, or sample delay')
    symbols = profiler.load_symbols(args.symbols)
    if not symbols:
        parser.error('No text symbols found; build the x86 release kernel first')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    serial_path = args.output.with_suffix('.log').resolve()
    counters = defaultdict(Counter)
    addresses = defaultdict(Counter)
    with tempfile.TemporaryDirectory(prefix='mos-open-close-profile-') as raw:
        directory = Path(raw)
        probe = directory / 'probe'
        subprocess.run(['gcc', '-m32', '-O2', '-fno-pie', '-no-pie',
                        '-fno-stack-protector', '-fno-builtin', '-nostdlib',
                        '-static', '-Wall', '-Werror', '-Wl,-e,_start',
                        '-DOPEN_CLOSE_BENCH_ONLY', '-DOPEN_CLOSE_PROFILE',
                        f'-DOPEN_CLOSE_PAIRS={args.pairs}', '-o', str(probe),
                        str(ROOT / 'tools/user/open_close_probe.c')], check=True)
        disk = disk_image(probe, directory)
        monitor_path = directory / 'monitor.sock'
        command = ['qemu-system-x86_64', '-accel', args.accel,
                   '-cpu', 'host' if args.accel == 'kvm' else 'qemu64,+sep',
                   '-smp', '1', '-m', '512', '-display', 'none',
                   '-serial', f'file:{serial_path}',
                   '-monitor', f'unix:{monitor_path},server=on,wait=off',
                   '-kernel', str(args.kernel.resolve()),
                   '-drive', f'file={disk},format=raw,if=ide',
                   '-append', 'verbose=0', '-no-reboot', '-net', 'none',
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']
        vm = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        deadline = time.monotonic() + args.timeout
        monitor = None
        try:
            while not monitor_path.exists():
                if vm.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('QEMU failed to start its monitor')
                time.sleep(.01)
            monitor = profiler.connect(str(monitor_path))
            profiler.cmd(monitor, 'cpu 0')
            phase = None
            partial = ''
            with serial_path.open() as serial:
                while vm.poll() is None:
                    if time.monotonic() > deadline:
                        raise RuntimeError(f'Profile timed out: {serial_path}')
                    partial += serial.read()
                    lines = partial.split('\n')
                    partial = lines.pop()
                    for line in lines:
                        if line.startswith('PROFILE_BEGIN '):
                            phase = line.split()[1]
                            print('Sampling', phase, flush=True)
                        if line.startswith('PROFILE_END '):
                            phase = None
                    if phase is None:
                        time.sleep(.002)
                        continue
                    try:
                        profiler.cmd(monitor, 'stop')
                        registers = profiler.cmd(monitor, 'info registers')
                        profiler.cmd(monitor, 'cont')
                    except OSError:
                        if vm.poll() is not None:
                            break
                        raise
                    match = re.search(r'EIP=([0-9a-fA-F]{8})', registers)
                    if match:
                        address = int(match[1], 16)
                        name = profiler.addr_to_func(address, symbols) if address >= 0xc0000000 else '(userspace)'
                        counters[phase][name] += 1
                        addresses[phase][hex(address)] += 1
                    time.sleep(args.delay / 1000)
            if vm.returncode != 1 or 'OPEN_CLOSE_BENCHMARK_PASS' not in serial_path.read_text():
                raise RuntimeError(f'Guest failed: {serial_path}\n{vm.communicate()[1].decode()}')
        finally:
            if vm.poll() is None:
                vm.kill()
                vm.wait()
            if monitor:
                monitor.close()
        if any(not counters[kind] for kind in ('normal', 'symlink', 'mount')):
            raise RuntimeError('Missing samples; increase --pairs')
    output = {
        'method': 'HMP stop/register sampling, release kernel, one CPU',
        'acceleration': args.accel,
        'kernel': str(args.kernel.resolve()),
        'kernel_sha256': hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
        'samples': {kind: dict(counter.most_common()) for kind, counter in counters.items()},
        'addresses': {kind: dict(counter.most_common()) for kind, counter in addresses.items()},
    }
    args.output.write_text(json.dumps(output, indent=2) + '\n')
    for kind, counter in counters.items():
        total = sum(counter.values())
        print(kind, total, 'samples')
        for name, count in counter.most_common(20):
            print(f'  {count / total * 100:5.1f}% {name}')
    print('Profile:', args.output)


if __name__ == '__main__':
    main()
