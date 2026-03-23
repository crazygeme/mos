# ELF Parser & Program Loader

**Source:** `src/elf/elf.c`, `src/elf/exec.c`
**Headers:** `include/elf/elf.h`, `include/elf/exec.h`

---

## Overview

| File | Responsibility |
|------|---------------|
| `elf.c` | Parse ELF headers, map `PT_LOAD` segments, handle BSS, load dynamic linker |
| `exec.c` | `sys_execve`: full exec lifecycle — pre-exec cleanup, script detection, stack setup, jump to entry |

Only 32-bit (`ELFCLASS32`) ET_EXEC executables are supported as the main binary. The dynamic linker must be `ET_DYN`.

---

## 1. ELF data structures (`elf.h`)

### ELF file header (`Elf32_Ehdr`)

```
e_ident[16]   magic + class + data + version + OS ABI
e_type        ET_EXEC (2) or ET_DYN (3)
e_machine     EM_386 (3)
e_entry       virtual address of the entry point
e_phoff       file offset of the program header table
e_phnum       number of program headers
e_phentsize   size of one program header entry (= sizeof Elf32_Phdr)
e_shoff       file offset of the section header table (not used by loader)
```

Magic bytes: `0x7f 'E' 'L' 'F'` at `e_ident[0..3]`.
Class check: `e_ident[EI_CLASS] == ELFCLASS32`.

### Program header (`Elf32_Phdr`)

```c
typedef struct elf32_phdr {
    Elf32_Word p_type;    // segment type
    Elf32_Off  p_offset;  // offset in file
    Elf32_Addr p_vaddr;   // virtual address in process image
    Elf32_Addr p_paddr;   // physical address (ignored)
    Elf32_Word p_filesz;  // size in the file
    Elf32_Word p_memsz;   // size in memory (>= p_filesz; extra = BSS)
    Elf32_Word p_flags;   // PF_R | PF_W | PF_X
    Elf32_Word p_align;   // alignment
} Elf32_Phdr;
```

Segment types used by the loader:

| `p_type` | Value | Meaning |
|----------|-------|---------|
| `PT_LOAD` | 1 | Loadable segment (code / data / BSS) |
| `PT_INTERP` | 3 | Path to the dynamic linker |
| `PT_PHDR` | 6 | Location of the program header table itself |
| `PT_GNU_STACK` | `0x6474e551` | Stack executable flag (not enforced) |

Permission flags (`p_flags`):

| Flag | Bit | `mmap` prot |
|------|-----|------------|
| `PF_X` | 0 | `PROT_EXEC` |
| `PF_W` | 1 | `PROT_WRITE` |
| `PF_R` | 2 | `PROT_READ` |

### `mos_binfmt` — loader output record

```c
typedef struct _mos_binfmt {
    unsigned elf_load_addr;    // VA of the page holding the phdr table
    unsigned e_phoff;          // phdr table offset within that page
    unsigned e_phnum;          // number of program headers
    unsigned interp_load_addr; // biased entry point (jump target for execve)
    unsigned e_entry;          // raw e_entry from the ELF header
    unsigned interp_bias;      // load bias of the interpreter (ld.so)
    unsigned start_brk;        // PAGE_ALIGN_UP(end of BSS) — initial brk
} mos_binfmt;
```

---

## 2. ELF loader (`elf.c`)

### `elf_map(path, fmt)` — top-level entry point

```c
unsigned elf_map(char *path, mos_binfmt *fmt);
```

Returns the raw `e_entry` of the binary, or 0 on error.
The actual jump target is `fmt->interp_load_addr`.

**Validation:**
1. Open the file; read `Elf32_Ehdr` at offset 0.
2. Check `e_ident[0] == 0x7f` (magic).
3. Check `e_ident[EI_CLASS] == ELFCLASS32`.
4. Check `e_type == ET_EXEC`.

**Three dispatch paths:**

