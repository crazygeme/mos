# Desktop concurrency and resource lifetimes

The descriptor-table mutex protects descriptor lookup, installation, removal,
and close-on-exec flags. Socket descriptor installation acquires this mutex
while the task owns the network-core mutex. File readiness and ioctl callbacks
therefore execute without ownership of the descriptor-table mutex.

`fs_fd_poll` and `fs_ioctl` obtain a referenced file description through
`fs_io_begin`, release the descriptor-table mutex, and invoke the backend
callback. `fs_io_end` releases the reference after the callback. The active
scope is linked to the task's I/O scopes so process termination releases the
reference through `fs_cancel_io`.

A readiness callback may register multiple subscriptions in a poll table.
`fs_fd_poll` retains one additional file reference on the first subscription
registered by that callback. `poll_table_cleanup` removes subscriptions in
reverse registration order and releases this reference after all subscriptions
from that callback have been removed. Concurrent descriptor closure cannot
release the backing file or readiness queues while those subscriptions remain
registered. Each reused poll entry initializes its retained reference to null.

The same reference and subscription cleanup applies to normal completion,
timeouts, interrupted waits, and process termination. The implementation is
shared by the x86 and x64 syscall backends.

## Address-space mapping transactions

Each address space contains a recursive mapping mutex. Anonymous mapping
allocation holds this mutex from address selection through region insertion.
Region replacement, splitting, removal, protection changes, heap adjustment,
and mapping relocation use the same mutex. Enumeration retains ownership while
callbacks inspect or duplicate regions. The VMA spinlock protects individual
tree accesses; the mapping mutex protects operations that span multiple tree
accesses. Recursion permits mapping helpers to participate in an enclosing
transaction without releasing ownership between steps.

VM mapping and fault-lock ownership is counted per task. Forced thread-group
termination permits lock owners to continue scheduling until they release their
VM locks. Reaping waits for both active CPU execution and VM-lock ownership to
end, preventing an abandoned lock from blocking address-space cleanup.

## Validation

`test/fd_callback_lifetime_host.py` executes the production descriptor and
poll-table functions with AddressSanitizer and UndefinedBehaviorSanitizer.
The checks cover callback lock ownership, descriptor installation from a
callback, descriptor closure during an ioctl, multiple readiness subscriptions,
poll-table reuse, subscription cancellation, and task I/O cancellation.

`test/fd_callback_threads.py` executes inside the guest. It transfers 5,000
file descriptors with `SCM_RIGHTS` while another thread polls the socket and
executes `FIONREAD`. It also exercises 64 descriptor-closure races with active
readiness subscriptions. Each transfer receives an acknowledgement before the
next ancillary message is sent.

`test/mmap_threads.c` allocates mappings concurrently in eight threads across
512 rounds. The checks require disjoint returned ranges, independent stored
values, successful protection changes, and successful unmapping.

`test/mmap_exit_threads.c` executes 32 child-process runs with eight mapping
threads per child. Each child exits while its threads allocate, protect, and
release mappings. Completion requires successful child reaping in every run.
