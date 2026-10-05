# Readiness subscriptions and polling

`poll`, `ppoll`, `select`, and `pselect6` use
`src/fs/impl/poll.c:poll_wait_loop`. Epoll uses the same interruptible wait
protocol with a persistent ready queue described in [Epoll](epoll.md).

## Wait protocol

The `poll_ops` interface supplies readiness checks, registration, and cleanup.
The loop checks readiness, registers subscriptions, and checks again before
sleeping. The final check follows publication of the task's waiting state with
local interrupts disabled. A producer that changes readiness during this
interval either becomes visible to the check or wakes the waiting task.

Finite waits use absolute millisecond deadlines. Individual sleep intervals
are bounded by the unsigned timer argument. Registration-capacity exhaustion
sets `poll_table.unsupported` and limits sleep intervals to one scheduler tick.
Readiness takes precedence over a simultaneous interrupting signal. A signal
without readiness terminates the wait with `EINTR`.

## Driver interface

`file_operations.poll(file, events, table)` returns an `FS_POLL_*` readiness
mask. A null table performs a readiness query. A non-null table registers the
requested readiness queues, including queues that are currently ready.

| Internal bit | Readiness |
| --- | --- |
| `FS_POLL_READ` | Input or readable completion |
| `FS_POLL_WRITE` | Output space |
| `FS_POLL_EXCEPT` | Priority condition |
| `FS_POLL_HUP` | Hangup |
| `FS_POLL_ERR` | Error |
| `FS_POLL_RDHUP` | Stream receive shutdown |

`poll_subscribe(table, queue, lock)` places an entry from the table's fixed
storage into a producer queue. Each entry records its task or callback,
callback argument, producer lock, and removal function. `poll_notify(queue)`
executes subscriptions while the producer holds that lock. Callbacks must not
sleep or acquire a filesystem mutex.

`poll_table_cleanup` removes all registrations under their producer locks.
Task subscriptions install cancellation state for process exit. Persistent
callback subscriptions have a null task and retain ownership independently of
individual waits. The producer queue supports multiple poll tasks and epoll
instances concurrently.

## Readiness producers

Circular buffers have separate read and write subscriber queues. Buffer
writes notify readers; freeing full-buffer space notifies writers. Endpoint
closure notifies the affected direction. Pipes, FIFOs, terminals, PTYs, and
PS/2 mouse input use these queues.

Sockets maintain ordinary blocking-I/O waiters and readiness subscriptions.
`sock_wakeup` wakes the blocking waiters and notifies all readiness subscribers.
Kernel log devices use a subscriber queue under the record-buffer lock.

Poll and select allocate temporary table storage and query their descriptor
sets on each readiness check. Epoll registers interests at control time and
revalidates only candidates in its ready queue.
