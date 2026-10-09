# Performance Journal

Performance records describe implementation changes, benchmark configurations,
measurements, and validation coverage. Entries are ordered by date.

---

## 2026-10-09 - Heap-region growth and process-launch scaling

`sys_brk` extends a compatible anonymous heap tail with `vm_extend_map`, keeping
contiguous growth in one VMA. The tail must lie within the heap, end at the
previous page boundary, have no backing file, and retain the heap's mapping
flags and protection. Protected or replaced incompatible tails use a separate
mapping. The extension helper rejects overlapping growth and increments the VMA
generation used by the lookup cache.

The launch workload is `for i in $(seq N); do /bin/true; done`. Its list
increases Bash's resident memory: 10,000 items add 247 pages (988 KiB) compared
with an arithmetic loop. In revision `1fd5119`, each page-aligned heap expansion
creates another VMA, increasing fork cloning, tree, lock, allocation, and exec
teardown work. The kernel allocator's first-fit search amplified the extra small
allocations in a fragmented heap.

A separate baseline profile contains 5,758 active samples. Kernel `malloc`
accounts for 11.5%, predominantly its free-list search, while
`arch_mm_clone_region` accounts for 1.4% and `vm_add_map_with_lock` for 3.7%. An
arithmetic-loop capture has 4,330 active samples and 3.2% in `malloc`. With
heap-tail extension, the list-loop profile contains 5,756 active samples and
2.9% in `malloc`. The arithmetic-loop command is `for ((i=0;i<N;i++)); do
/bin/true; done`. Sampling pauses perturb the guest; these are hotspot
observations rather than cycle accounting. The optimization reduces VMA
bookkeeping; inherited pages use the regular COW PTE cloning path.

### Process-launch benchmark

Each kernel configuration boots a separate snapshot guest and runs each command
five times:

```sh
time -p bash -c 'for i in $(seq 1000); do /bin/true; done'
time -p bash -c 'for i in $(seq 10000); do /bin/true; done'
```

The configurations are revision `1fd5119` and that revision with
`brk_extend_tail` extending compatible heap mappings through `vm_extend_map`.
Both guests use release x86 kernels, two KVM vCPUs, `coreduo`, 4 GiB RAM, RH9
text-mode init, and snapshot disks on an AMD Ryzen 9 5950X host. CPU affinity is
unrestricted. Timing runs have no monitor sampling.

| Launch count | Separate VMAs, seconds | Extended heap VMA, seconds | Separate median | Extended median |
| --- | --- | --- | ---: | ---: |
| 1,000 | .10, .10, .10, .10, .10 | .09, .09, .09, .09, .09 | 0.10 s | 0.09 s |
| 10,000 | 1.53, 1.47, 1.36, 1.31, 1.32 | .92, .93, .93, .92, .93 | 1.36 s | 0.93 s |

The 10,000-launch median decreases by 31.6%; scaling between counts changes from
13.6x to 10.3x. Absolute results depend on host scheduling and warm-up. The
measurements compare these two MOS heap-growth configurations.

### Validation

`sh /proc/tests/sparse_fork` checks sparse address-space cloning and
copy-on-write isolation. `tools/user/fork_scale_probe.c` provides a freestanding
fork scaling probe.

Release and test kernels build for x86 and AMD64. All 29 mmap and 10 heap tests
pass on x86 with one and two CPUs, and AMD64 with two and four CPUs. The tests
cover contiguous growth, retained data, partial-page shrinking, regrowth, and
preservation of a protected tail. Native fork/COW probes pass in those
configurations; ELF/shebang exec probes pass on both architectures with two
CPUs.


Raw timings, profiles, and configuration are written under
`out/benchmarks/process-launch-2026-10-09/`. Kernel unit-test and probe logs are
written under `out/<arch>/release/`.

---

## 2026-10-09 - Open/close lookup and allocation costs

Commit: `94ca69f822237c5e51ea08c968853f2142a3714d`. Baseline: parent revision
`7f58566bd8ffa387bcccc509985fca27a2d3041d`. Measurements: 2026-10-09.

