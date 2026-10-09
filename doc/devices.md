# Devices and drivers

## Source ownership

| Directory | Responsibility |
| --- | --- |
| `src/device/core` | Device inventory, character-device dispatch, block-device I/O and lifecycle, generic device/FIFO nodes, and pixel display services |
| `src/device/pci` | PCI enumeration, configuration access, BAR resources, and PCI attributes |
| `src/device/ps2` | i8042 transport, port discovery, IRQ routing, and port attributes |
| `src/device/platform` | Fixed PC platform-device discovery, including CMOS RTC |
| `src/device/virtual` | Virtual-device inventory, including TTY, PTYs, loop, and kernel pseudo devices |
| `src/driver` | Registered-driver matching, probing, and concrete hardware and virtual drivers |
| `src/driver/audio`, `input`, `storage` | Hardware protocols and their file endpoints in the same driver |
| `src/driver/tty` | Virtual terminal and PTY drivers, termios and ANSI state |
| `src/console` | Shared character/font/cursor rendering on pixel surfaces |
| `src/driver/video` | Display hardware, pixel surfaces, mode synchronization and update submission |
| `src/fs/impl/devfs.c` | Device filesystem root and mount |
| `src/fs/impl/sysfs.c` | Shared attribute tree and sysfs mount views |
| `src/fs/impl/entries.c` | Shared directory, attribute, snapshot, alias, and provider-node presentation |

There is no `src/dev` directory or `impl` layer under device or driver.
`device/chardev.h` and `device/blockdev.h` expose the respective device
subsystems. `device/devnode.h` owns device-number encoding and the generic
node backend used by entries and ordinary filesystem `mknod`.
`device/devnums.h` contains the established device numbers.

Per-hardware audio, mouse, disk, keyboard, serial, font, and timing headers
have been removed. Audio, mouse, and ATA protocol helpers are private to their
drivers. TTY state and helpers are private. `console/render.h` owns character
rendering and fonts; `device/framebuffer.h` owns pixel-device operations.
`driver/driver.h` owns discovery and driver binding, without display contracts.
Network drivers use `net/net.h`; clocks and logging use the kernel utility interface.

## Discovery and binding

`device_register()` records an inventory item; it never probes a driver.
`device_t` retains bus identity, bus-local address, PCI IDs and resources,
selected driver, probe result, successful binding, and its sysfs entry.
Records persist for the kernel lifetime, including unbound hardware.

`DRIVER_REGISTER` places descriptors in the linker registry. `drivers_init()`
registers them before enumeration. Registration order has no matching
semantics. PCI drivers match vendor/device IDs and optional class masks;
PS/2 drivers match port identities; virtual drivers match `virtual_id`;
platform drivers match platform-device identities.
Exactly one matching driver is required. Conflicts leave the item unbound.
A probe runs at most once; failure records an error without a successful binding.
Automatic retries, runtime rescans, and hotplug are not implemented.

PCI and virtual discovery precede the early driver pass. Display drivers load
before the virtual TTY driver, which initializes the bootstrap console and its
endpoints through the same matching and probing path as other virtual devices.
PS/2 discovery runs after process and IRQ setup; its early protocol drivers then
load before AP startup. Platform discovery exposes the CMOS RTC, whose driver
creates its character endpoint during probe. Driver worker tasks start through
registered callbacks after the bootstrap PID reservations. Ordinary drivers load at init level 2, before the root
filesystem mounts at level 3. These phases preserve the scheduling and IRQ
requirements of disk and network initialization.

PCI and PS/2 scans execute once. Querying their inventory does not rescan or
initialize hardware. Bus identity prevents PS/2 port numbers from colliding
with PCI addresses. `DEVICE_BUS_USB` reserves an identity; USB enumeration and
host-controller drivers are not implemented.

## File endpoints and attributes

A successful driver probe declares its endpoints with `vfs_entry_device()`.
This combines node creation and registration of the appropriate character or
block dispatch handler. The device-number registry remains independent of
path placement, so an ordinary `mknod` with the same major/minor dispatches to
the same driver. `vfs_entry_device()` also supports FIFO nodes.

