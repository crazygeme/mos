# Architecture backends

Each target architecture owns its toolchain flags, linker script, private
headers, and architecture-specific sources under `arch/<arch>/`.

- `x86`: existing 32-bit i686 kernel; output in `out/x86/<build>`.
- `x64`: reserved x86-64 backend; output in `out/x64/<build>` once enabled.

Common kernel code remains under `src/`. Architecture-specific code is grouped
directly by subsystem, for example `arch/x86/mm` and `arch/x86/ps`; there is no
extra `src` directory. Architecture-specific headers live under
`include/arch/<arch>/`. The build selects only the source and header directories
matching `ARCH` through include-path priority; headers do not dispatch with
architecture preprocessor conditionals.
