# Build Dependencies & Running

## Install dependencies

### Ubuntu / Debian

```sh
sudo apt update
sudo apt install build-essential gcc-multilib qemu-system-x86 python3 gdb-multiarch dnsmasq
```

On Linux, the system `gcc` is used directly with `-m32 -march=i686`.
`gcc-multilib` provides the 32-bit headers and libraries needed for cross-compilation.

### macOS (Homebrew)

```sh
brew install qemu python dnsmasq

# i686-elf cross-compiler (via nativeos tap)
brew tap nativeos/i386-elf-toolchain
brew install i386-elf-binutils i386-elf-gcc

# Optional: GDB for debugging
brew install i386-elf-gdb
```

On macOS, the build uses `i686-elf-gcc` / `i686-elf-ld` instead of the system tools.

---

## Building

```sh
make            # build kernel → out/kernel
make rebuild    # clean + build
make test       # build test kernel → out/kernel-test
make clean      # remove build artifacts
```

---

## Running

All run modes go through `run.sh`:

- `./run.sh` — Boot into SysV init (`/sbin/init`)
- `./run.sh bash` — Boot directly into `/bin/bash`
- `./run.sh bash debug` — Boot into bash, wait for GDB on `tcp::8888`
- `./run.sh tap bash` — Boot into bash with TAP/NAT networking
- `./run.sh verbose` — Boot with focused serial diagnostics (`verbose=2`)
- `./run.sh verbose=1` — Boot with full syscall trace logging
- `./run.sh verbose=0` — Boot with verbose logging disabled
- `./run.sh kvm` — Enable KVM acceleration (Linux only)
- `./run.sh curses` — Use current terminal as VM console (no GUI window)
- `./run.sh logtofile` — Write kernel log to `out/krn.log` instead of stdio

### TAP networking

`./run.sh tap` creates a `tap0` interface (`10.0.5.1/24`), enables IP
forwarding, sets up iptables masquerade, and starts `dnsmasq` for DHCP.
Requires `sudo` for the network setup steps.

---

## Debugging

```sh
./run.sh bash debug        # start QEMU paused, listening on tcp::8888

# in another terminal:
gdb-multiarch out/kernel   # Linux
i386-elf-gdb out/kernel    # macOS

(gdb) target remote :8888
(gdb) continue
```

---

## Profiling

See **[Profiling](profiling.md)** for full documentation, including flamegraph
usage and an annotated example.

Quick start:

```sh
# Terminal 1 — start kernel with monitor socket exposed
./run.sh profile

# Terminal 2 — flat EIP histogram
./tools/profile.py

# Terminal 2 — call-stack flamegraph (SVG written to out/)
./tools/profile_stack.py
```

Press **Ctrl-C** to stop sampling; the guest resumes automatically.

---

## Disk image

The RH9 userspace lives in `rh9.qcow2`. A pre-built image is provided as
`redhat9.img.zip`. Use the helper scripts to mount and modify it:

```sh
./tools/mountdisk.sh    # mount rh9.qcow2 → mnt/
# make changes (sudo cp ... mnt/bin/, etc.)
./tools/umountdisk.sh   # unmount and disconnect NBD
```

Always unmount before launching QEMU — the kernel flushes dirty pages on
unmount, so skipping this step can leave the image in an inconsistent state.

`mountdisk.sh` uses `qemu-nbd` to attach the qcow2 image to `/dev/nbd0` and
mounts partition 1 at `mnt/`. Requires `qemu-utils` (Linux) or `qemu` (macOS).