```
elf_find_interp()
  │
  ├─ No PT_INTERP found (static binary)
  │    elf_map_programs()          — map all PT_LOAD, no bias
  │    fmt->interp_load_addr = e_entry
  │
  ├─ PT_INTERP found, path == interp path (ld.so run directly)
  │    elf_map_programs()          — treat as static
  │    fmt->interp_load_addr = e_entry
  │
  └─ PT_INTERP found, path != interp path (normal dynamic binary)
       elf_map_elf_hdr()           — map PT_LOAD + record PT_PHDR metadata
       elf_map_dynamic(interp)     — load ld.so at a biased address
       fmt->interp_load_addr = fmt->interp_bias + interp->e_entry
```

---

### `elf_load_segment` — map one `PT_LOAD` segment

```c
static void elf_load_segment(file *fp, Elf32_Phdr *phdr, unsigned bias);
```

`bias` is added to `p_vaddr` before mapping (0 for ET_EXEC; `interp_bias` for ET_DYN).

**Page-aligned fast path** (`p_vaddr` is page-aligned — the common case):

```
va_begin       = p_vaddr & PAGE_MASK
elf_bss        = p_vaddr + p_filesz     (first BSS byte)
last_bss       = p_vaddr + p_memsz      (one past last BSS byte)
file_pages_end = PAGE_ALIGN_UP(elf_bss)
mem_pages_end  = PAGE_ALIGN_UP(last_bss)
```

Case 1 — **no BSS** (`filesz == memsz`):
```
[va_begin, mem_pages_end)  → lazy file-backed mmap (MAP_FIXED)
   Page fault handler reads from file on demand;
   any sub-page trailing bytes are zero-filled by the handler.
```

Case 2 — **BSS present** (`memsz > filesz`): three regions:

```
Region 1: [va_begin, file_page_end)
  Whole pages covered by file data.
  → lazy file-backed mmap (MAP_FIXED)

Region 2: [file_page_end, file_pages_end)   (the "boundary page")
  Only exists when elf_bss is not page-aligned.
  Contains file bytes [file_page_end, elf_bss) and BSS bytes [elf_bss, file_pages_end).
  → anonymous mmap (MAP_FIXED | PROT_WRITE)
  → elf_read() copies file bytes eagerly into [file_page_end, elf_bss)
  → BSS tail left zero from anonymous zero-fill
  → PTE downgraded to segment's true prot if PROT_WRITE was not requested

Region 3: [file_pages_end, mem_pages_end)
  Pure BSS pages.
  → anonymous mmap (MAP_FIXED | PROT_WRITE); zero-filled on fault
```

**Unaligned `p_vaddr`** (uncommon): single anonymous mapping over the full page range; file data copied eagerly via `elf_read()`; BSS left zero.

#### Why the three-region split?

A naïve file-backed mapping over the full `p_memsz` would be wrong: when the page fault handler reads a full `PAGE_SIZE` from the file, bytes beyond `p_filesz` may contain section headers or debug info, silently corrupting the BSS.

---

### `elf_map_programs` — static binary loader

Iterates all program headers, calls `elf_load_segment` for each `PT_LOAD`, and sets `fmt->start_brk = PAGE_ALIGN_UP(highest memsz VA)`.

### `elf_map_elf_hdr` — dynamic binary loader (executable side)

Same as `elf_map_programs` but also processes `PT_PHDR`:
- `fmt->e_phnum` = total number of program headers
- `fmt->e_phoff` = in-page offset of the phdr table (bits below page boundary of `PT_PHDR.p_vaddr`)
- `fmt->elf_load_addr` = page base of the phdr table

These three values are written into the auxiliary vector for the dynamic linker.

### `elf_map_dynamic` — interpreter (ld.so) loader

```c
static unsigned elf_map_dynamic(char *path, mos_binfmt *fmt);
```

