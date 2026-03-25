# ATA/IDE Hard Disk Driver

**Source:** `src/hw/hdd.c`
**Header:** `include/hw/hdd.h`
**Config:** `include/config.h` (`HDD_CACHE_*`)

---

## Overview

The ATA/IDE driver provides sector-level access to IDE disks with two transfer modes:

| Mode | Trigger | Transfer mechanism |
|---|---|---|
| **Bus Master DMA** | PCI IDE controller BAR4 found | PRDT → hardware DMA → bounce buffer → caller |
| **PIO fallback** | No DMA available | Busy-loop `REP INSD`/`REP OUTSD` |

Above the raw disk layer sits a **write-back LRU block cache** (enabled by `HDD_CACHE_OPEN=1`) and an **lwext4 block device interface** for mounting ext4 partitions.

---

## Layer Stack

```
 ext4_blockdev (lwext4)
        │
 hdd_bdev_bread / hdd_bdev_bwrite
        │
 partition_cache_read / partition_cache_write   ← LRU cache (HDD_CACHE_OPEN)
        │
 partition_read / partition_write               ← adds partition start offset
        │
 disk_read / disk_write                         ← DMA or PIO, one sector (busy-loop)
        │
 ATA channel hardware (IRQ 14 / IRQ 15 — registered but not used for sync)
```

---

## PCI Bus Master DMA

### Detection

During `hdd_init`, `pci_scan` walks all PCI devices looking for:

```
PCI_CLASS    = 0x01  (Mass Storage)
PCI_SUBCLASS = 0x01  (IDE)
BAR4 bit 0   = 1     (I/O space BAR)
```

When found, the Bus Master I/O base is extracted:

```c
g_bm_base = (unsigned short)(bar4 & ~0x3u);
```

The PCI Command register is then updated to enable both **I/O space** (bit 0) and **Bus Master** (bit 2).

### Per-channel resources

`dma_init_channel` runs after device identification and allocates:

| Resource | Size | Notes |
|---|---|---|
| `prdt` (PRDT entry) | 8 bytes | Heap-allocated; phys = virt − `KERNEL_OFFSET` |
| `dma_buf` (bounce buffer) | 4 KiB (1 page) | `vm_alloc(1)`; page-aligned, never crosses 64 KiB boundary |

Channel BM base offsets:

| Channel | BM base |
|---|---|
| Primary (ide0) | `g_bm_base + 0` |
| Secondary (ide1) | `g_bm_base + 8` |

### Physical Region Descriptor Table (PRDT)

```c
typedef struct {
    uint32_t phys_addr;   // physical address of DMA buffer
    uint16_t byte_count;  // 0 = 64 KiB
    uint16_t eot;         // bit 15 set = end-of-table
} __attribute__((packed, aligned(4))) prdt_entry_t;
```

Each transfer uses a single-entry PRDT covering exactly 512 bytes (one sector).

### Bus Master registers

| Offset | Name | Direction | Purpose |
|---|---|---|---|
| +0 | `BM_CMD` | W | bit 0 = start, bit 3 = direction (1=read, 0=write) |
| +2 | `BM_STATUS` | R/W | bit 0 = active, bit 1 = error, bit 2 = IRQ (W1C) |
| +4 | `BM_PRDT` | W | physical address of PRDT |

### DMA read sequence

```
select_sector → clear BM_STATUS → write PRDT phys addr →
BM_CMD = READ → issue CMD_READ_DMA → BM_CMD = READ|START →
poll BM_STATUS bit 0 until clear → BM_CMD = 0 (stop) →
read reg_status (ack IRQ) → check BM_STATUS for error →
memcpy dma_buf → caller
```

### DMA write sequence

```
select_sector → memcpy caller → dma_buf →
clear BM_STATUS → write PRDT phys addr → BM_CMD = WRITE →
issue CMD_WRITE_DMA → BM_CMD = WRITE|START →
poll BM_STATUS bit 0 until clear → BM_CMD = 0 (stop) →
read reg_status (ack IRQ) → check BM_STATUS for error
```

---

## PIO Mode (fallback)

Used when `bm_base == 0` (DMA not available).

**Read:** issue `CMD_READ_SECTORS` → `wait_while_busy` (poll `alt_status`) → read `reg_status` (ack IRQ) → `REP INSD`
**Write:** issue `CMD_WRITE_SECTORS` → `wait_while_busy` (poll DRQ) → `REP OUTSD` → `wait_while_busy` (poll BSY) → read `reg_status` (ack IRQ)

---

## Interrupt Handling

