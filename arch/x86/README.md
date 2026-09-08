# x86 backend

This is the active 32-bit i686 backend. Architecture-specific implementations
are grouped directly by subsystem (`boot`, `int`, `mm`, and `ps`) without an
extra `src` directory. Architecture-neutral process, VM, and interrupt policy
remains in the top-level `src` tree.
