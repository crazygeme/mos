# IPC Buffering and Performance

Anonymous pipes allocate a 64 KiB circular buffer. Named FIFOs and PTY
directions allocate 4 KiB circular buffers. Unix stream sockets allocate
256 KiB receive rings per endpoint, including socket pairs and accepted
connections. Unix datagram sockets allocate 4 KiB receive rings. Socket rings
reserve one byte to distinguish full and empty states.

The circular-buffer library copies each available span with at most two
contiguous memory transfers. The second transfer handles wrapping at the
buffer boundary. Buffer indices and occupancy are updated once per span under
the buffer lock. The index calculation supports capacities that are not powers
of two. Whole-record writes publish all record bytes before notification.

Pipe and FIFO data notifications make eligible readers, writers, and poll
waiters runnable without immediately yielding the producer or consumer.
Blocking operations wait when the buffer state prevents progress. Nonblocking
operations retain partial-transfer and `EAGAIN` behavior. Closing the final
writer exposes EOF after queued bytes have been consumed; closing the final
reader produces `EPIPE` on subsequent writes.

Socket waits without a deadline register an indefinite interruptible wait
without sampling the hardware clock. Finite waits sample the clock before and
after waiter registration and retain deadline expiration and signal handling.

The stream receive-ring allocation requires 256 KiB of kernel heap memory per
endpoint. `SO_RCVBUF` reports the allocated ring size. Receive and send buffer
size options do not resize the rings. Unix datagram record limits and ancillary
descriptor queues use the configured datagram ring and descriptor-queue sizes.

## Measured Performance

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

## Validation

Run the following commands inside a MOS guest with Python 3:

```sh
python3 test/ipc_buffers.py
python3 test/unix_peercred.py
python3 test/unix_send_credentials.py
```

`ipc_buffers.py` validates complete payloads through pipes, named FIFOs, socket
pairs, and named sockets. Fragment sizes include one-byte, irregular, 64 KiB,
and larger-than-ring writes. It checks ring wrapping, EOF, nonblocking empty
and full buffers, writable readiness, broken peers, datagram truncation,
finite socket receive and send timeouts, and scatter/gather transfers with
`SCM_RIGHTS` across stream-ring boundaries. A 60-second alarm bounds execution.

The shell tests `test/posix_pipe.sh`, `test/posix_nonblock_ipc.sh`, and
`test/posix_socket.sh` provide additional pipe, FIFO, PTY, and socket checks.
The nonblocking test's embedded C probe can run independently of its shell
prerequisite checks. `test/posix_socket_wait.sh` requires the test kernel's
`/proc/tests/.result` output path; a standalone harness can direct that output
to a writable regular file.