IRQ 14 (primary, vector `0x2E`) and IRQ 15 (secondary, vector `0x2F`) are registered but play no role in I/O synchronisation.

All transfers use **busy-loop polling** — `wait_while_busy` polls `reg_alt_status` until BSY clears, and DMA completion polls `BM_STATUS` bit 0. After polling confirms completion, `reg_status` is read to ack the IRQ line before the interrupt handler can fire.

The registered `interrupt_handler` is a no-op and exists only to prevent unhandled-IRQ faults from spurious asserts.

---

## Channel Structure

```c
typedef struct _channel {
    char           name[8];        // "ide0" / "ide1"
    unsigned short reg_base;       // ATA I/O base (0x1F0 / 0x170)
    unsigned char  irq;            // IRQ vector (0x2E / 0x2F)
    mutex_t        lock;           // serialises all I/O on this channel
    ata_disk       devices[2];     // [0]=master, [1]=slave

    /* DMA fields (all zero when DMA unavailable) */
    unsigned short bm_base;        // Bus Master I/O base
    prdt_entry_t  *prdt;           // PRDT table (kernel virtual address)
    void          *dma_buf;        // 512-byte bounce buffer (virtual)
    unsigned int   dma_buf_phys;   // physical address of dma_buf
} channel;
```

---

## LRU Write-Back Block Cache

Enabled by `HDD_CACHE_OPEN 1` in `config.h`.

### Parameters

| Constant | Value | Meaning |
|---|---|---|
| `HDD_CACHE_SIZE` | 40960 pages | Maximum cache memory |
| `CACHE_SECTOR_COUNT` | `HDD_CACHE_SIZE * 4096 / 512` | Max cached sectors |
| `PREREAD_SECTOR` | 8 | Read-ahead group size (sectors) |

### Organisation

Cache entries are keyed on the **head sector** of each 8-sector group (`HEAD_SECTOR(s) = s / 8 * 8`). Each `block_cache_item` holds 8 × 512 = 4096 bytes and one dirty bit covering the whole group.

- **Hash table** (`rb-tree`): O(log n) lookup by head sector.
- **LRU list**: newest at head, oldest at tail. On miss, the tail entry is evicted and flushed if dirty.

### Write policy

Controlled by `HDD_CACHE_WRITE_POLICY`:

| Policy | Constant | Behaviour |
|---|---|---|
| Write-back (default) | `HDD_CACHE_WRITE_BACK` | Writes update cache + dirty bit; flushed on eviction or `hdd_flush()` |
| Write-through | `HDD_CACHE_WRITE_THOUGH` | Every write immediately calls `partition_write` |

### Cache operations

| Function | Description |
|---|---|
| `partition_cache_read` | Hit: copy from cache, promote to MRU. Miss: allocate/evict, read-ahead 8 sectors |
| `partition_cache_write` | Hit: update cache, mark dirty. Miss: allocate/evict, write into new slot |
| `partition_cache_flush` | Walk LRU list and write all dirty entries to disk |
| `partition_cache_evict` | Free all cache entries for a partition (called on close) |
| `hdd_flush()` | Flush all partitions (called on graceful shutdown) |
| `hdd_close()` | Flush + evict all partitions |

---

## Partition Discovery

`parse_partition` reads the MBR at sector 0 and recursively follows extended partition chains (types `0x05`, `0x0F`, `0x85`, `0xC5`). Each found partition is registered in `hdd_partitions[]`.

If no partition table is found (signature missing or all entries empty), the whole disk is registered as a single partition (type `0x21`).

Linux ext4 partitions (type `0x83`) are additionally registered with lwext4 via `ext4_device_register`.

---

## Initialisation

Registered at boot priority 2 via `KERNEL_INIT(2, hdd_init)`.

```
pci_scan → detect IDE Bus Master (g_bm_base)
enable PCI Bus Master bit on IDE controller
for each channel (ide0, ide1):
    mutex_init
    int_register(irq, interrupt_handler)  ← no-op handler, prevents unhandled-IRQ fault
    reset_channel  →  check_device_type  →  identify_ata_device
    dma_init_channel  →  allocate PRDT + bounce buffer
```

---

## QEMU Configuration

To expose a DMA-capable IDE disk:

```bash
-drive file="disk.qcow2",format=qcow2,if=ide,index=0,media=disk
```

With the default `pc` (i440FX + PIIX3) machine, SeaBIOS assigns BAR4 at `0xc040`; the primary channel Bus Master base becomes `0xc040`, secondary `0xc048`.
