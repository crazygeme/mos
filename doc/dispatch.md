# Kernel Dispatch and Lookup

## Subsystem interfaces

Headers under `impl` are private to their owning implementation. Subsystems
and architecture adapters include public headers for shared services.
`syscall/syscall.h` declares syscall services and shared argument layouts;
`syscall/impl/syscall_internal.h` declares the internal path-resolution helper.

`ps/ps.h` declares process-memory reads and writes. These functions copy between
kernel buffers and the target task's user mappings, resolve missing pages and
write faults, and return zero on completion or `-EFAULT` on failure. A failure
may occur after preceding pages have been copied. Scheduler locks, futex wait
queues, and timer helpers remain private to `ps/impl`. Futex services and
thread-exit cleanup are implemented in `ps/impl/ps_futex.c`.

`dev/devnums.h` defines device major numbers and fixed minor numbers shared by
device registration and procfs.

## Syscall namespaces

The i386 namespace is declared in `arch/abi/i386/calls.def`. The AMD64
namespace is declared in `arch/x64/syscall/impl/calls.def`. Each declaration
specifies the Linux syscall number, namespace name, and typed service
invocation. Each dispatcher indexes a constant table of interrupt-frame
callbacks. Undeclared slots and numbers outside the table return `-ENOSYS`.

The i386 table contains 244 entries. The AMD64 table contains 213 entries.
The counts include shared compatibility stubs and the diagnostic
`restart_syscall` entry; declaration does not imply complete Linux behavior.
Native entry points expose the services present in the i386 namespace.
`socketcall` maps to native socket entry points. The shared-memory operations
of `ipc` map to `shmget`, `shmat`, `shmdt`, and `shmctl`. Semaphore and message
queue operations remain unsupported.

Credential variants, stat variants, directory variants, time-width variants,
and legacy signal interfaces share their corresponding native service.
`waitpid`, `umount`, `stime`, and `nice` correspond to `wait4`, `umount2`,
`settimeofday`, and `setpriority`. Legacy `break`, `ftime`, `gtty`, `lock`,
`prof`, `stty`, and VM86 entries have no AMD64 syscall number. The x64 kernel
also exposes `arch_prctl` for native FS and GS bases. Undeclared Linux services
remain unavailable.

I386 adapters zero-extend pointers from 32-bit argument registers and assemble
split 64-bit arguments explicitly. AMD64 adapters preserve pointer-sized
arguments and returns and convert incompatible userspace structures before
calling shared services. Socket and syscall services share the `struct iovec`
declaration in `fs/iovec.h`. Native conversions include stat, statfs, sysinfo,
times, interval timers, POSIX timers, signal wait information, directory
records, and ptrace register words. Native timer values preserve pointer width. Native socket timestamp ioctls
write two 64-bit time fields.
Signals remain limited to 1–32. Shared timeout readers retain their existing
32-bit seconds limits where the native adapter uses a legacy time structure.
Exec preserves pending signals and the blocked signal mask. Caught handlers
reset to the default disposition, ignored handlers remain ignored, and the
alternate signal stack is disabled.

Native legacy directory records contain 64-bit inode and offset fields with
eight-byte alignment and a trailing type byte. `getdents64` uses the Linux
fixed-width record layout. Conversion restores the directory cursor to the
last emitted record when the output buffer cannot hold the next converted
record. Ext4 directory offsets identify backing-store positions directly;
seeking restores the cookie without replaying preceding directory entries.
The shared legacy directory service retains 32-bit inode and cookie fields.
The final ext4 directory record uses the directory's byte size as its next
offset. Seeking to that offset returns end-of-directory. Internal iterator
termination values are not exported as directory offsets. IA-32 libc directory
enumeration requires offsets representable by its signed 32-bit `off_t`.

Ext4 mount registration supplies the complete VFS target pathname with a
trailing slash to lwext4. Its temporary pathname buffer is bounded by
`MAX_PATH`.

## Operation dispatch

Bus matching and probing use bus-operation callbacks. The selected PCI match
is retained for probing. Device file opening uses inode-type callbacks.
TTY, PTY, audio, loop, disk, mouse, RTC, pipe, socket, and VirtIO GPU ioctl interfaces
use command callbacks. Exact ioctl dispatch verifies the complete command,
including direction and size, after indexing by type and number. Table names
identify command families or access classes. OSS mixer
commands use the number and direction rules of that interface.

PCI sysfs attributes select their show, read, and write callbacks at
registration. Font operations use explicit callbacks. Stat adapters and
lookup routines are ordinary typed functions.

## Lookup and allocation indexes

| Inventory | Lookup key | Allocation or registration |
| --- | --- | --- |
| Devices | Bus and address | Indexed registration and list tail |
| Filesystem types | Filesystem name | Name index |
| Mounts and entry children | Complete pathname components | Mount tree |
| Character and block devices | Inode type and major | Ordered minor ranges per major |
| Unix sockets | Filesystem or abstract namespace key | Two-level slot bitmap |
| POSIX timers | Timer ID, with owner validation | Free list |
| Shared memory | ID, key, or owner and attached address | Two-level slot bitmaps |
| GPU resources and clients | Resource ID or context ID | Two-level slot bitmaps |
| GPU client handles | Resource ID | Two-level slot bitmap and handle array |
| Unix98 PTYs | Foreground process group and pair index | Allocation bitmap |
| BSD PTYs | Foreground process group and pair index | Fixed device index |

Mount resolution searches complete path prefixes from deepest to shallowest.
A mount at `/a` does not match `/ab`. Both PTY group indexes select the lowest
pair index for a matching group. Group updates and master closure maintain the
indexes. Unix98 directory generation uses one snapshot of the allocation bits.

Iteration remains necessary for hardware polling, wildcard driver matching,
minor-range matching, timer expiration, directory enumeration, data movement,
and owned-object cleanup. Balanced-tree traversal follows search paths;
it does not scan the complete inventory.

## Source validation and runtime checks

Run the host-side namespace audit with Python 3:

```sh
python3 test/syscall_tables.py
```

The audit compares declarations against Linux i386 and AMD64 UAPI headers,
checks unique numbers and adapter names, and checks native coverage against
the i386 service set. Header paths are configurable with `--i386-header` and
`--amd64-header`.

`DispatchTest` covers slot exhaustion and reuse across bitmap boundaries,
complete ioctl identity, and mount component boundaries in a test-enabled
kernel. These checks require compilation and kernel execution. Guest
regressions include `dev_pts.sh`, `tty_basic.sh`, `tty_vc.sh`,
`posix_sysv_shm.sh`, `posix_fd_pass.sh`, `posix_mount_state.sh`,
`posix_exec_signal.sh`, `posix_signal.sh`,
`posix_dirent.sh`, `xorg_compat.py`, and `x64_console_abi.py`.

Source audits do not establish compilation or runtime correctness.