lwext4 resolves an existing inode's type in one traversal and returns stat
metadata from the inode reference acquired during that lookup. A 64-slot
positive pathname cache stores paths of up to 255 bytes and their inode numbers.
Directory insertion and removal advance a namespace generation that invalidates
cached traversal results. Inode metadata is read on every lookup, so chmod,
truncation, replacement, and inode reuse remain observable. Separate 16-slot
caches store inode-table locations and block-buffer lookup results; buffer
removal clears matching lookup slots.

The VFS open-link interface returns a resolved symlink target from the lookup
handle, avoiding a temporary file description and another readlink lookup.
Inline targets are copied from the acquired inode; block-backed targets use the
block cache. Ext4 file descriptions embed the inode and open handle in one
allocation, while tmpfs embeds its inode and file description together. Ordinary
ext4 closes bypass orphan scans and inode metadata reads; final-link removal
marks open handles for orphan processing. Path buffers initialize only the first
byte, and ordinary path resolution avoids a temporary string copy. Root
read/write opens bypass the redundant DAC metadata query; non-root opens retain
permission checks against current metadata.

The benchmark uses release x86 kernels built from the two stated revisions, KVM
with `-cpu host`, two vCPUs, 512 MiB RAM, and an AMD Ryzen 9 5950X host. CPU
affinity is unrestricted. Each guest boots a freestanding init probe on a
disposable 128 MiB ext3 image handled by lwext4. Each sample performs 100,000
successful open/close pairs after 1,000 warmup pairs. There are three samples
per path, no monitor sampling, and no file-content I/O in the timed loop. The
table reports median microseconds per pair; a pair includes both syscalls.

| Path | `7f58566`, µs/pair | `94ca69f`, µs/pair | Throughput ratio |
| --- | ---: | ---: | ---: |
| Regular file on lwext4 | 1.776 | 0.835 | 2.13x |
| Relative symlink on lwext4 | 4.400 | 1.103 | 3.99x |
| Regular file on mounted tmpfs | 0.834 | 0.685 | 1.22x |

The corresponding elapsed samples, in microseconds per 100,000 pairs, are
177607/178385/174403 and 80020/83504/83975 for regular files;
440033/442291/392710 and 110297/112284/110283 for symlinks; 83354/83938/73030
and 68536/68884/63861 for tmpfs. These are warm-cache, single-process
measurements; they do not measure cold storage latency or concurrent-open
scalability.

The filesystem probe passes on `94ca69f`, covering symlink chains and loops,
inline and block-backed targets, mount crossings, rename and replacement, unlink
with open or duplicated descriptors, truncation, inotify open/close events, and
non-root access after chmod. Host checks for lookup collisions, buffer
lifetimes, fresh metadata, and filesystem notifications pass for the filesystem
implementation in `94ca69f`. The measured comparison covers x86; AMD64
throughput is not measured in this record.


Raw samples, kernel hashes, probe source, and validation logs are stored under
`out/benchmarks/commit-performance-2026-10-09/`.

---

## 2026-10-08 - Unix socket stream performance

Unix stream `read`/`write` rely on the network core mutex already held by socket
file operations, removing a redundant receive-ring spinlock. Unix rings have no
IRQ producers. Blocking operations enroll their waiter before `sock_wait`
releases core ownership, preserving the readiness-to-sleep ordering. Datagram
and ancillary helpers use receive-ring locks.

Unix wakeups skip the queue lock when both blocking and poll subscriber lists
are empty and asynchronous I/O is disabled. Core ownership excludes new
subscriptions during the check; cancellation may remove subscriptions, so the
check loads only the list-head pointers atomically. Registered poll/epoll and
SIGIO observers retain the full notification path. The network core mutex still
serializes Unix socket operations and protects peer lifetime.

The MOS configurations are revision `a705cbd` and commit `6c646b0`, which
implements the Unix ring-lock and notification changes described above. RH9
Linux and both MOS configurations boot through `./grub.sh smp=1 logtofile` using
their text-mode entries and one benchmark binary. RH9's 2.4.20-8 kernel has
`CONFIG_SMP` disabled. All three recorded `/proc/cpuinfo` files contain only
processor 0. QEMU 10.2.1 uses KVM, `coreduo`, 4096 MiB RAM, and host CPU 6
affinity. Display output and audio are disabled.

