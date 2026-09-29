# Devices and drivers

## Source ownership

| Directory | Responsibility |
| --- | --- |
| `src/device` | Bus discovery, device inventory, class interfaces, and device attributes |
| `src/device/impl/pci` | PCI enumeration, configuration access, BAR resources, PCI sysfs entries |
| `src/device/impl/ps2` | i8042 configuration, PS/2 port discovery, command transport, IRQ dispatch, serio sysfs entries |
| `src/driver` | Driver registration, matching, hardware protocols, and concrete hardware implementations |
| `src/driver/impl/net/intel_nic_e1000.c` | Intel e1000-family network controller driver |
| `src/driver/impl/input` | PS/2 keyboard translation and mouse command/packet protocols |
| `src/driver/impl/video` | Bochs, VMware SVGA, VirtIO GPU, and DRM class attributes |
| `src/dev` | File endpoints, device numbers, and `/dev` node publication |

Both device and driver directories have build flags discovered by the normal
recursive source build. Public includes use `device/`, `driver/`, and `dev/`.
The TTY interface resides in `dev/tty.h`.

## Discovery and binding

`device_t` owns the bus identity, bus-local address, selected driver,
successful binding, and probe result. PCI records additionally retain IDs,
class, and BAR resources. Device identity includes the bus, so a PS/2 port
number cannot collide with a PCI address. Records remain valid for the kernel
lifetime, including unbound devices.

`DRIVER_REGISTER(descriptor)` puts `driver_t` descriptors in the linker
registry. `drivers_init()` registers them before enumeration. Registration
order has no matching or initialization semantics. PCI drivers declare
vendor/device IDs and optional class masks. PS/2 drivers declare their port
and receive callback. Exactly one matching driver is required for selection.
Overlapping matches, including exact-ID and class-based matches, leave the
device unbound and log the conflicting driver names. Duplicate registration
of the same descriptor is idempotent. Multiple matching ID entries within one
descriptor still represent one driver. Initialization timing is determined by
bus discovery and the device initialization phase.

`pci_scan()` executes once on the bootstrap CPU before application processors
start. It records functions, caches BAR sizes, and selects drivers. Early
framebuffer probes and console callbacks run during discovery. Ordinary PCI
probes run in `devices_init()` at init level 2, before root mounting at level 3.
`pci_for_each()` and `pci_get_resources()` only inspect PCI records; they
neither enumerate hardware nor include PS/2 records.

`ps2_scan()` executes once after process and interrupt setup, before AP startup.
The i8042 controller configures the keyboard and auxiliary ports and registers
both IRQ handlers. The bus publishes port objects and probes their registered
protocol drivers. Keyboard input uses the firmware-configured translated scan
code stream. The mouse driver confirms command acknowledgements before binding.
Controller output is routed by its auxiliary-channel status bit. During a
command transaction, replies are consumed by the command path rather than the
interrupt path. Keyboard worker startup occurs after PID 0 and PID 1 are reserved.

A probe runs at most once per selected device. Success records the binding;
failure records the error and leaves the device unbound. Automatic retries,
fallback drivers, runtime rescanning, and hotplug are not implemented.
`DEVICE_BUS_USB` reserves a bus identity; USB host-controller drivers and USB
enumeration are not implemented, so USB peripherals are not discovered.

## Filesystem views

`DEV_INIT` callbacks publish available device interfaces after `/dev` is mounted
at init level 6. Hardware initialization completes before node publication.
The mouse node is published only for a bound auxiliary device. Virtual devices
such as null, zero, PTYs, and loop devices also use this layer. Physical devices
without character or block interfaces appear in `/sys` without `/dev` nodes.

`device/impl/sysfs.c` owns the shared sysfs tree and filesystem mount entry.
Each bus or driver publishes its own attributes. The filesystem represents
both devices and drivers; it does not own hardware discovery or initialization.

- PCI entries are populated by `device/impl/pci/sysfs.c` at init level 5.
  `/sys/bus/pci/devices` indexes canonical entries under
  `/sys/devices/pci0000:00`; bridge topology is represented as a flat inventory.
- PS/2 entries are populated by `device/impl/ps2/sysfs.c` at init level 5.
  `/sys/bus/serio/devices` indexes ports under `/sys/devices/platform/i8042`.
- Bus `drivers` directories list registered drivers. Successful bindings have
  reciprocal links between device and driver entries.
- `driver/impl/video/drm_sysfs.c` publishes DRM class and character-device
  indexes when ready GPU endpoints are published at init level 6.

`test/device_test.c` covers matching conflicts, duplicate registration, probe results,
bus isolation, and repeated scan calls. `test/sys_devices.sh` checks device
aliases, binding links, and inventory stability across sysfs mounts.
