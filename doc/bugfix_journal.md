# Bug Fix Journal

Implementation records are ordered by date, with the newest entries first.
Each entry describes its dated revision; validation limits and implementation
details in older entries may be superseded by later fixes.

---

## 2026-10-08 - SQLite byte-range locks and transaction synchronization

POSIX advisory locks use process-owned byte ranges identified by filesystem and
inode. Read locks permit shared access; write locks exclude overlapping ranges
owned by other processes. Range replacement and partial unlocking preserve
unaffected intervals. Blocking requests wait interruptibly. Descriptor closure,
descriptor replacement, and process exit release the corresponding process
locks. BSD open-file locks remain independent. Native AMD64 and IA-32 lock
interfaces preserve their respective structure layouts and offset widths.

The AMD64 and IA-32 `fdatasync` system calls use the file synchronization path,
including the metadata synchronization provided by `fsync`. SQLite can acquire
its pending, reserved, and shared lock ranges and commit user dictionary
transactions.

Runtime validation on the AMD64 guest covers file synchronization and SQLite
integrity checks for both user dictionaries. Process isolation, non-overlapping
locks, partial unlocking, and offsets above 4 GiB are verified with separate
processes.

---

## 2026-10-08 - CPU usage accounting and atomic operation widths

CPU usage is sampled at `HZ=100`. IRQ0 supplies the bootstrap CPU sample, and
`SMP_TICK_VECTOR` supplies a sample on each additional online CPU. Accounting
runs before timer callbacks and scheduling. The saved interrupt frame selects
user or system time according to the interrupted privilege level. A kernel
idle task contributes to the receiving CPU's idle counter.

Each sample increments three native-width counters (`unsigned long`): the
executing task, its thread group, and its CPU. i386 uses 32-bit counters and
AMD64 uses 64-bit counters. Only thread-group increments require locked atomic
adds because threads may execute simultaneously on different CPUs. Task and
per-CPU counters have one timer writer and use relaxed atomic loads/stores;
task migration is serialized by the scheduler. Child-total writers are
serialized by the scheduler lock. Snapshots use untorn native-width loads,
then widen to 64 bits for aggregation and time conversion. Per-CPU storage
occupies separate cache lines. The heap returns eight-byte-aligned payloads.
Accounting takes no clock
lock and reads no hardware clock. Syscall entry, syscall return, and context
switches perform no CPU-time timestamp measurements.

### Ownership and Lifetime

`task_stats_t` holds per-thread user and system ticks and the process start
stamp. `task_usage_t` holds thread-group user and system totals, waited-for
child totals, and an ownership reference count. Process creation and fork
allocate zeroed group totals. `CLONE_THREAD` retains the existing group totals;
sharing an address space without `CLONE_THREAD` does not share CPU totals.
Exec preserves CPU usage.

Thread exit releases a group reference without adding usage to child totals.
The group's cumulative counters retain the contribution of exited threads.
Reaping a child process transfers its group CPU usage and waited-for descendant
usage into the parent's group child counters. Parent lookup and transfer occur
under the scheduler lock, preserving the parent accounting object's lifetime.
Sleeping tasks and zombies accrue no time while off CPU.

Per-CPU counters persist independently of task lifetime. Kernel task execution
contributes system time. CPU totals remain present after task removal and
reaping. CPU counters begin when scheduling is enabled.

### Reporting Interfaces

| Interface | CPU-time fields |
| --- | --- |
| `times()` | Calling group's user/system ticks and waited-for child ticks |
| `getrusage(RUSAGE_SELF)` | Calling group's user/system timevals |
| `getrusage(RUSAGE_CHILDREN)` | Waited-for child and descendant CPU timevals |
| `wait4()` | Reaped group's CPU usage and waited-for descendant CPU usage |
| `CLOCK_PROCESS_CPUTIME_ID` | Calling group's user plus system time |
| `CLOCK_THREAD_CPUTIME_ID` | Calling thread's user plus system time |
| `/proc/<tgid>/stat` | Thread-group CPU ticks |
| `/proc/<tgid>/task/<tid>/stat` | Individual thread CPU ticks |
| `/proc/stat` | Cumulative samples for each CPU and their aggregate |
| `/proc/uptime` | Monotonic uptime and cumulative CPU idle samples |

The i386 `times()` wire fields retain 32-bit clock values; native AMD64 fields
retain 64-bit values. Timeval and timespec conversions use 64-bit arithmetic.
Invalid resource-usage selectors return `EINVAL`. CPU clock resolution queries
return 10,000,000 nanoseconds. Other resource-usage fields retain their
independent accounting paths.

### Sampling Constraints

A sample represents one 10 ms interval. Execution shorter than an interval may
receive no sample, and repeated workloads can alias with the periodic timer.
CPU clocks measure sampled execution rather than exact sub-tick duration.
Delayed or coalesced timer interrupts can reduce the number of samples; uptime
continues to follow the monotonic clock. IRQ and deferred interrupt work are not
reported as separate CPU categories. Nice, iowait, and steal fields remain zero.

Counters wrap modulo their native width. At 100 Hz an i386 counter wraps after
approximately 497 accumulated CPU-days; thread-group and child totals can
reach that limit sooner in wall time through parallel execution. This is an
explicit performance tradeoff: i386 accounting uses no 64-bit locked operations.
64-bit reporting and time conversion do not extend the underlying counter range.

### Atomic Operation Widths

The shared kernel and both architecture backends use these widths:

| State | i386 | AMD64 | Operation |
| --- | --- | --- | --- |
| Sampled task/group/CPU/child ticks | 32 bits | 64 bits | Relaxed native-width loads; locked adds only for shared group ticks |
| Reference counts, lock words, semaphore counts | 32 bits | 32 bits | Locked updates retain synchronization and lifetime ordering |
| SMP online counts, TLB generations/acknowledgments | 32 bits | 32 bits | Acquire/release publication and native-width updates |
| Physical-page dirty flags | 8 bits | 8 bits | Locked bit operations on byte flags |
| Socket shutdown flags and umask | 32 bits | 32 bits | Native-width locked bit updates/exchange |
| Scheduler call count | 32 bits | 32 bits | Relaxed locked increment |

Reference counts and lock words do not benefit from being widened to 64 bits
on AMD64. Read-only physical-page reference and pipe endpoint count queries
use acquire loads instead of locked adds of zero. Spinlock release uses a
32-bit release store; saving the caller's interrupt level uses a plain local
store. Contended spinlocks poll with relaxed loads before retrying the acquire
exchange. Other reference updates and synchronization barriers retain their
existing ordering requirements.

### Validation

`CPUAccounting` covers mode attribution, idle accounting, stable off-CPU totals,
native-width rollover, widened time conversion, thread-group reference lifetime,
fork reset, syscall reporting,
CPU clock IDs, native AMD64 clock field width, and simultaneous updates on two
CPUs. `Timekeeping` covers clock domains, wall-clock changes, delayed interrupts,
and concurrent clocks and timers.

`tools/user/cpu_accounting_probe.c` validates userspace accounting, sleeping,
thread exit, delayed child reaping, descendant usage, CPU clock resolution,
invalid selectors, and agreement between reporting interfaces. Source staging
before guest launch uses:

```sh
cp tools/user/cpu_accounting_probe.c tools/guest/root/
```

Guest compilation
and execution use:

```sh
gcc -O2 -std=gnu99 -pthread -o /tmp/cpu_probe cpu_accounting_probe.c
/tmp/cpu_probe
```

## 2026-10-07 - Directory-only open enforcement

After commit `11d9a69bb282ef47f331cfab6b8e7e4b7bc36441`, copying
`./kernel` to `/boot/kernel` attempts to use `/boot/kernel/kernel` as the
destination. That commit changes special-file ownership and the shared
`getcwd` return value. Regular-file stat conversion and open dispatch are
unchanged. Internal pathname-resolution calls discard the `getcwd` return
value and consume the populated buffer. The temporal association does not
establish which change exposed the copy path.

The open path through `sys_openat`, `fs_open`, and `ext4_path_open` does not
enforce `O_DIRECTORY`. An SSH session on the x64 guest reproduced the failure
with GNU coreutils 9.7 and regular source and destination files. The trace
shows `openat(AT_FDCWD, destination, O_RDONLY | O_PATH | O_DIRECTORY)` returning
descriptor 3 for a regular file. The subsequent `newfstatat(3, "source", ..., 0)`
fails with `ENOTDIR`, and the copy diagnostic names `destination/source`.
Independent stat output identifies `/boot/kernel` as a regular file.

`fs_open` now checks the resolved inode type whenever `O_DIRECTORY` is set.
For a non-directory, it releases the opened file and returns `ENOTDIR` before
permission checks and descriptor installation. Both `O_PATH` and ordinary
opens use this check, including targets reached through final symlinks.

The regression script `test/directory_open.py` checks rejection of regular
files, file symlinks, and FIFOs; successful directory and directory-symlink
opens; file replacement by `cp`; and copying into a directory. It passes on
the host. Compiler syntax checks pass for x86 and x64, and patch whitespace
checks pass. The guest reproduction failed as expected before the correction.
Corrected kernel build and guest runtime validation remain pending.

## 2026-10-07 - FIFO ownership and getcwd syscall return value

GNU Make 4.4.1 initializes its FIFO jobserver by creating a named pipe with
mode `0600`, opening the read endpoint with `O_RDONLY | O_NONBLOCK`, and
opening the write endpoint with `O_WRONLY`. MOS previously cleared the stat
structure without filling its ownership fields. The resulting UID and GID
were zero, so the discretionary access check selected group or other bits
for a non-root creator and rejected the read open with `EACCES`.

Special-file metadata now retains the creator's effective UID and GID, matching
the existing tmpfs credential convention. FIFO and generic special-file stat
handlers return those stored credentials. FIFO metadata uses the backend
superblock retained by the VFS before the open permission check.

The Linux `getcwd` syscall returns the pathname length including its terminating
NUL. MOS previously returned the destination address. The shared handler now
returns the length, rejects a zero size with `EINVAL`, and returns `ERANGE`
without copying when the destination is too small. A null destination with a
nonzero size returns `EFAULT`. Both syscall namespaces use the shared handler;
internal pathname-resolution callers continue to consume its output buffer.

glibc 2.37 stores the syscall result in an `int`, tests for a positive result,
and uses the result as an allocation size for `getcwd(NULL, 0)`. Returning an
x64 buffer address violates these assumptions. The reported `getcwd: No such
file or directory` still requires a syscall trace to establish its exact path;
the return-value correction does not establish that the directory exists.

The regression script `test/fifo_jobserver.sh` requires a non-root account and
checks FIFO path and descriptor ownership, the jobserver open sequence, token
transfer, the raw `getcwd` return length, the libc wrapper, and undersized-buffer
errors. Compiler syntax checks passed for both x86 and x64, covering the
modified implementations, internal pathname-resolution callers, and syscall
dispatchers. Shell syntax and patch whitespace checks passed. Kernel build
and guest execution remain pending.

