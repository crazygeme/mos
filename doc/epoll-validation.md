# Epoll validation results

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

## Native AMD64 timing

All values are average nanoseconds per operation. Empty and single-ready
wait costs remain constant across the measured interest-set sizes.

| Interests | MOS empty wait | Linux empty wait | MOS ready wait | Linux ready wait | MOS modification | Linux modification |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 248 | 290 | 286 | 419 | 254 | 355 |
| 64 | 246 | 289 | 287 | 418 | 261 | 390 |
| 256 | 244 | 289 | 276 | 423 | 269 | 404 |

## i386 timing

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
The test implementation and execution commands are described in
[Epoll](epoll.md).