1. Open `path` (e.g. `/lib/ld-linux.so.2`); validate as `ELFCLASS32` + `ET_DYN`.
2. `elf_map_get_dynamic_pages()` — scan `PT_LOAD` segments to find the total page span (`max_va - min_va`).
3. `vm_get_usr_zone(page_count)` — reserve a contiguous virtual-address window in the mmap zone. Stored in `fmt->interp_bias`.
4. `elf_map_programs_at(bias)` — map all `PT_LOAD` segments at `p_vaddr + interp_bias`.
5. `fmt->interp_load_addr = interp_bias + e_entry`.

The kernel will jump to `interp_load_addr`; the interpreter then loads shared libraries, resolves relocations, and finally jumps to the executable's `e_entry`.

---

## 3. `sys_execve` — full exec lifecycle (`exec.c`)

```c
int sys_execve(const char *f, char **argv, char **envp);
```

### Step 1 — Path resolution and permission check

```c
resolve_path(f, file_name);   // → absolute path in file_name
fp = fs_open_file(file_name, O_RDONLY, ...);
stat → check S_IXUSR and st_size >= 4
```

### Step 2 — Script detection (`#!`)

Reads the first 256 bytes of the file:

| First bytes | Action |
|-------------|--------|
| `0x7f 'E' 'L'` | ELF binary — proceed normally |
| `#!` | Script: extract interpreter path from the first line; prepend it as argv[0] |
| Anything else | Return `-ENOEXEC` |

For a script `#!/bin/bash`, `file_name` is replaced with `/bin/bash` and the original `argv` is shifted by one (POSIX behaviour).

### Step 3 — Save argv/envp to kernel heap

`save_argv()` and `save_envp()` deep-copy all strings via `strdup()` before the address space is destroyed. This is necessary because the user-space pointers become invalid after `cleanup()`.

For scripts, `save_argv` prepends the interpreter path as `argv[0]`.

### Step 4 — Save command/environment to `task_struct`

```c
cur->user->command   = argv[0] + '\0' + argv[1] + '\0' + …   (for /proc/pid/cmdline)
cur->user->environment = envp[0] + envp[1] + …               (for /proc/pid/environ)
```

### Step 5 — `cleanup()` — tear down old address space

1. If `FORK_FLAG_VFORK`: `cond_notify(&vfork_event)` — unblocks the parent.
2. Close all fds with `O_CLOEXEC` set.
3. `vm_destroy(cur->user->vm)` — free all `vm_region` descriptors.
4. `ps_cleanup_all_user_map(cur)` — unmap all user page table entries.
5. Reset `start_brk = brk = 0`; replace VM descriptor with a fresh `vm_create()`.

### Step 6 — Load ELF image

```c
elf_map(file_name, &fmt);
eip = fmt.interp_load_addr;
cur->user->start_brk = fmt.start_brk;
cur->user->brk = fmt.start_brk;
```

### Step 7 — Map vDSO and user stack

```c
mm_vdso_map();    // map kernel signal trampoline code into user space
do_mmap(esp_bottom, USER_STACK_PAGES * PAGE_SIZE, PROT_READ | PROT_WRITE, ...);
```

Stack occupies `[KERNEL_OFFSET - USER_STACK_PAGES * PAGE_SIZE, KERNEL_OFFSET)`.

### Step 8 — `setup_user_stack` — build the initial stack

Lays out the stack in the System V i386 ABI format, growing downward from `KERNEL_OFFSET`:

```
[high address = KERNEL_OFFSET]
  0x00000000              ← 4-byte end sentinel
  filename string
  envp strings            ← NUL-terminated, packed
  argv strings            ← NUL-terminated, packed
  "i686"                  ← AT_PLATFORM string
  <16-byte alignment pad>
  AT_NULL (0, 0)          ┐
  AT_PLATFORM             │
  AT_HWCAP, AT_PAGESZ,    │  auxiliary vector (auxv)
  AT_CLKTCK               │
  AT_PHDR, AT_PHENT,      │
  AT_PHNUM, AT_BASE,      │
  AT_FLAGS, AT_ENTRY,     │
  AT_UID, AT_EUID,        │
  AT_GID, AT_EGID         ┘
  envp[]                  ← NULL-terminated pointer array
  argv[]                  ← NULL-terminated pointer array
  argc                    ← int, read first by crt0
[returned esp]
```

