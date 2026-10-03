# AMD64 backend

The x64 backend includes long-mode boot, four-level paging, compatibility and
native syscall entry, per-CPU task state, and APIC-based SMP. Output is placed
in `out/x64/<build>`. The flat `kernel.boot` image is used for Multiboot launch;
`kernel.dbg` retains ELF64 debugging symbols.

Architecture sources are grouped by subsystem, with public headers at module
roots and implementations under `impl/`. The shared i386 syscall namespace
resides in `arch/abi/i386`.

[Architecture and validation status](../../doc/x64.md) describes ABI layouts,
memory ownership, SMP, launch commands, verified runtime configurations, and
current limits.
