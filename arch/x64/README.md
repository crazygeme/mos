# x64 backend

This directory owns the future x86-64 implementation. Code will be grouped by
subsystem (`boot`, `int`, `mm`, `ps`, and so on), with headers at the module
root and source below its `impl/` directory.

The common build reserves `out/x64/<build>` and supplies the x86-64 compiler
and linker flags. `ARCH_READY` remains disabled until the backend has a valid
entry point and linker script.
