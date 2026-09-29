# Kernel stack storage

MOS x86 task stacks share one 4 KiB allocation with the task descriptor.
The usable call stack is therefore smaller than one page. Bootstrap uses
a separate stack. User processes have an independent stack limit of
`USER_STACK_PAGES * PAGE_SIZE` (16 MiB with the current configuration).

## Buffer ownership

| Path | Storage and lifetime |
| --- | --- |
| `strstr()` | KMP prefix table on the heap; allocation failure uses a constant-stack search without workspace. The fallback can take quadratic time. |
| `kvformat()` | 64-byte automatic output buffer, flushed incrementally to its callback. |
| `printk()` | Static 512-byte record payload plus length, protected by `printk_record_lock`. Lock order is record lock, active TTY lock, then log lock when emitting records. |
| `log_read()` / `sys_syslog()` | Shared static 2144-byte formatting buffer. Formatting and copying both complete under `log_lock`; readers release the lock before waiting. |
| ATA IDENTIFY | Heap sector buffer, released before partition discovery and diagnostic output. |
| ext4 directory checks and mkdir | Heap `ext4_dir` descriptors, including their embedded 255-byte filenames. |
| Kernel command-line parsing | Static 256-byte copy used only during single-threaded bootstrap. |
| Kernel syslog tests | Heap scratch buffers owned by test wrappers, including when a helper returns through a fatal assertion. |
| Sysfs attributes and directory listings | Heap buffers owned by the open file; PCI resource tables belong to heap-allocated device data. |

Small bounded automatic objects remain, including 32-byte PCI names,
109-byte UNIX pathname buffers, and eight-element descriptor arrays.
Their sizes alone do not establish a bound for the complete call chain.

## Source coverage and configuration

Stack-storage inspection covers `src/`, `arch/`, kernel C tests, `tools/`,
and `third_party/`, including aggregate types containing arrays and
inline/header declarations. Shell tests containing C programs execute
those programs in userspace; their source is embedded in the kernel as
static strings by `tools/gen_ktest_scripts.sh`.

`third_party/Makefile` selects lwIP's `LWIPNOAPPSFILES`.
`src/lwipopts.h` sets `PPP_SUPPORT=0`, `LWIP_NETCONN=0`, `LWIP_SOCKET=0`,
and `LWIP_IPV6=0`. Larger automatic arrays in PPP authentication/logging,
application examples, and standalone third-party tests do not execute on
the current kernel paths. These configurations must be included when
assessing the impact of enabling additional library features.

Large array members are not automatically stack allocations. PCI caches,
TTY state, task FPU storage, ext4 mount state, and GPU tables are backed by
static or heap storage. The partition-table structure declared inside
`read_partition_table()` is instantiated through a heap pointer.

## Validation limits

Source inspection and syntax checks do not measure compiler-generated
stack frames, register spills, inlining, interrupt nesting, or cumulative
call-chain depth. Recursive traversal in VFS, PCI bus enumeration, and
extended partition handling requires separate call-depth analysis.
No runtime stack-peak bound is established by this inventory.
