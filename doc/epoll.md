# Epoll

MOS implements Linux epoll services for i386, native AMD64, and i386
compatibility execution on AMD64. The implementation is
`src/fs/impl/epoll.c`. Both ABIs use a twelve-byte event record containing a
32-bit event mask and an opaque 64-bit data value.

| Service | i386 number | AMD64 number |
| --- | --- | --- |
| `epoll_create` | 254 | 213 |
| `epoll_ctl` | 255 | 233 |
| `epoll_wait` | 256 | 232 |
| `epoll_pwait` | 319 | 281 |
| `epoll_create1` | 329 | 291 |
| `epoll_pwait2` | 441 | 441 |

## Interests and delivery

An epoll descriptor owns a red-black tree indexed by open file description
and descriptor number. Duplicate descriptors can have separate interests.
An interest remains active while any reference to its open file description
exists. Final file release removes all associated interests before invoking
the file's release operation. Epoll subscriptions do not retain monitored
files and therefore do not prolong pipe or socket endpoint lifetime.

Adding an interest registers persistent callbacks in the file's producer
queues, including queues that are already ready. Readiness is rechecked after
registration. Modification updates the mask and data, rearms one-shot
interests, and rechecks readiness. Deletion ignores the event pointer and
removes subscriptions, tree membership, and any queued candidate.

Callbacks append an interest to the ready queue at most once. Waiting
revalidates candidates against their current readiness and requested mask.
Level-triggered interests return to the queue tail; this rotates delivery
across calls with small output arrays. Edge-triggered interests require
another producer notification after delivery. One-shot delivery disables an
interest until `EPOLL_CTL_MOD` rearms it. Hangup and error are reported without
explicit subscription to their public event bits. Stream receive shutdown is
available through `EPOLLRDHUP`.

Ordinary control lookup costs `O(log N)` for `N` interests. An empty wait does
not traverse the interest tree or allocate memory. Delivery examines ready
candidates rather than all monitored descriptors. Persistent registration
avoids per-wait reconstruction of file subscriptions.

## Waiting and nesting

Waiters subscribe to the epoll descriptor's notification queue and use the
shared polling wait protocol. Timeout zero queries immediately; negative
millisecond timeouts wait indefinitely. `epoll_pwait` and `epoll_pwait2` accept
the eight-byte Linux kernel signal-mask ABI. Unblockable signals remain
unblocked. Interrupted waits restore the original mask through signal-return
state. `epoll_pwait2` accepts signed 64-bit seconds and nanoseconds on both
ABIs; deadlines round upward to milliseconds and scheduler timer resolution.

An epoll descriptor is readable through poll and select when a candidate
remains ready. Epoll descriptors may monitor other epoll descriptors. Graph
validation rejects cycles and paths exceeding five epoll instances. The graph
and file-lifetime operations are serialized by a recursive mutex; producer
callbacks use spinlocks and remain usable in interrupt context.

## Supported files and constraints

Sockets, pipes, FIFOs, terminals, PTYs, kernel log devices, and PS/2 mouse input
provide readiness subscriptions. A file must have a poll operation and register
at least one producer queue to be added. Regular files, directories, and
callbacks without subscription support return `EPERM`. `EPOLLEXCLUSIVE`
returns `EINVAL`. Power-management wake locks are not provided.

## Validation

The gnu-mos sysroot installs `posix_epoll.sh` in `/root` and `/home/ezheng`.
The script contains the C regression program and compiles it with the
configured LFS userspace toolchain. Its working directory is
`${HOME}/tests/posix_epoll`. The checks cover control
errors, opaque data, level and edge delivery, one-shot rearming, duplicate
lifetime, descriptor reuse, ready-before-add subscriptions, fairness, nesting,
UNIX and TCP sockets, PTYs, timeouts, signals, concurrent waiters, and process
exit. When libevent is installed, the test requires its epoll backend and
checks persistent edge callbacks.

The benchmark reports average nanoseconds per empty wait, single-ready wait,
zero-timeout poll, and interest modification at 1, 64, and 256 monitored
pipes. Empty and single-ready waits execute 100,000 iterations per size;
poll and modification execute 10,000 iterations. Diagnostic syscall tracing
must be disabled during timing.

Libevent 2.1.12 and Xorg 21.1.18 package configurations enable their epoll
backends. Package build versions are maintained in their respective version
files.

## Measured validation results

Configuration: MOS package build 43, release mode; QEMU 10.2.1 with KVM,
host CPU model, and 512 MiB guest memory. The Linux reference kernel is
7.0.0-38-generic. Each reference uses the same statically linked program and
virtual CPU count as its MOS comparison. Libevent 2.1.12 epoll callbacks are
included in the correctness checks.

The i386 kernel uses one virtual CPU. Native AMD64 and i386 compatibility
execution use two virtual CPUs. All three MOS modes pass the ABI and device
tests, concurrent waiters, process-exit cleanup, nested edge notifications,
and the 15 circular-buffer plus five syslog unit tests.

Xorg 21.1.18 and libevent 2.1.12 build successfully for both architectures.
The Xorg configuration defines `HAVE_EPOLL_CREATE1`; the libevent configuration
defines `EVENT__HAVE_EPOLL`, `EVENT__HAVE_EPOLL_CREATE1`, and
`EVENT__HAVE_EPOLL_CTL`.

### Native AMD64 timing

All values are average nanoseconds per operation. Empty and single-ready
wait costs remain constant across the measured interest-set sizes.

| Interests | MOS empty wait | Linux empty wait | MOS ready wait | Linux ready wait | MOS modification | Linux modification |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 248 | 290 | 286 | 419 | 254 | 355 |
| 64 | 246 | 289 | 287 | 418 | 261 | 390 |
| 256 | 244 | 289 | 276 | 423 | 269 | 404 |

### i386 timing

The one-CPU i386 comparison uses the Linux i386 syscall ABI on both systems.

| Interests | MOS empty wait | Linux empty wait | MOS ready wait | Linux ready wait |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 232 | 1222 | 264 | 1371 |
| 64 | 230 | 1220 | 268 | 1365 |
| 256 | 228 | 1233 | 260 | 1333 |

The measurements cover zero-timeout waits with zero or one ready pipe and
modification of an existing interest. Poll measurements at 256 descriptors
provide a linear-scan comparison in the JSON reports.

Serial output and machine-readable reports are in `out/epoll-validation/`.
The execution commands are listed in the validation section above.
