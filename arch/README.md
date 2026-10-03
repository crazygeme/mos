# Architecture backends

Each target architecture owns its toolchain flags, linker script, private
headers, and architecture-specific sources under `arch/<arch>/`.

- `x86`: existing 32-bit i686 kernel; output in `out/x86/<build>`.
- `x64`: AMD64 kernel with i386 compatibility and APIC SMP.
  See [architecture and validation status](../doc/x64.md). Output is in `out/x64/<build>`.

Common kernel code remains under `src/`. Architecture-specific code is grouped
directly by subsystem, for example `arch/x86/mm` and `arch/x86/ps`. Public
headers live at each module root and implementations live below `impl/`, for
example `arch/x86/mm/mmu.h` and `arch/x86/mm/impl/cpu.c`.

The build puts `arch/<arch>` before the common `src` tree on the include path.
Architecture headers are therefore selected by the build and never dispatched
with architecture preprocessor conditionals. Toolchain glue required under the
`arch/` include namespace remains in `arch/<arch>/arch/`.
