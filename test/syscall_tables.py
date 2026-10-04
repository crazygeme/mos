#!/usr/bin/env python3
"""Validate syscall namespace numbers and cross-ABI service coverage."""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]

# These i386 interfaces have no AMD64 syscall number. Multiplexers are checked
# against their native entry points; legacy aliases use their shared service.
ALIASES = {
    'oldstat': 'stat', 'stat64': 'stat', 'fstat64': 'fstat',
    'lstat64': 'lstat', '_llseek': 'lseek', '_newselect': 'select',
    'waitpid': 'wait4', 'mmap2': 'mmap', 'readdir': 'getdents',
    'fcntl64': 'fcntl', 'ftruncate64': 'ftruncate',
    'ugetrlimit': 'getrlimit', 'statfs64': 'statfs', 'fstatfs64': 'fstatfs',
    'fstatat64': 'newfstatat', 'clock_gettime64': 'clock_gettime',
    'futex_time64': 'futex', 'signal': 'rt_sigaction',
    'sigaction': 'rt_sigaction', 'sigreturn': 'rt_sigreturn',
    'umount': 'umount2', 'stime': 'settimeofday', 'nice': 'setpriority',
}
LEGACY = {'break', 'ftime', 'gtty', 'lock', 'prof', 'stty', 'vm86', 'vm86old'}
SOCKETS = {'socket', 'bind', 'connect', 'listen', 'accept', 'accept4',
           'getsockname', 'getpeername', 'socketpair', 'sendto', 'recvfrom',
           'shutdown', 'setsockopt', 'getsockopt', 'sendmsg', 'recvmsg'}
IPC = {'shmget', 'shmat', 'shmdt', 'shmctl'}


def definitions(path, macro):
    entries = re.findall(r'^' + macro + r'\((\d+),\s*(\w+),\s*(.*)\)$',
                         path.read_text(), re.M)
    numbers = [int(n) for n, _, _ in entries]
    names = [name for _, name, _ in entries]
    assert len(numbers) == len(set(numbers)), f'{path}: duplicate numbers'
    assert len(names) == len(set(names)), f'{path}: duplicate adapter names'
    assert numbers == sorted(numbers), f'{path}: unordered numbers'
    assert 'UNIMPLEMENTED' not in path.read_text(), f'{path}: null declarations'
    return {name: (int(number), call) for number, name, call in entries}


def namespace(path):
    return {name: int(number) for name, number in
            re.findall(r'^#define __NR_(\w+)\s+(\d+)\s*$', path.read_text(), re.M)}


def normalize(name):
    name = ALIASES.get(name, name)
    return name[:-2] if name.endswith('32') else name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    headers = Path('/usr/include/x86_64-linux-gnu/asm')
    parser.add_argument('--i386-header', type=Path, default=headers / 'unistd_32.h')
    parser.add_argument('--amd64-header', type=Path, default=headers / 'unistd_64.h')
    args = parser.parse_args()
    i386 = definitions(ROOT / 'arch/abi/i386/calls.def', 'I386_SYSCALL')
    native = definitions(ROOT / 'arch/x64/syscall/impl/calls.def', 'NATIVE_SYSCALL')
    for entries, header in [(i386, args.i386_header), (native, args.amd64_header)]:
        numbers = namespace(header)
        for name, (number, _) in entries.items():
            assert numbers.get(name) == number, f'{name}: invalid ABI number {number}'
    shared = {normalize(name) for name in i386} - LEGACY - {'socketcall', 'ipc'}
    supported = set(native)
    assert not shared - supported, f'Missing native services: {sorted(shared - supported)}'
    assert not supported - shared - SOCKETS - IPC - {'arch_prctl'}, (
        f'Native-only services: {sorted(supported - shared - SOCKETS - IPC - {"arch_prctl"})}')
    assert SOCKETS <= supported, f'Missing socket services: {sorted(SOCKETS - supported)}'
    assert IPC <= supported, f'Missing IPC services: {sorted(IPC - supported)}'
    assert i386['fchown32'][1].startswith('sys_fchown(')
    assert '((uint64_t)ARG2 << 32) | ARG1' in i386['ftruncate64'][1]
    print(f'Syscall table audit passed: {len(i386)} i386 and {len(native)} AMD64 entries.')


if __name__ == '__main__':
    main()
