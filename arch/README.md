# Architecture backends

Each target architecture owns its toolchain flags, linker script, private
headers, and architecture-specific sources under `arch/<arch>/`.

- `x86`: existing 32-bit i686 kernel; output in `out/x86/<build>`.
- `x64`: reserved x86-64 backend; output in `out/x64/<build>` once enabled.

Common kernel code remains under `src/`. New architecture-specific code should
be placed in `arch/<arch>/src` and headers in `arch/<arch>/include`; the build
selects only the directory matching `ARCH`.
