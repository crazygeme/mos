# Process execution and interface layouts

The x86 backend accepts ELF32 images with machine type `EM_386`. The x64
backend accepts ELF32/i386 and ELF64/AMD64 images. The executable and its
interpreter must select the same format.

## Executable pathname interfaces

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

## Image and namespace layouts

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

## Procfs thread groups

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

## Unix IPC and descriptor directories

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

## Filesystem notifications

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

## Event counters and namespace probes

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

## Validation

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
