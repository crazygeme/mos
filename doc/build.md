# Build and Run

**Source:** `Makefile`, `build/config.mk`, `build/helpers.mk`,
`arch/x86/config.mk`, `arch/x64/config.mk`, and `run.sh`.

## Toolchains

Both x86 (i686) and x64 (AMD64) are implemented. `make` defaults to x86 and
release mode; `run.sh` defaults to x64 and release mode.

On Linux, both backends use host GCC and binutils. The x86 build requires
32-bit compilation support. Typical Debian/Ubuntu dependencies are:

```sh
sudo apt install build-essential gcc-multilib qemu-system-x86 qemu-utils \
  python3 dnsmasq unzip iproute2 iptables
sudo apt install gdb-multiarch  # optional debugger
```

On macOS, install QEMU and Python, plus GCC and binutils cross toolchains
with the prefixes selected by the architecture configuration:

- `i686-elf-*` for x86.
- `x86_64-elf-*` for x64, including `objcopy` for the flat boot image.

## Build configurations and artifacts

```sh
make ARCH=x86 BUILD=release
make ARCH=x64 BUILD=release
make ARCH=x64 BUILD=debug
make ARCH=x64 test
make ARCH=x64 test-debug
make ARCH=x64 BUILD=release rebuild
make ARCH=x64 BUILD=release clean
make format
```

Output lives in `out/<arch>/<build>/`. Both architectures produce `kernel`,
`kernel.dbg`, and `assemble.s`. Test builds produce `kernel-test`,
`kernel-test.dbg`, and `assemble-test.s`. The x64 build additionally produces
flat `kernel.boot` and `kernel-test.boot` images for Multiboot launch; the
ELF64 files retain debugging symbols. Shell scripts in `test/*.sh` are
converted into generated C sources and included in test kernels.

The common build is freestanding (`-nostdlib -nostdinc -fno-builtin`), disables
PIE, and enables debug symbols and strict warnings. x86 uses `-march=i686
-m32`; x64 uses `-march=x86-64 -m64 -mno-red-zone -mcmodel=kernel` and
`-mgeneral-regs-only`. Subtree `cflags.mk` files select optimization per build,
typically `-O0` for debug and `-O2` for release. Third-party lwext4 and lwIP
retain their own build flags.

## Launching the Red Hat 9 image

```sh
./run.sh                       # AMD64 release, two CPUs, 8192 MiB RAM
./run.sh arch=x86 ram=1024      # x86 release with 1 GiB RAM
./run.sh smp=4 ram=8192 kvm     # AMD64 with four CPUs and KVM
./run.sh bash                   # direct /bin/bash boot
./run.sh test                   # selected architecture's test kernel
./run.sh arch=x86 test debug    # x86 debug tests, paused for GDB
```

The script builds the selected kernel, uses `rh9.qcow2`, and extracts
`redhat9.img.zip` when the disk image is missing. It mounts the image to stage
configuration and test files from `tools/guest/` before launching QEMU.
Disk setup requires `sudo`. The RH9 root password is `123456`.

The x64 launch uses `qemu-system-x86_64`, the `qemu64` CPU model under software
emulation, and the flat boot image. KVM selects the host CPU model. The
virtual hardware includes VMware VGA, an IDE disk, AC97 audio, and an e1000
NIC with MAC `52:54:00:12:34:56`.

| Argument | Behavior |
| --- | --- |
| `arch=x86\|x64` | Select kernel architecture; default x64 |
| `ram=N` | Guest memory in MiB, 32–65536; default 8192 |
| `smp=N` | Virtual CPUs, 1–32; default 2 |
| `test` | Build and boot tests; use QEMU user-mode networking |
| `debug` | Select debug build; pause at startup with GDB on port 8888 |
| `profile` | Select debug build; expose `/tmp/qemu-profiler.sock` |
| `bash` | Boot directly into `/bin/bash` |
| `verbose` or `verbose=2` | Focused diagnostics |
| `verbose=1` | Full syscall tracing |
| `verbose=0` | Disable verbose logging |
| `kvm` | Enable Linux KVM acceleration |
| `logtofile` | Write serial output to `out/<arch>/<build>/krn.log` |
| `-h` | Print launcher help |

Set `MOS_AUDIO_BACKEND` to override the host audio backend, for example
`MOS_AUDIO_BACKEND=none ./run.sh test`. Linux defaults to PipeWire; macOS
defaults to CoreAudio.

## Networking

Non-test Linux boots create a user-owned TAP interface `tap0`, assign
`10.0.5.1/24`, enable IPv4 forwarding, add an iptables NAT rule for
`10.0.5.0/24`, and start dnsmasq for DHCP. These steps require `sudo`.
The script removes the TAP interface, NAT rule, and dnsmasq process on exit.
macOS uses QEMU `vmnet-shared` and launches QEMU with `sudo`. Test launches
use QEMU user-mode networking and skip TAP setup.

## Debugging and profiling

```sh
./run.sh bash debug
# In another terminal on Linux:
gdb-multiarch out/x64/debug/kernel.dbg
```

Attach in GDB with `target remote :8888`, then `continue`. For x86, launch
with `arch=x86` and load `out/x86/debug/kernel.dbg`. On macOS, use the matching
cross debugger.

```sh
./run.sh arch=x86 profile
./tools/profile.py
./tools/profile_stack.py
```

The sampling tools connect to `/tmp/qemu-profiler.sock`; reports go under
`out/`. See [Profiling](profiling.md) for commands and measurement limits.

## Userspace images

The bundled launcher manages the RH9 image. For modern GNU userspace with
glibc 2.42 and an Xfce desktop on x86 or x64, follow
[GNU/MOS](https://github.com/crazygeme/gnu-mos)'s build, setup, and run
instructions. Each architecture has its own userspace and disk image.

For offline RH9 changes, use `./tools/mountdisk.sh`, copy files into the
mounted tree, and run `./tools/umountdisk.sh` before launching QEMU.
See [Disk Image](disk_image.md) for details.
