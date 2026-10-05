# AMD64 backend

The x64 backend includes long-mode boot, four-level paging, compatibility and
native syscall entry, per-CPU task state, and APIC-based SMP. Output is placed
in `out/x64/<build>`. The flat `kernel.boot` image is used for Multiboot launch;
`kernel.dbg` retains ELF64 debugging symbols.

Architecture sources are grouped by subsystem, with public headers at module
roots and implementations under `impl/`. The shared i386 syscall namespace
resides in `arch/abi/i386`.

[Architecture and validation status](../../doc/bugfix_journal.md#2026-10-05---amd64-kernel-and-process-abi) describes ABI layouts,
memory ownership, SMP, launch commands, verified runtime configurations, and
current limits.

The backend executes ELF32/i386 binaries in compatibility mode and ELF64/AMD64
binaries in long mode. ELF preparation selects format-specific readers, stack
construction, user register initialization, and VM limits. Syscall adapters
serialize their namespace's wire layouts. Shared services receive internal
records without inspecting the executable mode. Fork and clone inherit the
saved user context. The architecture interprets its code selector when
restoring segments, delivering signals, and installing clone TLS.
The default `run.sh kvm test` launch uses the i386 programs in `rh9.qcow2`.
