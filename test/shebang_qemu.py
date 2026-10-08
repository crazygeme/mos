#!/usr/bin/env python3
"""Check shebang exec semantics and compare exec costs on an isolated guest."""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import tempfile

from epoll_qemu import disk_image

ROOT = Path(__file__).resolve().parents[1]


def run(kernel, probe, directory, output, args, baseline=False):
    scripts = {
        'shebang-chain': '#!/bin/shebang-probe\n',
        'shebang-basic': '#!/bin/shebang-probe\nignored body\n',
        'shebang-arg': '#! \t/bin/shebang-probe\t-u -O  \r\nignored body\n',
        'shebang-no-newline': '#!/bin/shebang-probe',
        'shebang-empty-argv': '#!/bin/shebang-probe\n',
        'shebang-empty': '#! \t\nbody\n',
        'shebang-truncated': '#!/' + 'x' * 80 + '\nbody\n',
        'shebang-missing': '#!/bin/absent\nbody\n',
        'shebang-invalid-interp': '#!/root/plain\nbody\n',
        'shebang-no-exec': '#!/bin/shebang-probe\n',
        'plain': 'not an ELF image\n',
    }
    files = [(probe, 'bin/shebang-probe')]
    for name, text in scripts.items():
        source = directory / name
        source.write_text(text)
        source.chmod(0o644 if name == 'shebang-no-exec' else 0o755)
        files.append((source, 'root/' + name))
    elf_bytes = bytearray(probe.read_bytes())
    malformed = {'elf-short': elf_bytes[:20]}
    wire = bytearray(elf_bytes)
    wire[18:20] = b'\xff\xff'
    malformed['elf-machine'] = wire
    wire = bytearray(elf_bytes)
    wire[5] = 2
    malformed['elf-data'] = wire
    wire = bytearray(elf_bytes)
    offset, length = (28, 4) if wire[4] == 1 else (32, 8)
    wire[offset:offset + length] = (0x7fffffff).to_bytes(length, 'little')
    malformed['elf-phdr'] = wire
    for name, data in malformed.items():
        source = directory / name
        source.write_bytes(data)
        source.chmod(0o755)
        files.append((source, 'root/' + name))
    disk = disk_image(probe, directory, files)
    command = ['qemu-system-x86_64', '-accel', args.accel,
               '-cpu', 'host' if args.accel == 'kvm' else 'qemu64,+sep',
               '-smp', '2', '-m', '512', '-display', 'none', '-serial', 'stdio',
               '-monitor', 'none', '-kernel', str(kernel.resolve()),
               '-drive', f'file={disk},format=raw,if=ide', '-append', 'verbose=0',
               '-no-reboot', '-net', 'none',
               '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=args.timeout)
    except subprocess.TimeoutExpired as error:
        output.write_bytes(error.stdout or b'')
        raise SystemExit(f'Guest timed out: {output}')
    output.write_text(result.stdout + result.stderr)
    if args.kernel_tests and not baseline:
        if '[  FAILED  ]' in result.stdout or '[  PASSED  ] 4 tests.' not in result.stdout:
            raise SystemExit(f'ELF kernel tests failed or missing: {output}')
    for line in result.stdout.splitlines():
        if 'BENCH ' in line or '_PASS' in line or 'FAIL ' in line:
            print(line)
    marker = 'SHEBANG_BENCHMARK_PASS' if baseline else 'SHEBANG_PROBE_PASS'
    if result.returncode != 1 or marker not in result.stdout:
        raise SystemExit(f'Shebang guest failed: {output}')
    samples = re.findall(r'BENCH (elf|script|failed-script|elf-chain|script-chain) count=(\d+) elapsed_us=(\d+)', result.stdout)
    rows = {}
    for kind in ('elf', 'script', 'failed-script', 'elf-chain', 'script-chain'):
        values = [int(us) / int(count) for label, count, us in samples if label == kind]
        if len(values) != 3 or any(us <= 0 for us in values):
            raise SystemExit(f'Missing or invalid benchmark results: {output}')
        rows[kind] = {'median_us_per_operation': statistics.median(values), 'samples_us_per_operation': values}
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=('x86', 'x64'), default='x86')
    parser.add_argument('--kernel-tests', action='store_true', help='Run ElfTest on a test kernel')
    parser.add_argument('--compat', action='store_true', help='Run an i386 probe on x64')
    parser.add_argument('--kernel', type=Path)
    parser.add_argument('--baseline-kernel', type=Path)
    parser.add_argument('--accel', choices=('kvm', 'tcg'), default='kvm')
    parser.add_argument('--execs', type=int, default=100)
    parser.add_argument('--failed-execs', type=int, default=10000)
    parser.add_argument('--timeout', type=int, default=90)
    args = parser.parse_args()
    if args.compat and args.arch != 'x64':
        parser.error('--compat requires --arch x64')
    if not 1 <= args.execs <= 100000 or not 1 <= args.failed_execs <= 10000000:
        parser.error('Invalid exec iteration count')
    bits = 32 if args.arch == 'x86' or args.compat else 64
    destination = ROOT / 'out' / args.arch / 'release'
    kernel = args.kernel or destination / ('kernel' if args.arch == 'x86' else 'kernel.boot')
    destination.mkdir(parents=True, exist_ok=True)
    stem = 'shebang-compat' if args.compat else 'shebang'
    if args.kernel_tests and args.baseline_kernel:
        parser.error('--kernel-tests cannot be combined with --baseline-kernel')
    with tempfile.TemporaryDirectory(prefix='mos-shebang-qemu-') as raw:
        directory = Path(raw)
        probe = directory / 'probe'
        compile_command = ['gcc', f'-m{bits}', '-O2', '-fno-pie', '-no-pie',
                           '-fno-stack-protector', '-fno-builtin', '-nostdlib',
                           '-static', '-mno-red-zone', '-mgeneral-regs-only',
                           '-Wall', '-Werror', '-Wl,-e,_start', '-o', str(probe),
                           f'-DEXEC_ITERATIONS={args.execs}',
                           f'-DFAILED_EXEC_ITERATIONS={args.failed_execs}',
                           str(ROOT / 'tools/user/shebang_probe.c')]
        if args.kernel_tests:
            compile_command.append('-DEXEC_KERNEL_TESTS')
        subprocess.run(compile_command, check=True)
        baseline = None
        if args.baseline_kernel:
            old = directory / 'baseline-probe'
            baseline_command = list(compile_command)
            baseline_command[baseline_command.index('-o') + 1] = str(old)
            subprocess.run(baseline_command + ['-DSHEBANG_BENCH_ONLY'], check=True)
            stage = directory / 'before'
            stage.mkdir()
            baseline = run(args.baseline_kernel, old, stage, destination / f'{stem}-before.log', args, True)
        stage = directory / 'after'
        stage.mkdir()
        measured = run(kernel, probe, stage, destination / f'{stem}.log', args)
    output = {'acceleration': args.accel, 'arch': args.arch, 'bits': bits, 'benchmarks': measured}
    if baseline:
        output['baseline'] = baseline
        output['speedup'] = {kind: round(baseline[kind]['median_us_per_operation'] /
                                         measured[kind]['median_us_per_operation'], 3)
                             for kind in measured}
        print('Speedups:', output['speedup'])
    (destination / f'{stem}.json').write_text(json.dumps(output, indent=2) + '\n')


if __name__ == '__main__':
    main()
