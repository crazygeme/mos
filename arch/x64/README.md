# x64 backend

This directory owns the future x86-64 implementation: long-mode bootstrap,
descriptor tables, traps, context switching, page tables, and linker script.

The common build reserves `out/x64/<build>` and supplies the x86-64 compiler
and linker flags. `ARCH_READY` remains disabled until the backend has a valid
entry point and linker script.