Auxiliary vector entries written by `NEW_AUX_ENT`:

| Entry | Value |
|-------|-------|
| `AT_PHDR` | `fmt.elf_load_addr + fmt.e_phoff` (VA of phdr table) |
| `AT_PHENT` | `sizeof(Elf32_Phdr)` |
| `AT_PHNUM` | `fmt.e_phnum` |
| `AT_BASE` | `fmt.interp_bias` (ld.so load bias) |
| `AT_FLAGS` | 0 |
| `AT_ENTRY` | `fmt.e_entry` (executable's raw entry point) |
| `AT_UID/EUID/GID/EGID` | 0 (root) |
| `AT_HWCAP` | `0x0183FBFF` (i686 capability mask) |
| `AT_PAGESZ` | 4096 |
| `AT_CLKTCK` | 100 |
| `AT_PLATFORM` | pointer to `"i686"` string on stack |

### Step 9 — Transfer control to user space

```c
cur->type = ps_user;
switch_to_user_mode(eip, esp_top);
// never returns
```

`switch_to_user_mode` loads the user-mode segment selectors (`USER_CODE`, `USER_DATA`), sets up the iret frame, and executes `iret` to switch to ring 3 at `eip:esp`.

---

## 4. First user process (`kinit_userspace`)

Called via `KERNEL_INIT(8, kinit_userspace)` — the last kernel initialisation step:

1. `ps_update_tss(esp0)` — set TSS esp0 to this task's kernel stack top.
2. Open fds 0, 1, 2 on `/dev/tty0` (stdin/stdout/stderr).
3. `run_if_exist("/sbin/init", …)` — exec init with `PATH=/bin:/usr/bin:/sbin` and `TERM=linux`.
   - If `TestControl.init_binary` is set (e.g. `bash` from the kernel cmdline), exec that instead.
   - If the binary is missing, `DIE()`.

---

## 5. Lifecycle summary

```
sys_execve("/bin/ls", argv, envp)
  │
  ├─ resolve_path → /bin/ls
  ├─ open + stat: check S_IXUSR, size >= 4
  ├─ read first 256 bytes
  │    0x7fELF → ELF binary
  │    #!path  → script: replace file_name = path, prepend argv[0]
  │    other   → -ENOEXEC
  │
  ├─ save_argv / save_envp   (deep copy to kernel heap)
  ├─ cleanup()
  │    ├─ cond_notify(vfork) if applicable
  │    ├─ close O_CLOEXEC fds
  │    ├─ vm_destroy + ps_cleanup_all_user_map
  │    └─ reset brk, fresh vm_create
  │
  ├─ elf_map("/bin/ls", &fmt)
  │    ├─ read Elf32_Ehdr, validate magic + class + ET_EXEC
  │    ├─ elf_find_interp → "/lib/ld-linux.so.2"
  │    ├─ elf_map_elf_hdr:
  │    │    PT_PHDR → fmt.elf_load_addr, e_phoff, e_phnum
  │    │    PT_LOAD → elf_load_segment (lazy file-backed or anon BSS)
  │    └─ elf_map_dynamic("/lib/ld-linux.so.2"):
  │         ET_DYN validation
  │         vm_get_usr_zone → fmt.interp_bias
  │         elf_map_programs_at(bias)
  │         fmt.interp_load_addr = bias + ld.so.e_entry
  │
  ├─ mm_vdso_map()           (signal trampoline)
  ├─ do_mmap(stack region)
  ├─ setup_user_stack → esp  (auxv + argv[] + envp[] + argc)
  │
  └─ switch_to_user_mode(fmt.interp_load_addr, esp)
       → ld.so starts at ring 3
       → ld.so reads AT_PHDR/AT_PHNUM → loads libm.so, libc.so, …
       → ld.so resolves PLT relocations
       → ld.so jumps to fmt.e_entry  (_start in /bin/ls)
       → crt0 calls main(argc, argv, envp)
```
