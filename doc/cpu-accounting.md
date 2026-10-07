# CPU Usage Accounting

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

## Ownership and Lifetime

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

## Reporting Interfaces

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

## Sampling Constraints

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

## Atomic Operation Widths

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

## Validation

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
