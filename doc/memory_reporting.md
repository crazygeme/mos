# Physical Memory Reporting

The physical page allocator registers usable Multiboot memory regions within
the architecture address limit. The i386 backend uses non-PAE paging and
manages physical addresses below 4 GiB. The AMD64 backend manages physical
addresses below the configured 128 GiB RAM mirror limit.

Low physical memory comprises allocator pages below the 768 MiB kernel
direct-map boundary. High physical memory comprises allocator pages at or
above that boundary. These classes describe physical allocator regions and
are independent of process virtual address limits.

`/proc/mos` reports physical totals, used memory, and free memory in bytes.
The `Raw` column contains exact unsigned 64-bit byte counts. The `Value`
column contains abbreviated binary units. Page counts are widened to 64 bits
before conversion to bytes. The formatter accepts `%h` for `unsigned`,
`%lh` for `unsigned long`, and `%llh` for `unsigned long long` values.

`/proc/meminfo` reports `LowTotal` and `HighTotal` in KiB. Each total multiplied
by 1024 equals the corresponding raw byte total in `/proc/mos`. Their sum
is the allocator-managed RAM total. Firmware reservations, physical address
holes, and boot-reserved allocator metadata are excluded; the reported total
can therefore be below the configured guest RAM size. Used and free values
are sampled separately for each proc file opening.

The proc totals can be inspected with:

```sh
cat /proc/mos
cat /proc/meminfo
```

The guest regression check is available in a test kernel:

```sh
sh /proc/tests/posix_mem_reporting
```
