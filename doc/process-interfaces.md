# Process execution and interface layouts

The x86 backend accepts ELF32 images with machine type `EM_386`. The x64
backend accepts ELF32/i386 and ELF64/AMD64 images. The executable and its
interpreter must select the same format.

ELF preparation selects a format descriptor from the architecture's supported
format table. The descriptor provides typed header and program-header readers,
an initial-stack builder, user register initialization, and VM limits. Stack
construction uses a fixed word type for each format. The VM retains its task
size, mmap base, and brk limit. Fork and clone copy these limits and the saved
user register context.

The i386 and AMD64 syscall namespaces select their wire adapters directly.
Shared-memory control returns an internal status record. Its adapters serialize
56-byte i386 and 112-byte AMD64 records. Socket timestamp retrieval returns an
internal timeval, which the AMD64 ioctl adapter converts to two signed 64-bit
fields. Ptrace adapters use fixed-width memory words and register layouts for
the caller's namespace, independently of the traced task's execution mode.
The architecture backend captures the stopped CPU register context.

Robust-list registration records the head address, head size, and typed reader
on the thread. Exit cleanup traverses the list with that reader. The getter
serializes the registered address and size using its syscall namespace.
Executable initialization supplies a 12-byte i386 or 24-byte AMD64 default
head size. Fork and clone clear the registered head and reader.

Signal frame construction resides in the interface implementations. The
architecture selects delivery from the saved user code selector and interprets
clone TLS at the register-context boundary. Shared signal disposition logic
does not select an executable format.

Memory services validate reserved ranges through the architecture backend.
Physical allocation limits, DMA limits, allocation preferences, and block-cache
limits are architecture configuration values. NX enforcement follows the paging
hardware and applies to both i386 and AMD64 processes on x64. Non-PAE x86 page
tables do not provide NX support.

## Validation

`python3 test/abi_adapters.py` compiles production adapters against isolated
host-side kernel services and runs them with undefined-behavior checks. It
validates robust readers and cross-layout getters, ptrace word and register
serialization, shared-memory records, ELF header conversion, initial stacks,
and executable register initialization. These checks do not exercise kernel
scheduling or privilege transitions.

`python3 test/syscall_tables.py` validates syscall numbers and service coverage
for both namespaces. The embedded kernel tests validate ELF image acceptance
and page execute permissions. Guest validation uses `./run.sh kvm test` from
the MOS source directory with the configured RH9 image. Native AMD64 guest
interfaces require an AMD64 userspace image.
