#!/usr/bin/env python3
"""Boot isolated i386/native fast-call probes without touching guest images."""
import argparse
import re
from pathlib import Path
import subprocess
import tempfile
from epoll_qemu import disk_image

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=('x86', 'x64'), required=True)
    parser.add_argument('--compat', action='store_true', help='i386 probe on x64')
    parser.add_argument('--no-sep', action='store_true', help='TCG CPU without SYSENTER')
    parser.add_argument('--tcg', action='store_true')
    parser.add_argument('--cpus', type=int, default=2)
    parser.add_argument('--build', choices=('debug', 'release'), default='release')
    args = parser.parse_args()
    if args.no_sep and not args.tcg:
        parser.error('--no-sep requires --tcg')
    arch = args.arch
    bits = 32 if arch == 'x86' or args.compat else 64
    destination = ROOT / 'out' / arch / args.build
    destination.mkdir(parents=True, exist_ok=True)
    log = destination / f'fast-syscall-{bits}{"-no-sep" if args.no_sep else ""}{"-tcg" if args.tcg else "-kvm"}.log'
    with tempfile.TemporaryDirectory(prefix='mos-fast-syscall-') as raw:
        directory = Path(raw)
        probe = directory / 'probe'
        subprocess.run(['gcc', f'-m{bits}', '-O2', '-fno-pie', '-no-pie',
                        '-fno-stack-protector', '-fno-builtin', '-nostdlib', '-static',
                        '-mno-red-zone', '-mgeneral-regs-only', '-Wall', '-Werror',
                        '-Wl,-e,_start', '-o', str(probe),
                        str(ROOT / 'tools/user/fast_syscall_probe.c')], check=True)
        mapped_file = directory / "fastcall"
        mapped_file.write_bytes(bytes(4096) + b"\x5a" + bytes(4095))
        disk = disk_image(probe, directory, [(mapped_file, "root/fastcall")])
        kernel = destination / ('kernel.boot' if arch == 'x64' else 'kernel')
        cpu = ('qemu64' if arch == 'x64' else 'qemu32') + (',-sep' if args.no_sep else ',+sep')
        command = ['qemu-system-x86_64', '-accel', 'tcg' if args.tcg else 'kvm',
                   '-cpu', cpu if args.tcg else 'host', '-smp', str(args.cpus),
                   '-m', '512', '-display', 'none', '-serial', 'stdio', '-monitor', 'none',
                   '-kernel', str(kernel), '-drive', f'file={disk},format=raw,if=ide',
                   '-append', 'verbose=0', '-no-reboot', '-net', 'none',
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=0x04']
        trace = log.with_suffix('.trace')
        if args.tcg:
            command += ['-d', 'in_asm', '-D', str(trace)]
        try:
            run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, timeout=60)
        except subprocess.TimeoutExpired as error:
            output = error.stdout or b''
            log.write_bytes(output if isinstance(output, bytes) else output.encode())
            raise SystemExit(f'Guest timed out: {log}')
        log.write_text(run.stdout)
        print('\n'.join(line for line in run.stdout.splitlines()
                        if 'ENTRY ' in line or 'FAST_SYSCALL_' in line or 'FAIL' in line))
        expected = 'ENTRY SYSCALL' if bits == 64 else ('ENTRY INT80' if arch == 'x64' or args.no_sep else 'ENTRY SYSENTER')
        if run.returncode != 1 or 'FAST_SYSCALL_PASS' not in run.stdout or expected not in run.stdout:
            raise SystemExit(f'Guest failed (exit {run.returncode}): {log}')
        if args.tcg and bits == 64:
            disassembly = trace.read_text()
            # QEMU builds without a disassembler emit OBJD-T hex bytes.
            if 'sysretq' not in disassembly and not re.search(r'OBJD-T: [0-9a-f]*480f07', disassembly):
                raise SystemExit(f'SYSRETQ was not executed: {trace}')
        print(f'PASS {arch}/{bits}-bit ({args.cpus} CPUs); {log}')


if __name__ == '__main__':
    main()
