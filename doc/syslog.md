# Kernel logging

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
