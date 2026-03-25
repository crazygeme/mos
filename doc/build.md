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

1. Run `profiling.sh` — it waits for an OS instance to attach to.
2. In another terminal: `./run.sh profile`
3. Press any key in the `profiling.sh` terminal to arm the profiler.
4. Run workloads inside MOS.
5. Press any key again to capture, then Ctrl-C to generate the report.

The `profile` kernel command-line flag enables scheduler instrumentation
(`TestControl.profiling = 1`) which records per-task cycle counts.

---

## Disk image

The RH9 userspace lives in `rh9.qcow2`. A pre-built image is provided as
`redhat9.img.zip`. To mount and modify it:

```sh
sudo modprobe nbd
sudo qemu-nbd -c /dev/nbd0 rh9.qcow2
sudo mount /dev/nbd0p1 mnt/

# make changes...

sudo umount mnt/
sudo qemu-nbd -d /dev/nbd0
```
