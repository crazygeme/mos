# RH9 Display Interfaces

## Kernel entry points

RH9 XFree86 4.3.0 uses the IA-32 syscall namespace on both kernel
architectures. `arch/abi/i386/calls.def` dispatches `ioctl` (54), `mmap` (90),
`ioperm` (101), `iopl` (110), `vm86old` (113), `vm86` (166), and `mmap2`
(192). IA-32 pointers are zero-extended from 32-bit argument registers.
`mmap2` converts page offsets to byte offsets using 64-bit arithmetic.

Console ioctls use complete command identity. Keyboard and display commands
occupy type `0x4b`; virtual-terminal commands occupy type `0x56`.
`KDSETMODE` (`0x4b3a`) accepts the mode as an immediate argument.
`KDGETMODE` (`0x4b3b`) writes an integer through its argument pointer.
Entering graphics mode records the owning process and suspends text rendering
on the selected virtual terminal.

`/dev/mem` mappings expose physical memory directly, including framebuffer
and firmware addresses. The IA-32 VM86 service emulates selected VBE BIOS
operations. Its VMware modes use 32-bit pixels and a row length of four bytes
per pixel.

## Framebuffer storage

XFree86 searches platform-specific module directories before the generic
module directory. RH9's `modules/linux/libint10.a` invokes the IA-32 VM86
service and receives the MOS VBE mode list. Complete directory enumeration,
including the final entry, is required for that module search.

The VMware SVGA console driver configures 32-bit framebuffer storage. Pixel
depth and framebuffer storage width are distinct: depth 24 can use either
three or four bytes per pixel. XFree86's VESA driver prefers 24-bit storage
when the BIOS advertises it. Its framebuffer writes must use the same pixel
storage width and row length as the active display device.

XFree86 selects four-byte framebuffer pixels with the following invocation
from an RH9 text console:

```sh
startx -- -fbbpp 32 -fp /usr/X11R6/lib/X11/fonts/misc
```

The equivalent configuration setting is `DefaultFbBpp 32` in the `Screen`
section of `/etc/X11/XF86Config`, alongside `DefaultDepth 24`.

The server log `/var/log/XFree86.0.log` reports framebuffer bpp, virtual
dimensions, pitch in pixels, BIOS identification, and selected VBE modes.
Those values describe the server's selected format; the display device's
active format must agree. Syscall namespace validation is available through
`python3 test/syscall_tables.py`. That audit checks declarations and service
coverage; display correctness requires guest execution.