References: GNU Make 4.4.1 `src/posixos.c`, `jobserver_setup()`;
glibc 2.37 `sysdeps/unix/sysv/linux/getcwd.c`, `__getcwd()`.
Source archives: [GNU Make 4.4.1](https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz),
[glibc 2.37](https://ftp.gnu.org/gnu/glibc/glibc-2.37.tar.xz).

## 2026-10-07 - Descriptor callback lifetimes and concurrent mapping transactions

The descriptor-table mutex protects descriptor lookup, installation, removal,
and close-on-exec flags. Socket descriptor installation acquires this mutex
while the task owns the network-core mutex. File readiness and ioctl callbacks
therefore execute without ownership of the descriptor-table mutex.

`fs_fd_poll` and `fs_ioctl` obtain a referenced file description through
`fs_io_begin`, release the descriptor-table mutex, and invoke the backend
callback. `fs_io_end` releases the reference after the callback. The active
scope is linked to the task's I/O scopes so process termination releases the
reference through `fs_cancel_io`.

A readiness callback may register multiple subscriptions in a poll table.
`fs_fd_poll` retains one additional file reference on the first subscription
registered by that callback. `poll_table_cleanup` removes subscriptions in
reverse registration order and releases this reference after all subscriptions
from that callback have been removed. Concurrent descriptor closure cannot
release the backing file or readiness queues while those subscriptions remain
registered. Each reused poll entry initializes its retained reference to null.

The same reference and subscription cleanup applies to normal completion,
timeouts, interrupted waits, and process termination. The implementation is
shared by the x86 and x64 syscall backends.

### Address-space mapping transactions

Each address space contains a recursive mapping mutex. Anonymous mapping
allocation holds this mutex from address selection through region insertion.
Region replacement, splitting, removal, protection changes, heap adjustment,
and mapping relocation use the same mutex. Enumeration retains ownership while
callbacks inspect or duplicate regions. The VMA spinlock protects individual
tree accesses; the mapping mutex protects operations that span multiple tree
accesses. Recursion permits mapping helpers to participate in an enclosing
transaction without releasing ownership between steps.

VM mapping and fault-lock ownership is counted per task. Forced thread-group
termination permits lock owners to continue scheduling until they release their
VM locks. Reaping waits for both active CPU execution and VM-lock ownership to
end, preventing an abandoned lock from blocking address-space cleanup.

### Validation

`test/fd_callback_lifetime_host.py` executes the production descriptor and
poll-table functions with AddressSanitizer and UndefinedBehaviorSanitizer.
The checks cover callback lock ownership, descriptor installation from a
callback, descriptor closure during an ioctl, multiple readiness subscriptions,
poll-table reuse, subscription cancellation, and task I/O cancellation.

`test/fd_callback_threads.py` executes inside the guest. It transfers 5,000
file descriptors with `SCM_RIGHTS` while another thread polls the socket and
executes `FIONREAD`. It also exercises 64 descriptor-closure races with active
readiness subscriptions. Each transfer receives an acknowledgement before the
next ancillary message is sent.

`test/mmap_threads.c` allocates mappings concurrently in eight threads across
512 rounds. The checks require disjoint returned ranges, independent stored
values, successful protection changes, and successful unmapping.

`test/mmap_exit_threads.c` executes 32 child-process runs with eight mapping
threads per child. Each child exits while its threads allocate, protect, and
release mappings. Completion requires successful child reaping in every run.

## 2026-10-07 - Process ABI, fault delivery, and page-cache lifetime corrections

The x86 backend accepts ELF32 images with machine type `EM_386`. The x64
backend accepts ELF32/i386 and ELF64/AMD64 images. The executable and its
interpreter must select the same format.

### Executable pathname interfaces

`/proc/self/exe` and `/proc/<pid>/exe` are symbolic links to the resolved
pathname stored on the process's main executable file. The link identifies
the main ELF image, independently of `argv[0]`, the working directory, and
the `PT_INTERP` dynamic linker. Script execution identifies the shebang
interpreter. Fork, vfork, and clone retain a reference to the executable;
successful execution replaces the reference, rejected execution preserves
it, and process cleanup releases it.

Both i386 and AMD64 syscall namespaces provide `readlink` and `readlinkat`.
`readlinkat` resolves relative paths against a directory descriptor or
`AT_FDCWD`; absolute paths ignore the descriptor. Returned pathname bytes
are truncated to the buffer size and do not include a terminating NUL.
The glibc dynamic linker uses `readlinkat` on `/proc/self/exe` to resolve
`$ORIGIN` in an executable's `DT_RPATH` or `DT_RUNPATH`.

The stored executable pathname is not reconstructed after renaming or
unlinking the executable and is not rebased for a chroot caller. Empty-path
`readlinkat` operations are not supported and return `ENOENT`.

### Image and namespace layouts

ELF preparation selects a format descriptor from the architecture's supported
format table. The descriptor provides typed header and program-header readers,
an initial-stack builder, user register initialization, and VM limits. Stack
construction uses a fixed word type for each format. The VM retains its task
size, mmap base, and brk limit. Fork and clone copy these limits and the saved
user register context.

The i386 and AMD64 syscall namespaces select their wire adapters directly.
Shared-memory control returns an internal status record. Its adapters serialize
56-byte i386 and 112-byte AMD64 records. Socket timestamp retrieval returns an
internal timeval, which the AMD64 ioctl adapter converts to two signed 64-bit
fields. Ptrace adapters use fixed-width memory words and register layouts for
the caller's namespace, independently of the traced task's execution mode.
The architecture backend captures the stopped CPU register context.

Robust-list registration records the head address, head size, and typed reader
on the thread. Exit cleanup traverses the list with that reader. The getter
serializes the registered address and size using its syscall namespace.
Executable initialization supplies a 12-byte i386 or 24-byte AMD64 default
head size. Fork and clone clear the registered head and reader.

Signal frame construction resides in the interface implementations. The
architecture selects delivery from the saved user code selector and interprets
clone TLS at the register-context boundary. Shared signal disposition logic
does not select an executable format.

Synchronous page faults deliver `SIGSEGV` directly from the faulting register
context. Caught faults return through the installed handler; ignored or blocked
fault signals use the default fatal disposition. `SA_SIGINFO` frames contain
the exact fault address, `SEGV_MAPERR` or `SEGV_ACCERR`, the page-fault trap
number, and the processor error code. Fault reports include the exact address
and instruction pointer.

Default fatal signals and `exit_group` terminate all members of the thread
group, including its leader when initiated by a worker. The leader retains the
encoded signal or exit status for the parent's wait operation. Ordinary thread
exit preserves the remaining group. Signal zero with `tkill` checks the target
and sender credentials without queuing a signal.

`PTRACE_DETACH` requires a stopped tracee, clears its tracing state, and resumes
execution with an optional signal. Detaching a running tracee returns `ESRCH`.
A signal queued during a stopped task's resumption is processed before returning
to userspace.

Both syscall namespaces provide `clock_getres`; i386 additionally provides the
64-bit time variant. Realtime, monotonic, raw monotonic, boottime, and coarse
aliases report the microsecond time core's 1000-nanosecond resolution. A null
output pointer validates the clock ID without writing a result. Unsupported
clock IDs return `EINVAL`.

Memory services validate reserved ranges through the architecture backend.
Physical allocation limits, DMA limits, allocation preferences, and block-cache
limits are architecture configuration values. NX enforcement follows the paging
hardware and applies to both i386 and AMD64 processes on x64. Non-PAE x86 page
tables do not provide NX support.

File-page cache lookup returns a retained physical page. Mapping installation
and buffered reads release that reference only after acquiring the mapping
reference or completing the copy. Cache invalidation and eviction release the
cache's reference independently of active readers. Concurrent cache misses
retain the selected existing page before releasing the cache lock.

Ext4 page reads and writeback use operation-local handles with independent file
positions. Page faults do not seek or restore the shared descriptor's ext4
cursor, including while filesystem I/O waits for a mount lock.

### Procfs thread groups

`/proc/self` selects the calling task's thread-group ID. Root process
enumeration includes thread-group leaders; threads remain accessible by their
numeric IDs. `/proc/<pid>/task` lists live member thread IDs. The directory's
`st_nlink` is two plus the live member count, and metadata queries on an open
directory descriptor recompute that count. Directory entries use `DT_DIR`.
`/proc/<pid>/task/<tid>` exposes per-thread files and symbolic links only when
the thread belongs to the selected group. Exited threads disappear from the
task directory. Status records report the thread ID as `Pid`, group ID as
`Tgid`, and live member count as `Threads`. Stat records report the same live
count in `num_threads`. Chromium's thread helper uses directory-relative stat
and this link count to determine whether a process has one thread.

### Unix IPC and descriptor directories

Unix `SOCK_SEQPACKET` sockets provide ordered records for socket pairs and
named connections. Each endpoint has a 256 KiB receive ring. A send commits
the complete payload, sender credentials, and associated descriptor references
under the receive lock. The maximum payload is 262127 bytes. Full queues
block until room is available or return `EAGAIN` for nonblocking operations;
oversized records return `EMSGSIZE` without queuing a partial record.

Receives consume one record, discard any truncated tail, and set `MSG_TRUNC`
when the payload exceeds the supplied buffers. `MSG_TRUNC` input requests
the full record length as the return value. `MSG_PEEK` preserves the queued
record and duplicates any returned descriptor references. Descriptor passing
supports 16 descriptors per message. `SO_PASSCRED` is supported on Unix
sequenced-packet sockets and enables `SCM_CREDENTIALS` records containing
the sending process ID, user ID, and group ID captured at send time.
`FIONREAD` reports the sum of queued payload lengths. Peer closure and
write shutdown make EOF readable after buffered records are consumed.

`/dev/fd` is a descriptor directory for the calling process. Procfs descriptor
links retain their target while the link handle exists. Following a link
reopens an anonymous pipe through its backend with independent file status
flags and reader/writer counts. Named targets are reopened by pathname;
unlinked targets without an anonymous-object reopen operation cannot be
reopened. These operations support Bash process-substitution pipes.

### Filesystem notifications

The i386 and AMD64 syscall namespaces provide `inotify_init`, `inotify_init1`,
`inotify_add_watch`, and `inotify_rm_watch`. Inotify descriptors provide
filesystem event queues, blocking and nonblocking reads, close-on-exec flags,
`FIONREAD`, asynchronous `SIGIO`, and poll/select/epoll subscriptions. Events
contain the Linux 16-byte header and a NUL-terminated name padded to a multiple
of 16 bytes. Each read consumes complete records; an undersized first buffer
returns `EINVAL` without consuming the record. Consecutive identical unread
events are coalesced. Queue overflow produces `IN_Q_OVERFLOW` with descriptor
`-1`, and watch removal produces `IN_IGNORED`.

Watch identity uses the backend recorded by the opened file and its inode
number. Hard links and virtual-entry aliases share a watch descriptor within
an instance. Sysfs mount views retain the same canonical entry identity.
Virtual entries supply their parent identity and child name directly.
Renames retain inode watches and
produce paired `IN_MOVED_FROM` and `IN_MOVED_TO` records with a nonzero matching
cookie, together with `IN_MOVE_SELF` on the moved inode. Removing the last
link produces `IN_DELETE_SELF` and removes the watch when the last tracked
open reference closes. Open references include `O_PATH` descriptors and
references retained by executable images and memory mappings. `O_PATH`
operations do not produce open, access, or close events. Unmount produces
`IN_UNMOUNT` followed by `IN_IGNORED` for watches within the removed filesystem.

Successful filesystem operations produce access, modification, attribute,
open, close, creation, deletion, link, and rename notifications. Directory
watches include the immediate child name and `IN_ISDIR` for directory events;
self-deletion and self-movement events do not include `IN_ISDIR`. Watch masks
support replacement, `IN_MASK_ADD`, `IN_MASK_CREATE`, `IN_ONESHOT`,
`IN_ONLYDIR`, `IN_DONT_FOLLOW`, and `IN_EXCL_UNLINK`. Duplicate descriptors and
forked processes share one watch set and queue. Instance closure releases all
remaining watches and quota charges. Wait cancellation removes subscriptions
and releases borrowed file references.

Open files retain registration state independently of installed watches.
Without active watches, open registration performs no notification metadata
lookup or allocation, and access and modification hooks return without taking
the notification lock. Metadata capture occurs when a watch is installed or
before a namespace mutation. Files opened before watch installation therefore
participate in subsequent notifications, including after rename or unlink.
Notification capture failures do not invalidate a successful filesystem open.

The `/proc/sys/fs/inotify` directory exposes three root-owned controls with
mode `0644`: `max_user_watches` defaults to 8192, `max_user_instances` defaults
to 128, and `max_queued_events` defaults to 16384. Values accept decimal
integers from zero through `INT_MAX`. Watch and instance limits apply across
all instances charged to the creating real UID. New watches exceeding the
watch limit return `ENOSPC`; new instances exceeding the instance limit return
`EMFILE`. Changing either limit preserves existing resources. Queue capacity
is captured when an instance is created. `/proc/sys/kernel/osrelease` reports
the configured kernel release through the same directory hierarchy.

Directory watches do not recurse; recursive clients register individual
directories. Memory-mapped access and modification do not generate inotify
events. Notification operations depend on the filesystem's supported
operations: tmpfs does not provide hard-link or rename operations. Filesystems
without numeric inode identity use filesystem-relative pathname identity.
Allocation failures during notification capture or queuing produce queue
overflow notifications.

Mount-root callbacks accept only the exact mount root, with an optional
trailing slash. Descendant lookup requires a path-aware operation; unmatched
descendants of device nodes and static proc entries cannot open the root node.

### Event counters and namespace probes

The i386 and AMD64 namespaces provide `eventfd` and `eventfd2`. Counter
descriptors are shared by duplicate descriptors and forked processes. The
counter's maximum value is `UINT64_MAX - 1`; writing `UINT64_MAX` returns
`EINVAL`. Reads require at least eight bytes and return eight bytes. Writes
require exactly eight bytes. Empty reads and overflow writes block, or return
`EAGAIN` with `EFD_NONBLOCK`. `EFD_SEMAPHORE` reads consume one counter unit;
ordinary reads drain the counter. `EFD_CLOEXEC` controls descriptor inheritance
across execution. Readiness subscriptions support poll, select, and epoll.

Clone namespace flags are recognized but unsupported and return `EINVAL`.
No user, PID, mount, network, IPC, UTS, or cgroup namespace isolation is
created. Chromium's user-namespace probe interprets this result as unavailable
support. Namespace sandbox operation requires kernel interfaces beyond the
configured MOS implementation.

### Validation

`python3 test/page_cache_reads.py --directory PATH` validates concurrent
file-backed faults against distinct deterministic page contents. Eight workers
read disjoint shuffled pages through one private mapping. The probe also checks
that page reads preserve the descriptor offset; the selected directory must
support regular files and mappings.

`python3 test/thread_faults.py` compiles and runs a probe in the target system.
It validates recovery from synchronous faults on ordinary and alternate stacks,
fault metadata, default and blocked or ignored fatal faults in worker threads,
group exit from a worker, signal-zero probing, clock resolution, and trace
detachment with signal injection. Parent waits are bounded to detect retained
threads and unavailable exit status.

`python3 test/proc_exe.py` validates executable links in the running system:
symbolic-link metadata, target identity, buffer truncation, `readlinkat`
directory resolution, fork inheritance, rejected execution, and execution
through a relative symlink with an independent argument-zero string.
`python3 test/proc_tasks.py` validates live thread enumeration, thread-group
membership, status identities, directory-relative metadata, link counts on
open directory descriptors, and thread creation and termination.

`python3 test/unix_seqpacket.py` validates record boundaries, truncation,
peeking, empty records, large records, descriptor and credential delivery,
nonblocking queue exhaustion, shutdown, named connections, and socket flags.
`python3 test/dev_fd.py` validates pipe reopening and endpoint lifetime,
regular-file positions, and Bash process substitution in the running system.
`python3 test/inotify.py` validates event records, masks, rename cookies,
hard-link identity, unlinked inode lifetime, watch installation after open and
namespace changes, `O_PATH` references, vectored
reads, queue byte counts, asynchronous signals, poll/epoll subscriptions,
descriptor sharing, and blocking reads. Its working directory must support
hard links and renames; `--directory PATH` selects that directory. The
root-only guest command `python3 test/inotify.py --guest --limits --mounts`
also validates quota controls, queue overflow, capacity capture, and unmount
notifications. These options temporarily modify and restore inotify controls
and create and remove a tmpfs mount.

`python3 test/filesystem_notifications_host.py` compiles the production VFS,
virtual-entry, and notification code against isolated host services. Address
and undefined-behavior checks cover unmatched descendant lookup, canonical
alias identity, parent events, mount-view lifetime, late watch installation,
and allocation-free notification registration with no active watches.

`python3 test/eventfd.py` validates counters, descriptor flags, semaphore mode,
epoll notifications, and blocking operations across fork. The MOS-only
`python3 test/clone_namespaces.py` validates `EINVAL` for namespace flags.

`python3 test/abi_adapters.py` compiles production adapters against isolated
host-side kernel services and runs them with undefined-behavior checks. It
validates robust readers and cross-layout getters, ptrace word and register
serialization, shared-memory records, ELF header conversion, initial stacks,
and executable register initialization. These checks do not exercise kernel
scheduling or privilege transitions.

`python3 test/syscall_tables.py` validates syscall numbers and service coverage
for both namespaces. The embedded kernel tests validate ELF image acceptance
and page execute permissions. Guest validation uses `./run.sh kvm test` from
the MOS source directory with the configured RH9 image. Native AMD64 guest
interfaces require an AMD64 userspace image.

## 2026-10-07 - Concurrent timekeeping and timer synchronization

The timing subsystem separates elapsed time from calendar time. Network,
scheduler and relative timer deadlines use monotonic time, so changes to the
wall clock cannot prolong or prematurely expire a wait. Filesystem timestamps
and absolute `CLOCK_REALTIME` timers use calendar time.

### Clock Sources and Domains

`src/device/time.c` owns the clock origin, monotonic clamp, IRQ sample and wall
offset. PIT supplies the boot clock and remains the interrupt source. KVM
pvclock is selected after CPU setup enables SSE2; each additional CPU registers
its own clock slot before becoming online. Systems without that facility keep
the PIT fallback. Switching sources preserves elapsed boot time.

| Interface | Domain and purpose |
| --- | --- |
| `time_now_us()`, `time_now_ms()` | Precise monotonic elapsed time |
| `time_coarse_ms()` | Last monotonic IRQ sample for deadline expiration |
| `time_deadline_ms()` | Rounded, saturating monotonic millisecond deadline |
| `time_now_tickets()` | 64-bit serviced PIT ticks for boot stamps and diagnostics |
| `time_wall_us()`, `time_wall_sec()` | Calendar time for syscalls and metadata |

RTC snapshots support binary and BCD encodings, 12-hour and 24-hour formats,
and Gregorian leap years. The same conversion serves initialization and
`/dev/rtc`. Relative POSIX timers remain monotonic even when created with
`CLOCK_REALTIME`; absolute realtime timers follow wall-clock adjustments.

### Concurrent Clock Synchronization

The timing change originally relied on serialized kernel entry. Removing that
serialization permits simultaneous reads and updates on different CPUs. Local
interrupt masking alone cannot protect shared state: i386 can tear 64-bit
values, concurrent monotonic updates can regress the published sample, and
interleaved PIT or CMOS port transactions can return incorrect hardware data.

An IRQ-masked `time_lock` protects the shared clock state, PIT tick
increment, pending-wrap tracking and PIT latch transaction. The IRQ path
publishes the coarse sample and releases this lock before broadcasting ticks,
processing alarms or queuing network work. Clock operations never acquire
the scheduler or network locks. Scheduler and network callers may acquire
`time_lock` while holding their own locks, without a reverse acquisition path.
Spinlock contention continues to poll pending TLB shootdowns through the common
locking implementation.

A separate `rtc_lock` serializes CMOS index/data transactions. RTC
resynchronization reads the calendar before acquiring `time_lock`. A task-owned
`timer_lock` serializes POSIX timer allocation, lookup, rearming, deletion,
process-exit cleanup and service-task polling. Its scope cleanup releases the
mutex on every ordinary return, including syscall errors.

Network core ownership, deferred-service locking, concurrent scheduler
handoff and address-space shootdown coordination coordinate simultaneous execution. Network service
deadlines and IRQ comparisons both use monotonic milliseconds; converting one
side to raw ticks would prevent timely lwIP timeout processing.

### Validation

`Timekeeping` covers calendar conversion, clock domains, wall-clock changes,
repeated reads, delayed IRQ handling and parallel clock/RTC readers. The parallel
test rendezvous requires simultaneous kernel execution on two CPUs and checks
monotonic samples, coherent tick counts, valid calendar fields and concurrent
POSIX timer creation, rearming, lookup and deletion. The `smp` suite covers parallel execution and TLB shootdowns.

`tools/user/timing_probe.c` retains wall-clock jump, sleep, polling, socket
timeout, interval timer, POSIX timer and ping regression coverage.

The x86 and AMD64 release kernels and targeted test kernels build successfully.
The `CPUAccounting` and `Timekeeping` suites pass in two-CPU KVM guests for both
architectures. The RH9 CPU accounting probe passes on both kernels.

Build both test kernels:

```sh
make ARCH=x86 BUILD=release all test
make ARCH=x64 BUILD=release all test
```

Run the following in each booted test kernel with at least two CPUs:

```sh
echo Timekeeping > /proc/tests/.runner
echo smp > /proc/tests/.runner
```

Run the timing probe and existing epoll regressions separately to verify
userspace waits and networking.

### CPU Usage

[CPU usage accounting](bugfix_journal.md#2026-10-08---cpu-usage-accounting-and-atomic-operation-widths) uses per-CPU timer samples and
separate task and thread-group totals. Elapsed and wall-clock reads do not
provide CPU usage measurements. CPU clocks advertise a 10 ms resolution.

## 2026-10-06 - x86 process-launch performance validation

Date: 2026-10-06. Revision: `2e6426d` (`refine multi-arch codes`).

The release x86 kernel completed the 1,000-process benchmark in a median of
0.13 seconds with one virtual CPU and 0.12 seconds with two. PIT clock access
accounted for approximately 10% of active samples in both configurations.
Explicit TLB-flush routines were rarely sampled. These measurements identify
clock access as an optimization candidate; they do not establish the benefit
of PCID on x64.

### Configuration and workload

The kernel was built with `make -j8 ARCH=x86 BUILD=release`. The host processor
was an AMD Ryzen 9 7950X. QEMU used KVM, the `coreduo` CPU model, 4 GiB RAM,
VMware VGA, and one or two virtual CPUs. The RH9 disk was opened with
`snapshot=on`; temporary disk changes were discarded when each VM exited.

The guest reported Bash 2.05b.0, GNU coreutils 4.5.3, and glibc 2.3.2. The benchmark scripts were:

```sh
# test.sh
for i in $(seq 1000); do /bin/true; done

# full.sh
for i in $(seq 10); do echo $i; time -p ./test.sh; done
```

Each timed run included the shell executing `test.sh`, its `seq` invocation,
and 1,000 executions of `/bin/true`. Timings were captured from an ordinary
`full.sh` execution without monitor sampling. Output was redirected in the
guest and read after all ten runs completed.

The kernel was booted directly using `-kernel out/x86/release/kernel -append
'bash verbose=0'`. This starts the benchmark without the complete RH9 init
and desktop workload used by the GRUB boot path. Networking used QEMU user
networking. Consequently, these results are not a controlled reproduction of
the earlier Linux comparison. No fresh Linux measurement or pre-optimization
MOS comparison was performed.

### Elapsed times

| Virtual CPUs | Ten reported real times, seconds | Median | Mean | Range |
| --- | --- | --- | --- | --- |
| 1 | .14, .14, .14, .14, .13, .12, .12, .12, .12, .13 | .130 | .130 | .12–.14 |
| 2 | .14, .14, .14, .13, .12, .12, .12, .12, .12, .12 | .120 | .127 | .12–.14 |

Both sequences show a warm-up trend. The 0.01-second reporting precision and
overlapping ranges prevent attributing a small difference to CPU count.
The reported user and system times were not used to estimate kernel costs.

### Sampling results

Separate profiling runs repeatedly executed `test.sh`. A QMP monitor driver
paused the VM, read each CPU's registers using an explicit `cpu-index`, and
resumed execution after each sample. The running interval was randomized
between 1 and 5 milliseconds. Symbols came from the matching release
`kernel.dbg`, including assembly symbols without size metadata.

Each main capture lasted 40 seconds of host wall time, including monitor
overhead. The one-CPU capture contained 4,429 active samples. The two-CPU
capture contained 5,812 samples, of which 2,906 were in `smp_idle`. The table
excludes idle samples; userspace remains part of the denominator.

| Sample location | One CPU, % active | Two CPUs, % active |
| --- | ---: | ---: |
| Userspace | 16.26 | 17.00 |
| Page-fault entry (`intr0e_stub`) | 9.19 | 8.12 |
| Port reads and writes from `time_now_us` | 9.80 | 10.50 |
| `_spinlock_lock.part.0` | 5.80 | 6.13 |
| `load_ldt` | 3.61 | 3.58 |
| `mm_destroy_user_map` | 3.25 | 3.65 |
| `mm_copy_phys_page` | 2.48 | 2.34 |
| `tlb_flush` | 0 samples | 0.07 |

These are sampled instruction locations, not hardware cycle measurements.
Privileged instructions and emulated device access may stop at recognizable
instruction boundaries. The sampling pauses also perturb execution. The
figures therefore provide prioritization evidence rather than exact savings
predictions or a complete attribution of KVM host work.

### Clock callers and other costs

All port samples in the main captures returned into `time_now_us`. The ports
were PIT data at `0x40` and the latch command at `0x43`.
[The PIT clock implementation](../src/driver/impl/timer/pit.c) latches and reads
the hardware counter on each clock query.

A supplementary 20-second two-CPU capture collected 1,394 active samples,
including 165 PIT port samples. Caller addresses were read using stack offsets
verified against the exact release disassembly, rather than a general frame
pointer walk. Clock origins accounted for those 165 samples as follows:

| Origin | PIT samples |
| --- | ---: |
| `ps_fire_timers_unsafe` | 68 |
| `ext4_touch_file` | 63 |
| `setup_stack` | 29 |
| `timer_arm_unsafe` | 3 |
| Network service update | 1 |
| System service task | 1 |

[Scheduler timer checking](../src/ps/impl/alg/ps_alg_rr.c) reads the clock before
examining timer deadlines. [Filesystem timestamp updates](../src/fs/impl/root.c)
read the clock before updating inode timestamps. [ELF stack setup](../arch/abi/elf_stack.h)
reads the clock to seed the random generator. Reducing PIT queries in these
paths, or supplying a faster clock source, is a concrete next investigation.
Timer accuracy and filesystem timestamp semantics must remain correct.

[LDT loading](../arch/x86/ps/impl/task.c) performs `LLDT 0` even when a task has
no LDT. Nearly every `load_ldt` sample was at the return immediately after that
instruction. Tracking the loaded LDT per CPU is another candidate, subject to
correct task and descriptor lifecycle handling.

The sampled [spinlock acquisition](../src/lib/impl/lock.c) costs appear on one
CPU as well as two. Samples concentrated around atomic exchanges, including
the exchange used to save interrupt state; none appeared in the retry loop.
This evidence indicates acquisition overhead rather than measured contention.
Page-fault entry and memory copying also warrant investigation. A page fault
is not an ordinary TLB miss.


### Implications for PCID

PCID is not available in this 32-bit execution mode. A low sample count inside
`tlb_flush` does not measure translation refill costs after CR3 changes;
those costs occur at subsequent memory accesses. Hardware TLB-miss counters were not collected.

An x64 comparison with PCID enabled and disabled, together with translation
miss counters, is needed to quantify its benefit. This x86 profile supports
investigating PIT clock queries first, without ruling out a PCID improvement.

The measurement driver used build-specific memory addresses, structure offsets,
and caller stack offsets. Reproducing these measurements on another revision
requires recalculating those offsets.

## 2026-10-05 - Address space TLB shootdowns

TLB requests contain the physical page-directory address for user mappings.
Every online remote CPU acknowledges each published request. A CPU flushes
non-global user translations only when its active CR3 matches that address.
A zero request root invalidates global and non-global translations on every
online CPU. CR3 activation discards translations when entering another address
space; PCID is not enabled.

A dedicated lock serializes request publication through final acknowledgment.
Spinlock waiters and AP startup waiters poll requests with interrupts disabled.
The target set depends only on CPU availability and does not dereference remote
task or VM objects. Scheduler state remains independent of request publication.

## 2026-10-05 - Ext4 open-inode lifetime

Ext4 open file objects are registered by filesystem and inode number under the
mount lock. Unlink removes the directory entry and decrements the on-disk link
count immediately. Independent opens, duplicated descriptors, transferred
descriptors, and file-backed mappings retain the backing inode. Final release
of a zero-link inode invalidates cached data, truncates its blocks, and releases
the inode allocation. No temporary directory entry is created. Descriptor stat
reports the on-disk link count. File timestamps and size refreshes use the
backing inode rather than a pathname that can be removed or reused.

Rename replacement uses the same unlink operation for an occupied destination.
The registry includes file objects retained without a descriptor-table entry.
Cache invalidation precedes inode reuse and follows the cache-to-mount lock order.
Journaling is disabled; interrupted metadata writes and unreleased zero-link
inodes require filesystem checking after an unclean shutdown.

Validation covers hard-link counts, independent opens, duplicated descriptors,
namespace removal, mappings retained after descriptor closure, rename
replacement, fork inheritance, and restoration of the free-inode count.
`test/posix_unlink_open.sh` passes on the AMD64 kernel with RH9 userspace.

## 2026-10-05 - AMD64 kernel RAM allocation

The AMD64 physical RAM mirror covers the configured 128 GiB physical-address
range. Ordinary contiguous kernel allocations may use every managed RAM frame,
including frames above 4 GiB. Page-table pointer conversion and allocation
release support both the kernel image alias and the RAM mirror. Kernel pointer
classification uses the architecture user-address limit.

AMD64 `LowTotal` includes all allocator-managed RAM; `HighTotal` is zero because
no managed RAM requires a temporary kernel mapping. The i386 backend retains
its 768 MiB direct-map boundary and non-PAE 4 GiB physical-address limit.
Firmware holes and allocator metadata are excluded from both totals.

`vm_alloc_dma()` retains physical addresses below 4 GiB for ATA, AC97, and e1000
allocations. This device constraint is independent of ordinary kernel allocation.

## 2026-10-05 - Conditional event notification scheduling

`cond_notify()` clears the event state and wakes one waiter under the wait-list
lock. It schedules only when a waiter was awakened, interrupts were enabled,
and task scheduling is enabled. Notification under an outer spinlock or a
nonpreemptible callback only makes the waiter runnable. Notification without a
waiter preserves the event for the next wait without invoking the scheduler.
Circular buffers use this single notification interface.

## 2026-10-05 - Deadline-driven lwIP deferred work

Network stack operations publish the next timeout deadline through
`sys_timeouts_sleeptime()`. PIT handling compares that deadline against kernel
ticks and queues deferred work only when due. Timeout callbacks execute on the
DSR worker under the network scheduling guard. An exhausted DSR queue retries
on the next tick. Loopback queues trigger deferred processing immediately.
Timeout deadlines are refreshed after network operations and before blocking
socket waits, including TCP timer creation and DHCP setup. The periodic process service retains POSIX timer
polling and graphics refresh pacing.

Reference: [lwIP timeout interface](https://www.nongnu.org/lwip/2_1_x/timeouts_8h.html).

---

The x86 and AMD64 release and test builds complete. The AMD64 test kernel
passes 258 kernel tests with two CPUs and 8 GiB of configured RAM. The link-count,
pipe, descriptor-passing, memory-reporting, socket-wait, and open-inode lifetime
regressions pass.
The guest reports `LowTotal: 8321920 kB` and `HighTotal: 0 kB`. Cache sizing retains
the configured adaptive policy. Loopback ICMP validation receives all three
transmitted packets. Authenticated root login reaches the RH9 GNOME desktop
with the AMD64 release kernel, two CPUs, and 8 GiB of configured RAM.

## 2026-10-05 - Kernel Dispatch and Lookup

### Subsystem interfaces

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

### Syscall namespaces

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

### Operation dispatch

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

### Lookup and allocation indexes

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

### Source validation and runtime checks

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

---

## 2026-10-05 - IPC Buffering and Performance

Anonymous pipes allocate a 64 KiB circular buffer. Named FIFOs and PTY
directions allocate 4 KiB circular buffers. Unix stream sockets allocate
256 KiB receive rings per endpoint, including socket pairs and accepted
connections. Unix datagram sockets allocate 4 KiB receive rings. Socket rings
reserve one byte to distinguish full and empty states.

The circular-buffer library copies each available span with at most two
contiguous memory transfers. The second transfer handles wrapping at the
buffer boundary. Buffer indices and occupancy are updated once per span under
the buffer lock. The index calculation supports capacities that are not powers
of two. Whole-record writes publish all record bytes before notification.

Pipe and FIFO data notifications make eligible readers, writers, and poll
waiters runnable. Condition notification yields when a waiter is awakened and
the current interrupt and scheduling state permit a context switch.
Blocking operations wait when the buffer state prevents progress. Nonblocking
operations retain partial-transfer and `EAGAIN` behavior. Closing the final
writer exposes EOF after queued bytes have been consumed; closing the final
reader produces `EPIPE` on subsequent writes.

Socket waits without a deadline register an indefinite interruptible wait
without sampling the hardware clock. Finite waits sample the clock before and
after waiter registration and retain deadline expiration and signal handling.

The stream receive-ring allocation requires 256 KiB of kernel heap memory per
endpoint. `SO_RCVBUF` reports the allocated ring size. Receive and send buffer
size options do not resize the rings. Unix datagram record limits and ancillary
descriptor queues use the configured datagram ring and descriptor-queue sizes.

### Measured Performance

The following measurements use the x86 release kernel on an AMD Ryzen 9 7950X
host with KVM, one virtual CPU, 2 GiB of guest RAM, and QEMU's host CPU model.
Separate guest processes transfer 64 KiB blocks for at least two seconds per
sample. Each transport has three samples. Latency measurements use 64-byte
messages, 1,000 warmup round trips, and 1,000 measured round trips. Throughput
uses decimal bytes per second.

| Transport | Median throughput | Median round-trip latency |
| --- | ---: | ---: |
| Anonymous pipe | 5.485 GB/s | 7.562 µs |
| Unix stream socket pair | 7.944 GB/s | 13.555 µs |
| Named Unix stream socket | 7.955 GB/s | 13.620 µs |

These measurements describe this virtual-machine configuration. Native Linux
measurements with different CPU placement, parallelism, buffer sizes, or
virtualization settings require separate comparisons.

The named-socket measurement harness permits `ENOENT` when unlinking the
listening path after closing the listener. Unix listener release removes the
kernel namespace entry. This handling affects connection cleanup outside the
measured transfer interval.

### Validation

Run the following commands inside a MOS guest with Python 3:

```sh
python3 test/ipc_buffers.py
python3 test/unix_peercred.py
python3 test/unix_send_credentials.py
```

`ipc_buffers.py` validates complete payloads through pipes, named FIFOs, socket
pairs, and named sockets. Fragment sizes include one-byte, irregular, 64 KiB,
and larger-than-ring writes. It checks ring wrapping, EOF, nonblocking empty
and full buffers, writable readiness, broken peers, datagram truncation,
finite socket receive and send timeouts, and scatter/gather transfers with
`SCM_RIGHTS` across stream-ring boundaries. A 60-second alarm bounds execution.

The shell tests `test/posix_pipe.sh`, `test/posix_nonblock_ipc.sh`, and
`test/posix_socket.sh` provide additional pipe, FIFO, PTY, and socket checks.
The nonblocking test's embedded C probe can run independently of its shell
prerequisite checks. `test/posix_socket_wait.sh` requires the test kernel's
`/proc/tests/.result` output path; a standalone harness can direct that output
to a writable regular file.

---

## 2026-10-05 - Physical Memory Reporting

The physical page allocator registers usable Multiboot memory regions within
the architecture address limit. The i386 backend uses non-PAE paging and
manages physical addresses below 4 GiB. The AMD64 backend manages physical
addresses below the configured 128 GiB RAM mirror limit.

On i386, low physical memory comprises allocator pages below the 768 MiB
kernel direct-map boundary. High physical memory comprises pages at or above
that boundary. On AMD64, all managed RAM has a permanent kernel mapping and is
reported as low memory; high memory totals are zero. These classes are
independent of process virtual address limits.

`/proc/mos` reports physical totals, used memory, and free memory in bytes.
The `Raw` column contains exact unsigned 64-bit byte counts. The `Value`
column contains abbreviated binary units. Page counts are widened to 64 bits
before conversion to bytes. The formatter accepts `%h` for `unsigned`,
`%lh` for `unsigned long`, and `%llh` for `unsigned long long` values.

`/proc/meminfo` reports `LowTotal` and `HighTotal` in KiB. Each total multiplied
by 1024 equals the corresponding raw byte total in `/proc/mos`. Their sum
is the allocator-managed RAM total. Firmware reservations, physical address
holes, and boot-reserved allocator metadata are excluded; the reported total
can therefore be below the configured guest RAM size. Used and free values
are sampled separately for each proc file opening.

The proc totals can be inspected with:

```sh
cat /proc/mos
cat /proc/meminfo
```

The guest regression check is available in a test kernel:

```sh
sh /proc/tests/posix_mem_reporting
```

---

## 2026-10-05 - RH9 Display Interfaces

### Kernel entry points

RH9 XFree86 4.3.0 uses the IA-32 syscall namespace on both kernel
architectures. `arch/abi/i386/calls.def` dispatches `ioctl` (54), `mmap` (90),
`ioperm` (101), `iopl` (110), `vm86old` (113), `vm86` (166), and `mmap2`
(192). IA-32 pointers are zero-extended from 32-bit argument registers.
`mmap2` converts page offsets to byte offsets using 64-bit arithmetic.

Console ioctls use complete command identity. Keyboard and display commands
occupy type `0x4b`; virtual-terminal commands occupy type `0x56`.
`KDSETMODE` (`0x4b3a`) accepts the mode as an immediate argument.
`KDGETMODE` (`0x4b3b`) writes an integer through its argument pointer.
Entering graphics mode records the owning process and suspends text rendering
on the selected virtual terminal.

`/dev/mem` mappings expose physical memory directly, including framebuffer
and firmware addresses. The IA-32 VM86 service emulates selected VBE BIOS
operations. Its VMware modes use 32-bit pixels and a row length of four bytes
per pixel.

### Framebuffer storage

XFree86 searches platform-specific module directories before the generic
module directory. RH9's `modules/linux/libint10.a` invokes the IA-32 VM86
service and receives the MOS VBE mode list. Complete directory enumeration,
including the final entry, is required for that module search.

The VMware SVGA console driver configures 32-bit framebuffer storage. Pixel
depth and framebuffer storage width are distinct: depth 24 can use either
three or four bytes per pixel. XFree86's VESA driver prefers 24-bit storage
when the BIOS advertises it. Its framebuffer writes must use the same pixel
storage width and row length as the active display device.

XFree86 selects four-byte framebuffer pixels with the following invocation
from an RH9 text console:

```sh
startx -- -fbbpp 32 -fp /usr/X11R6/lib/X11/fonts/misc
```

The equivalent configuration setting is `DefaultFbBpp 32` in the `Screen`
section of `/etc/X11/XF86Config`, alongside `DefaultDepth 24`.

The server log `/var/log/XFree86.0.log` reports framebuffer bpp, virtual
dimensions, pitch in pixels, BIOS identification, and selected VBE modes.
Those values describe the server's selected format; the display device's
active format must agree. Syscall namespace validation is available through
`python3 test/syscall_tables.py`. That audit checks declarations and service
coverage; display correctness requires guest execution.

---

## 2026-10-05 - Kernel stack storage

MOS x86 task stacks share one 4 KiB allocation with the task descriptor.
The usable call stack is therefore smaller than one page. Bootstrap uses
a separate stack. User processes have an independent stack limit of
`USER_STACK_PAGES * PAGE_SIZE` (16 MiB with the current configuration).

### Buffer ownership

| Path | Storage and lifetime |
| --- | --- |
| `strstr()` | KMP prefix table on the heap; allocation failure uses a constant-stack search without workspace. The fallback can take quadratic time. |
| `kvformat()` | 64-byte automatic output buffer, flushed incrementally to its callback. |
| `printk()` | Static 512-byte record payload plus length, protected by `printk_record_lock`. Lock order is record lock, active TTY lock, then log lock when emitting records. |
| `log_read()` / `sys_syslog()` | Shared static 2144-byte formatting buffer. Formatting and copying both complete under `log_lock`; readers release the lock before waiting. |
| ATA IDENTIFY | Heap sector buffer, released before partition discovery and diagnostic output. |
| ext4 directory checks and mkdir | Heap `ext4_dir` descriptors, including their embedded 255-byte filenames. |
| Kernel command-line parsing | Static 256-byte copy used only during single-threaded bootstrap. |
| Kernel syslog tests | Heap scratch buffers owned by test wrappers, including when a helper returns through a fatal assertion. |
| Sysfs attributes and directory listings | Heap buffers owned by the open file; PCI resource tables belong to heap-allocated device data. |

Small bounded automatic objects remain, including 32-byte PCI names,
109-byte UNIX pathname buffers, and eight-element descriptor arrays.
Their sizes alone do not establish a bound for the complete call chain.

### Source coverage and configuration

Stack-storage inspection covers `src/`, `arch/`, kernel C tests, `tools/`,
and `third_party/`, including aggregate types containing arrays and
inline/header declarations. Shell tests containing C programs execute
those programs in userspace; their source is embedded in the kernel as
static strings by `tools/gen_ktest_scripts.sh`.

`third_party/Makefile` selects lwIP's `LWIPNOAPPSFILES`.
`src/lwipopts.h` sets `PPP_SUPPORT=0`, `LWIP_NETCONN=0`, `LWIP_SOCKET=0`,
and `LWIP_IPV6=0`. Larger automatic arrays in PPP authentication/logging,
application examples, and standalone third-party tests do not execute on
the current kernel paths. These configurations must be included when
assessing the impact of enabling additional library features.

Large array members are not automatically stack allocations. PCI caches,
TTY state, task FPU storage, ext4 mount state, and GPU tables are backed by
static or heap storage. The partition-table structure declared inside
`read_partition_table()` is instantiated through a heap pointer.

### Validation limits

Source inspection and syntax checks do not measure compiler-generated
stack frames, register spills, inlining, interrupt nesting, or cumulative
call-chain depth. Recursive traversal in VFS, PCI bus enumeration, and
extended partition handling requires separate call-depth analysis.
No runtime stack-peak bound is established by this inventory.

---

## 2026-10-05 - Kernel logging

`printk()` sends messages to the console and to a shared kernel record buffer.
The buffer retains 64 records, each containing up to 511 message bytes, a
sequence number, a timestamp in microseconds since boot, and a syslog priority.
`printk()` uses `kern.info` priority and splits output at newlines and record
boundaries. Recording starts at kernel initialization level 1. Diagnostic
`klog()` output is sent to the serial port.

The following interfaces use the same buffer and producer wakeups:

| Interface | Read behavior |
| --- | --- |
| `/dev/kmsg` | Independent cursor per open; one complete record per read |
| `/proc/kmsg` | Shared consuming byte stream |
| `klogctl(2)` / syslog action 2 | Same consuming cursor as `/proc/kmsg` |
| syslog actions 3 and 4 | Snapshot of retained records since the clear marker |

`/dev/kmsg` is character device 1:11 with mode 0600. Records use
`priority,sequence,timestamp,-;message\n` format. Control bytes, non-ASCII bytes,
and backslashes are encoded as `\xhh`. A short read buffer returns `EINVAL`
without consuming the record. Buffer overrun returns `EPIPE` and positions the
reader at the oldest retained record. Zero-offset seeks support `SEEK_SET`
(oldest record), `SEEK_END` (next record), and `SEEK_DATA` (clear marker).
Writes accept up to 511 bytes and an optional `<priority>` prefix. The default
priority is `user.info`; kernel-facility priorities are assigned the user
facility. Larger writes return `EMSGSIZE`.

`/proc/kmsg` has mode 0400 and returns `<priority>message\n` records. Reads may
split records and consume multiple records. An overrun skips overwritten data.
Both files support blocking reads, nonblocking `EAGAIN`, and poll/select
readiness. Blocking reads with no available data are interruptible by signals.

Syslog action 3 does not consume records. Actions 4 and 5 advance the snapshot
clear marker without removing records from device readers or the shared
consuming stream. Action 9 returns unread stream bytes; action 10 returns the
configured text-buffer capacity. Console-control actions 6–8 return success
without changing console output.

The `SyslogTest` kernel tests cover independent readers, record escaping,
short buffers, nonblocking reads, overrun recovery, snapshot clearing, and the
shared proc/syscall cursor. These tests require exclusive access to the legacy
consuming stream and no concurrent log producers.

---

## 2026-10-05 - AMD64 Kernel and Process ABI

### Implementation status

The x64 build is enabled. The backend includes a Multiboot long-mode entry,
four-level paging, interrupt and syscall entry, software context switching,
per-CPU GDT/TSS state, local APIC startup, and TLB shootdowns. The x86 debug and x64 debug/release builds complete. RH9 dynamic-loader and
interactive shell startup are verified on x64 with one, two, and four CPUs.
Native AMD64 execution and selected i386 regressions are verified as recorded
below.

The x86 backend remains available independently. Architecture-dependent
implementation resides under `arch/`, with common VM and process policy under
`src/`.

| Ownership | Implementation |
| --- | --- |
| IA-32 page tables, COW walking, and SMP startup | `arch/x86/mm`, `arch/x86/ps` |
| AMD64 paging, interrupt frames, task state, and AP startup | `arch/x64/mm`, `arch/x64/int`, `arch/x64/ps`, `arch/x64/boot` |
| i386 syscall numbering shared by both kernels | `arch/abi/i386` |
| AMD64 syscall numbering and compatibility conversions | `arch/x64/syscall` |
| ELF verification, image replacement, VM policy, scheduler | `src/elf`, `src/mm`, `src/ps` |

### Building and launching

Build the debug kernel with:

```sh
make ARCH=x64 BUILD=debug
```

`kernel.dbg` retains ELF64 symbols. `kernel.boot` is a flat Multiboot image with
explicit physical load, BSS, and entry addresses. The flat image enters the
32-bit bootstrap before switching to long mode; it avoids relying on a
Multiboot loader accepting ELF64.

For a normal launch with two CPUs and 8 GiB of guest RAM:

```sh
./run.sh arch=x64 smp=2 ram=8192 verbose=2
```

The runner builds the release kernel and selects a long-mode-capable QEMU CPU.
The `ram=N` argument selects memory in MiB; its default is 8192.
The `debug` argument selects the debug image and pauses execution for a debugger
on TCP port 8888. One-CPU and two-CPU launches should both be checked before
increasing the CPU count. The existing runner performs guest disk preparation.

The native probe is built independently of kernel tests:

```sh
make ARCH=x64 BUILD=debug user64-smoke
```

Install `out/x64/debug/user/x64-smoke` into the guest filesystem and execute it
from the RH9 shell. It uses direct AMD64 syscalls without a libc dependency.
Its checks cover mappings above 4 GiB, FS and GS bases, signal delivery and
return, concurrent child processes, scheduling, COW isolation, and wait. A
successful run prints `AMD64 ABI smoke: PASS`; a failure exits with a numbered
check code. The existing guest exec, process, shared-futex, signal, descriptor,
and socket tests provide i386 regression coverage.

### CPU mode and ABI separation

| Property | i386 compatibility process | Native AMD64 process |
| --- | --- | --- |
| ELF identity | ELFCLASS32, EM_386 | ELFCLASS64, EM_X86_64 |
| Word and pointer size | 4 bytes | 8 bytes |
| Code selector | `0x23`, L=0, D=1 | `0x2b`, L=1, D=0 |
| Address limit | `0xc0000000` | `0x0000800000000000` |
| Syscall arguments | EBX, ECX, EDX, ESI, EDI, EBP | RDI, RSI, RDX, R10, R8, R9 |
| Syscall instruction | `int 0x80` | `syscall` |
| TLS | GDT slots 6–8 or LDT selectors | FS/GS bases through `arch_prctl` |
| Signal context | Explicit i386 wire fields | AMD64 register context, red zone, and restorer |

The process ABI survives fork, clone, and vfork. Exec selects the new ABI from
ELF class and machine and clears old TLS state. Interpreter identity must match
the main executable. ELF32 headers and program headers are normalized into an
internal description; file layouts are never reinterpreted by pointer size.
Argv, envp, auxiliary vectors, AT_PHENT, and platform strings use the selected
process ABI. ELFCLASS32 with EM_X86_64 is rejected; the x32 ABI is not supported.

Compatibility conversions cover exec pointer vectors, iovec arrays, socket
message headers and ancillary data, signal actions and alternate stacks,
stat objects, ptrace words, resource limits, and robust-list pointers. Native
conversions cover stat, time, rusage, limits, file locks, and signals. Missing
native syscall entries return ENOSYS rather than invoking a handler with an
incompatible layout. The native namespace exposes the corresponding services declared in the i386
namespace, including native socket and shared-memory entry points. Shared
compatibility stubs retain their service behavior. See [kernel dispatch and
lookup](bugfix_journal.md#2026-10-05---kernel-dispatch-and-lookup) for namespace coverage and ABI constraints.

Native `newfstatat` preserves `AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH`, and the
other lookup flags accepted by the shared stat service. Native `select` and
`pselect6` convert 64-bit time fields and descriptor-set words before entering
the shared readiness wait service. Finite waits use millisecond resolution;
timeout seconds must fit a nonnegative signed 32-bit value. Interrupted
`pselect6` waits retain the temporary mask through signal delivery and restore
the original mask on signal return. Native `clock_nanosleep` accepts relative
and absolute waits for `CLOCK_REALTIME` and `CLOCK_MONOTONIC`.

Native `ppoll` converts 64-bit timeouts to the shared poll wait service and
validates the full-width signal-mask size. Its interrupted waits use the same
signal-mask restoration contract as `pselect6`.

`test/x64_console_abi.py` validates pathname flags, descriptor readiness across
word boundaries, timeouts, temporary signal masks, pipe wakeups, and libc sleep
calls. The test requires AMD64 userspace and Python 3.

Native socket entry points preserve full-width address and buffer pointers.
Connection, binding, listening, address queries, socket options, datagram I/O,
shutdown, and socket pairs use the shared socket services. `accept4` and socket
pair creation apply nonblocking and close-on-exec flags to returned descriptors.
Native `sendto` uses the message service for both Unix-domain and IPv4 sockets.

Native futex timeouts use the shared 64-bit userspace timeout reader.
Native `ftruncate` passes its 64-bit length to the shared file truncation
service. DRI3 shared-fence files support sizing before shared memory mapping.
`test/x64_console_abi.py` verifies relative `FUTEX_WAIT` and absolute
`FUTEX_WAIT_BITSET` timeout completion, as well as shared-fence file truncation
and visibility between mappings of an unlinked file.

Upstream glibc 2.3.2 startup sources confirm four-byte i386 stack slots and
eight-byte AMD64 slots. The AMD64 sigaction wrapper installs SA_RESTORER, and
its clone wrapper uses the native syscall register convention. Distribution
NPTL patches remain a separate validation dependency for RH9 thread tests.

### Paging and memory ownership

The bootstrap uses supervisor-only 2 MiB identity and high-half aliases.
Runtime setup removes the bootstrap low alias and retains the shared kernel
mapping. Individual direct-map permissions split large pages into 4 KiB leaves.
Each process owns its lower-half tables; upper-half ancestors are shared.

| Region | Virtual range |
| --- | --- |
| Kernel image and low RAM direct map | `0xffffffffc0000000`–`0xfffffffff0000000` |
| Managed RAM permanent mirror | `0xffff800000000000`–`0xffff802000000000` |
| Temporary physical aliases | `0xfffffffff0000000`–`0xfffffffff8000000` |
| Shared supervisor device window | `0xc0000000`–`0x100000000` |
| Native mmap allocation base | `0x100000000` |

The device window preserves the existing driver API that dereferences PCI BAR
addresses directly. Physical device ranges use `MOS_DEVICE_IO_BEGIN` and
`MOS_DEVICE_IO_END`; the VirtIO GPU probe validates its BAR mappings against
these limits. The high kernel virtual I/O range has separate bounds.
Native mappings, image segments, and remote process-memory
access cannot replace that window. Compatibility processes end below it.

Virtual-terminal shell tasks preserve the full kernel stack address and use
`KERNEL_TASK_BYTES` for the privilege-entry stack limit.

Page-table entries use a physical-address mask distinct from virtual alignment
masks. Native VM permissions encode NX separately from physical frame bits;
i386 compatibility retains the existing executable-data semantics. Long-mode
startup requires NX support and enables EFER.NXE on every CPU.

Private managed pages use COW at fork. Shared and direct physical mappings keep
their ownership semantics. TLB shootdowns precede release or reuse of unmapped
frames and table pages. Failed table allocation rolls back the new walk; failed
fork copying releases the unqueued child. Empty private tables are reclaimed.
Allocator-managed high RAM uses the permanent supervisor-only, NX mirror.
COW copies and page-cache reads therefore avoid temporary aliases. Firmware
and device resources retain their existing mapping paths; temporary aliases
remain shared and reference counted.

Newly present user mappings do not need translation invalidation. Replaced
mappings, permission changes, and unmaps synchronously invalidate CPUs using
the affected address space. User shootdowns reload CR3 and retain global
kernel translations. Shared kernel mapping changes still invalidate global
translations on every online CPU. A dedicated request lock serializes
publication and acknowledgments; user invalidation checks active CR3 without
PCID.

Kernel stacks occupy four pages with matching buddy alignment. Heap headers,
free-list pointers, file-descriptor tables, FPU-buffer alignment, and variadic
argument handling respect kernel pointer width.

### SMP and interrupt entry

ACPI MADT discovery supplies processor APIC IDs. INIT/SIPI starts each AP through
a real-mode trampoline at physical `0x7000`. APs load the bootstrap CR3, enter
long mode, activate a dedicated bootstrap stack, and install per-CPU descriptor
state before joining the scheduler. The trampoline mapping is removed after
startup acknowledgement. The supported processor count is 1–32.

Kernel and userspace execution can proceed concurrently on multiple CPUs.
The scheduler holds `ps_lock` through stack handoff to prevent concurrent
execution or reclamation of an active task. APIC tick IPIs drive remote
scheduling. TLB IPIs and lock-wait polling acknowledge invalidation generations
even with IF clear. Subsystem locks protect networking, page-table allocation,
shared mappings, PIT sampling, and serial transmission.

Kernel GS points to the current CPU. Native userspace GS and FS bases are
preserved separately; compatibility selectors are restored after swapping out
the kernel GS base. Interrupt entry tests the active GS base rather than only
CS, covering NMIs in the SYSCALL stack-transition window. NMI, double fault,
and machine check have independent per-CPU IST stacks. Critical interrupt
handlers avoid the scheduler and subsystem locks. Native return uses IRETQ;
SYSRET address and flag constraints are therefore not assumed.

Authenticated RH9 root login has also been validated with 8 GiB and two KVM
CPUs, reaching the GNOME desktop and a working graphical terminal. This check
required repair of a damaged GConf saved-state inode in the guest filesystem.
The ATA block callbacks and lwext4 write cleanup now propagate I/O failures
instead of hanging on invalid disk blocks or reporting zero-byte success.
See the [8 GiB desktop screenshot](screenshot/x64_8g_desktop.png) and
[fix journal](bugfix_journal.md) for the diagnosis and repair record.

Filesystem and block caches grow on demand within an adaptive combined budget
of 25% of managed RAM, capped at 4 GiB. On an 8 GiB guest this is approximately
1.49 GiB for filesystem pages and 508 MiB for block data. Memory pressure
reduces the budgets and triggers reclaim; the growth policy targets up to
256 MiB of free headroom. Current cache budgets are visible in `/proc/mos`.

### Current limits and validation requirements

The AMD64 allocator and permanent RAM mirror support physical addresses below
128 GiB; runtime validation includes an 8 GiB guest. The i386 kernel remains
limited to physical addresses below 4 GiB. Kernel stacks, heap, page tables,
and legacy DMA buffers remain in low RAM. File mappings retain 64-bit byte
offsets through VMA splitting, page faults, shared caches, and filesystem page
callbacks. Native mmap accepts aligned nonnegative 64-bit offsets. Several
other file and syscall services, including tmpfs file size, retain legacy
limits; native wrappers reject unsupported representations where implemented.
Signal support remains limited to the existing signals 1–32. Native ptrace,
complete IPC/network/time syscall coverage, XSAVE/AVX state, and modern libc
compatibility are not established. VM86 is unavailable in long mode and returns
ENOSYS; applications requiring that interface remain on the x86 backend.

Runtime validation used QEMU 10.2.1 with `qemu64`, 4 GiB RAM, VMware SVGA,
IDE storage, and snapshot-backed RH9 storage. No persistent guest changes were
required. The following checks completed successfully:

| Configuration | Result |
| --- | --- |
| x64 debug, 2 CPUs | RH9 Bash prompt; both CPUs online |
| x64 release, 1 CPU | RH9 Bash prompt; visible console; native probe PASS |
| x64 release, 4 CPUs | RH9 Bash prompt; four CPUs online; native probe PASS |
| i386 `posix_pthread.sh`, 2 CPUs | Thread, synchronization, and TLS tests; exit 0 |
| i386 `posix_futex_shared.sh`, 2 CPUs | Shared mappings and distinct-address futex tests; exit 0 |
| i386 `posix_fd_pass.sh`, 4 CPUs | Ancillary descriptor tests; exit 0 |
| x86 debug, 1 CPU | Successful build and RH9 Bash prompt |
| x86 and x64 debug test builds | Successful compile and link |
| x64 debug test kernel, 2 CPUs | 17 MM, 20 mmap, 12 physical allocation, and 9 heap allocation tests passed; native probe PASS |

Boot debugging corrected the fixed-width Multiboot information layout,
interrupt enabling after IDT initialization, explicit VMware PCI memory
mapping, and the i386 resource-limit buffer width. These faults respectively
prevented valid boot data access, timer progress, console initialization, and
safe Bash startup. Test support uses full-width addresses and explicitly
aligned registration records. Large-page flag lookup preserves permissions
when shared kernel mappings split into individual pages.

The full guest regression suite, additional processor counts, KVM, and modern
64-bit libc distributions remain outside this validation. Native programs must
use the AMD64 SYSCALL interface; native-mode INT 0x80 interoperability is not
established. Ancillary conversion closes received descriptors omitted by a
truncated i386 control buffer. Compatibility message-array syscalls preserve
the common backend's existing timeout limitations.

### Process-launch performance

A KVM release build with two CPUs, 4 GiB RAM, `-cpu host`, RH9 Bash, and verbose
logging disabled was timed with:

```sh
time for ((i=0;i<1000;i++)); do /bin/true; done
```

| Implementation | Elapsed time |
| --- | --- |
| Initial x64 backend | 4.307 s |
| Invalidation skipped for newly present pages | 2.802 s |
| Permanent managed-RAM mirror added | 0.796 s |
| Address-space-targeted user shootdowns added | 0.170, 0.180, 0.182 s |

The final median is 0.180 s, approximately 24 times faster than the baseline.
The workload includes fork, ELF loading, dynamic-loader startup, and exit.
Temporary high-memory mapping scans and global cross-CPU invalidations were
responsible for the measured regression. The benchmark records elapsed guest
time; debugger capture stops execution only when Bash prints the completed
timing result. Host scheduling and processor count can affect absolute times.

Post-change KVM validation completed with four CPUs: 18 MM, 20 mmap, 12
physical-allocation, and 9 heap-allocation tests passed. The expanded native
probe passed with two and four CPUs; i386 pthread and shared-futex regressions
passed with two CPUs. Both kernel architectures build successfully.

The native probe additionally starts a process sharing its VM that repeatedly
writes a cached page. After the parent removes write permission, the writer
must terminate with SIGSEGV. This checks permission invalidation for a shared
address space alongside the existing COW, TLS, and signal checks. Debugger
inspection confirmed that the parent and writer were running on CPUs 0 and 3
with the same page-table root during the permission change.

### References

- [Intel software developer manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [Linux AMD64 syscall table](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl)
- [Linux x86 signal contexts](https://github.com/torvalds/linux/blob/master/arch/x86/include/uapi/asm/sigcontext.h)
- [QEMU Multiboot loader](https://github.com/qemu/qemu/blob/master/hw/i386/multiboot.c)
- [GNU glibc source releases](https://ftp.gnu.org/gnu/glibc/)

---

## 2026-10-04 - RPC statd interface ioctl compatibility

### Reported fault

The AMD64 kernel reported the following fault during RH9 service startup:

```text
[609][1203]: segfault: /sbin/rpc.statd: error code 2, address 74706000, eip c0253256
```

In the corresponding AMD64 release symbol file, instruction
`0xffffffffc0253256` is the `rep stosl` instruction in `memset`. Page-fault
error code 2 denotes a supervisor write to a non-present page. The original
diagnostic used `%x` for both addresses, losing their upper 32 bits. The fault
diagnostic now uses `%lx`, and the formatter preserves the full unsigned-long
value for hexadecimal output instead of converting it through a 32-bit int.

### Source analysis

The nfs-utils 1.0.1 implementation of `rpc.statd` calls
`pmap_unset(SM_PROG, SM_VERS)` in its startup loop. In glibc 2.3.2,
`pmap_unset()` calls `__get_myaddress()`, which obtains interfaces through
`ioctl(fd, SIOCGIFCONF, &ifc)` before requesting interface flags.

Sources:

- [nfs-utils 1.0.1 source archive](https://downloads.sourceforge.net/project/nfs/nfs-utils/1.0.1/nfs-utils-1.0.1.tar.gz),
  `utils/statd/statd.c`.
- [glibc 2.3.2 source archive](https://ftp.gnu.org/gnu/glibc/glibc-2.3.2.tar.gz),
  `sunrpc/pmap_clnt.c` and `sysdeps/gnu/net/if.h`.

The exact installed RH9 package revisions have not been established. These
upstream versions establish the RPC interface-enumeration path and ABI layout;
distribution-specific patches remain outside this verification.

| Layout | i386 | AMD64 |
| --- | --- | --- |
| `ifconf` size | 8 bytes | 16 bytes |
| Buffer pointer offset | 4 bytes | 8 bytes |
| Buffer pointer width | 4 bytes | 8 bytes |
| `ifreq` array stride | 32 bytes | 40 bytes |

Previously, the i386 syscall table dispatched ioctl directly to the common
handler. The AMD64 socket implementation interpreted an i386 `ifconf` as a
native structure, loading unrelated bytes beyond the object as a buffer
pointer. Interface enumeration then passed that pointer to `memset`. This is
a confirmed ABI defect on the service's startup path and is consistent with
the reported fault. A subsequent RH9 startup reports no further
`rpc.statd` fault after the interface-ioctl correction.

### Correction

The i386 syscall table now selects `compat_ioctl`. On the x86 kernel this
aliases the existing handler. On the AMD64 kernel the implementation resides
in `arch/x64/syscall/impl/compat_ioctl.c`.

For `SIOCGIFCONF`, the compatibility handler reads an explicit eight-byte
i386 structure, queries the available interface count, and allocates a native
buffer bounded by that count and the caller's capacity. It copies each
returned name and socket address into a 32-byte i386 entry and returns an
i386 byte count. The original buffer pointer remains unchanged. Other ioctl
requests retain their existing dispatch.

The native `ifreq` includes the pointer-width `ifmap` union member, restoring
the AMD64 40-byte stride while preserving the x86 32-byte stride. Native
`ifconf` buffer pointers retain all 64 bits. Interface enumeration also
supports a NULL buffer for the Linux byte-count query and returns only whole
entries when capacity is limited.

### Validation

Source syntax checks pass for both kernel architectures. The native probe
passes its syntax check, the guest test passes shell syntax validation, and
the patch passes whitespace validation. The rebuilt kernel completes the
RPC startup path without the reported fault. The regression scripts, native
probe, and full-width formatter test remain pending guest execution.

`test/posix_ifconf.sh` checks enumeration, interface flags, returned byte
counts, unchanged buffer pointers, surrounding sentinel bytes, NULL-buffer
queries, short buffers, single-entry buffers, and glibc's RPC local-address
helper. For an i386 process it places `0x74706000` immediately after `ifconf`
to reproduce the incorrect pointer load deterministically.

The `kprint.sprintf_lx_kernel_address` kernel test checks that hexadecimal
fault addresses preserve all bits on AMD64 and retain the x86 representation.

The native `tools/user/x64_smoke.c` probe checks 40-byte interface entries,
buffer bounds, size queries, and short-buffer behavior using a destination
above 4 GiB. Its interface checks use exit codes 21 through 31.

Runtime verification consists of normal RH9 startup with `rpc.statd`, the
interface regression script on both kernels, and the AMD64 probe on x64.

## 2026-10-04 - XFree86 SHMAT result pointer sign extension on AMD64

### Symptom

RH9 X server startup repeatedly faults while storing a shared-memory
attachment result:

```text
[1201][1483]: segfault: /usr/X11R6/bin/X: error code 2, address bffff000, eip c0235eb5
```

The corresponding AMD64 release symbol file resolves the instruction to
`mos_shmat()` at the `*user_raddr = mapped` assignment. Disassembly shows
`movslq` extending the saved third IPC argument before the four-byte store.

### Root cause

glibc 2.3.2 implements i386 `shmat()` using the IPC multiplexer. Its third
argument is the address of a local stack variable used to receive the mapped
address. The kernel declares that argument as `int`. Casting it directly to
AMD64 `uintptr_t` sign-extends a pointer with bit 31 set, converting
`0xbffffxxx` into `0xffffffffbffffxxx`. The store then targets an unmapped
supervisor address rather than the i386 stack. The original 32-bit fault
diagnostic hides the extension.

XFree86 4.3.0 uses `shmat()` in its Linux int10 initialization and shared-memory
extensions. The source establishes these call sites; the particular X startup
caller has not been established by a runtime backtrace.

Sources:

- [XFree86 4.3.0 source](https://ftp.xfree86.org/pub/XFree86/4.3.0/source/),
  `programs/Xserver/hw/xfree86/os-support/linux/int10/linux.c` and
  `programs/Xserver/Xext/xf86bigfont.c`.
- [glibc 2.3.2 source](https://ftp.gnu.org/gnu/glibc/glibc-2.3.2.tar.gz),
  `sysdeps/unix/sysv/linux/shmat.c`.

### Correction and validation

The IPC SHMAT branch converts the argument through `uint32_t` before widening
to `uintptr_t`. This preserves the complete i386 address as an unsigned
32-bit value. The result store remains four bytes wide. Native AMD64 shared
memory syscalls remain outside the existing i386 IPC multiplexer.

`test/posix_sysv_shm.sh` exercises a raw IPC SHMAT with a result pointer whose
bit 31 is set, verifies the result-store bounds, then exercises glibc SHMAT,
shared backing, IPC_RMID while attached, and detach. Runtime verification of
the correction remains pending. A subsequent X startup reaches VESA
initialization and reports an unsupported vm86 call rather than this store
fault.

## 2026-10-04 - Missing VBE emulation in the AMD64 compatibility kernel

### Symptom and root cause

After the SHMAT correction, XFree86 reaches its VESA initialization and exits
with `unknown type(0xffffffff)=0xff`, followed by `no screens found`.

XFree86's `linux_vm86.c` calls the vm86old syscall and switches on the low byte
of its return value. The syscall wrapper converts an error to `-1`, whose low
byte is `0xff`; the default case prints exactly this diagnostic. The AMD64
backend had unconditional `-ENOSYS` stubs for both vm86 syscalls. The x86
backend already implements selected VBE calls through software emulation, so
the required behavior does not depend on hardware virtual-8086 mode being
available in long mode.

### Correction

The existing VBE emulator moves from `arch/x86/syscall/impl/syscall_vm86.c`
to `arch/abi/i386/syscall_vm86.c`, where both kernels build it. The AMD64
stubs are removed. Flags, bitmap fields, CPU type, and interrupt vectors use
explicit 32-bit wire values; static assertions require an 84-byte register
block and 160-byte i386 vm86 structure. Segment-address conversion widens
the calculated unsigned address through `uintptr_t`.

The supported BIOS calls, VMware port programming, and fallback behavior
retain the existing x86 implementation. This provides the i386 VBE syscall
contract on AMD64; it does not implement arbitrary real-mode instruction
execution or a native AMD64 vm86 syscall.

### Validation

Both architecture source trees pass syntax validation after sharing the
emulator. `test/posix_vm86_vbe.sh` exercises both entry points, controller
and mode information, output buffer bounds, returned register state, and
save-state size queries. Guest execution and X startup verification remain
pending.

## 2026-10-04 - AMD64 desktop framebuffer faults and large-memory support

### Failure and diagnosis

RH9 XFree86 progressed through VBE, keyboard, mouse, and font initialization,
then repeatedly faulted at its first framebuffer store. Debugger inspection
identified a write to virtual address `0x40156000`, with a leaf PTE of
`0x000ffffffd000017`. The intended physical framebuffer address was
`0xfd000000`. A signed 32-bit VMA offset had been widened to an unsigned
64-bit physical address after sign extension, setting reserved physical
address bits. The fault handler treated the existing mapping as resolved,
so the same instruction faulted repeatedly without making progress.

Normal SysV startup exposed a second independent failure. The filesystem
checker reached byte offset `0x80002000`, where `_llseek` returned
`-2147475456`. The kernel had stored the correct 64-bit position but returned
its truncated low word instead of the required zero success status. glibc's
`llseek` implementation uses a nonzero status as its return value; e2fsprogs
therefore could not read the next inode block and entered maintenance mode.

### Corrections

VMA offsets, split-region offsets, fault offsets, shared-page cache keys,
and filesystem page callbacks now retain 64 bits. Framebuffer offsets no
longer undergo signed 32-bit extension, and native mmap no longer rejects
valid offsets solely because they exceed `0x7fffffff`. Automatic mappings
larger than 4 GiB are allowed to select a native address above the shared
supervisor device window; explicitly fixed mappings still cannot overlap
that window. The i386 mmap2 page offset is widened before multiplication.

The x64 RAM mirror and physical-memory discovery ceiling now extend to
128 GiB of physical address space. Page-cache and SysV shared-memory backing
addresses use the architecture's physical-address type. User and cache
allocation prefer available RAM above 4 GiB, retaining low RAM for kernel
objects and legacy DMA buffers. Kernel and DMA allocations retain their
existing low-address constraints. The i386 kernel remains non-PAE.

Raw physical aliases carry a software PTE flag so unmapping or destroying
an alias cannot decrement an allocator reference owned by another mapping.
RAM aliases use the permanent mirror's write-back cache type; MMIO mappings
retain cache-disable semantics. The i386 sysinfo result uses page-sized units
and allocator RAM totals, preventing byte-count overflow and excluding
reserved physical holes. `_llseek` returns zero on success and reports the
complete position only through its result pointer.

The native socket creation syscall now dispatches its three integer arguments
to the existing socket backend. This enables the native ABI probe's interface
ioctl checks, which previously stopped at an unsupported socket syscall.
The runner accepts `ram=N` in MiB, with an unchanged 4096 MiB default.
Hexadecimal diagnostics consume full-width `long long` arguments, preserving
physical addresses above 4 GiB and subsequent arguments on both architectures.

### Regression coverage

`test/posix_llseek.sh` checks raw syscall status, full result values, result
buffer bounds, libc seek behavior, negative seeks, and agreement between
sysinfo and `/proc/meminfo`. `test/posix_vm86_vbe.sh` faults two framebuffer
pages above 2 GiB without changing display contents.

The mmap suite checks a writable raw RAM alias, full physical address
translation, cache attributes, reference preservation after unmapping, and
distinct file-cache entries separated by 4 GiB. The AMD64 ABI probe checks
physical mapping offsets above 4 GiB and a shared anonymous region exceeding
4 GiB, including independent endpoint contents, protection changes, and
partial unmapping.

### Sources

- [XFree86 4.3.0 source](https://www.xfree86.org/pub/XFree86/4.3.0/source/),
  VESA framebuffer mapping and Linux int10 initialization.
- [glibc 2.3.2 source](https://ftp.gnu.org/gnu/glibc/glibc-2.3.2.tar.gz),
  `sysdeps/unix/sysv/linux/llseek.c`.
- [e2fsprogs 1.32 source](https://sourceforge.net/projects/e2fsprogs/files/e2fsprogs/1.32/),
  `lib/ext2fs/llseek.c` and `lib/ext2fs/unix_io.c`.

### Validation

Both architecture release kernels and test kernels build successfully.
Shell syntax and whitespace validation pass. An 8 GiB AMD64 guest reaches
the graphical RH9 login screen after completing filesystem checks. The native
ABI probe passes, including physical offsets above 4 GiB and a shared mapping
larger than 4 GiB. The mmap, mm, and physical allocator suites pass all 22,
18, and 12 tests respectively. All 59 formatter tests pass, including
full-width physical address diagnostics. Large seeks, sysinfo accounting, VESA
framebuffer mappings, System V shared memory, pthreads, and shared futex
compatibility checks also pass. The raw RAM alias test confirms allocation
and translation above 4 GiB. Full physical capacity beyond 8 GiB has not
been tested in a guest.

### Authenticated desktop startup and invalid disk blocks

The initial 8 GiB check established arrival at the graphical login screen,
without validating completion of an authenticated session. An authenticated
run subsequently froze while GConf read `/root/.gconfd/saved_state`.

A debugger trace located the blocked CPU in the ATA DMA completion loop.
GConf's file read resolved to sector 14819236616, beyond the partition's
41929587 sectors. The block-device adapter narrowed this sector to 32 bits
before validation and issued an invalid ATA command. The adapter also discarded partition I/O failures and
reported success to the filesystem.

Filesystem checking identified illegal block pointers in inode 44, the
saved-state file, and cleared that inode in a disposable test snapshot.
Normal boots with both 4 GiB and 8 GiB encountered filesystem-check failures
from the same base image. This damage must be distinguished from physical
address truncation in memory mappings.

The block-device callbacks now validate the full 64-bit sector range against
the partition capacity before narrowing it for ATA. Read and write callbacks
return EIO for invalid ranges and incomplete partition transfers. The
[GConf 2.2.0 source](https://download.gnome.org/sources/GConf/2.2/GConf-2.2.0.tar.gz)
confirms that saved-state read errors terminate parsing and are logged;
they do not require an indefinite kernel I/O wait.

The lwext4 write cleanup also replaced a failed transfer's error with the
inode release result. Successful release consequently converted an I/O
failure into a zero-byte successful write. glibc 2.3.2's
`libio/fileops.c:_IO_new_file_write` subtracts successful write counts and
retries remaining bytes, so zero progress caused an endless retry loop.
Cleanup now preserves the transfer error, and the filesystem adapter returns
the corresponding negative errno.

Host regressions execute the actual block callbacks and `ext4_fwrite` body
with injected failures. They cover full-width invalid sector numbers,
partition-end crossing, valid final-sector transfers, failed and incomplete
transfers, and preservation of the original error through inode cleanup.
The write regression fails against the original cleanup behavior and passes
with the fix. Both architecture release and test kernels build successfully.

A persistent repair was performed inside the guest after preserving QEMU
snapshot `mos-before-desktop-repair-20261004` in `rh9.qcow2`. The filesystem
checker cleared the damaged saved-state inode and corrected allocation
bitmaps and counters. The repaired guest was then started normally with
8 GiB, two CPUs, and KVM, without bypassing startup filesystem checks. Root
login reached the complete GNOME desktop. A graphical terminal reported
`x86_64`, over 8 billion bytes of managed RAM, and `DESKTOP_LOGIN_OK`.
The native AMD64 ABI probe also passed from the graphical terminal, including
its file I/O, mappings above 4 GiB, and signal checks. The screenshot is
[8 GiB desktop](screenshot/x64_8g_desktop.png).

## 2026-10-04 - Adaptive filesystem and block cache budgets

### Policy and implementation

Filesystem page caching previously lacked an explicit size budget, while
block caching retained a fixed 64 MiB ceiling. AMD64 now assigns a combined
budget of one quarter of allocator-managed RAM, capped at 4 GiB. Three
quarters of this budget serve filesystem pages and one quarter serves block
lines. An 8 GiB guest with 8521646080 bytes of managed RAM receives budgets
of 1597808640 bytes for filesystem pages and 532602880 bytes for block data.
These are demand-driven ceilings; cache contents are not allocated at boot.

Cache growth also leaves a free-memory target of one eighth of managed RAM,
bounded between 16 and 256 MiB and at most half of managed RAM. As application
allocations consume headroom, the budget is reduced using current free pages
and existing cache pages. Cache misses shed excess LRU entries in batches of
up to 32 pages. Block lines are flushed before normal eviction. At least one
line per cache, and one block line per active partition, remain eligible so
filesystem I/O can make progress at small budgets. Allocator-driven user
reclaim can recover both file and block pages when file pages remain pinned.

Buddy list insertions and removals maintain a free-page counter; managed RAM
is counted from usable memory-map ranges, excluding holes. Budget calculation
therefore has constant cost instead of rescanning physical page metadata on
every cache miss. Current budgets appear in `/proc/mos` alongside usage and
peak counters. File invalidation searches for the affected inode
and removes only that inode's pages, avoiding a complete cache scan on every
write. A regression verifies invalidation at offsets 0 and 4 GiB while
preserving neighboring inodes. The i386 block cache retains its 64 MiB ceiling
because block lines hold aliases in the limited kmap window. On machines
without high memory, cache allocation directly uses available low-memory
pages. Previously the high-memory miss triggered reclamation on each cache
allocation, discarding earlier entries even when free low RAM was available.

### Validation

Both architecture release and test builds pass. In an 8 GiB, two-CPU KVM
guest, all 16 physical allocator tests and 23 mmap tests pass. New policy
checks cover large RAM, pressure, the aggregate ceiling, and agreement with
allocator accounting. `test/posix_cache_growth.sh` writes a temporary 256 MiB
file, synchronizes it, reads it twice, and verifies every byte. The probe
passes with block-cache usage exceeding the previous 64 MiB ceiling.
Subsequent memory statistics report 277377024 bytes of block buffers and
276422656 bytes of filesystem page cache.

A 512 MiB i386 guest also passes all 16 physical allocator tests and 23 mmap
tests, including cache retention and inode invalidation. The final AMD64
release kernel reaches the complete GNOME desktop after root login with
8 GiB and two CPUs. Validation guests use temporary disk snapshots.

## 2026-09-29 — Socket waits during GNOME login

- The login trace showed Metacity abandoning its ICE connection after a read
  returned `ETIMEDOUT`. GNOME then waited out its client-registration deadline
  before starting the panel and Nautilus; one recorded poll lasted 83.55 seconds.
- MOS imposed a 30-second deadline on blocking socket operations even when no
  application timeout was set. Socket `FIONBIO` also returned success without
  changing the file flags, so programs using it could unexpectedly block.
- Implemented `FIONBIO`, removed the implicit deadline, applied explicit socket
  timeouts to UNIX reads/writes/messages and accept, and preserved signal
  interruption and partial I/O. TCP connect now respects nonblocking mode and
  `SO_SNDTIMEO`. Datagram reads check nonblocking mode; UNIX sendmsg receives
  the syscall flags, including the file's `O_NONBLOCK`, rather than msg_flags.
- Added `posix_socket_wait.sh` for ioctl toggling through dup, nonblocking reads,
  explicit receive/send/accept timeouts, signals, and successful receipt after
  a 32-second delay. Runtime validation was blocked by a disk-image lock and a host socket
  bind failure. No GNOME before/after timing result is claimed.

### Follow-up: TCP data lost before userspace accept

The next login trace disproved the timeout-only explanation. SettingsDaemon
(PID 1610) wrote its 26-byte FAM request at tick 4474; the newly spawned FAM
(PID 1615) did not call accept until tick 4497. FAM then waited in select, while
SettingsDaemon waited for its reply and gnome-session waited for SettingsDaemon.
The trace still showed no progress at tick 31878. Removing the implicit timeout
had removed the retry escape from this underlying stalled exchange.

`tcp_on_accept()` previously queued only the raw lwIP PCB. MOS installed its
receive callbacks and allocated a receive buffer later in `do_accept()`. In
that interval lwIP's default `tcp_recv_null()` acknowledged and freed payloads.
Thus a client sending before userspace accept could lose its first request
permanently; faster accept scheduling could avoid the bug.

Allocate and attach the child socket in `tcp_on_accept()` instead, queue that
socket, and preserve its buffered data, EOF and errors when accepting it.
Balance lwIP delayed-backlog accounting on the child PCB, and detach callbacks
before freeing queued sockets on listener close or fd-allocation failure.
Listener cleanup uses listener callbacks rather than stream-only PCB fields.
Release compilation and whitespace review completed; no runtime tests were run
for this follow-up. Login improvement remains to be
verified with the rebuilt kernel.

## 2026-09-29 - TCP listener exposed as a connected socket (`fam` crash)

The reported `fam` fault at `eip c026ed3a` resolves in the supplied release
build to `tcp_output()` reading `seg->tcphdr`. `do_listen()` replaced the full
TCP PCB with lwIP's smaller `tcp_pcb_listen`, but marked the MOS socket
`SS_CONNECTED`. This let writes and write-readiness checks read beyond the
listener allocation, including the send buffer and unsent queue. It also made
`getpeername()` incorrectly succeed on a listener. Depending on adjacent pool
contents, sending could block or dereference an invalid segment pointer.

Keep the listener `SS_UNCONNECTED`; accept readiness continues to use its
pending-connection queue. Preserve the listener rejection in `connect()` so it
cannot change the socket back to a connection state. Check state before reading
the send buffer in the vectored send loop, including after a wait.

Socket options also need to respect the smaller allocation. Keep `TCP_NODELAY`
in the MOS socket, apply it only to full PCBs, and inherit it at accept. Its old
write overlapped the listener's accept callback. `SO_SNDBUF` and `TCP_MAXSEG`
queries now use defaults when no full TCP PCB is available.

Validation: a baseline QEMU guest blocked on a listener write; GDB confirmed
that the socket was `SS_CONNECTED` while pointing into the listen-PCB pool.
The patched guest passes `posix_tcp_listener` (invalid data operations, peer
lookup, polling, options, accept, and bidirectional traffic) and the existing
`posix_socket` suite. The original `fam` session itself was not reproduced.
The broader `posix_socket_wait` suite stalled in `unix_read()` / `sock_wait()`
while testing signal interruption; it is not counted as a passing check.

## 2026-09-29 - `posix_socket_wait` blocked forever waiting for SIGALRM

The listener investigation above exposed a separate alarm-delivery bug.
`check_alarm()` ran only from `do_signal()` on return to userspace. A task
blocked in `sock_wait()` with no receive timeout never returned to userspace,
so its `alarm(1)` never became pending and could not interrupt the read.
The periodic service polled POSIX timers, but not the per-task alarm fields
shared by `alarm()` and `setitimer(ITIMER_REAL)`.

The service now checks those alarms under `ps_lock` and queues SIGALRM through
`ps_queue_signal_unsafe()`, waking a waiting recipient when the signal is
unmasked. The return-to-userspace check uses the same locked helper so the
same expiration cannot fire twice. Masked alarms become pending without
waking the task; canceled timers remain inactive.

The socket-wait regression now also covers periodic ITIMER_REAL interruption
of read/recv/recvmsg, cancellation, and a masked one-shot alarm becoming
pending during poll and being delivered after unmasking.

## 2026-09-29 - Restore alarm behavior after performance and ping regressions

Restore the alarm check from before ee20379: an unarmed task returns without
reading the clock or locking, and an armed task checks expiration on return to
userspace. Remove the periodic alarm poll. Withdraw the subsequent active-alarm
queue, IRQ0 wakeup/scheduling changes, and clock-function changes following
a ping regression.

This intentionally restores the known limitation that an indefinitely blocked
read cannot discover its own alarm expiration. A replacement asynchronous
wakeup design is not included. The cause of the reported ping regression has
not been established by runtime diagnosis. Ping recovery was not validated by runtime tests or benchmarks.

## 2026-09-29 - Bound socket waits by the current task's alarm

Restoring the original user-return alarm check also restored the hang in
`posix_socket_wait`: `alarm(1)` cannot interrupt an indefinite socket read
when expiration is checked only after that read returns.

Keep the original alarm check and also call it before and after a socket wait.
Bound that wait by the earlier of the socket timeout and the current task's
alarm, using the existing timed-wait queue. Leave the socket deadline intact:
an unmasked, non-ignored alarm yields EINTR rather than a synthetic EAGAIN.
Masked or ignored alarms may wake the internal wait but do not abort the I/O;
the socket operation retries with its original deadline. Interval alarms
retain the original rearming behavior.

This change is limited to socket waits. It does not add asynchronous alarm
handling to other indefinite waits, nor change clock functions, IRQ handlers,
the scheduler, or the periodic service. Wakeup latency remains subject to the
existing timed-wait scheduler. Runtime tests and benchmarks were not run; socket behavior and ping
remain unverified for this revision.

## 2026-09-29 - Separate alarm expiration from interruptible I/O waits

Replace the socket-specific alarm deadline workaround with independent alarm
expiration and a common interruptible-wait contract. `ps_alarm.c` keeps only
armed alarms in an expiry-ordered RB-tree, protected by `ps_lock`. Set/query
operations use the existing monotonic clock. IRQ0 exit checks due entries
using the existing tick counter, queues SIGALRM, and requests scheduling if a
recipient becomes runnable. No PIT/clock implementation or wall-time setter is
changed, and interrupt context does not sample PIT ports. Cancellation, fork,
exit and reaping maintain the alarm-node lifetime. Periodic alarms advance from
the previous deadline, skipping missed periods rather than drifting.

User return only delivers pending signals. It does not read the clock or scan
alarms. Empty alarm queues return immediately from the IRQ hook; other ticks
inspect the earliest expiry rather than scanning every process. Resolution is
still HZ=100 (10 ms), with handler execution subject to interrupt masking and
scheduling. This is not a high-resolution-timer implementation.

Tasks explicitly distinguish interruptible waits from internal lock waits.
The common wait entry checks actionable pending signals and publishes the
waiting state under `ps_lock`, so a signal cannot be lost between checking and
sleeping. Signal enqueue only wakes an interruptible recipient (or handles
stopped-task continuation/termination). Ignored signals do not cause EINTR;
masked signals remain pending. Sigtimedwait has an explicit awaited-signal
mask so blocked signals can wake its synchronous wait. Signal selection skips
ignored signals before choosing the handler for an interrupted syscall.

Socket receive/send paths, pipe and TTY/PTY cyclic-buffer waits, poll/select,
log reads, pause/sigsuspend, nanosleep, futex and user file-lock waits use the
common rules. AC97 DMA waits also honor interruption and stop DMA on exit.
Kernel mutex/semaphore waits remain uninterruptible. Existing partial-I/O
results and per-socket timeouts remain independent of alarm expiration.
The audit also fixes FIFO O_NONBLOCK handling, zero-length pipe reads/writes,
and PTY master read EINTR mapping.

Scope: this unifies alarm expiration and interruptible waiting within MOS's
existing per-task alarm and signal model. It does not implement Linux's full
thread-group signal routing, SA_RESTART syscall-restart ABI, or a replacement
for POSIX timer polling. Pre-existing protocol/driver limitations such as
recvmmsg's timeout argument and asynchronous nonblocking audio are outside
this change.

Validation: release kernel and test-kernel compilation plus static review.
Runtime correctness, ping behavior, and throughput remain unverified
for this revision.

## 2026-04-30 - Consolehelper-launched GUI tools failed through broken shebang argv

Several GNOME control-panel applications launched through `consolehelper`
failed immediately even though their desktop entries, authentication wrapper,
and X startup-notification traffic were otherwise working. The visible example
was Hardware Browser, but the same bug affected other wrapped GUI tools that
ultimately execute installed shell scripts.

### Symptom

Launching Hardware Browser reached the `consolehelper` path, read
`/etc/security/console.apps/hwbrowser`, authenticated through the usermode
wrapper, and then failed from `/bin/sh`:

```text
open(/root/hwbrowser, 8000, 0) = -2
stat64(/usr/sbin/hwbrowser, ...) = -2
stat64(/usr/bin/hwbrowser, ...) = 0
open(/usr/bin/hwbrowser, 8000, 0) = 3
read(3, "\x7fELF...", 80) = 80
write(2, "hwbrowser: /usr/bin/hwbrowser: cannot execute binary file\n", 58)
exit_group(126)
```

The shell was searching for `hwbrowser` by name and found the
`/usr/bin/hwbrowser` consolehelper ELF wrapper, not the intended
`/usr/share/hwbrowser/hwbrowser` shell script.

### Root cause

Linux shebang execution rewrites the argument vector as:

```text
interpreter, optional-interpreter-argument, script-path, original-argv[1]...
```

MOS instead preserved the caller's `argv[0]` after the interpreter:

```text
interpreter, optional-interpreter-argument, original-argv[0], original-argv[1]...
```

The RH9 hwbrowser package installs `/usr/share/hwbrowser/hwbrowser` as a
`#!/bin/sh` script. The usermode `consolehelper` source shows that it reads
`PROGRAM=/usr/share/hwbrowser/hwbrowser` from the console.apps file and
executes that path while setting the wrapped service name as the script's
displayed command name. With MOS's argv layout, `/bin/sh` received
`argv[1] == "hwbrowser"` instead of the script path. It therefore searched
`PATH`, opened `/usr/bin/hwbrowser`, saw an ELF binary while already running as
the shell, and emitted `cannot execute binary file`.

### Fix

`execve` shebang handling now inserts the resolved script pathname after the
interpreter and optional interpreter argument, then appends the original
arguments starting at `argv[1]`. This matches Linux behavior and allows shells,
Python, and other interpreters to open the actual script regardless of the
caller's chosen `argv[0]`.

The `posix_exec` test now includes a compiled regression helper that calls
`execve()` on a script with a deliberately misleading `argv[0]`. The script
verifies that `$0` is the script pathname and exits with a distinct status.
`make` and shell syntax validation pass.

### Key lesson

Shebang execution is an ABI boundary, not an implementation detail. Preserving
the caller's `argv[0]` is correct for native ELF execution, but scripts require
the kernel-supplied script path so the interpreter can open the intended file.
Desktop wrappers and privilege helpers often depend on this distinction because
their service name and executable path are deliberately different.

---

## 2026-04-30 - X server segfault after System Monitor activity

Opening GNOME System Monitor could trigger an X server crash shortly afterward.
The crash terminated `/usr/X11R6/bin/X`, after which GNOME clients reported
that their display connection had been lost.

### Symptom

The syscall log showed `gnome-system-monitor` actively polling `/proc/stat`,
`/proc/meminfo`, and mount information, while the process that actually
received `SIGSEGV` was the X server:

```text
segfault: error code 4, address 4003a000, eip 4003af50
exit(/usr/X11R6/bin/X, status=b)
```

The fault was a user-mode non-present-page fault at `0x4003a000`, with
execution also inside the same unmapped page.

### Root cause

The X server repeatedly resized an anonymous executable loader allocation with
`mremap(40017000, ..., MREMAP_MAYMOVE)`. MOS handled in-place `mremap` growth
by adding a new adjacent anonymous VMA with `MAP_FIXED`, instead of extending
the original VMA descriptor. This left one logical resized mapping represented
as multiple VM regions.

The implementation also checked only the first page after the old mapping end
when deciding whether in-place growth was possible. A later VMA inside the
requested growth range could therefore be missed.

After a later shrink, `0x4003a000` was legitimately unmapped, but X still
jumped through an address in that old expanded range. The fragmented VMA state
violated the single-resized-mapping behavior expected by Linux userspace and
made stale loader/allocator state observable as an X server crash.

### Fix

`mremap` growth now extends the existing VM descriptor in place when the full
target range is free. The new `vm_extend_map()` helper validates that the
mapping starts and ends at the expected addresses, checks the whole extension
range for overlap, then updates the VM key and region end together.

If in-place extension is not possible, `mremap` falls back to the existing
move-and-copy path when `MREMAP_MAYMOVE` is present, or returns `ENOMEM` when
movement is not allowed.

Focused mmap tests now cover successful in-place growth and rejection of growth
across a later overlapping VMA. `make` and `make test` both build successfully.

### Key lesson

`mremap` is not just an allocation convenience. Userspace loaders and allocators
depend on Linux's resized-mapping semantics, including coherent VMA identity
and full-range collision checks. Representing one resized mapping as adjacent
independent VMAs can corrupt higher-level assumptions even when individual page
fault handling appears correct.

---

## 2026-04-30 - Loopback `ping` could hang or print timeout errors after very fast replies

Loopback ICMP echo could stop making progress after a reply with a
sub-millisecond round-trip time. After the initial blocking issue was exposed,
the same path could also print intermittent `ping: recvmsg: Connection timed
out` messages between successful replies.

### Symptom

`ping 127.0.0.1` usually printed replies normally, but could hang immediately
after an unusually small RTT such as `0.034 ms`. Enabling verbose kernel
logging made the hang disappear. Once socket receive timeouts were honored, the
hang changed into visible `recvmsg` timeout errors after similarly fast replies.

### Root cause

Two timing-sensitive socket behaviors were involved:

- `sock_wait()` added the current task to the socket waiter list before the
  scheduler marked the task as sleeping. A loopback reply could arrive in that
  small window. The wakeup saw the task as still running, skipped the transition
  to ready state, and the task then went to sleep with the wakeup already lost.
- RH9 `iputils` `ping` uses `SO_RCVTIMEO` to let `recvmsg()` wait only until the
  next probe should be sent. MOS accepted `SO_RCVTIMEO` but did not apply it, so
  `ping` could block in `recvmsg()` instead of returning to send the next echo
  request. The first timeout implementation returned `ETIMEDOUT`; Linux returns
  `EAGAIN`/`EWOULDBLOCK` for socket receive timeout expiry, and RH9 `ping`
  treats only that result as a normal timeout.

### Fix

Socket waits now prepare the timed scheduler wait while holding the socket wait
lock, then release the socket lock and schedule. This closes the lost-wakeup
window between waiter registration and sleeping.

INET sockets now store `SO_RCVTIMEO` and `SO_SNDTIMEO`, and receive/send paths
use those values instead of always using the internal socket guard timeout.
Application-requested socket timeout expiry returns `EAGAIN`; the internal
30-second guard still returns `ETIMEDOUT`.

### Key lesson

Loopback networking can expose scheduler races because the reply may be
generated before the sender has fully entered its sleep path. Socket timeout
ABI details also matter: returning the wrong errno can turn a normal user-space
polling strategy into a visible application error.

---

## 2026-04-30 - RH9 `shutdown -h now` stalled across xinetd, killall5, and final halt

The RH9 shutdown path progressed through several separate compatibility
boundaries before reaching the final halt. Each failure looked like a hang in
the shutdown UI, but the syscall log showed a different missing Linux behavior
at each stage.

### 1. `xinetd` stayed alive after `SIGTERM`

- **Caught by:** tracing the shutdown log around `/etc/rc0.d/K50xinetd` and
  comparing it with RH9 `xinetd` 2.3.11 signal handling.
- **Symptom:** the service script printed `Stopping xinetd:` and sent
  `kill(1303, 15)`, but `xinetd` repeatedly woke from `select()` and never
  exited.
- **Key log evidence:**

```text
kill(1303, 15)
sig_deliver(15)
write(4, "\x0f", 1)
_newselect(6, ...)
ioctl(3, 541b, ...) = -25
```

- **Root cause:** `xinetd` writes received signals into an internal signal
  pipe, then calls `ioctl(pipe_fd, FIONREAD, &count)` before draining that
  pipe. MOS pipes did not implement `FIONREAD`, so `xinetd` logged the failure
  and returned without consuming the pending `SIGTERM` byte. The pipe remained
  readable and `select()` woke immediately again.
- **Fix:** in [pipe.c](../src/fs/impl/pipe.c), implement pipe `FIONREAD` using the
  cycle-buffer byte count and expose the ioctl operation on both pipe ends.

### 2. `killall5` left the shutdown shell stopped

- **Caught by:** tracing `/etc/rc0.d/S01halt` after the service shutdown phase
  and reading RH9 SysVinit `killall5` source.
- **Symptom:** the script reached
  `Sending all processes the TERM signal...`, then PID 1 resumed polling
  `/dev/initctl` while the rc0 shell stopped making progress.
- **Key log evidence:**

```text
execve(/sbin/killall5)
kill(-1, 19)
...
kill(-1, 18)
```

- **Root cause:** `killall5` intentionally broadcasts `SIGSTOP`, scans
  `/proc`, sends the requested signal to processes outside its own session,
  then broadcasts `SIGCONT`. MOS stopped tasks on `SIGSTOP`, but default
  `SIGCONT` only behaved as an ignored signal and did not resume tasks in
  `ps_stopped`.
- **Fix:** in [ps_signal.c](../src/ps/impl/ps_signal.c), make `SIGCONT` clear
  pending stop signals and wake stopped tasks. Stop signals now clear pending
  `SIGCONT`, and `SIGKILL` wakes stopped tasks so they can terminate.

### 3. `killall5 -9` killed PID 1

- **Caught by:** the next shutdown attempt progressed through the TERM/KILL
  phase, then printed the kernel-side shutdown message before `/sbin/halt`
  issued the terminal reboot syscall.
- **Symptom:** after `Sending all processes the KILL signal...`, init exited
  and MOS entered the kernel shutdown helper early.
- **Key log evidence:**

```text
kill(1, 9)
exit(/sbin/init, status=8900)
```

- **Root cause:** Linux protects global init from ordinary default signal
  actions. RH9 SysVinit relies on that behavior; `killall5 -9` may attempt to
  signal PID 1, but PID 1 must not die unless it deliberately exits. MOS applied
  normal `SIGKILL` semantics to PID 1.
- **Fix:** protect PID 1 from default-disposition signal termination and stop
  actions while still allowing init's installed handlers, such as its
  `SIGTSTP`/`SIGCONT` handlers, to run normally.

### 4. Final `reboot(2)` calls came from the rc0 shell, not PID 1

- **Caught by:** enabling durable reboot syscall logging and tracing the final
  `/sbin/halt` calls.
- **Symptom:** `/sbin/halt` issued `BMAGIC_POWEROFF` and `BMAGIC_HALT`, but
  MOS treated them as another request to init rather than powering off.
- **Key log evidence:**

```text
reboot(magic1=fee1dead, magic2=28121969, cmd=89abcdef) from pid 1478
reboot(magic1=fee1dead, magic2=28121969, cmd=4321fedc) from pid 1478
reboot(magic1=fee1dead, magic2=28121969, cmd=cdef0123) from pid 1478
reboot(magic1=fee1dead, magic2=28121969, cmd=0) from pid 1478
```

- **Root cause:** MOS only performed the hardware halt/poweroff action when
  `reboot(2)` was called by PID 1. RH9's final `/sbin/halt` runs from the rc0
  script process and expects `BMAGIC_POWEROFF` or `BMAGIC_HALT` to stop the
  system directly.
- **Fix:** in [syscall_sys.c](../src/syscall/impl/syscall_sys.c), treat terminal
  `POWER_OFF` and `HALT` reboot commands as direct hardware actions regardless
  of caller PID. Restart remains routed conservatively unless issued by PID 1.

### Key lesson

Orderly SysV shutdown depends on several old Linux process and device
semantics outside the final `reboot(2)` syscall. Signal-pipe ioctls, job-control
resume behavior, PID 1 signal immunity, and forced halt command handling all
need to match Linux closely or the same shutdown command can appear to hang at
different user-visible messages.

---

## 2026-04-29 - GNOME desktop home and trash links lost their icons

The RH9 GNOME desktop showed the `root's Home` and `Trash` desktop entries
without their expected special icons. Real Linux preserves the home and trash
icons because Nautilus can update the `.desktop` files in place during desktop
startup.

### 1. Nautilus truncated special desktop links, then aborted the rewrite

- **Caught by:** tracing the syscall log around `/root/.gnome-desktop` and
  comparing the observed operations with Nautilus 2.2 and GNOME desktop item
  save behavior.
- **Symptom:** both special desktop entries initially contained valid
  `X-Nautilus-Icon` keys, but later became zero-length regular files. After
  that, Nautilus could no longer parse the entries as special links, so the
  desktop fell back to missing or generic icon handling.
- **Key log evidence:**

```text
read(17, "[Desktop Entry]...Type=X-nautilus-home...X-Nautilus-Icon=gnome-fs-home\n", 32768) = 128
open(/root/.gnome-desktop/root's Home, 8201, 0) = 17
ftruncate64(17, 0)
lstat64(/root/.gnome-desktop/root's Home, ...) = 0, -rwx------, ... size=0 ...

read(19, "[Desktop Entry]...Type=X-nautilus-trash...X-Nautilus-Icon=gnome-fs-trash-empty\n", 32768) = 124
open(/root/.gnome-desktop/Trash, 8201, 0) = 19
ftruncate64(19, 0)
```

- **Root cause:** Nautilus refreshes desktop link metadata through
  `gnome_desktop_item_save()`. That function opens the existing file for
  writing, truncates it with `gnome_vfs_truncate_handle(..., 0)`, then writes
  the refreshed desktop-entry keys. MOS ext4 handled `O_TRUNC` during open, but
  ext4 regular files did not provide a file-operation implementation for
  `ftruncate`. As a result, `ftruncate64(fd, 0)` returned an error after the
  open had already emptied the file. GNOME desktop item save then aborted
  before writing the replacement contents.
- **Fix:**
  - in [root.c](../src/fs/impl/root.c), add `ext4_file_ftruncate()` for ext4
    regular files
  - wire it into `ext4_file_fops.ftruncate`
  - use `ext4_ftruncate()` when shrinking and `ext4_fenlarge()` when growing
  - invalidate the shared page cache, update the VFS inode size, clamp the file
    position when needed, and refresh file modification/change timestamps

### Key lesson

`O_TRUNC` support alone is not enough for Linux desktop compatibility. Old
GNOME userspace performs an explicit `ftruncate64()` as part of saving
`.desktop` files, and failure after a successful truncating open can destroy
the original metadata before the replacement contents are written.

---

## 2026-04-29 - RH9 `LABEL=/` fstab root remount failed before `mount(2)`

Real Red Hat 9 systems commonly use an `/etc/fstab` root entry whose source is
`LABEL=/`:

```text
LABEL=/  /  ext3  defaults  1 1
```

MOS could boot the same image only after changing that source to `/`, but that
replacement was not compatible with the real Linux boot path.

### 1. `mount` failed while resolving the label in userspace

- **Caught by:** tracing the boot log around the
  `Remounting root filesystem in read-write mode` step and comparing it with
  the Red Hat 9 util-linux 2.11y `mount` source.
- **Symptom:** `/bin/mount` printed `mount: no such partition found` and
  exited with status `100` during `mount -n -o remount,rw /`. No kernel
  `mount(2)` syscall for the root remount appeared after the failure.
- **Key log evidence:**
  - `/bin/mount` read the root fstab entry as `LABEL=/ ... / ... ext3 ...`
  - it opened `/proc/partitions` and found `hda1`
  - it opened `/dev/hda1` successfully
  - both block-device size ioctls failed:

```text
ioctl(4, 80041272, ...) = -25
ioctl(4, 1260, ...) = -25
```

- **Root cause:** RH9 `mount` resolves `LABEL=/` before calling `mount(2)`.
  Its label probe scans `/proc/partitions`, opens each candidate block device,
  asks block-device size ioctls such as `BLKGETSIZE64` and `BLKGETSIZE`, then
  reads the ext2/ext3 superblock at offset 1024 to compare the volume label.
  MOS exposed `/proc/partitions` and `/dev/hda1`, but the HDD block device did
  not implement those ioctls, so userspace stopped before reading the
  superblock and never issued the remount syscall.
- **Fix:**
  - in [ioctl.h](../src/fs/ioctl.h), define Linux-compatible
    `BLKGETSIZE`, `BLKSSZGET`, and `BLKGETSIZE64`
  - in [hdd.c](../src/dev/impl/hdd.c), implement those ioctls for IDE partition
    block devices using the discovered partition sector count
  - in [loop.c](../src/dev/impl/loop.c), implement the same block-size ioctls for
    configured loop devices so label and filesystem probes see normal block
    device behavior there as well

### Key lesson

Boot compatibility with old Linux userspace can depend on pre-syscall probing
behavior. A failing `mount` command does not necessarily mean the kernel
rejected `mount(2)`; it may mean userspace could not resolve an fstab source
because a block-device compatibility ioctl was missing.

---

## 2026-04-29 - `exit_group()` could leave stale futex waiters on killed thread stacks

The observed crash was a kernel page fault in the release build:
`segfault: error code 0, address eedad000, eip c022f039`. Resolving the
release address placed the fault at `ps_futex_wake_locked+0x39`, while walking
the global futex waiter list.

### 1. Futex wake dereferenced a stale waiter node

- **Caught by:** resolving `c022f039` against the release kernel symbols and
  checking the generated instruction stream, which showed the faulting
  instruction loading the waiter state from the list entry being walked by
  `ps_futex_wake_locked()`.
- **Symptom:** the kernel faulted at `ps_futex_wake_locked()` while reading
  through an address near `0xeedad000`, below the kmap window and not in a
  normal user mapping.
- **Root cause:** `sys_futex(FUTEX_WAIT)` stores a `futex_waiter` object on the
  sleeping task's kernel stack and links that stack object into the global
  `futex_waiters` list. A normal futex wake or timeout removes the node before
  the waiter returns. However, `exit_group()` kills sibling threads by removing
  them from scheduler structures and then reaping their task pages. If one of
  those sibling threads was blocked in `FUTEX_WAIT`, its stack-resident waiter
  node could remain in the global futex list after the task page was freed.
  A later `FUTEX_WAKE` then walked the stale list entry and dereferenced freed
  stack memory.
- **Fix:**
  - in [syscall_futex.c](../src/ps/impl/ps_futex.c), add
    `ps_futex_remove_task_locked()` to unlink any futex waiter owned by a task
    while `ps_lock` is held
  - in [ps_internal.h](../src/ps/impl/ps_internal.h), expose the helper to process
    teardown code
  - in [ps_syscall.c](../src/ps/impl/ps_syscall.c), call the helper from
    `ps_kill_thread_group()` before removing a killed sibling thread from the
    process manager and before its kernel stack can be reaped

### Key lesson

Any wait object stored on a task's kernel stack must be unlinked from global
wait queues before asynchronous thread teardown can free that stack. Scheduler
queue removal and timer disarming are not enough when a blocking syscall also
publishes a stack-local waiter through a subsystem-specific list.

---

## 2026-04-29 - `gnome-terminal` `Ctrl-C` still could not interrupt `ping`

This failure looked at first like the earlier raw-read wakeup bug, but the
remaining symptom was narrower: interactive programs that do not read stdin
still ignored `Ctrl-C` while running behind a PTY. The right approach was to
follow where MOS actually interprets PTY input bytes instead of assuming the
existing `ISIG` checks were enough.

### 1. PTY `Ctrl-C` depended on the foreground program reading stdin

- **Caught by:** tracing the PTY line-discipline path and comparing it with
  the already-working virtual-console path in
  [tty.c](../src/dev/impl/tty.c).
- **Symptom:** pressing `Ctrl-C` in `gnome-terminal` still failed to stop
  `ping` immediately even though `Ctrl-C` worked better for programs blocked in
  terminal reads.
- **Root cause:** MOS handled PTY signal characters too late. On the PTY path,
  `VINTR`, `VQUIT`, and `VSUSP` were recognized only when the slave side later
  executed `read()`. That works for shells or utilities actively reading the
  terminal, but a foreground program like `ping` usually does not read stdin at
  all. The `^C` byte therefore remained queued in the PTY input buffer and no
  `SIGINT` reached the foreground process group.
- **Fix:**
  - in [pts.c](../src/dev/impl/pts.c), move PTY signal-character handling to
    `pts_master_write()` so the line discipline interprets `VINTR`, `VQUIT`,
    and `VSUSP` at input-arrival time, matching real terminal behavior more
    closely
  - consume those signal characters before they enter the PTY slave input
    queue, so a foreground job that never reads stdin still receives the
    signal
  - clear the partial canonical buffer when such a signal character arrives,
    preserving the same line-reset behavior already used by canonical reads
  - remove the duplicate PTY read-side signal-generation path so the same byte
    cannot raise the signal a second time later

### Key lesson

For PTYs, `ISIG` handling must happen when input is delivered to the terminal,
not only when a process eventually reads from it. Otherwise foreground jobs
that do not consume stdin, such as `ping`, can never see `Ctrl-C` even though
the signal-character logic exists in the read path.

## 2026-04-29 - `gnome-terminal` lost immediate `Ctrl-C` and turned Up-arrow into `8`

This bug looked at first like a terminal-emulator or readline problem because
the visible failures happened inside `gnome-terminal` and bash. The useful
approach was to separate the two symptoms and follow each one down to the
kernel boundary instead of patching PTY behavior blindly.

### 1. `Ctrl-C` generated `SIGINT` only after a follow-up key

- **Caught by:** reproducing the shell behavior in `gnome-terminal`, then
  comparing MOS raw TTY/PTTY reads with bash 2.05b's real tty setup in
  `rltty.c`.
- **Symptom:** pressing `Ctrl-C` did not interrupt the foreground program
  immediately. No visible effect occurred until a subsequent keystroke such as
  Backspace or an arrow key arrived.
- **Root cause:** bash/readline puts the terminal into noncanonical mode but
  keeps `ISIG` enabled. MOS raw console and PTY-slave read paths were updated
  to recognize signal characters and send `SIGINT`, `SIGQUIT`, or `SIGTSTP`,
  but they still stayed inside the same blocking read after consuming the
  signal byte. That left the caller asleep until another input byte woke the
  read path.
- **Fix:**
  - in [tty_ldisc.c](../src/dev/impl/tty.c) and
    [tty_ldisc.h](../src/dev/impl/tty_ldisc.h), add a shared helper that recognizes
    `VINTR`, `VQUIT`, and `VSUSP` and signals the foreground process group
  - in [tty.c](../src/dev/impl/tty.c) and [pts.c](../src/dev/impl/pts.c), return
    `-EINTR` immediately when a raw read consumes only a signal character, or
    return the already-collected byte count when the signal arrives after data
  - in canonical handling, stop treating signal characters like ordinary input
    bytes when `ISIG` is active

### 2. Up-arrow intermittently arrived as keypad `8`

- **Caught by:** reading
  `out/x86/release/krn.log`, then correlating the
  raw keyboard reads with the X server's output into the PTY.
- **Symptom:** Up-arrow in `gnome-terminal` sometimes produced the expected
  cursor sequence, but often inserted a literal `8`.
- **Key log evidence:**
  - the good case showed `read(5, "\xe0H", 64) = 2`, followed by X writing
    `"\x1b[A"` to the PTY
  - the bad case showed `read(5, "H", 64) = 1` and `read(5, "\xc8", 64) = 1`,
    followed by pid `1604` writing `"8"` to the PTY
- **Why that matters:** bare `0x48` / `0xc8` is keypad 8 press/release in the
  PC set-1 stream, while extended cursor-Up is `0xe0 0x48` / `0xe0 0xc8`.
  The PTY and shell were behaving correctly; X was faithfully turning the scan
  codes it received into either Up or keypad 8.
- **Root cause:** MOS's keyboard DSR assumed that an `0xe0` prefix and the
  following scan byte were both available immediately. When the controller
  delivered them as separate bytes or separate interrupts, the prefix could be
  dropped and the next byte was emitted alone as `0x48`/`0xc8`. That made X
  interpret the cursor key as keypad 8.
- **Fix:**
  - in [keyboard.c](../src/driver/impl/input/ps2_keyboard.c), make the keyboard DSR drain all
    pending keyboard bytes from the i8042 output buffer instead of processing
    only one logical code per callback
  - preserve an `0xe0`/`0xe1` prefix across bytes and combine it with the next
    scan byte before raw, medium-raw, or translated handling
  - keep ignoring auxiliary-device bytes in the keyboard path so mouse traffic
    stays owned by the PS/2 mouse driver

### 3. Linux console keyboard compatibility also needed Linux-style keysyms

- **Caught by:** checking how XFree86 4.3.0 reads `KDGKBENT` and keyboard mode
  ioctls from a Linux console.
- **Symptom:** even before the prefix-loss bug was isolated, MOS was exporting
  keyboard symbols that did not match Linux `keyboard.h`, which made extended
  key interpretation fragile for X.
- **Root cause:** MOS's `KDGKBENT` encoding and default keymap entries did not
  match the Linux console values that XFree86 expects for keypad, cursor, and
  modifier symbols.
- **Fix:**
  - in [ioctl.h](../src/fs/ioctl.h), switch the keysym type/value encoding
    and exported constants to Linux-compatible values
  - in [keyboard.c](../src/driver/impl/input/ps2_keyboard.c), populate the default keymap entries
    for keypad, cursor, navigation, and right-side modifier keycodes with the
    expected Linux symbols

### Key lesson

When an old X stack turns an arrow key into a printable digit, inspect the raw
keyboard bytes before blaming the PTY or shell. Here the visible `"8"` was a
faithful consequence of MOS occasionally dropping the `0xe0` extended-key
prefix, and the `Ctrl-C` delay was a separate `-EINTR` wakeup bug in raw tty
reads.

## 2026-04-26 - `strace ps aux` over SSH could #GP on `intr_exit: pop %gs`

Initial triage suggested a ptrace or TLS setup bug because the visible crash
was always a user task dying with `#GP(error_code = 0x30)` while traced
through SSH. The decisive clue came from logging both the task's saved TLS
state and the live hardware GDT entries at fault time.

### 1. Traced `/usr/sbin/sshd` and `/bin/ps` faulted in `intr_exit`

- **Caught by:** logging `#GP` state in [int.c](../src/int/impl/int.c) and matching
  the faulting EIP against [assemble.s](../out/x86/debug/assemble.s), which
  showed the crash at `intr_exit: pop %gs`.
- **Symptom:** `strace ps aux` in an SSH session could kill either `sshd` or
  the traced `/bin/ps` with logs like `gs=0x33` or `gs=0x1b`, always faulting
  in the interrupt-exit path rather than in ordinary userspace code.
- **Root cause:** MOS stores Linux TLS slots 6..8 in the shared CPU GDT even
  though their contents are per-task. Some ptrace-stop and nested
  interrupt/syscall-return paths reached `intr_exit` without refreshing those
  live GDT entries for the current task first. The logs showed the mismatch
  directly: the task's `tls_desc[0]` for slot 6 was valid while live `gdt[6]`
  was zero, so `pop %gs` revalidated selector `0x33` against an empty
  descriptor and raised `#GP(error_code = 0x30)`. The later `/bin/ps` traces
  also exposed a second half of the problem: MOS left user `%fs/%gs` live in
  ring 0, making nested interrupt returns depend on user selectors while still
  executing kernel code.
- **Fix:**
  - in `int.S`, switch `%fs` and `%gs` to
    `KERNEL_DATA_SELECTOR` on interrupt and syscall entry, just like `%ds/%es`
  - in [int.c](../src/int/impl/int.c), reload the current task's live TLS GDT slots
    and LDT on every interrupt/syscall exit before any saved user `%gs` is
    restored
  - in [ps.c](../src/ps/impl/ps.c), centralize that live TLS/LDT reload logic in
    `ps_load_task_segments()` so both the scheduler and interrupt-exit path use
    the same code
  - keep the already-needed clone/TLS fixes in [ps_clone.c](../src/ps/impl/ps_clone.c)
    and `ps_tls.c` so new threads inherit only the active
    TLS state and plain `set_thread_area()` does not silently rewrite the
    saved user `%gs`

## 2026-04-26 - GUI Emacs mapped a frame but stalled before usable content

This issue required three separate kernel fixes because the visible symptom
changed as each blocker was removed. The important part was not to patch
around the blank window directly, but to keep explaining why old RH9 Emacs
behaved differently from simpler X clients.

### 1. `gettimeofday()` could move backward and trap Emacs in `SIGALRM`

- **Caught by:** reading `out/x86/release/krn.log`
  during the GUI Emacs investigation, then comparing the timer path with the
  real Emacs 21.2 sources under
  `emacs-21.2-rh9-src`.
- **Symptom:** GUI `emacs` did not appear at all, and the syscall log showed a
  hot loop of `sig_deliver(14)`, `setitimer()`, and `gettimeofday()` during X
  startup.
- **Root cause:** MOS could return a wall-clock sample that jumped forward by
  one PIT tick and then backward on the next call. Emacs 21.2 uses
  `ITIMER_REAL` and `SIGALRM` for atimers during GUI startup, so that
  non-monotonic clock made its timer machinery spin instead of progressing
  through the X event loop.
- **Fix:** in [time.c](../src/driver/impl/timer/pit.c), keep the IRQ-pending compensation for
  the PIT race, but only add a missing tick when the latched counter proves a
  wrap happened inside the same `tickets` epoch. This stopped the false
  one-jiffy jumps caused by noisy PIC IRR reads.

### 2. `ITIMER_REAL` was too fine-grained compared with RH9/Linux 2.4

- **Caught by:** rerunning after the timekeeping fix and observing that Emacs
  now got much farther into X setup but still spent excessive time in
  `SIGALRM`/`setitimer()` churn.
- **Symptom:** a frame began to appear, but Emacs still behaved as if its timer
  retries were far more aggressive than on the target RH9 userspace.
- **Root cause:** old Linux 2.4 i386 `ITIMER_REAL` behavior is effectively
  jiffy-based at `HZ=100`. MOS was honoring very small nonzero timer values too
  precisely. Emacs's deferred 1 ms retry path in its atimer code therefore ran
  much more aggressively on MOS than on the RH9 baseline it was built for.
- **Fix:** in [syscall_proc.c](../src/syscall/impl/syscall_proc.c), round nonzero
  `ITIMER_REAL` values and intervals up to the next jiffy before arming the
  task alarm state.

### 3. X sockets accepted `FASYNC`/`F_SETOWN` but never delivered `SIGIO`

- **Caught by:** reading the updated Emacs-focused log once a frame existed and
  correlating it with the X input path in
  `xterm.c`
  and the `SIGIO` setup in
  `keyboard.c`.
- **Symptom:** Emacs mapped a window and exchanged real X traffic, but it kept
  falling back to sluggish polling / sync-style behavior instead of using its
  intended async X input path. The log showed pid `1632` installing a `SIGIO`
  handler and enabling `F_SETOWN` plus `FASYNC` on the X socket, but no
  `sig_deliver(29)` ever reached that pid.
- **Root cause:** MOS stored async ownership on the socket file descriptor, but
  the socket wakeup path never translated readability/writability changes into
  `SIGIO`. The kernel already did this for mouse input, so Emacs's X socket was
  silently missing an old BSD/Linux compatibility behavior it expected.
- **Fix:**
  - in [socket.h](../src/net/socket.h), add a back-pointer from
    `mos_sock` to the owning open file used for async notification
  - in [sock.c](../src/net/impl/sock.c), preserve that pointer in `sock_to_fd()`
  - in [sock.c](../src/net/impl/sock.c), teach `sock_wakeup()` to send the async
    owner `SIGIO` (or the configured alternate signal) when `FASYNC` is set

### Key lesson

Old GUI applications often fail through compatibility layering rather than one
big crash. Emacs needed:
- a monotonic `gettimeofday()`
- Linux-2.4-like `ITIMER_REAL` granularity
- actual `SIGIO` delivery on X sockets after accepting `F_SETOWN`/`FASYNC`

Once all three matched old Linux expectations closely enough, GUI Emacs worked
normally instead of stopping at a blank or half-alive frame.

## 2026-04-26 - Nautilus text preview crashed after `exit_group()` left sibling threads alive

Initial triage suggested a stack-limit or signal-trampoline bug because the
visible fault address tracked `MAX_USER_STACK`, and the first crash always
landed on a page boundary near the top of user memory. The decisive clue was
following the thread-group lifetime all the way through
`nautilus-text-view` shutdown instead of stopping at the first `SIGSEGV`.

### 1. File preview worker crashed at `befff000` / `befff002`

- **Caught by:** reading
  `out/x86/release/krn.log` during the Nautilus
  file-preview investigation, then mapping the faulting addresses against the
  kernel signal and VDSO code.
- **Symptom:** previewing simple files such as `hello.c` in Nautilus could run
  for a long time, but opening file contents through `nautilus-text-view`
  eventually ended with `segfault: address befff000, eip befff002`.
- **What made it misleading:** `befff000` moved when `MAX_USER_STACK` changed,
  so it initially looked like the kernel was always walking one page past the
  allowed user stack floor.
- **Why that interpretation was wrong:** on MOS, the VDSO helper page is mapped
  just below the mmap/stack boundary in
  [vdso.c](../arch/x86/mm/impl/vdso.c), so `befff000` was actually the VDSO page, and
  `eip = befff002` landed inside `__kernel_vsyscall` rather than inside a true
  stack-growth fault.

### 2. The real failure was thread-group exit, not stack growth

- **Caught by:** correlating the crash with the earlier `exit_group(0)` lines
  from the same `nautilus-text-view` instance in the syscall log.
- **Symptom:** pid `1637` called `exit_group(0)` and exited cleanly, but sibling
  thread `1638` remained alive and later faulted while returning through the
  VDSO helper. An intermediate attempt to kill siblings by queuing `SIGKILL`
  removed the VDSO crash but exposed a new debug-build hang in
  [_task_sched()](../src/ps/impl/sched/ps_switch.c) while the scheduler tried to
  consider threads whose shared process state had already been torn down.
- **Root cause:** `sys_exit_group()` only routed through `sys_exit()`, so the
  caller destroyed process-wide state in `do_exit()` while same-`tgid`
  `CLONE_THREAD` siblings still existed. Because MOS shares the VM and higher
  process state across NPTL threads, any surviving sibling could later run with
  freed address-space or file-table state. The first visible fallout happened
  to be a VDSO fault; after a partial fix, the same bug surfaced as a scheduler
  hang.
- **Fix:**
  - in [syscall_proc.c](../src/syscall/impl/syscall_proc.c), teach
    `sys_exit_group()` to terminate the whole thread group instead of only the
    caller
  - in [ps_syscall.c](../src/ps/impl/ps_syscall.c), add `ps_kill_thread_group()`
    that synchronously removes same-`tgid` sibling threads from scheduler
    structures before the leader continues into `do_exit()`
  - reap those sibling threads' per-thread resources immediately, while
    explicitly preventing the shared VM from being freed multiple times

### Key lesson

For old RH9/NPTL userspace, `exit_group()` compatibility is not optional. If a
thread-group leader exits and frees shared process state before sibling threads
are actually gone, the eventual symptom may look like a random signal-frame
crash, a VDSO fault, or even a scheduler hang. When the log shows a helper
thread surviving past `exit_group(0)`, debug thread-group teardown first.

---

## 2026-04-19 - Nautilus hung in `nautilus_self_check_directory()` and later crashed in GLib allocation

This issue initially presented as a GUI application hang, but the useful clue
was that the hang consistently appeared while Nautilus was walking its
self-check startup path around directory and volume monitoring. The userspace
source and syscall log showed that the real failure was in old RH9/NPTL thread
setup, with a second bug waiting behind it in shared heap bookkeeping.

### 1. `nautilus -c` blocked during `nautilus_self_check_directory()`

- **Caught by:** reading `out/x86/release/krn.log` during investigation of
  `nautilus_self_check_directory`, then correlating that with the RH9 Nautilus
  and `gnome-vfs` sources under
  `nautilus-2.4.2`
  and
  `gnome-vfs-2.4.2`.
- **Symptom:** Nautilus loaded `libfam.so`, went through the file-monitor
  setup path used to watch `/etc/fstab`, issued `clone(...)`, and then the
  parent thread stopped in a futex handshake instead of continuing.
- **Root cause:** MOS installed TLS descriptors but did not consistently keep
  the task's saved user `%gs` selector in sync with that descriptor state.
  On Linux/i386, RH9 glibc/NPTL reads the current `%gs` to build the child
  `clone(CLONE_SETTLS)` descriptor. Because MOS could return to userspace with
  a stale or zero `%gs`, the newborn helper thread never completed NPTL
  startup and the parent blocked forever in the thread-creation futex.
- **Fix:**
  - in `ps_tls.c`, update the saved user `%gs` selector
    whenever `set_thread_area()` or clone TLS installation picks a TLS slot
  - handle the RH9 case where `%gs` is LDT-backed rather than one of the Linux
    GDT TLS slots, and install equivalent child TLS for `CLONE_SETTLS`
  - stop inheriting stale occupied `tls_desc[]` state into newly created
    threads
  - clear stale TLS/LDT descriptor state across `execve()`

### 2. After the TLS fix, Nautilus moved forward and then crashed with `GLib-ERROR **: gmem.c:173: failed to allocate 32774 bytes`

- **Caught by:** rerunning after the TLS/thread fix and reading the next
  failure in `out/x86/release/krn.log`.
- **Symptom:** Nautilus no longer hung in helper-thread startup. It progressed
  into later desktop initialization, but a helper thread eventually aborted in
  GLib allocation and the main client then reported follow-on GUI protocol
  fallout such as an unexpected Xlib async reply.
- **Root cause:** MOS treated `start_brk/brk` as per-task state inside
  `user_enviroment` even for `CLONE_VM` threads. That meant multiple threads
  in one shared address space could grow or shrink the heap mapping while still
  observing different cached program-break values. Old GLib allocation paths
  then reasoned about heap state using stale `brk` bookkeeping even though the
  mappings themselves were shared correctly.
- **Fix:**
  - introduce a refcounted shared heap-state object in
    [ps.h](../src/ps/ps.h)
  - make plain `fork()` copy heap state, while `CLONE_VM` and `vfork()`
    share it
  - make `execve()` detach from any shared heap state before resetting the new
    image's `start_brk/brk`
  - update [syscall_proc.c](../src/syscall/impl/syscall_proc.c) and `/proc` heap
    reporting paths to use the shared heap-state object

### Key lesson

For old RH9 desktop software, "threads share an address space" is not just
about sharing page tables. The kernel also has to share the higher-level
address-space bookkeeping that userspace expects to be process-wide, such as
the active TLS selector and the current program break. If those metadata stay
per-task while mappings are shared, old NPTL and GLib code will fail in ways
that look like random userspace hangs or allocation bugs.

## 2026-04-17 - SSH large output stalls until keypress (`tcp_on_sent` missing)

**Symptom**: Running `ls -alh /usr/lib` over SSH would stall partway through
the output. Pressing any key resumed it briefly, then it stalled again. Running
the same command inside the guest directly, or redirecting to a file over SSH,
worked fine.

**Root cause**: `tcp_setup_callbacks` registered `tcp_recv` and `tcp_err` but
never called `tcp_sent`. When sshd filled the lwIP TCP send buffer
(`tcp_sndbuf == 0`), `sock_tcp_stream_write` called `sock_wait` to block until
space freed. Send buffer space is freed when the remote peer ACKs data —
but that path goes through lwIP's internal ACK processing, which invokes the
`tcp_sent` callback. With no callback registered, `sock_wakeup` was never
called. The writer stayed blocked indefinitely. The only escape was a received
data packet (e.g. an SSH window-adjust or keystroke from the client), which
fired `tcp_on_recv` → `sock_wakeup` as a side effect.

**Fix**: Added `tcp_on_sent` callback (calls `sock_wakeup`) and registered it
via `tcp_sent(pcb, tcp_on_sent)` in `tcp_setup_callbacks` (`src/net/sock_cb.c`).

**Why only SSH and not file redirect**: File redirect sends no terminal output
over the TCP connection, so the TCP send buffer never fills. Inside the guest
there is no TCP socket at all.

---

## 2026-04-17 - `gnome-terminal` PTY poll wakeups could go stale

**Symptom**: Large output such as `ls -alh /usr/lib` could stall in
`gnome-terminal`, even though the same workload worked on a tty, in `screen`,
and over SSH.

**Root cause**: The PTY transport uses `cyb_notify_poll()` to wake tasks
sleeping in `poll()`. That path called [ps_put_to_ready_queue()](../src/ps/impl/alg/ps_alg_rr.c)
unconditionally on the remembered poll task. In a fast poll-driven consumer
like old VTE, multiple PTY wakeups can arrive while the main-loop task is
already runnable or back in userspace rather than still blocked in `poll()`.
That stale wake relabeled the task as `ps_ready` again even though it was no
longer asleep, making the scheduler state timing-sensitive in exactly the way
that verbose kernel logging could mask.

**Fix**:
- in [ps_alg_rr.c](../src/ps/impl/alg/ps_alg_rr.c), make the public
  `ps_put_to_ready_queue()` wake helper transition tasks only when they are
  still in `ps_waiting`
- leave `ps_put_to_ready_queue_unsafe()` unchanged for internal scheduler paths
  that intentionally enqueue newly created or timer-expired tasks
- keep PTY/socket/lock wakeups on the public helper so stale poll notifications
  stop perturbing runnable tasks

---

## 2026-04-14 - PTY controlling `/dev/tty` lookup broke `man`, and `strncpy()` still overflowed

This entry addresses two user-visible regressions in SSH-backed sessions:
`man ping` consistently failed once it invoked `less`, and the SSH client still
occasionally reported a bogus packet length even after the TCP tail-drop fix.
The first issue was a controlling-terminal resolution bug on PTYs; the second
was a stale libc helper overflow that could scribble on adjacent state.

### 1. `man ping` failed in SSH sessions with `Error executing formatting or display command`

- **Caught by:** reading `out/krn.log` around the `man`/`less`
  pipeline after reproducing `man ping` from an SSH-backed shell
- **Symptom:** `man` successfully formatted the page and `less` started, but
  the pager then failed while reopening `/dev/tty`, after which `man` printed
  `Error executing formatting or display command`.
- **Root cause:** MOS only resolved `/dev/tty` through the virtual-console
  table in [tty.c](../src/dev/impl/tty.c). That works for local VTs, but an SSH
  shell runs on a PTY slave. When `less` reopened `/dev/tty` from that PTY
  session, the kernel could not map the calling process group back to the PTY
  controlling terminal, so it fell through to the wrong device path and the
  pager lost its real tty.
- **Fix:**
  - in [tty.c](../src/dev/impl/tty.c), teach `/dev/tty` lookup to fall back from
    virtual consoles to PTY-backed controlling terminals
  - in [pty.c](../src/dev/impl/pty.c) and [ptmx.c](../src/dev/impl/ptmx.c), add helpers
    that reopen the PTY slave corresponding to the caller's controlling
    process group
  - in [pts_internal.h](../src/dev/impl/pts_internal.h), expose the shared helper
    declarations needed by that lookup path
  - extend [dev_pts.sh](../test/dev_pts.sh) with a regression that creates a
    PTY session, makes it controlling via `TIOCSCTTY`, and verifies a child can
    reopen `/dev/tty`

### 2. An old `strncpy()` bug could still corrupt nearby state

- **Caught by:** reviewing low-level helpers while chasing the remaining
  intermittent SSH corruption symptom
- **Symptom:** the failure mode was sporadic and looked like memory corruption:
  the SSH client would sometimes decode nonsense packet framing even though the
  earlier TCP receive truncation bug had already been fixed.
- **Root cause:** [kstring.c](../src/lib/impl/kstring.c) implemented `strncpy()`
  incorrectly. When the source length was at least `len`, the function copied
  `len` bytes and then still wrote a terminating NUL at `dst[len]`, one byte
  past the destination buffer. That is not POSIX `strncpy()` behavior and could
  overwrite adjacent state in exactly the kind of hard-to-reproduce way that
  shows up as protocol garbage later.
- **Fix:** in [kstring.c](../src/lib/impl/kstring.c), make `strncpy()` follow the
  real contract: copy at most `len` bytes, pad with NULs only inside that
  range, and never append a byte past the caller-provided buffer.

## 2026-04-14 - AF_UNIX stream `SCM_RIGHTS` coalescing stalled `gnome-terminal` for 30 seconds

This issue first looked like a slow GNOME Terminal startup problem in
userspace, but the log/source correlation pointed to a kernel protocol bug.
The helper was not slow at opening PTYs; it was getting stuck because MOS
merged two adjacent fd-passing records into one stream receive.

### 1. `gnome-terminal` paused for about 30 seconds before opening

- **Caught by:** reading `out/krn.log` around the
  `gnome-pty-helper` exchange and matching it against the old VTE helper source
  in `vte-0.11.11`
- **Symptom:** the helper successfully opened `/dev/pts/0` and updated
  `utmp`/`wtmp`, but GNOME Terminal did not continue until the helper timed out
  roughly 30 seconds later and VTE fell back to plain `/dev/ptmx` allocation.
- **Root cause:** `gnome-pty-helper` sends two back-to-back `SCM_RIGHTS`
  messages, one for the PTY master and one for the slave, while VTE reads them
  with two separate `recvmsg()` calls. MOS's AF_UNIX stream path in
  [sock_un.c](../src/net/impl/sock_un.c) drained all currently buffered bytes in one
  `recvmsg()` and then exposed all ready ancillary records at once. That let
  the first `recvmsg()` consume both one-byte helper payloads even though VTE
  only picked up one fd from that call. The second `recvmsg()` then blocked
  waiting for a second record that had effectively already been coalesced away,
  until the socket timeout fired.
- **Fix:**
  - in [sock_un.c](../src/net/impl/sock_un.c), preserve `SCM_RIGHTS` boundaries on
    AF_UNIX stream sockets by stopping each `recvmsg()` at the next queued
    rights boundary
  - release at most one queued rights record per stream `recvmsg()` so
    back-to-back fd-passing sends are observed as separate receives
  - extend [posix_fd_pass.sh](../test/posix_fd_pass.sh) with a regression that
    sends two consecutive `SCM_RIGHTS` messages over a Unix stream socket and
    verifies they are received one-at-a-time

## 2026-04-11 - TCP receive callback dropped tail bytes, corrupting SSH streams

This issue appeared as an intermittent OpenSSH client failure rather than a
clean disconnect. The client would sometimes abort with
`Bad packet length ...`, which is a strong sign that the encrypted byte stream
itself was corrupted.

### 1. SSH occasionally failed with `Bad packet length 1349676916`

- **Caught by:** running an SSH client against the guest and seeing sporadic
  packet framing failures instead of a stable session
- **Symptom:** the client reported `Bad packet length 1349676916.` This is not
  a normal SSH protocol rejection; it means the client decoded garbage where a
  valid packet header should have been.
- **Root cause:** [sock_cb.c](../src/net/impl/sock_cb.c) handled incoming TCP pbufs
  incorrectly when the socket receive ring did not have enough free space for
  the whole segment. `tcp_on_recv()` copied as many bytes as fit, called
  `tcp_recved()` only for those bytes, but then still freed the entire pbuf.
  That silently discarded the tail of the TCP stream. For a byte-stream
  protocol like SSH, losing even a few bytes irreversibly corrupts packet
  framing and surfaces as a bogus packet length.
- **Fix:** in [sock_cb.c](../src/net/impl/sock_cb.c), make TCP receive all-or-nothing:
  if the receive ring cannot hold `p->tot_len`, return `ERR_MEM` and leave the
  pbuf unconsumed so lwIP can retry later. Only copy, acknowledge, and free the
  pbuf once the whole segment fits.

## 2026-04-11 - fs page-cache refill allocated before evicting, breaking `gcc` under tighter cache pressure

Initial triage suggested an `execve()` or early-userspace startup problem
because `cc1` crashed very early during `gcc -o hello hello.c` under the full
System V boot, while the same command worked in a simpler `/bin/bash` boot.
The real trigger was the filesystem page-cache replacement order.

### 1. `gcc` failed under System V boot when `PAGE_CACHE_SIZE` was small

- **Caught by:** comparing the `gcc` repro under SysV init vs direct bash boot
- **Symptom:** `cc1` segfaulted during early startup under the normal init
  boot, but succeeded when the system booted straight to bash. Increasing
  `PAGE_CACHE_SIZE` made the problem disappear.
- **Root cause:** [cache.c](../src/fs/impl/cache.c) handled fs page-cache misses in
  the wrong order. On a miss it first called `fs_page_cache_load()`, which
  allocates a fresh user page, and only after that checked whether the cache
  was already full and evicted an old entry. Under heavy boot-time pressure,
  this meant the cache refill path still required one extra free page even when
  an evictable cache page already existed. With a smaller `PAGE_CACHE_SIZE`,
  misses and refills happened often enough for `cc1`'s early file-backed faults
  to hit this path reliably.
- **Fix:** in [cache.c](../src/fs/impl/cache.c), evict one LRU fs page-cache entry
  before calling `fs_page_cache_load()` on a miss when the cache size has already
  reached `PAGE_CACHE_SIZE`.

## 2026-04-11 — PTY writes silently truncated output beyond one buffer page

Initial triage suggested an `ssh`/`select()` wakeup problem because the client
showed only the first chunk of `ls -alh /bin`. The trace was more direct: the
PTY layer was dropping data once the stream grew past the 4 KiB cyclic buffer.

### 1. `ssh` showed only the first 4096 bytes of PTY output

- **Caught by:** running `ls -alh /bin` in an SSH session and reading
  `out/krn.log`
- **Symptom:** `ls` exited normally, but the client only displayed the first
  part of the directory listing.
- **Root cause:** [pts.c](../src/dev/impl/pts.c) used `cyb_putbuf(..., 0, 0)` in
  both `pts_master_write()` and `pts_slave_write()`, which made the cyclic
  buffer act as nonblocking. Those functions then unconditionally returned the
  caller's requested size even when `cyb_putbuf()` had accepted only a partial
  write. Once the PTY buffer filled, the tail of the stream was silently
  discarded.
- **Fix:**
  - in [pts.c](../src/dev/impl/pts.c), honor the file's `O_NONBLOCK` state in
    `pts_master_write()` and `pts_slave_write()`
  - return the actual byte count from `cyb_putbuf()` instead of pretending the
    whole request succeeded
  - translate broken-reader cases back to `-EIO` for PTY semantics
  - keep the `ONLCR` translation path consistent with the same partial-write
    rules

### 2. The first large-transfer regression test deadlocked

- **Caught by:** running `script.posix_nonblock_ipc`
- **Symptom:** the new PTY large-write test blocked forever.
- **Root cause:** the test tried to do a blocking 8 KiB write into a 4 KiB PTY
  buffer with no concurrent reader. That is valid kernel behavior, so the test
  itself was wrong.
- **Fix:** update [posix_nonblock_ipc.sh](../test/posix_nonblock_ipc.sh) to
  fork a child writer while the parent drains the PTY master, and add
  `<sys/wait.h>` for the child exit check.

## 2026-04-11 — PTY master spurious HUP breaks `screen`; `ssh` client can't exit

Two regressions from the same select/HUP change pulled in opposite directions:
`screen` killed its window immediately after forking the shell, and `ssh` hung
forever after the remote session closed.

### 1. `screen` window died immediately after creation

- **Caught by:** running `screen` from a login shell
- **Symptom:** screen created one window, printed the shell prompt briefly, then
  exited cleanly (status 0). Kernel log showed `read(ptmx_master) = 0` at the
  first `select()` after the `fork()`.
- **Root cause (mechanism):** `select()` propagated `FS_POLL_HUP` on the ptmx
  master into `readfds` for all CHR devices. At the point screen called
  `select()`, the forked child had not yet opened the slave — so
  `cyb_writer_count(s2m) == 0` and HUP fired. Screen read 0 bytes (EOF),
  interpreted it as window exit, and tore the window down.
- **Root cause (deeper):** `cyb_writer_count(s2m) == 0` cannot distinguish
  "no slave has ever written" from "a slave opened and then exited". A plain
  flag `slave_ever_opened` would fix this, *but* `fs_chown` calls
  `vfs_open(O_RDONLY)` on the slave path, which (unconditionally) called
  `cyb_writer_open(s2m)` and would have set that flag — silently defeating the
  gate before the real shell opened the slave.
- **Fix:**
  - [pts_internal.h](../src/dev/impl/pts_internal.h): add `slave_ever_opened` to
    `pts_pair`.
  - [fs.c](../src/fs/impl/fs.c): change `fs_stat`, `fs_chmod`, and `fs_chown` to
    open with `O_PATH` instead of `O_RDONLY`. These functions only need inode
    access; `O_PATH` is the correct flag, and it already gates the cyclic
    buffer and slave-count operations in the slave open path.
  - [pty.c](../src/dev/impl/pty.c) and [ptmx.c](../src/dev/impl/ptmx.c): set
    `slave_ever_opened = 1` inside the existing `if (!(flag & O_PATH))` block —
    no new conditionals needed.
  - [pts.c](../src/dev/impl/pts.c): gate master HUP on
    `p->slave_ever_opened && cyb_writer_count(p->s2m) == 0`.

### 2. `ssh` client could not exit after the remote session closed

- **Caught by:** running `ssh user@host`, logging out, observing the client
  hanging.
- **Symptom:** the ssh client process stayed alive indefinitely after the server
  session ended.
- **Root cause:** an earlier attempt to fix the screen regression suppressed
  CHR HUP from `readfds` in `select()`. The ssh client's controlling terminal
  is a pty slave; when the master is closed, the slave gets HUP. Without HUP
  in `readfds`, the client's `select()` returned but stdin showed nothing, so
  the client did not know to exit.
- **Fix:** revert [select.c](../src/fs/impl/select.c) to the unified rule: any
  `FS_POLL_HUP` on a READ-subscribed fd sets `readfds`. No per-file-type
  special-casing. The spurious HUP is prevented at the source (pts.c gate),
  not masked in select.c.

---

## 2026-04-10 — Pipe and PTY EOF readiness split for `select()` vs `poll()`

This bug showed up during full System V boot rather than in the smaller POSIX
tests. `xinetd` was launched under `initlog`, and the system could burn
through nearly all RAM while boot still looked superficially alive. The real
problem was not `xinetd` itself; it was a subtle mismatch in how MOS exposed
EOF through `poll()` versus `select()` on byte-stream IPC objects.

### 1. `initlog` spun forever after daemon startup and consumed memory

- **Caught by:** booting RH9 userspace with `xinetd` enabled
- **Symptom:** `initlog` repeatedly looped on its capture pipe after
  `xinetd` daemonized, steadily growing heap via `brk()`/`mmap()` and
  eventually exhausting RAM.
- **Root cause:** the earlier SSH/self-pipe work taught anonymous pipes to
  surface EOF through readiness, but it did so too bluntly: empty closed
  streams looked like ordinary read-ready data to both `select()` and `poll()`.
  That was acceptable for EOF-observing `select()` users, but it broke
  `poll(POLLIN)` callers like `initlog`, which then woke immediately on EOF
  and spun.
- **Fix:** split EOF/HUP from ordinary read readiness:
  - in [fs.h](../src/fs/fs.h), add an internal
    `FS_POLL_HUP` readiness bit
  - in [pipe.c](../src/fs/impl/pipe.c), report buffered
    data as `FS_POLL_READ` and closed-writer EOF as `FS_POLL_HUP`
  - in [pts.c](../src/dev/impl/pts.c), apply the same split
    to PTY master/slave poll readiness so pseudo-terminals behave consistently
    with pipes
  - in [select.c](../src/fs/impl/select.c), treat
    `FS_POLL_HUP` as readable when the caller asked for read readiness, so
    EOF wakeups still work for `select()`
  - in [poll.c](../src/fs/impl/poll.c), translate
    `FS_POLL_HUP` to `POLLHUP` instead of `POLLIN`, matching the behavior
    expected by daemon-monitoring loops

### 2. The fix had to preserve the earlier SSH `vim` and `exit` behavior

- **Caught by:** regression review immediately after the first attempt
- **Symptom:** simply hiding EOF from pipe poll readiness would have avoided
  the `initlog` spin, but it would also have regressed the SSH fixes that rely
  on `select()` waking when a pipe reaches EOF.
- **Root cause:** `select()` and `poll()` were sharing one anonymous-pipe
  readiness signal, even though user space relied on different interpretations
  of EOF.
- **Fix:** keep nonblocking `read()` semantics unchanged, and teach the common
  polling layer to expose EOF differently for the two APIs instead of removing
  EOF readiness entirely.

### 3. Regression coverage now checks the split explicitly

- **Caught by:** the need to keep both the boot fix and SSH fix at once
- **Symptom:** the previous `posix_nonblock_ipc` test covered `EAGAIN` versus
  EOF on `read()`, but it did not verify how EOF appeared through `select()`
  and `poll()`.
- **Root cause:** readiness semantics had no direct script coverage.
- **Fix:** extend [posix_nonblock_ipc.sh](../test/posix_nonblock_ipc.sh)
  to verify:
  - `select()` reports EOF on pipes and PTYs as readable
  - `poll(POLLIN)` reports EOF via `POLLHUP` without `POLLIN`
  - ordinary nonblocking `read()` behavior remains `-EAGAIN` with live peers
    and `0` only on real EOF

### Result

- Manual verification:
  - boot with `xinetd` no longer runs `initlog` into a memory spiral
  - SSH `vim` still works
- Regression verification:
  - `./run.sh kvm test logtofile`
  - result: `rc=0`

## 2026-04-10 — SSH `vim` burst, SSH `exit` hang, and nonblocking IPC semantics

This entry began with `vim` disconnecting over SSH and exposed two separate
POSIX-visible bugs: stream sockets treated temporary TCP backpressure as a
hard write failure, and nonblocking anonymous pipes/PTYs reported EOF too
eagerly instead of `EAGAIN`. The visible symptoms initially appeared
unrelated, but both surfaced through OpenSSH.

### 1. `vim` over SSH failed on the first full-screen redraw

- **Caught by:** manual SSH session using `vim`
- **Symptom:** interactive shell traffic worked, but starting `vim` caused the
  SSH session to drop during the first large terminal repaint.
- **Root cause:** the TCP stream send path tried to enqueue one large write at
  a time and treated lwIP `ERR_MEM` as a fatal send error. Under screen-sized
  SSH packets, `tcp_write()` could temporarily reject the request because the
  send buffer had less space than the whole payload.
- **Fix:** in [sock.c](../src/net/impl/sock.c) and
  [sock_msg.c](../src/net/impl/sock_msg.c), split TCP stream
  writes into `tcp_sndbuf()`-sized chunks and retry blocking sends until space
  is available instead of failing immediately. Keep the `tcp_sent` wakeup path
  in `sock_cb.c` so blocked writers resume once ACKs free buffer space.

### 2. SSH `exit` hung because OpenSSH's self-pipe saw false EOF

- **Caught by:** manual SSH session exiting the remote shell
- **Symptom:** the shell exited, but the SSH connection stayed open and the
  server spun inside OpenSSH's `notify_done()` loop.
- **Root cause:** MOS anonymous pipe reads returned `0` for an empty
  nonblocking pipe even while writers were still open. OpenSSH drains its
  SIGCHLD self-pipe with `while (read(...) != -1)`, so receiving `0` instead of
  `-EAGAIN` made it loop forever on what looked like EOF.
- **Fix:** in [pipe.c](../src/fs/impl/pipe.c), make
  nonblocking reads return `-EAGAIN` when the pipe is empty but has live
  writers, and reserve `0` for true EOF only. Also update pipe poll readiness
  to report readable on buffered data or real EOF.

### 3. PTYs had the same nonblocking-read mismatch

- **Caught by:** follow-up review after fixing anonymous pipes
- **Symptom:** PTY reads ignored `O_NONBLOCK`, and empty PTYs could not cleanly
  distinguish "try again later" from real EOF.
- **Root cause:** PTY master/slave read paths always called `cyb_getbuf(..., 1,
  1)`, which forced blocking behavior and collapsed nonblocking semantics.
- **Fix:** in [pts.c](../src/dev/impl/pts.c), teach both
  master and slave read paths to honor `O_NONBLOCK`, return `-EAGAIN` on empty
  PTYs with a live peer, and preserve `0` for peer-close EOF. PTY poll
  readiness was also updated to treat EOF as readable.

### 4. `sendmsg()` / `recvmsg()` had duplicated logic across AF_INET and AF_UNIX

- **Caught by:** refactor after the bug fixes stabilized
- **Symptom:** the generic and AF_UNIX message paths each had their own copies
  of iovec sizing, scatter/gather ring-buffer copies, `MSG_DONTWAIT` checks,
  and cmsg append logic, which made the bug fixes easy to apply inconsistently.
- **Root cause:** message-socket helpers had grown independently in
  `sock_msg.c` and `sock_un.c`.
- **Fix:** factor shared helpers into the socket layer:
  - in [sock.h](../src/net/sock.h) and
    [sock.c](../src/net/impl/sock.c), add `rx_iov_write()`,
    `rx_iov_read()`, and `rx_discard()`
  - in [sock_msg.c](../src/net/impl/sock_msg.c), expose
    shared helpers for iovec total length, nonblocking flag handling, and cmsg
    append
  - in [sock_un.c](../src/net/impl/sock_un.c), reuse those
    helpers instead of maintaining local duplicates

### 5. Regression coverage was missing for this exact class of bug

- **Caught by:** the need to make the SSH fixes stick
- **Symptom:** nothing in the script suite directly checked the crucial
  `-EAGAIN` versus EOF distinction for nonblocking pipes and PTYs.
- **Root cause:** existing tests covered basic pipes and PTYs, but not the
  OpenSSH-triggering edge case where an empty nonblocking endpoint must not
  look like EOF while the writer side is still alive.
- **Fix:** add [posix_nonblock_ipc.sh](../test/posix_nonblock_ipc.sh),
  which verifies:
  - anonymous pipe nonblocking reads return `-EAGAIN` while the writer is open
  - anonymous pipe reads return `0` only after writer close
  - PTY master/slave reads follow the same rule

### Result

- Manual verification:
  - `vim` now works over SSH
  - remote `exit` now closes the SSH session correctly
- Regression verification:
  - `./run.sh kvm test logtofile`
  - result: `rc=0`

---

## 2026-04-10 — POSIX script-suite fixes from the long `./run.sh test` loop

This entry began as a hang in `posix_wait.sh` and ended with a full green run
of the `/proc/tests/all_script` suite. The failures were mostly not hard
crashes; they were quiet POSIX-visible mismatches that only became obvious
once the script suite was run continuously.

### 1. `waitpid()` and `sleep 0` combined into a false hang

- **Caught by:** `test/posix_wait.sh`
- **Symptom:** the suite could get stuck forever in the wait test.
- **Root cause:** two issues compounded:
  - `waitpid(..., WNOHANG)` treated `WNOHANG` like an exact option value
    instead of a bit flag
  - `nanosleep()` with zero duration blocked instead of returning immediately
- **Fix:** in `src/ps/ps_syscall.c`, make `WNOHANG` checks use
  `options & WNOHANG` and replace stale child-count checks with live scans of
  the parent/child tree; in `src/syscall/syscall_sys.c`, make zero-length
  `nanosleep()` return immediately.

### 2. Open-file deletion lifetime

Unlink removes the directory entry while independent open file objects and
mapping references retain the backing inode. The final reference releases the
storage when the on-disk link count is zero. Ext4 tracks this lifetime by
filesystem and inode identity.

### 3. Append, positional I/O, and inode size tracking were incomplete

- **Caught by:** `test/posix_fcntl.sh`
- **Symptom:** append writes could overwrite existing bytes, and positional I/O
  could disturb the live file cursor or backend cursor.
- **Root cause:** `pread()` / `pwrite()` used the file position incorrectly,
  and ext4 writes did not fully honor `O_APPEND` or refresh inode size.
- **Fix:** in `src/fs/fs.c`, make `pread()` and `pwrite()` operate on a
  temporary position and restore the underlying seek position; in
  `src/fs/root.c`, make append writes use EOF and update inode size on growth.

### 4. `RLIMIT_FSIZE`, `umask`, and `creat()` semantics were too weak

- **Caught by:** `test/posix_rlimit.sh` and `test/posix_umask.sh`
- **Symptom:**
  - writes past `RLIMIT_FSIZE` succeeded instead of failing with `EFBIG`
  - files and directories ignored the process umask on creation
  - `creat()` bypassed the normal open/create path
- **Root cause:** those paths were either stubs or incomplete shortcuts.
- **Fix:**
  - in `src/fs/fs.c`, enforce `RLIMIT_FSIZE` in write paths
  - in `src/syscall/syscall_proc.c`, implement `getrlimit()` by delegating to
    `ugetrlimit()`
  - in `src/fs/fs.c` and `src/syscall/syscall_fs.c`, apply umask to newly
    created files and directories
  - in `src/syscall/syscall_fs.c`, route `creat()` through the normal
    `fs_open(..., O_CREAT | O_TRUNC, mode)` path
  - in `src/fs/root.c`, explicitly fix ext4 directory mode after creation

### 5. Legacy `signal(2)` and self-signal delivery were missing edge behavior

- **Caught by:** `test/posix_signal.sh`
- **Symptom:** shell trap/self-signal cases did not behave like normal Unix
  signal delivery.
- **Root cause:**
  - syscall 48 (`signal`) was still unimplemented
  - self-directed `kill(getpid(), sig)` did not reliably deliver soon enough
    for the shell trap expectations used by the test harness
- **Fix:**
  - implement `signal(2)` in `src/ps/ps_signal.c` and register it in
    `src/syscall/syscall.c`
  - for self-directed unmasked signals, trigger delivery immediately on the
    current user return frame in `src/ps/ps_signal.c`
  - update `test/posix_signal.sh` to signal the current shell via a helper
    process using `PPID`, which is stable under the wrapper script model

### 6. Broken pipes returned `EPIPE` but did not raise `SIGPIPE`

- **Caught by:** `test/posix_pipe.sh`
- **Symptom:** shell pipelines such as `cat file | head -n 1` failed even
  though the pipe buffer code detected the broken-pipe condition.
- **Root cause:** pipe writes returned `-EPIPE` but never queued `SIGPIPE` for
  the writer.
- **Fix:** in `src/fs/pipe.c`, when `cyb_putbuf()` reports `-EPIPE`, queue
  `SIGPIPE` for the current user task.

### 7. A couple of test scripts were asserting the wrong shell semantics

- **Caught by:** `test/posix_environ.sh` and `test/posix_signal.sh`
- **Symptom:** some failures were in the tests themselves rather than the
  kernel.
- **Root cause:**
  - `${EMPTY_EXPORT:-notset}` treats an empty variable as defaulted, so it was
    not a valid check for "exported but empty"
  - `$$` under the wrapped script execution model was not the right stable way
    to target the shell process under test
- **Fix:**
  - in `test/posix_environ.sh`, check the actual variable value directly
  - in `test/posix_signal.sh`, use a helper shell that signals `PPID`

### Result

- Final verification command:
  - `./run.sh test curses logtofile`
- Final result:
  - `Total 40 Cases, 0 Failed`

---

## 2026-04-07 — Page-fault VMA lookup now uses a Linux-style `mmap_cache`

### Symptom

- The old page-fault path did a full VM-tree lookup for every fault and mixed
  two unrelated ideas under the same name: a file-page cache in
  `src/mm/pagefault.c` and Linux's actual `mmap_cache`, which is a per-task
  cache of the last `find_vma()` result.

### Root cause

- MOS had no Linux-style VMA cache at all, so repeated faults in the same VMA
  kept rewalking the interval tree.
- Stack growth logic was separate from VMA lookup instead of following the
  Linux pattern: `find_vma()`, then if the fault lands in a hole immediately
  below the grow-down stack VMA, expand that VMA and retry.
- The old in-file "mmap cache" name made the code harder to reason about,
  because it was really a custom readonly private-file page cache rather than
  a VMA cache.

### Fix

- Add `user->mmap_cache` as a Linux-style cache of the last `find_vma()`
  result.
- Add `vm_find_vma()` plus cached helpers so the fault path can distinguish
  "address is inside this VMA" from "address is in a hole but this is the next
  VMA above it".
- Rework `src/mm/pagefault.c` to use that flow for demand faults and stack
  growth.
- Remove the custom readonly `MAP_PRIVATE` file-page cache from the page-fault
  layer and move file-backed caching down into the filesystem page-cache path,
  so `read()` and mmap faults share cached file pages.
- Keep that shared file-backed cache in `src/fs/cache.c`, separate from the
  generic fs descriptor code in `src/fs/fs.c`.
- Invalidate the per-task VMA cache on `mmap`, `munmap`, `mprotect`, `fork`,
  and `exec`.

### Why this is better

- It matches the real Linux mechanism more closely, so the fault path is easier
  to compare against upstream behaviour.
- It removes both the naming collision and the extra fault-layer cache that was
  hiding the real design issue.
- It gives repeated faults in the same mapping a fast path without changing the
  existing VMA lookup, on-demand paging, and COW model, while also making file
  reads and mmap faults converge on one cache.

---

## 2026-04-07 — Shared-mapping cleanup after the boot/MM fixes

### Symptom

- After the boot fix, anonymous `MAP_SHARED` pages still lived forever once
  faulted in, and the emergency file-backed `MAP_SHARED` fallback in
  `src/mm/pagefault.c` was broader than intended.

### Root cause

- `anon_shared_map` had no lifetime tracking tied to VM-region teardown, so
  `munmap`, VMA splits/merges, and process exit never reclaimed the cached
  pages when the last sharer disappeared.
- The file-backed fallback path would also activate for files that already had
  a proper filesystem page-cache identity, even though those faults should be
  satisfied by the fs page cache or fail cleanly.

### Fix

- Add `mm_anon_shared_get()` / `mm_anon_shared_put()` and a small
  `anon_shared_refs` table in `src/mm/cache.c`.
- Wire those refs through `vm_add_map`, `vm_region_invalid`, `vm_mprotect`,
  `do_munmap`, and VMA coalescing so shared anonymous objects survive transient
  descriptor surgery but are reclaimed when the last live mapping goes away.
- Narrow the MM-cache-layer `file_shared_map` fallback so it is only used for
  `MAP_SHARED` files that cannot participate in the filesystem page cache.

### Why this is better

- Shared anonymous pages now have a real lifetime tied to live VM mappings
  instead of kernel uptime.
- The file fault path is closer to Linux: use the filesystem page cache when
  available, and keep the local fallback strictly as a compatibility escape
  hatch rather than a shadow cache.

---

## 2026-04-07 — Generic block-device lookup for ext mounts

### Symptom

- `ext4_get_sb()` in `src/fs/root.c` knew too much about storage internals: it
  stripped `/dev/` by hand, walked `hdd_partitions`, checked loop-device state
  directly, and special-cased root-device selection with `hdd_partitions[0]`.

### Root cause

- MOS had no generic block-device registry, so the ext mount path had to know
  where HDD partitions and loop devices each stored their bookkeeping.
- The public HDD partition array leaked hardware-driver internals into the fs,
  exec, proc, and `/dev` layers.

### Fix

- Add a small generic block-device registry in `src/dev/blockdev.c`.
- Register discovered HDD partitions and loop slots there, marking only
  attached/usable devices as mountable.
- Switch `ext4_get_sb()`, root mount, and exec-time remount to resolve devices
  through that registry instead of peeking at `hdd_partitions` / `loop_devs`.
- Make the HDD partition table private to `src/driver/impl/storage/ata.c` and expose only
  accessors used by `/dev/hdd` and `/proc/partitions`.

### Why this is better

- The ext mount path is closer to Linux: filesystems consume a generic block
  device abstraction instead of parsing driver-private tables.
- HDD bookkeeping is no longer exported as a global array.
- Future block devices can become mount sources by registering once, without
  editing ext4 mount code again.

---

## 2026-04-07 — Bugs flushed out by the new `/proc/tests` script suite

After moving guest tests into `test/*.sh` and wiring them into
`/proc/tests/all_script`, the expanded script coverage started finding a
different class of bugs: not just kernel panics, but quiet POSIX-visible
semantic mismatches.  This section is a compact summary of the issues the new
script suite exposed and the fixes that landed.

### 1. Existing-directory `mkdir` succeeded instead of failing

- **Caught by:** `test/ret_fs.sh`
- **Symptom:** `mkdir existing-dir` succeeded silently instead of failing with
  the usual `EEXIST` behaviour.
- **Root cause:** `lwext4` directory creation checked whether the path already
  existed, but returned success instead of `EEXIST`.
- **Fix:** Return `EEXIST` from `ext4_dir_mk()` for the already-existing case.

### 2. `mkdir` with a missing parent incorrectly succeeded

- **Caught by:** `test/posix_mkdir.sh`
- **Symptom:** creating `/a/missing/child` worked when the parent directory did
  not exist.
- **Root cause:** the root filesystem `mkdir` path delegated too early into the
  ext4 layer without first validating the parent directory.
- **Fix:** validate the parent in `src/fs/root.c` before issuing the create.

### 3. `rmdir` allowed non-empty directories

- **Caught by:** `test/posix_dirs.sh`
- **Symptom:** removing a non-empty directory succeeded.
- **Root cause:** the removal path did not enforce the standard
  `ENOTEMPTY` check before unlinking the directory entry.
- **Fix:** reject `rmdir()` on non-empty directories in the root filesystem.

### 4. `O_APPEND` opens did not actually append

- **Caught by:** `test/posix_io.sh`
- **Symptom:** writes to an `O_APPEND` descriptor could overwrite existing
  contents instead of always landing at EOF.
- **Root cause:** open/write handling did not consistently move the file
  position to the current end of file for append-mode descriptors.
- **Fix:** honor `O_APPEND` in the file path so append writes are serialized at
  EOF.

### 5. Re-mounting an image as a loop device could crash the kernel

- **Caught by:** `test/loopdev.sh` and `test/ret_mount.sh`
- **Symptom:** mounting the same image again, or exercising duplicate-mount
  paths, could crash partway through the guest test run.
- **Root cause:** loop-device and mount-state handling did not reject duplicate
  mounts cleanly and had unsafe assumptions in the remount path.
- **Fix:** make duplicate mounts return `-EBUSY`, tighten loop-device handling,
  and remove the crash path.

### 6. Renaming a directory into its own descendant was not rejected

- **Caught by:** `test/posix_rename.sh`
- **Symptom:** invalid rename patterns that should fail could corrupt the tree
  or behave unpredictably.
- **Root cause:** the rename path did not detect the ancestor/descendant case.
- **Fix:** reject attempts to move a directory into its own subtree.

### 7. `/dev/pts` names were generated in hexadecimal

- **Caught by:** `test/dev_pts.sh`
- **Symptom:** PTY paths under `/dev/pts` used hex-like names instead of the
  normal decimal numbering expected by userspace.
- **Root cause:** `src/dev/ptmx.c` formatted PTY indices with `%x`.
- **Fix:** emit PTY names with `%d` so `/dev/pts/0`, `/dev/pts/1`, ... match
  normal Unix behaviour.

### 8. Read-only mmap cache eviction leaked pages and later caused crashes

- **Caught by:** heavy userspace tests such as
  `test/dev_subsystem.sh`, `test/emacs_smoke.sh`, and `test/gcc_hello.sh`
- **Symptom:** long script runs would eventually die in unrelated places after
  enough exec/mmap churn.
- **Root cause:** cached file-backed pages were dereferenced on eviction but not
  physically freed when the last reference disappeared.
- **Fix:** free the physical pages when the readonly file-cache refcount drops
  to zero, and keep cache invalidation coherent on file changes.

### 9. `proc_buf_printf()` silently truncated large generated `/proc/tests` files

- **Caught by:** the generated script infrastructure itself
- **Symptom:** larger synthetic files such as the combined test wrappers could
  hang or produce incomplete content once formatting exceeded the old 512-byte
  scratch space.
- **Root cause:** `proc_buf_printf()` relied on a fixed-size temporary buffer
  and had no way to ask `vsprintf()` for the true output length first.
- **Fix:** teach `vsprintf(NULL, ...)` / `sprintf(NULL, ...)` to return the
  exact size, then grow `proc_buf_t` accordingly before formatting.

### 10. tmpfs had several quiet POSIX mismatches

- **Caught by:** `test/posix_tmpfs.sh`
- **Symptoms:**
  - `ftruncate()` shrink/regrow could expose stale bytes
  - sparse writes past EOF could leave uninitialized data in the gap
  - `chmod`, `chown`, and `utime` on tmpfs objects were incomplete
  - directory enumeration omitted `.` and `..`
  - `unlink(dir)` / `rmdir(dir)` semantics were wrong
- **Root cause:** the initial tmpfs implementation was a narrow `/dev/shm`
  vehicle and had not been brought up to normal POSIX-visible directory and
  metadata semantics.
- **Fix:** add proper zero-fill on shrink/regrow and sparse extension, implement
  metadata updates, report `.` / `..`, compute directory link counts more
  accurately, reject directory unlink, and make `rmdir()` remove directories
  through the correct path.

### Key Lesson

Guest shell-script coverage is excellent at finding the "boring" bugs that are
easy to miss during kernel bring-up: wrong errno values, stale data exposure
after truncate, malformed `/dev` naming, directory edge cases, and metadata
operations that quietly return success without doing the right thing.  These
are precisely the bugs that make a system feel non-POSIX even when the big
features already work.

---

## 2026-04-06 — OpenSSH 3.5p1 `sshd` crash / privilege-separation bring-up

### Symptoms

- Connecting to the guest with `ssh root@10.0.5.18` initially crashed the
  per-connection `sshd` child immediately after `fork()`.
- The kernel log showed the child resetting signal handlers with
  `rt_sigaction(...)` and then dying in libc.
- After the crash was fixed, `sshd` still rejected connections because
  privilege separation reached an unimplemented `chroot(2)`.

### Root Causes

#### 1. `select(2)` overflowed heap-allocated `fd_set` buffers

MOS `do_select()` copied and cleared a full `sizeof(fd_set)` regardless of the
caller's `nfds`.  OpenSSH 3.5p1 heap-allocates only the number of bytes needed
for the requested descriptor range.  The kernel therefore wrote past the end of
that heap buffer on every `pselect()`, corrupting malloc metadata and causing
later crashes inside glibc.

#### 2. `sys_brk(0)` was not query-only

On the first call, `sys_brk()` force-mapped one page and advanced `brk` even
when userspace only wanted to query the current break.  That is not Linux
behaviour and confuses allocators that expect `brk(0)` to be side-effect free.

#### 3. `rt_sigaction` used the wrong userspace layout

glibc 2.3.2 on Linux/i386 uses a `kernel_sigaction` layout with a full
1024-bit userspace `sigset_t` in memory.  MOS had modeled the structure as only
two words of mask storage, so the kernel and glibc disagreed about the stack
object layout.

#### 4. Privilege separation required `chroot(2)` plus jail-aware path lookup

After the memory corruption issues were fixed, OpenSSH reached its privsep
preauth child and attempted to:

```text
chroot(/var/empty/sshd)
chdir(/)
setgid(...)
setuid(...)
```

`__NR_chroot` was unimplemented, and path resolution only understood the
process CWD, not a per-process jailed root.

### Fixes

**`src/fs/select.c`**

- Reject `nfds > FD_SETSIZE`
- Size all snapshots and zeroing by the actual bitset size:

```c
set_bytes = (((unsigned)(nfds ? nfds : 1) + NFDBITS - 1) / NFDBITS) *
            sizeof(fd_mask);
```

- Replace `FD_ZERO()` on output buffers with `memset(..., set_bytes)` so the
  kernel never clears beyond the caller's allocation

**`src/syscall/syscall_proc.c`**

- Make `sys_brk(0)` a pure query again
- Model `rt_sigaction` using a 32-word userspace mask (`1024 / 32`)
- Zero all returned mask words and copy only the low word that MOS currently
  implements

**`include/ps/ps.h`, `src/ps/ps_fork.c`, `src/ps/ps_syscall.c`, `src/dev/tty.c`**

- Add per-process `root_path`
- Initialize it to `"/"` for new tasks
- Copy it across `fork()`
- Free it on task reap

**`src/fs/fs.c` and `src/syscall/syscall_fs.c`**

- Teach `resolve_path()` to prepend `root_path`
- Keep `cwd` jail-relative while VFS lookups use the rooted absolute path
- Add `sys_chroot()`
- Make `chdir()` and `fchdir()` update `cwd` within the jail
- Add overflow checks so rooted path prepends cannot scribble past `MAX_PATH`

### Result

- `sshd` no longer crashes on incoming connections
- OpenSSH privilege separation now completes its `chroot()` + UID/GID drop path
- SSH negotiation proceeds normally until modern-client compatibility checks
  reject old KEX / host-key / cipher algorithms

At this point the remaining interoperability issues are protocol-age policy
differences in the client, not kernel crashes in MOS.

### Key Lesson

When a legacy daemon crashes "later" inside libc, look for earlier kernel
overwrites of user-owned variable-length buffers.  Here the visible crash was
inside malloc, but the first real bug was a kernel `select()` implementation
that assumed every userspace `fd_set` was a full `FD_SETSIZE` object.  Once the
memory corruption was gone, the next blockers were ordinary ABI completeness
items (`chroot`, path-root semantics, legacy `rt_sigaction` layout).

---

## 2026-04-06 — GNU `screen` failed with `"No more PTYs"` and later hung blank

### Symptoms

- `screen` initially failed immediately with:
  ```text
  No more PTYs.
  ```
- After the first PTY fixes, `screen` got further but stopped with:
  ```text
  TIOCPKT ioctl: Function not implemented
  ```
- After that, `screen` started and cleared the display, but the UI stayed
  blank and keyboard input appeared dead.

### Investigation

#### Step 1 — `"No more PTYs"` did not mean PTY allocation failed

The kernel log showed:

- `open("/dev/ptmx")` succeeded
- `ioctl(TIOCGPTN)` succeeded
- `stat64("/dev/pts/0")` succeeded

Old `screen` reports `"No more PTYs"` whenever its `OpenPTY()` helper fails:

```c
if ((m = ptsname(f)) == NULL || grantpt(f) || unlockpt(f)) {
    close(f);
    return -1;
}
```

So the message was just a generic userspace error path, not evidence that the
kernel had run out of PTY slots.

#### Step 2 — `grantpt()` was falling back because `/dev/pts` did not look like `devpts`

Guest `glibc-2.3.2` `grantpt()` does:

1. `ptsname()`
2. `statfs("/dev/pts/N")`
3. If `f_type` is `DEVPTS_SUPER_MAGIC` (`0x1cd1`) or `DEVFS_SUPER_MAGIC`,
   return success
4. Otherwise fall back to old legacy PTY ownership code

Our `/dev/pts` implementation had no `statfs` callback, so glibc could not
recognize it as a real `devpts` mount. That kept `screen` on the wrong
compatibility path.

#### Step 3 — slave inode metadata also had to be persistent

`/dev/pts/N` metadata was synthesized from the current task doing `stat()`
instead of stored per PTY pair. That broke the Linux/Unix98 expectation that
the slave node has stable owner/mode state after `grantpt()`.

The PTY pair needed persistent:

- slave mode
- slave uid
- slave gid
- PTY lock state

#### Step 4 — `screen` requires packet mode on the PTY master

Once `grantpt()` and `unlockpt()` worked, `screen` failed on:

```text
ioctl(fd, TIOCPKT, ...) = -ENOSYS
```

`screen` enables `TIOCPKT` on the master and aborts if it is unsupported.
So basic Unix98 PTY allocation was not enough; packet mode support was also
required.

#### Step 5 — the first `TIOCPKT` implementation was too eager

After adding `TIOCPKT`, the trace showed:

```text
write(3, "\x1b[H\x1b[J...", 12)
read(6, "\x03", 4096) = 1
pselect(...)
```

That `0x03` was generated by our own PTY code as a synthetic
`TIOCPKT_FLUSHREAD | TIOCPKT_FLUSHWRITE` control packet after `TCFLSH`.
`screen` then sat in its event loop waiting for real PTY traffic.

Removing those fabricated flush packets let startup progress further.

#### Step 6 — the final hang was a PTY buffer lifetime bug

The remaining blank-screen hang turned out not to be packet mode at all.
The real bug was PTY buffer ownership.

The original PTY pair lifecycle used `cyb_create()` and then tried to infer
when to drop hidden reader/writer references later in `pts_pair_check_free()`.
That was incorrect:

- `pts_slave_release()` closed `p->s2m` and `p->m2s` only when
  `slave_count` reached zero
- `pts_master_release()` also closed its side
- `pts_pair_check_free()` then closed additional implicit refs again

This created two problems:

1. **Double-close / premature free**
   The shared `cy_buf` objects could be freed while pointers remained stored
   in `p->s2m` / `p->m2s`.

2. **Use-after-free in poll/read paths**
   Later code such as:
   ```c
   cyb_isempty(p->s2m)
   cyb_set_poll_read(p->s2m, task)
   ```
   could touch a freed `cy_buf`, which explained the observed hang inside the
   lock path.

### Root Causes

1. **`/dev/pts` did not implement `statfs()` with `DEVPTS_SUPER_MAGIC`**, so
   old glibc `grantpt()` took the wrong path.

2. **Unix98 slave metadata was not stored per PTY pair**, so ownership/mode
   checks did not behave like Linux `devpts`.

3. **`TIOCPKT` support was missing**, but `screen` requires it.

4. **The first packet-mode implementation injected fake flush packets**, which
   confused `screen` during startup.

5. **PTY buffer ownership was wrong**, causing freed `cy_buf` pointers to be
   reused from `p->s2m` / `p->m2s`.

### Fixes

**`src/dev/ptmx.c`**

- Added `statfs()` for `/dev/pts` returning `0x1cd1`
- Fixed `/dev/pts` directory entries to use the real PTY index
- Stored persistent slave metadata in each PTY pair
- Made slave open take explicit `cy_buf` endpoint refs
- Switched Unix98 PTY buffer allocation to `cyb_create_named()`

**`src/dev/pty.c`**

- Mirrored the same buffer lifetime fixes for BSD PTYs

**`src/dev/pts.c`**

- Added persistent slave `chmod` / `chown` support
- Implemented `TIOCSPTLCK`, `TIOCGPTLCK`, `TCFLSH`, and `TIOCPKT`
- Removed synthetic packet-mode flush notifications
- Changed `pts_pair_check_free()` to destroy only the pair-owned named buffers,
  instead of closing imaginary hidden slave refs

**`include/fs/ioctl.h`**

- Added missing `TIOCPKT` constants

### Key Lesson

PTY bugs are often two layers deep: the first visible error may be a libc
compatibility check (`grantpt`, `statfs`, ownership metadata), but once that
is fixed, interactive programs like `screen` also depend on Linux-specific
TTY behavior such as `TIOCPKT`. Most importantly, shared PTY transport objects
must have explicit ownership. If buffer lifetime is inferred indirectly from
`master_open` / `slave_count`, it is very easy to free the transport while a
poll or read path still holds the raw pointer.

---

## 2026-04-05 — `man` doesn't work (stderr opened O_WRONLY)

### Symptom

Running `man` from the shell produced no output or hung.  Other programs that
use a pager (e.g. `less`, `more`) behaved the same way.

### Root Cause

In two places the kernel opens the initial stdio file descriptors for the
first userspace process and for bash spawned from a TTY:

```c
// exec.c — kinit_userspace()
fs_open("/dev/tty1", O_RDONLY, 0);  // fd 0 — stdin
fs_open("/dev/tty1", O_WRONLY, 0);  // fd 1 — stdout
fs_open("/dev/tty1", O_WRONLY, 0);  // fd 2 — stderr  ← bug
```

`stderr` (fd 2) was opened `O_WRONLY`.  Pagers like `less` (invoked by `man`)
use fd 2 as their terminal I/O channel when stdin/stdout are redirected to a
pipe.  They call `read(2, …)` to receive keystrokes from the user.  Because
fd 2 was write-only, those reads returned `-EBADF`, and the pager could not
receive any input — appearing to hang.

The same mistake existed in `tty.c` (`tty_bash_spawner`) for consoles spawned
after VT switch.

### Fix

Open fd 2 as `O_RDWR` so the TTY file descriptor is usable for both reading
and writing, matching real Linux behaviour:

```c
fs_open("/dev/tty1", O_RDWR, 0);  // fd 2 — stderr
```

Changed in `src/elf/exec.c` and `src/dev/tty.c`.

### Key Lesson

The TTY fd used as the controlling terminal must be `O_RDWR`.  Even though
`stderr` is conventionally write-only for application output, the kernel-side
fd must allow reads because pagers and interactive programs re-use fd 2 as
their terminal input channel when stdin is a pipe.

---

## 2026-04-05 — xinetd crash + "not enough memory" (ugetrlimit / setrlimit / fs_close)

### Symptoms

- `/usr/sbin/xinetd` segfaults immediately after startup:
  ```
  [899]: segfault: error code 4, address 0, eip 806114f, cmd /usr/sbin/xinetd
  ```
  The crash is a NULL dereference inside xinetd's own code, not in a library.

- Some other services log `"not enough memory"` at startup.

- Both symptoms appeared together and seemed contradictory: the value that
  fixed the crash caused the OOM, and vice versa.

### Investigation

#### Step 1 — identify which resources are queried

The kernel log showed `ugetrlimit(3)`, `ugetrlimit(4)`, and `ugetrlimit(7)`
being called. Resources 3 and 4 were returning `RLIM_INFINITY` (the default
fallback). Resource 7 (RLIMIT_NOFILE) was the only one with special handling.

#### Step 2 — narrow down by varying the return value

| `rl[0]` (cur)   | `rl[1]` (max)   | xinetd result | other services      |
| --------------- | --------------- | ------------- | ------------------- |
| `MAX_FD`(341)   | `RLIM_INFINITY` | segfault      | ok                  |
| `RLIM_INFINITY` | `MAX_FD`(341)   | **no crash**  | "not enough memory" |
| `RLIM_INFINITY` | `RLIM_INFINITY` | **no crash**  | "not enough memory" |

The value of `rl[0]` (soft limit / `rlim_cur`) was the decisive variable.

#### Step 3 — understand xinetd's startup sequence

Reading xinetd's source (`init.c`, `set_fd_limit()`):

```c
getrlimit(RLIMIT_NOFILE, &rl);
maxfd = rl.rlim_max;
if (rl.rlim_max == RLIM_INFINITY)
    rl.rlim_max = FD_SETSIZE;   // cap to 1024
if (rl.rlim_max > FD_SETSIZE)
    rl.rlim_max = FD_SETSIZE;
rl.rlim_cur = rl.rlim_max;
setrlimit(RLIMIT_NOFILE, &rl);  // sets both to min(rl_max, 1024)
ps.ros.max_descriptors = rl.rlim_max;
```

Then:
```c
for (fd = 3; fd < ps.ros.max_descriptors; fd++)
    Sclose(fd);   // close all fds above stderr
```

#### Step 4 — find why rl_cur matters

glibc reads `RLIMIT_NOFILE` **once at startup** (before `main()`) and uses
`rl_cur` to size its internal fd table. With `rl_cur = 341` glibc allocates
a 341-slot table. Then xinetd calls `setrlimit({1024, 1024})` which our
kernel was **silently ignoring** (setrlimit was a no-op). xinetd believed
`max_descriptors = 1024` and its close loop ran up to fd 1023. When fd 342
was accessed, glibc's table was out of bounds → NULL dereference → segfault.

With `rl_cur = RLIM_INFINITY`, glibc takes a different code path and does
not allocate a fixed-size table, so no out-of-bounds access.

#### Step 5 — understand the "not enough memory" service

That service does roughly:
```c
getrlimit(RLIMIT_NOFILE, &rl);
table = malloc(rl.rlim_cur * sizeof(ptr));
if (!table) log("not enough memory");
```
With `rl_cur = RLIM_INFINITY = 0xFFFFFFFF`, `malloc` fails. On real Linux,
`RLIMIT_NOFILE` defaults to `{1024, 1024}` — not `RLIM_INFINITY` — so this
never happens there.

#### Step 6 — the real Linux default is {1024, 1024}

Linux kernel default for `RLIMIT_NOFILE` is soft=1024, hard=1024.
With `{1024, 1024}`:
- glibc allocates a 1024-slot table
- xinetd gets `rl_max = 1024`, no capping needed, `max_descriptors = 1024`
- xinetd's `setrlimit({1024, 1024})` matches what we already return
- The table size and `max_descriptors` agree → no out-of-bounds
- `malloc(1024 * sizeof(...))` succeeds → no OOM

#### Step 7 — setrlimit must track values per-process

Even with the correct default, `setrlimit` must actually store the new
value. A child process forked after xinetd calls `setrlimit({1024, 1024})`
should inherit `{1024, 1024}`, not the default. Without tracking, children
read the initial default and glibc allocates the wrong table size again.

#### Step 8 — fs_close returning wrong errno

`fs_close` was returning `-ENOENT` for fds that are not open. Linux returns
`-EBADF` for all invalid `close()` calls. xinetd's close loop is:
```c
if (Sclose(fd) && errno != EBADF) { exit(1); }
```
`ENOENT ≠ EBADF` would cause a premature `exit(1)`. This is correct POSIX
behavior that was simply wrong in our implementation.

### Root Causes

1. **`RLIMIT_NOFILE` default was not `{1024, 1024}`** — we returned either
   `MAX_FD` (too small, glibc table mismatch) or `RLIM_INFINITY` (services
   OOM on malloc). The Linux default of `{1024, 1024}` satisfies both.

2. **`setrlimit` was a no-op** — it did not store the new limit. Children
   would inherit the initial default instead of what the parent set.

3. **`fs_close` returned `-ENOENT` for unopened fds** instead of `-EBADF`.

### Fixes

**`include/ps/ps.h`** — add per-process rlimit storage:
```c
#define RLIM_NLIMITS 16
#define RLIM_INFINITY 0xFFFFFFFFu

typedef struct { unsigned long rlim_cur, rlim_max; } rlimit_t;

struct _task_struct {
    ...
    rlimit_t rlimits[RLIM_NLIMITS];
};
```

**`src/ps/ps_fork.c`** (`_ps_create`) — initialize defaults:
```c
for (int i = 0; i < RLIM_NLIMITS; i++)
    task->rlimits[i] = {RLIM_INFINITY, RLIM_INFINITY};
task->rlimits[3] = {USER_STACK_PAGES * PAGE_SIZE, ...}; // RLIMIT_STACK
task->rlimits[4] = {0, 0};                              // RLIMIT_CORE
task->rlimits[7] = {1024, 1024};                        // RLIMIT_NOFILE
```
Fork inherits automatically because `fork_alloc_child` does `*task = *cur`.

**`src/syscall/syscall_proc.c`** — wire up reads and writes:
```c
int sys_ugetrlimit(int resource, void *limit) {
    task_struct *cur = CURRENT_TASK();
    unsigned long *rl = limit;
    if (rl && resource >= 0 && resource < RLIM_NLIMITS) {
        rl[0] = cur->rlimits[resource].rlim_cur;
        rl[1] = cur->rlimits[resource].rlim_max;
    }
    return 0;
}

int sys_setrlimit(int resource, void *limit) {
    task_struct *cur = CURRENT_TASK();
    unsigned long *rl = limit;
    if (rl && resource >= 0 && resource < RLIM_NLIMITS) {
        cur->rlimits[resource].rlim_cur = rl[0];
        cur->rlimits[resource].rlim_max = rl[1];
    }
    return 0;
}
```

**`src/fs/fs.c`** (`fs_close`) — return correct errno:
```c
if (cur->fds[fd].used == 0)
    return -EBADF;   // was -ENOENT
```

### Key Lesson

When a service crashes at address 0 right after `execve`, check what value
glibc uses from `ugetrlimit` at startup — glibc sizes internal tables from
`rl_cur` before `main()` runs, and that sizing is permanent. Any subsequent
`setrlimit` call does not resize the already-allocated table. The kernel's
default must match the real Linux default (`{1024, 1024}` for RLIMIT_NOFILE)
so all userspace assumptions are met from the start.
