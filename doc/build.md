# Build And Run

**Source:** `Makefile`, `build/config.mk`, `build/helpers.mk`, `run.sh`

## Toolchains

MOS builds as a 32-bit freestanding i686 kernel.

### Linux

On Linux the tree uses the host binutils and GCC directly:

- `gcc`
- `ld`
- `ar`
- `strip`
- `objdump`

You need multilib support because the kernel is built with `-m32`.

Typical Debian/Ubuntu packages:

```sh
sudo apt update
sudo apt install build-essential gcc-multilib qemu-system-x86 qemu-utils python3 dnsmasq unzip
```

Optional debug tools:

```sh
sudo apt install gdb-multiarch
```

### macOS

On macOS the tree uses an `i686-elf-*` cross toolchain:

- `i686-elf-gcc`
- `i686-elf-ld`
- `i686-elf-ar`
- `i686-elf-strip`
- `i686-elf-objdump`

Typical Homebrew setup:

```sh
brew install qemu python dnsmasq unzip
brew tap nativeos/i386-elf-toolchain
brew install i386-elf-binutils i386-elf-gcc
brew install i386-elf-gdb
```

## Build Flags

The main compile settings come from `build/config.mk`, `build/helpers.mk`, and `Makefile`.

Important defaults today:

- freestanding build: `-nostdlib -nostdinc -fno-builtin`
- no PIE: `-fno-pie`
- debug info enabled: `-ggdb3`
- target ISA: `-march=i686 -m32`
- optimization level: `-O0`
- warnings: `-Werror -Wall`

Third-party libraries linked into the kernel:

- `third_party/lwext4`
- `third_party/lwip`

Build output goes under `out/`.

## Main Targets

```sh
make          # build out/kernel
make test     # build out/kernel-test
make run      # build out/kernel, then run ./run.sh
make run-test # build out/kernel-test, then run ./run.sh test
make rebuild  # clean + build
make clean    # remove out/
make format   # clang-format src/, include/, test/
```

Generated artifacts worth knowing about:

- `out/kernel`
- `out/kernel.dbg`
- `out/assemble.s`
- `out/kernel-test`
- `out/kernel-test.dbg`
- `out/assemble-test.s`

Shell-script tests in `test/*.sh` are converted into generated C sources before the test kernel is linked.

## Running

All normal guest boots go through `run.sh`.

```sh
./run.sh
```

Current behavior:

- RAM size is fixed at `512` MB.
- The guest disk image is `rh9.qcow2`.
- If the disk image is missing, `run.sh` extracts `redhat9.img.zip`.
- QEMU boots `out/kernel` by default, or `out/kernel-test` in test mode.
- The NIC device is QEMU `e1000` with MAC `52:54:00:12:34:56`.

## `run.sh` Arguments

Supported arguments in the current script:

- `test`
- `debug`
- `profile`
- `bash`
- `verbose`
- `verbose=0`
- `verbose=1`
- `verbose=2`
- `curses`
- `kvm`
- `logtofile`

Examples:

```sh
./run.sh
./run.sh bash
./run.sh bash debug
./run.sh verbose=1
./run.sh logtofile
./run.sh test
./run.sh profile
```

### Meaning of the flags

- `test`: boot `out/kernel-test` and switch QEMU networking to user mode
- `debug`: start QEMU paused with GDB stub on `tcp::8888`
- `profile`: boot `out/kernel.dbg` and expose a QEMU monitor socket at `/tmp/qemu-profiler.sock`
- `bash`: append `bash` to the kernel command line so the guest boots directly to `/bin/bash`
- `verbose`: shorthand for focused diagnostic logging
- `verbose=0`: disable verbose logging
- `verbose=1`: full syscall-trace style logging
- `verbose=2`: focused diagnostic logging
- `curses`: use the current terminal as the guest console; also forces serial log output to `out/krn.log`
- `kvm`: pass `-enable-kvm` on Linux
- `logtofile`: write the serial log to `out/krn.log`

## Networking During Run

Current behavior:

- non-test runs automatically set up host-side networking before QEMU starts
- Linux uses a TAP interface `tap0` plus host NAT and `dnsmasq`
- macOS uses QEMU `vmnet-shared`
- test mode skips all of that and uses QEMU user-mode networking instead

### Linux host setup

For non-test boots on Linux, `run.sh` does all of the following:

- creates `tap0`
- assigns `10.0.5.1/24`
- enables `net.ipv4.ip_forward=1`
- adds an iptables masquerade rule for `10.0.5.0/24`
- starts `dnsmasq` for DHCP on `tap0`

The script tears this setup down on exit.

Because of that, Linux non-test runs require `sudo` for the host networking steps.

## Debugging

To debug with GDB:

```sh
./run.sh bash debug
```

Then attach from another terminal:

```sh
gdb-multiarch out/kernel      # Linux
i386-elf-gdb out/kernel      # macOS
```

Inside GDB:

```gdb
target remote :8888
continue
```

If you want symbols that exactly match the running image, use `out/kernel.dbg`.

## Profiling

`./run.sh profile` boots the debug kernel image and opens a QEMU monitor socket:

- `/tmp/qemu-profiler.sock`

That is what the local profiling scripts talk to.

Typical flow:

```sh
./run.sh profile
./tools/profile.py
./tools/profile_stack.py
```

The generated reports are written under `out/`.

## Disk Image Workflow

The userspace image is `rh9.qcow2`.

To modify it offline:

```sh
./tools/mountdisk.sh
./tools/umountdisk.sh
```

Those helpers mount the qcow2 image so files can be copied in and out. Always unmount before launching QEMU again.
