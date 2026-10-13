# Signal and Timer Descriptors

MOS implements `signalfd`, `signalfd4`, `timerfd_create`, `timerfd_settime`,
and `timerfd_gettime` for i386 and native AMD64. The i386 time64 timer services
are also available, including i386 compatibility execution on AMD64.

| Service | i386 number | AMD64 number |
| --- | --- | --- |
| `signalfd` | 321 | 282 |
| `signalfd4` | 327 | 289 |
| `timerfd_create` | 322 | 283 |
| `timerfd_settime` | 325 | 286 |
| `timerfd_gettime` | 326 | 287 |
| `timerfd_gettime64` | 410 | Uses the native 64-bit service |
| `timerfd_settime64` | 411 | Uses the native 64-bit service |

Both descriptor types support nonblocking operation, close-on-exec,
`dup`, descriptor passing, inheritance, and poll/select/epoll subscriptions.
Final release removes persistent epoll interests and subscription resources.
An interrupted blocking read returns `EINTR`; a nonblocking read without an
available event returns `EAGAIN`.

## Signal descriptors

The syscall accepts an eight-byte kernel signal mask. MOS supports signals
1 through 32; mask bits above this range have no effect. `SIGKILL` and
`SIGSTOP` are excluded. The accepted mask belongs to the open file description
and can be replaced by passing an existing signal descriptor to `signalfd4`.
Replacing the mask does not change descriptor flags or the calling thread's
signal mask.

A read consumes pending signals directed to the reading thread and its
thread group. It cannot consume signals directed to another thread. Forked
processes and recipients of passed descriptors read their own pending signals.
Applications must block accepted signals to prevent asynchronous handlers or
default actions from consuming them.

Reads return complete 128-byte `signalfd_siginfo` records, in ascending signal
number order. Thread-directed pending state takes precedence when the same
signal is pending in both sets. Records include the signal code, sender PID
and UID for userspace sends, queued values, timer IDs and overrun counts, and
child exit status. Child exit records include accumulated user and system
CPU ticks. Standard signals coalesce; asynchronous queue payloads for signal
32 remain individually consumable. A process-directed payload queue accepts
at most 128 entries; additional queued sends return `EAGAIN`.

Persistent poll registrations capture the registering thread ID and retain
its thread group without retaining a task pointer. The periodic service
compares sequence counters for accepted signals and publishes notifications
outside `ps_lock`. Epoll subscriptions inherited by a child retain their
registration context; the child must register a separate interest for its own
signals.

## Timer descriptors

Supported clocks are `CLOCK_REALTIME`, `CLOCK_MONOTONIC`, and
`CLOCK_BOOTTIME`. MOS does not implement suspension, so boottime and monotonic
timers share the elapsed-time clock. Alarm clocks and wake-from-suspend timers
are unavailable.

A zero expiration value disarms a timer. Nonzero values arm a one-shot or
periodic timer. `TFD_TIMER_ABSTIME` selects an absolute deadline. Relative
realtime timers use monotonic elapsed time; absolute realtime timers follow
wall-clock changes. Rearming clears accumulated expirations and cancellation
state. `timerfd_gettime` reports the interval and time until the next deadline.

A read returns one native-endian unsigned 64-bit expiration count and resets
that count. Periodic timers count all elapsed intervals, including intervals
missed while the process was not scheduled. Duplicate and inherited
descriptors share timer state and consume the same counter.

`TFD_TIMER_CANCEL_ON_SET` takes effect for absolute realtime timers. A
wall-clock update cancels the active deadline, clears its expiration count,
and makes the descriptor readable. The next read returns `ECANCELED`.
Rearming establishes a new wall-clock generation. Cancellation is checked
by descriptor operations and by the periodic service.

The native AMD64 and i386 time64 layouts contain four signed 64-bit fields;
the legacy i386 layout contains four signed 32-bit fields. Negative seconds,
invalid nanoseconds, and deadlines exceeding the unsigned 64-bit nanosecond
range return `EINVAL`. Legacy reads of timer state exceeding signed 32-bit
seconds return `EOVERFLOW`.

Readiness checks account for elapsed time immediately. Background expiration
and signal notifications run at the 10-millisecond service interval and are
subject to scheduler latency. Timer descriptors do not provide high-resolution
wakeups or the `TFD_IOC_SET_TICKS` ioctl.

## Validation

`test/signal_timer_fd.sh` exercises descriptor flags, blocking and nonblocking
reads, signal masks, queued payloads, child exit records, thread-directed
signals, POSIX timer records, poll/epoll notifications, one-shot and periodic
timers, inherited and duplicate descriptors, clock cancellation, and i386
time64 layouts. The script compiles and executes its probe inside the guest.
