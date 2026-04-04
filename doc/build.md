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
- `./run.sh verbose` — Boot with serial log output
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

Profiling uses QEMU's HMP monitor to sample EIP and maps it to kernel symbols.

```sh
# Terminal 1 — start kernel with monitor socket exposed
./run.sh profile

# Terminal 2 — attach profiler (waits for your signal before sampling)
./tools/profile.py
```

`profile.py` loads symbols from `out/kernel.dbg`, connects to the QEMU monitor
socket at `/tmp/qemu-profiler.sock`, then waits for you to press Enter. Start
your workload in the guest first, then press Enter to begin sampling. Press
Ctrl-C at any time to stop early and print the report.

Options:

```sh
./tools/profile.py --samples 1000   # number of EIP samples (default: 500)
./tools/profile.py --delay 2        # ms between samples (default: 5)
```

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