Drivers use entries directories for `/dev/input` and `/dev/dri`. The devfs tree
exists before mounting; loading a driver populates the tree immediately.
Mounting `/dev` at init level 6 publishes that tree without initializing any
drivers. There is no `DEV_INIT` macro or device initialization linker table.
Virtual drivers publish null, zero, random, memory, PTY, loop, and init-control
endpoints through the same flow. `/dev/fd` delegates to the process filesystem.
`/dev/fd` itself is created by the matched virtual FD driver.
Mouse and GPU endpoints exist only after successful hardware initialization.

Buses describe discovered devices under `/sys`, including unbound hardware.
Registering a driver publishes its bus driver entry; successful binding adds
reciprocal device/driver links. Concrete drivers publish their additional
attributes during probe, including DRM attributes. Sysfs core code does not
call PCI, PS/2, or GPU registration functions.

Procfs static files use entries snapshots, including content larger than a page.
Its network and sysctl directories use entries as well. Providers register once
through `KERNEL_INIT`; each procfs mount creates a view of the shared tree.
`PROC_INIT` and its linker section have been removed. Dynamic process and
test providers retain their specialized lookup and file operations behind
entries nodes. Character/block class listings come from their device
subsystems; partition listings come from the generic block inventory.

## Block devices and filesystems

ATA and loop register capacity and sector I/O through `device/blockdev.h`.
The block layer validates sector ranges and pins the provider while a client
holds a handle. Detaching a mounted loop provider returns `EBUSY`. Storage
backends also provide generic flush, shutdown, cache accounting, and reclaim
operations; filesystem and memory-management clients do not call ATA helpers.

The ext4 filesystem creates its lwext4 adapter when mounting a selected block
device. ATA and loop do not include ext4 headers, construct ext4 structures, or
register ext4 devices. Filesystem registration (`fs_type`) remains independent
of bus-driver registration.

## Console and display ownership

The virtual TTY driver owns terminal state, ANSI parsing, termios, and character
nodes. It sends cells, cursor changes and text-row operations to the shared
renderer in `console/render.c`. Fonts live under `console/fonts`.

Display drivers register `framebuffer_ops` with the pixel-device core. That
interface provides an XRGB8888 surface with a byte pitch, live mode synchronization,
damage and presentation, optional pixel rectangle copy/fill acceleration, and
pixel snapshots for graphics VT switching. It contains no character-cell,
font, text cursor or text-row operations. CPU fallbacks copy overlapping rectangles
in the proper direction without temporary image arrays. VMware retains pixel
copy/fill acceleration; Bochs presents a shadow surface to physical display memory.

The periodic kernel task uses the console provider's `needs_refresh` and
`refresh` callbacks. It does not import the display driver or renderer interface.

Matched devices produce one device-information log line and one indented driver
line. Inventory records track whether that pair has been logged. Once the console
is ready, the existing records supply any deferred pairs without rescanning or
probing the device again.

## Validation sources

`test/device_test.c` covers matching conflicts, idempotent registration,
probe outcomes, bus isolation, repeated scans, inventory-only registration,
and virtual-device bindings. `test/blockdev_test.c` covers sector bounds,
I/O errors, and pinned-provider lifetime. `test/vfs_entries_test.c` covers aliases,
mount and open-file lifetime, device dispatch through both entries and ordinary
`mknod`, FIFO metadata, and snapshots larger than one page.
`test/proc_entries_test.c` checks independent procfs mount views, dynamic self
lookup, and open-file lifetime after unmount.

`test/test_console_renderer_host.py` exercises production rendering and the pixel
service on a replaceable surface with padded scanlines: glyph colors, cursor
erasure, overlapping scrolling, line insertion/deletion, invalid geometry and
mode changes. The guest device, PTY and loop scripts cover the filesystem-facing
interfaces. These are validation sources, not a claim that the final revision
has been run in a guest.