Command: `./unix_socket_perf --seconds 3 --ops-seconds 3 --repeats 5`. The table
uses the median of all five repetitions. GB/s is decimal throughput (`MB_per_s /
1000`); Mops/s counts successful measured 64-byte `write` calls.

| Transport / workload | Linux 2.4.20-8 | MOS `a705cbd` | MOS `6c646b0` | MOS change |
| --- | ---: | ---: | ---: | ---: |
| socketpair, 64 KiB GB/s | 19.835 | 24.455 | 24.724 | +1.10% |
| named, 64 KiB GB/s | 19.221 | 24.628 | 24.702 | +0.30% |
| socketpair, 64-byte Mops/s | 2.796 | 2.725 | 2.769 | +1.59% |
| named, 64-byte Mops/s | 2.798 | 2.703 | 2.935 | +8.57% |

The 0.30–1.10% large-copy throughput differences are within the recorded sample
ranges. MOS `6c646b0` is 24.7%/28.5% above this Linux baseline for
socketpair/named throughput. Small-write improvements differ by transport, so
the named-socket gain should not be attributed to every workload. Median RTT
changes from 1.325 to 1.293 us for socketpair and from 1.334 to 1.290 us for
named sockets.

A two-vCPU profile contains 76.7% of throughput samples in `memcpy` and 41.4% of
operation-rate samples in syscall entry/return. Benchmark samples used in the
table have no profiling. These cache-resident local stream measurements are
specific to RH9 userspace, this host, and this QEMU configuration; they do not
establish performance against modern Linux.

[Raw CSV, CPU information, sample ranges, and
configuration](benchmarks/unix-socket-2026-10-08/metadata.json) are stored
together. The `posix_unix_stream_stress` test checks four inherited writers at
64, 4097, 65536, and 262145 bytes, per-writer byte counts, EOF, blocking
POLLIN/POLLOUT wakeups, and SIGIO. Single-vCPU validation passes this test, the
Unix-only cases from `posix_socket` and `posix_socket_wait`, and the full
`posix_fd_pass` script. The same checks also pass with two guest CPUs (host
affinity 6,8), separately from the single-CPU performance comparison. Release
and test kernels build for x86 and AMD64; AMD64 runtime was not checked. The
wait script's result output is redirected to a regular file for the release
kernel, which has no `/proc/tests/.result`. Full networking/IPC coverage is
limited by UDP PCB exhaustion in the socket suite and a missing devpts mount
required by the nonblocking IPC script.

---

## 2026-10-08 - Interrupt return and segment-state caching

Commit: `50d92bc6092f4727a7c72b69d391251f19ebe32e`. Baseline: parent revision
`6c646b0c6db5e3d41c7679db951ffd155bce4659`. Measurements: 2026-10-09.

Each CPU caches its loaded LDT base and compares installed TLS descriptors
against the current task's descriptors. Matching state avoids repeated LLDT and
GDT writes. Descriptor updates publish complete state with local IRQs masked.
I386 kernel returns still refresh segment state because the return assembly
restores saved selectors in kernel mode.

AMD64 caches native FS/GS bases by task owner and value. Hardware bases are
saved at context switches and explicit base queries or updates; task/ABI
transitions and explicit setters invalidate the installed-state cache. Fork
snapshots read the current task's live native bases. Ordinary kernel-mode
interrupt returns skip user segment restoration. Maskable IRQ entry selects
SWAPGS from saved CS; CPU exceptions retain the MSR check needed for exceptions
inside entry/exit SWAPGS windows. Segment saves use stores instead of four
implicitly locked memory XCHG instructions. The user-return path invokes signal
processing when an unmasked signal is pending or saved-mask restoration is
required.

The comparison uses release AMD64 kernels built from the two stated revisions,
an AMD Ryzen 9 5950X host, KVM with `-cpu host`, two vCPUs, and 512 MiB RAM. CPU
affinity is unrestricted. A statically linked freestanding init probe compiled
with GCC `-O2` invokes native `getpid` through SYSCALL 1,000,000 times per
sample and verifies the returned PID. Five samples run in each guest.
Gettimeofday calls bound the timed interval; serial output is outside it, and no
monitor sampling is enabled.

