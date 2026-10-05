# Timing and Concurrent Kernel Execution

The timing subsystem separates elapsed time from calendar time. Network,
scheduler and relative timer deadlines use monotonic time, so changes to the
wall clock cannot prolong or prematurely expire a wait. Filesystem timestamps
and absolute `CLOCK_REALTIME` timers use calendar time.

## Clock Sources and Domains

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
| `time_now_tickets()` | 64-bit serviced PIT ticks for CPU accounting |
| `time_wall_us()`, `time_wall_sec()` | Calendar time for syscalls and metadata |

RTC snapshots support binary and BCD encodings, 12-hour and 24-hour formats,
and Gregorian leap years. The same conversion serves initialization and
`/dev/rtc`. Relative POSIX timers remain monotonic even when created with
`CLOCK_REALTIME`; absolute realtime timers follow wall-clock adjustments.

## Integration with Big Kernel Lock Removal

The timing change originally relied on serialized kernel entry. Removing that
serialization permits simultaneous reads and updates on different CPUs. Local
interrupt masking alone cannot protect shared state: i386 can tear 64-bit
values, concurrent monotonic updates can regress the published sample, and
interleaved PIT or CMOS port transactions can return incorrect hardware data.

An IRQ-masked `time_lock` now protects the shared clock state, PIT tick
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

Upstream network core ownership, deferred-service locking, concurrent scheduler
handoff and address-space shootdown coordination remain present. Network service
deadlines and IRQ comparisons both use monotonic milliseconds; converting one
side to raw ticks would prevent timely lwIP timeout processing.

## Validation

`Timekeeping` covers calendar conversion, clock domains, wall-clock changes,
repeated reads, delayed IRQ handling and parallel clock/RTC readers. The parallel
test rendezvous requires simultaneous kernel execution on two CPUs and checks
monotonic samples, coherent tick counts, valid calendar fields and concurrent
POSIX timer creation, rearming, lookup and deletion. The upstream
`smp` suite retains its parallel execution and TLB shootdown checks.

`tools/user/timing_probe.c` retains wall-clock jump, sleep, polling, socket
timeout, interval timer, POSIX timer and ping regression coverage. The removed
VM runner remains absent.

Compiler syntax checks cover both architecture variants. Full builds and runtime
regressions must be repeated for the combined revision; results from either
original commit do not validate the new concurrent clock implementation.

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
