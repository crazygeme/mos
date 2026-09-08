# x86 backend

This is the active 32-bit i686 backend. Architecture-specific implementations
are grouped directly by subsystem (`boot`, `int`, `mm`, and `ps`). Each module
keeps headers at its root and source below `impl/`. Architecture-neutral
process, VM, and interrupt policy remains in the top-level `src` tree using the
same layout.