| Revision | Five elapsed samples, µs | Median, µs/call |
| --- | --- | ---: |
| `6c646b0` | 297028, 296273, 296717, 297421, 296275 | 0.297 |
| `50d92bc` | 110250, 110213, 110212, 110809, 110175 | 0.110 |

The median throughput ratio is 2.69x, with a 62.9% decrease in elapsed time.
This measures the aggregate native syscall entry, dispatch, and return path. It
does not isolate each optimization's contribution or quantify IRQ latency,
exception latency, i386 performance, or application throughput. Both kernels
pass the probe's PID checks. The benchmark does not test NMI entry or all
selector/base transitions. Probe source, raw samples, and kernel hashes are
stored under `out/benchmarks/commit-performance-2026-10-09/`.

---

## 2026-10-06 - x86 process-launch performance validation

Date: 2026-10-06. Revision: `2e6426d` (`refine multi-arch codes`).

The release x86 kernel completed the 1,000-process benchmark in a median of 0.13
seconds with one virtual CPU and 0.12 seconds with two. PIT clock access
accounted for approximately 10% of active samples in both configurations.
Explicit TLB-flush routines were rarely sampled. The percentages describe
sampled instruction locations, with idle samples excluded.

### Configuration and workload

The kernel was built with `make -j8 ARCH=x86 BUILD=release`. The host processor
was an AMD Ryzen 9 7950X. QEMU used KVM, the `coreduo` CPU model, 4 GiB RAM,
VMware VGA, and one or two virtual CPUs. The RH9 disk was opened with
`snapshot=on`; temporary disk changes were discarded when each VM exited.

The guest reported Bash 2.05b.0, GNU coreutils 4.5.3, and glibc 2.3.2. The
benchmark scripts were:

```sh
# test.sh
for i in $(seq 1000); do /bin/true; done

# full.sh
for i in $(seq 10); do echo $i; time -p ./test.sh; done
```

Each timed run included the shell executing `test.sh`, its `seq` invocation, and
1,000 executions of `/bin/true`. Timing uses `full.sh` without monitor sampling
and redirects its output to a guest file.

The kernel was booted directly using `-kernel out/x86/release/kernel -append
'bash verbose=0'`. This starts the benchmark without the complete RH9 init and
desktop workload used by the GRUB boot path. Networking used QEMU user
networking. The dataset covers this MOS configuration and contains no matched
Linux measurement or alternative MOS implementation.

### Elapsed times

| Virtual CPUs | Ten reported real times, seconds | Median | Mean | Range |
| --- | --- | --- | --- | --- |
| 1 | .14, .14, .14, .14, .13, .12, .12, .12, .12, .13 | .130 | .130 | .12–.14 |
| 2 | .14, .14, .14, .13, .12, .12, .12, .12, .12, .12 | .120 | .127 | .12–.14 |

Both sequences show a warm-up trend. The 0.01-second reporting precision and
overlapping ranges prevent attributing a small difference to CPU count. The
reported user and system times were not used to estimate kernel costs.

### Sampling results

Separate profiling runs repeatedly executed `test.sh`. A QMP monitor driver
paused the VM, read each CPU's registers using an explicit `cpu-index`, and
resumed execution after each sample. The running interval was randomized between
1 and 5 milliseconds. Symbols came from the matching release `kernel.dbg`,
including assembly symbols without size metadata.

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
instruction boundaries. The sampling pauses also perturb execution. The figures
therefore provide prioritization evidence rather than exact savings predictions
or a complete attribution of KVM host work.

### Clock callers and other costs

All port samples in the main captures returned into `time_now_us`. The ports
were PIT data at `0x40` and the latch command at `0x43`. [The PIT clock
implementation](../src/driver/timer/time.c) latches and reads the hardware
counter on each clock query.

A supplementary 20-second two-CPU capture collected 1,394 active samples,
including 165 PIT port samples. Caller addresses were read using stack offsets
verified against the release disassembly. Clock origins accounted for those 165
samples as follows:

| Origin | PIT samples |
| --- | ---: |
| `ps_fire_timers_unsafe` | 68 |
| `ext4_touch_file` | 63 |
| `setup_stack` | 29 |
| `timer_arm_unsafe` | 3 |
| Network service update | 1 |
| System service task | 1 |

