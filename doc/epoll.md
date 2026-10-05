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

`test/posix_epoll.sh` is the guest regression test. It contains the canonical C
program and compiles it with the guest compiler. The checks cover control
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

`test/epoll_qemu.py` runs a statically linked copy of this program as init in
an isolated 512 MiB KVM guest. MOS uses a temporary ext3 disk; Linux uses a
temporary initramfs. The harness creates all guest storage independently of
system images and writes serial output plus a JSON report. MOS test kernels
also execute the circular-buffer and syslog unit suites. The default
performance checks allow at most 50 percent plus 50 ns growth between the
smallest and largest sets. `--baseline` compares wait and control timings
against a Linux report with the same virtual CPU count; `--max-slowdown`
defaults to 1.5.

Example comparison for native AMD64:

```sh
python3 test/epoll_qemu.py --linux --kernel /path/to/linux-vmlinuz \
  --probe /path/to/static-epoll-probe --output /tmp/linux-epoll.txt
python3 test/epoll_qemu.py --kernel out/x64/release/kernel-test.boot \
  --probe /path/to/static-epoll-probe --output /tmp/mos-epoll.txt \
  --baseline /tmp/linux-epoll.json
```

Libevent 2.1.12 and Xorg 21.1.18 package configurations enable their epoll
backends. Package build versions are maintained in their respective version
files.

Measured configuration and results are recorded in
[Epoll validation results](epoll-validation.md).