[Scheduler timer checking](../src/ps/impl/alg/ps_alg_rr.c) reads the clock
before examining timer deadlines. [Filesystem timestamp
updates](../src/fs/impl/root.c) read the clock before updating inode timestamps.
[ELF stack setup](../arch/abi/elf_stack.h) reads the clock to seed the random
generator. These three paths account for 160 of the 165 recorded PIT samples.

At revision `2e6426d`, LDT loading executes `LLDT 0` when a task has no LDT.
Nearly every `load_ldt` sample is at the return immediately after that
instruction. Per-CPU LDT caching is described in the 2026-10-08 segment-state
record.

The sampled [spinlock acquisition](../src/lib/impl/lock.c) costs appear on one
CPU as well as two. Samples concentrated around atomic exchanges, including the
exchange used to save interrupt state; none appeared in the retry loop. This
evidence indicates acquisition overhead rather than measured contention.
Page-fault entry and page-copy samples represent demand-paging or COW work; they
do not count ordinary TLB misses.

### Implications for PCID

PCID is not available in this 32-bit execution mode. A low sample count inside
`tlb_flush` does not measure translation refill costs after CR3 changes; those
costs occur at subsequent memory accesses. Hardware TLB-miss counters were not
collected.

The dataset contains neither AMD64 PCID variants nor translation-miss counters,
so it supplies no quantitative estimate of PCID benefit.

The measurement driver used build-specific memory addresses, structure offsets,
and caller stack offsets. Reproducing these measurements on another revision
requires recalculating those offsets.

---

## 2026-10-05 - Conditional event notification scheduling

`cond_notify()` clears the event state and wakes one waiter under the wait-list
lock. It schedules only when a waiter was awakened, interrupts were enabled, and
task scheduling is enabled. Notification under an outer spinlock or a
nonpreemptible callback only makes the waiter runnable. Notification without a
waiter preserves the event for the next wait without invoking the scheduler.
Circular buffers use this single notification interface.

---

## 2026-10-05 - Deadline-driven lwIP deferred work

Network stack operations publish the next timeout deadline through
`sys_timeouts_sleeptime()`. PIT handling compares that deadline against kernel
ticks and queues deferred work only when due. Timeout callbacks execute on the
DSR worker under the network scheduling guard. An exhausted DSR queue retries on
the next tick. Loopback queues trigger deferred processing immediately. Timeout
deadlines are refreshed after network operations and before blocking socket
waits, including TCP timer creation and DHCP setup. The periodic process service
retains POSIX timer polling and graphics refresh pacing.

Reference: [lwIP timeout
interface](https://www.nongnu.org/lwip/2_1_x/timeouts_8h.html).

---

The x86 and AMD64 release and test builds complete. The AMD64 test kernel passes
258 kernel tests with two CPUs and 8 GiB of configured RAM. The link-count,
pipe, descriptor-passing, memory-reporting, socket-wait, and open-inode lifetime
regressions pass. The guest reports `LowTotal: 8321920 kB` and `HighTotal: 0
kB`. Cache sizing retains the configured adaptive policy. Loopback ICMP
validation receives all three transmitted packets. Authenticated root login
reaches the RH9 GNOME desktop with the AMD64 release kernel, two CPUs, and 8 GiB
of configured RAM.

---

## 2026-10-05 - Kernel Dispatch and Lookup

### Subsystem interfaces

Headers under `impl` are private to their owning implementation. Subsystems and
architecture adapters include public headers for shared services.
`syscall/syscall.h` declares syscall services and shared argument layouts;
`syscall/impl/syscall_internal.h` declares the internal path-resolution helper.

`ps/ps.h` declares process-memory reads and writes. These functions copy between
kernel buffers and the target task's user mappings, resolve missing pages and
write faults, and return zero on completion or `-EFAULT` on failure. A failure
may occur after preceding pages have been copied. Scheduler locks, futex wait
queues, and timer helpers remain private to `ps/impl`. Futex services and
thread-exit cleanup are implemented in `ps/impl/ps_futex.c`.

`device/devnums.h` defines device major numbers and fixed minor numbers shared by
device registration and procfs.

### Syscall namespaces

The i386 namespace is declared in `arch/abi/i386/calls.def`. The AMD64 namespace
is declared in `arch/x64/syscall/impl/calls.def`. Each declaration specifies the
Linux syscall number, namespace name, and typed service invocation. Each
dispatcher indexes a constant table of interrupt-frame callbacks. Undeclared
slots and numbers outside the table return `-ENOSYS`.

The i386 table contains 244 entries. The AMD64 table contains 213 entries. The
counts include shared compatibility stubs and the diagnostic `restart_syscall`
entry; declaration does not imply complete Linux behavior. Native entry points
expose the services present in the i386 namespace. `socketcall` maps to native
socket entry points. The shared-memory operations of `ipc` map to `shmget`,
`shmat`, `shmdt`, and `shmctl`. Semaphore and message queue operations remain
unsupported.

Credential variants, stat variants, directory variants, time-width variants, and
legacy signal interfaces share their corresponding native service. `waitpid`,
`umount`, `stime`, and `nice` correspond to `wait4`, `umount2`, `settimeofday`,
and `setpriority`. Legacy `break`, `ftime`, `gtty`, `lock`, `prof`, `stty`, and
VM86 entries have no AMD64 syscall number. The x64 kernel also exposes
`arch_prctl` for native FS and GS bases. Undeclared Linux services remain
unavailable.

I386 adapters zero-extend pointers from 32-bit argument registers and assemble
split 64-bit arguments explicitly. AMD64 adapters preserve pointer-sized
arguments and returns and convert incompatible userspace structures before
calling shared services. Socket and syscall services share the `struct iovec`
declaration in `fs/iovec.h`. Native conversions include stat, statfs, sysinfo,
times, interval timers, POSIX timers, signal wait information, directory
records, and ptrace register words. Native timer values preserve pointer width.
Native socket timestamp ioctls write two 64-bit time fields. Signals remain
limited to 1–32. Shared timeout readers have 32-bit seconds limits where the
native adapter uses a legacy time structure. Exec preserves pending signals and
the blocked signal mask. Caught handlers reset to the default disposition,
ignored handlers remain ignored, and the alternate signal stack is disabled.

Native legacy directory records contain 64-bit inode and offset fields with
eight-byte alignment and a trailing type byte. `getdents64` uses the Linux
fixed-width record layout. Conversion restores the directory cursor to the last
emitted record when the output buffer cannot hold the next converted record.
Ext4 directory offsets identify backing-store positions directly; seeking
restores the cookie without replaying preceding directory entries. The shared
legacy directory service retains 32-bit inode and cookie fields. The final ext4
directory record uses the directory's byte size as its next offset. Seeking to
that offset returns end-of-directory. Internal iterator termination values are
not exported as directory offsets. IA-32 libc directory enumeration requires
offsets representable by its signed 32-bit `off_t`.

Ext4 mount registration supplies the complete VFS target pathname with a
trailing slash to lwext4. Its temporary pathname buffer is bounded by
`MAX_PATH`.

### Operation dispatch

Bus matching and probing use bus-operation callbacks. The selected PCI match is
retained for probing. Device file opening uses inode-type callbacks. TTY, PTY,
audio, loop, disk, mouse, RTC, pipe, socket, and VirtIO GPU ioctl interfaces use
command callbacks. Exact ioctl dispatch verifies the complete command, including
direction and size, after indexing by type and number. Table names identify
command families or access classes. OSS mixer commands use the number and
direction rules of that interface.

PCI sysfs attributes select their show, read, and write callbacks at
registration. Font operations use explicit callbacks. Stat adapters and lookup
routines are ordinary typed functions.

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

Mount resolution searches complete path prefixes from deepest to shallowest. A
mount at `/a` does not match `/ab`. Both PTY group indexes select the lowest
pair index for a matching group. Group updates and master closure maintain the
indexes. Unix98 directory generation uses one snapshot of the allocation bits.

Iteration remains necessary for hardware polling, wildcard driver matching,
minor-range matching, timer expiration, directory enumeration, data movement,
and owned-object cleanup. Balanced-tree traversal follows search paths; it does
not scan the complete inventory.

### Source validation and runtime checks

Run the host-side namespace audit with Python 3:

```sh
python3 test/test_syscall_tables.py
```

The audit compares declarations against Linux i386 and AMD64 UAPI headers,
checks unique numbers and adapter names, and checks native coverage against the
i386 service set. Header paths are configurable with `--i386-header` and
`--amd64-header`.

`DispatchTest` covers slot exhaustion and reuse across bitmap boundaries,
complete ioctl identity, and mount component boundaries in a test-enabled
kernel. These checks require compilation and kernel execution. Guest regressions
include `dev_pts.sh`, `tty_basic.sh`, `tty_vc.sh`, `posix_sysv_shm.sh`,
`posix_fd_pass.sh`, `posix_mount_state.sh`, `posix_exec_signal.sh`,
`posix_signal.sh`, `posix_dirent.sh`, `xorg_compat.py`, and
`x64_console_abi.py`.

Source audits do not establish compilation or runtime correctness.

---

## 2026-10-05 - IPC Buffering and Performance

Anonymous pipes allocate a 64 KiB circular buffer. Named FIFOs and PTY
directions allocate 4 KiB circular buffers. Unix stream sockets allocate 256 KiB
receive rings per endpoint, including socket pairs and accepted connections.
Unix datagram sockets allocate 4 KiB receive rings. Socket rings reserve one
byte to distinguish full and empty states.

The circular-buffer library copies each available span with at most two
contiguous memory transfers. The second transfer handles wrapping at the buffer
boundary. Buffer indices and occupancy are updated once per span under the
buffer lock. The index calculation supports capacities that are not powers of
two. Whole-record writes publish all record bytes before notification.

Pipe and FIFO data notifications make eligible readers, writers, and poll
waiters runnable. Condition notification yields when a waiter is awakened and
the current interrupt and scheduling state permit a context switch. Blocking
operations wait when the buffer state prevents progress. Nonblocking operations
retain partial-transfer and `EAGAIN` behavior. Closing the final writer exposes
EOF after queued bytes have been consumed; closing the final reader produces
`EPIPE` on subsequent writes.

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
sh test/ipc_buffers.sh
sh test/unix_peercred.sh
sh test/unix_send_credentials.sh
```

`ipc_buffers.py` validates complete payloads through pipes, named FIFOs, socket
pairs, and named sockets. Fragment sizes include one-byte, irregular, 64 KiB,
and larger-than-ring writes. It checks ring wrapping, EOF, nonblocking empty and
full buffers, writable readiness, broken peers, datagram truncation, finite
socket receive and send timeouts, and scatter/gather transfers with `SCM_RIGHTS`
across stream-ring boundaries. A 60-second alarm bounds execution.

The shell tests `test/posix_pipe.sh`, `test/posix_nonblock_ipc.sh`, and
`test/posix_socket.sh` provide additional pipe, FIFO, PTY, and socket checks.
The nonblocking test's embedded C probe can run independently of its shell
prerequisite checks. `test/posix_socket_wait.sh` requires the test kernel's
`/proc/tests/.result` output path; a standalone harness can direct that output
to a writable regular file.

---

## 2026-10-05 - AMD64 process-launch performance

A KVM release build with two CPUs, 4 GiB RAM, `-cpu host`, RH9 Bash, and verbose
logging disabled was timed with:

```sh
time for ((i=0;i<1000;i++)); do /bin/true; done
```

| Implementation | Elapsed time |
| --- | --- |
| Temporary RAM mappings and global user shootdowns | 4.307 s |
| Plus invalidation skipped for newly present pages | 2.802 s |
| Plus permanent managed-RAM mirror | 0.796 s |
| Plus address-space-targeted user shootdowns | 0.170, 0.180, 0.182 s |

The rows describe cumulative memory-backend configurations. The configuration
with address-space-targeted shootdowns has a median of 0.180 s, approximately 24
times the throughput of the temporary-mapping/global-shootdown configuration.
The workload includes fork, ELF loading, dynamic-loader startup, and exit.
Temporary high-memory mapping scans and global cross-CPU invalidations were
responsible for the measured regression. The benchmark records elapsed guest
time; debugger capture stops execution only when Bash prints the completed
timing result. Host scheduling and processor count can affect absolute times.

KVM validation with four CPUs passes 18 MM, 20 mmap, 12 physical-allocation, and
9 heap-allocation tests. The native probe passed with two and four CPUs; i386
pthread and shared-futex regressions passed with two CPUs. Both kernel
architectures build successfully.

The native probe additionally starts a process sharing its VM that repeatedly
writes a cached page. After the parent removes write permission, the writer must
terminate with SIGSEGV. This checks permission invalidation for a shared address
space alongside COW, TLS, and signal checks. During the permission change, the
parent and writer execute on CPUs 0 and 3 with the same page-table root.

Architecture and probe context: [AMD64 kernel and process
ABI](bugfix_journal.md#2026-10-05---amd64-kernel-and-process-abi).

---

## 2026-10-04 - Adaptive filesystem and block cache budgets

### Policy and implementation

AMD64 assigns a combined budget of one quarter of allocator-managed RAM, capped
at 4 GiB. Three quarters of this budget serve filesystem pages and one quarter
serves block lines. An 8 GiB guest with 8521646080 bytes of managed RAM receives
budgets of 1597808640 bytes for filesystem pages and 532602880 bytes for block
data. These are demand-driven ceilings; cache contents are not allocated at
boot.

Cache growth also leaves a free-memory target of one eighth of managed RAM,
bounded between 16 and 256 MiB and at most half of managed RAM. As application
allocations consume headroom, the budget is reduced using current free pages and
existing cache pages. Cache misses shed excess LRU entries in batches of up to
32 pages. Block lines are flushed before normal eviction. At least one line per
cache, and one block line per active partition, remain eligible so filesystem
I/O can make progress at small budgets. Allocator-driven user reclaim can
recover both file and block pages when file pages remain pinned.

Buddy list insertions and removals maintain a free-page counter; managed RAM is
counted from usable memory-map ranges, excluding holes. Budget calculation
therefore has constant cost instead of rescanning physical page metadata on
every cache miss. Current budgets appear in `/proc/mos` alongside usage and peak
counters. File invalidation searches for the affected inode and removes only
that inode's pages, avoiding a complete cache scan on every write. A regression
verifies invalidation at offsets 0 and 4 GiB while preserving neighboring
inodes. The i386 block cache retains its 64 MiB ceiling because block lines hold
aliases in the limited kmap window. On machines without high memory, cache
allocation directly uses available low-memory pages.

### Validation

Both architecture release and test builds pass. In an 8 GiB, two-CPU KVM guest,
all 16 physical allocator tests and 23 mmap tests pass. Policy checks cover
large RAM, pressure, the aggregate ceiling, and agreement with allocator
accounting. `test/posix_cache_growth.sh` writes a temporary 256 MiB file,
synchronizes it, reads it twice, and verifies every byte. The probe passes with
block-cache usage exceeding the previous 64 MiB ceiling. Subsequent memory
statistics report 277377024 bytes of block buffers and 276422656 bytes of
filesystem page cache.

A 512 MiB i386 guest also passes all 16 physical allocator tests and 23 mmap
tests, including cache retention and inode invalidation. The AMD64 release
kernel reaches the complete GNOME desktop after root login with 8 GiB and two
CPUs. Validation guests use temporary disk snapshots.

---

## 2026-04-07 - Per-task VMA lookup cache

`user->mmap_cache` stores the last `vm_find_vma` result for the task's address
space. `vm_find_vma` returns the mapping containing an address or the next
mapping above a hole. Repeated faults within one VMA can reuse that result
without another interval-tree search. Mapping changes invalidate the cached
result explicitly or through the address space's VMA generation.

The page-fault handler distinguishes addresses inside a mapping from holes below
a grow-down stack mapping. An eligible stack fault extends that mapping and
retries lookup. Demand paging and COW use the selected VMA's protection and
backing-store metadata.

File-backed pages are cached in `src/fs/impl/cache.c`, allowing `read` and mmap
faults to share cache entries. The per-task VMA cache stores region lookup
results rather than file contents. File-page invalidation and VMA-cache
invalidation follow their respective inode and address-space lifetimes.
