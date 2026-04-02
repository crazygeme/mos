# Virtual File System (VFS)

**Source:** `src/fs/vfs.c`, `src/fs/fs.c`, `src/fs/mount.c`, `src/fs/root.c`
**Headers:** `include/fs/vfs.h`, `include/fs/fs.h`, `include/fs/mount.h`

---

## Overview

The VFS is split into three layers:

| Layer               | File      | Responsibility                                       |
| ------------------- | --------- | ---------------------------------------------------- |
| Object model        | `fs.h`    | `inode`, `file`, `file_descriptor`, operation tables |
| Mount tree          | `vfs.c`   | `super_block`, path resolution, mount/umount         |
| File descriptor API | `fs.c`    | Per-process fd table, `open`/`read`/`write`/…        |
| Filesystem registry | `mount.c` | `fs_type` list, `sys_mount`/`sys_umount` dispatch    |
| ext4 backend        | `root.c`  | lwext4 wrapper, root mount, symlink following        |

---

## 1. Object model

### `inode` — in-memory filesystem object

```c
struct _inode {
    uint32_t            i_mode;     // S_IFREG | S_IFDIR | S_IFLNK | … + rwx bits
    uint64_t            i_ino;      // inode number
    uint64_t            i_size;     // file size in bytes
    const inode_operations *i_op;   // metadata ops (getattr / setattr / chown)
    void               *i_private;  // backend handle (e.g. ext4_file *)
};
```

### `inode_operations`

```c
typedef struct _inode_operations {
    int (*getattr)(inode *, struct stat *);
    int (*setattr)(inode *, uint32_t mode);   // chmod
    int (*chown)(inode *, uint32_t uid, uint32_t gid);
} inode_operations;
```

### `file` — open file instance

One `file` is allocated per `open()` call. Multiple file descriptors (via `dup`) can share the same `file` via `f_count`.

```c
struct _file {
    inode              *f_inode;   // underlying inode
    const file_operations *f_fop;  // per-open ops (read/write/seek/…)
    loff_t              f_pos;     // current position (updated by read/write/seek)
    unsigned            f_count;   // reference count (atomic)
    unsigned            f_mode;    // O_RDONLY / O_WRONLY / O_RDWR
    char               *f_name;    // strdup'd path (for debugging / /proc)
};
```

Reference counting:

```c
fs_get_file(f)   // atomic ++f_count
fs_put_file(f)   // atomic --f_count; when 0: f_fop->release(f) or kfree
```

### `file_operations`

```c
typedef struct _file_operations {
    int     (*release)(file *);
    ssize_t (*read)  (file *, void *buf, size_t, loff_t *pos);
    ssize_t (*write) (file *, const void *buf, size_t, loff_t *pos);
    loff_t  (*llseek)(file *, loff_t offset, int whence);
    int     (*poll)  (file *, unsigned type);   // FS_POLL_READ/WRITE/EXCEPT
    int     (*ioctl) (file *, unsigned cmd, void *buf);
    int     (*flush) (file *);
} file_operations;
```

### `file_descriptor` — per-process fd slot

```c
typedef struct _file_descriptor {
    file    *fp;    // open file (shared on dup)
    unsigned flag;  // O_CLOEXEC etc.
    unsigned used;  // 0 = free slot
} file_descriptor;
```

Each `task_struct` carries a flat array of `MAX_FD` (= `PAGE_SIZE / sizeof(file_descriptor)`) descriptors, protected by `task->fd_lock`.

### File type constants

| Constant   | `i_mode` bits | Meaning          |
| ---------- | ------------- | ---------------- |
| `S_IFREG`  | `0100000`     | Regular file     |
| `S_IFDIR`  | `0040000`     | Directory        |
| `S_IFLNK`  | `0120000`     | Symbolic link    |
| `S_IFCHR`  | `0020000`     | Character device |
| `S_IFBLK`  | `0060000`     | Block device     |
| `S_IFIFO`  | `0010000`     | Named pipe       |
| `S_IFSOCK` | `0140000`     | Socket           |

---

## 2. Mount tree (`vfs.c`)

### `super_block` — mounted filesystem descriptor

```c
struct super_block {
    const super_operations *s_op;   // filesystem callbacks
    void                   *s_fs_info; // private fs data (e.g. ext4_mount_info)
    unsigned                s_ref;  // reference count (atomic)
    mutex_t                 s_lock;
    hash_table             *s_mounts; // child mounts: path → super_block
};
```

`s_mounts` is a hash table keyed by mount-point path strings. Each entry's value is a child `super_block`. The tree is thus represented as a recursive structure of `super_block` nodes.

### `super_operations`

```c
struct super_operations {
    /* Open the mount root (empty path or bare trailing '/'). */
    file *(*open_root)(super_block *sb, int flag);

    /* Full path-aware open (used by real filesystems like ext4). */
    file *(*open)(super_block *sb, const char *path, int flag);

    /* Called when s_ref drops to 0. */
    void (*release)(super_block *sb);

    /* Directory and link operations (paths relative to this sb's root). */
    int (*mkdir)   (super_block *, const char *path, unsigned mode);
    int (*rmdir)   (super_block *, const char *path);
    int (*unlink)  (super_block *, const char *path);
    int (*link)    (super_block *, const char *old, const char *new);
    int (*symlink) (super_block *, const char *target, const char *linkpath);
    int (*rename)  (super_block *, const char *old, const char *new);
    int (*readlink)(super_block *, const char *path, char *buf,
                    size_t bufsiz, size_t *rcnt);
};
```

### Path resolution: `sb_path_resolve`

Traverses the mount tree recursively, matching child mount-point keys against the path prefix:

```
sb_path_resolve(sb, "/proc/net/dev", &out_sb, &out_path)

  root sb → s_mounts has key "/proc" → child sb_proc
    sb_path_resolve(sb_proc, "/net/dev", &out_sb, &out_path)
      sb_proc → s_mounts has no key matching "/net/dev"
      → *out_sb = sb_proc, *out_path = "/net/dev"
```

Rules:
- A key matches only if followed by `/` or `\0` in the path (no false prefix matches).
- Descends recursively into the deepest matching child.
- Returns the `super_block` that owns the suffix and the remaining path relative to it.

### `vfs_open`

```c
file *vfs_open(super_block *sb, const char *path, int flag);
```

1. Call `sb_path_resolve` → `(target_sb, rel_path)`.
2. If `target_sb != sb`: recurse into `vfs_open(target_sb, rel_path, flag)`.
3. If `rel_path` is empty or `"/"`: call `target_sb->s_op->open_root`.
4. Otherwise: call `target_sb->s_op->open(target_sb, rel_path, flag)`.

### `vfs_mount` / `vfs_umount`

**Mount:**
1. If `path` is already a direct key in `sb->s_mounts`: return `-EEXIST`.
2. If a direct child is a strict prefix of `path`: delegate to `vfs_mount(child, suffix, next)`.
3. Otherwise: insert `strdup(path) → child` into `sb->s_mounts`.

**Umount:**
1. If `path` is a direct key: `hash_remove_at` and call `sb_put` via the evict callback.
2. If a direct child is a prefix: delegate to `vfs_umount(child, suffix)`.
3. Otherwise: return `-ENOENT`.

### Path operations: `VFS_PATH_OP` / `VFS_PATH2_OP`

All single-path operations (`mkdir`, `rmdir`, `unlink`, `readlink`) share a macro that resolves the path and dispatches:

```c
VFS_PATH_OP(vfs_mkdir, mkdir, mode)
  → sb_path_resolve → dispatch to target_sb->s_op->mkdir(target_sb, rel_path, mode)
```

Two-path operations (`link`, `rename`) use `VFS_PATH2_OP`, which resolves both paths and enforces that they land on the same `super_block` (returns `-EXDEV` otherwise).

---

## 3. Filesystem type registry (`mount.c`)

### `fs_type`

```c
struct fs_type {
    const char *name;
    super_block *(*get_sb)(const char *dev, const char *target,
                           int flags, void *data);
    struct fs_type *next;   // intrusive singly-linked list
};
```

Registration:

```c
void fs_register_type(fs_type *fst);  // prepend to fs_type_list
```

### Built-in filesystem types

| Type       | Registered by                     | `get_sb`                                        |
| ---------- | --------------------------------- | ----------------------------------------------- |
| `ext4`     | `root.c` (`KERNEL_INIT 3`)        | lwext4 `ext4_mount`, wraps in `ext4_mount_info` |
| `proc`     | `mount.c` (`KERNEL_INIT 2`)       | stub (directory inode only)                     |
| `sysfs`    | `mount.c` (`KERNEL_INIT 2`)       | stub                                            |
| `tmpfs`    | `mount.c` (`KERNEL_INIT 2`)       | stub                                            |
| `devtmpfs` | `mount.c` (`KERNEL_INIT 2`)       | stub                                            |
| `none`     | `mount.c` (`KERNEL_INIT 2`)       | stub                                            |
| `devpts`   | `src/dev/pts.c` (`KERNEL_INIT 5`) | real get_sb                                     |

Stub filesystems return a `super_block` whose `open_root` returns a directory inode (so `stat` on the mount point works). Sub-path opens return `NULL` (`ENOENT`).

### `fs_do_mount` / `fs_do_umount`

```c
int fs_do_mount(const char *dev, const char *target,
                const char *type, unsigned flags, void *data);
int fs_do_umount(const char *target, int flags);
```

`fs_do_mount`:
1. If `target == "/"`: return 0 (root is already mounted at boot).
2. Look up `type` in `fs_type_list`.
3. Call `fst->get_sb(dev, target, flags, data)` → `sb`.
4. Call `vfs_mount(cur->root, target, sb)`. If `-EEXIST` (already kernel-mounted): drop extra `sb_put` ref and succeed.

`fs_do_umount`:
- Refuses to unmount `"/"` (returns `-EBUSY`).
- Calls `vfs_umount(cur->root, target)`.

---

## 4. File descriptor API (`fs.c`)

All `fs_*` functions operate on the **current task's** fd table (`CURRENT_TASK()->fds`).

### `fs_open`

```c
int fs_open(const char *path, int flag, char *mode);
```

1. `fs_open_file(path, flag, mode, follow_link=1)`:
   - Calls `vfs_open(cur->root, path, flag)`.
   - If result is a symlink and `O_NOFOLLOW` not set: re-open the symlink target.
   - Sets `fp->f_name = strdup(path)`.
2. Set `fp->f_mode = flag & O_ACCMODE`.
3. `fs_install_fd(fp, flag)` — find lowest free slot, fill it, return fd number.

### Path resolution: `resolve_path`

Converts user-supplied paths to absolute paths before passing to `vfs_open`:

| Input           | Output                                          |
| --------------- | ----------------------------------------------- |
| `"."`           | cwd                                             |
| `".."`          | parent of cwd                                   |
| `"/foo/bar/.."` | `/foo/`                                         |
| `"./rel"`       | `cwd + "/" + "rel"`                             |
| Absolute path   | copy verbatim, normalise trailing `/.` or `/..` |

### Core operations

| Function                                | Behaviour                                                   |
| --------------------------------------- | ----------------------------------------------------------- |
| `fs_read(fd, offset, buf, len)`         | If `offset != -1`: set `f_pos = offset`; call `f_fop->read` |
| `fs_write(fd, offset, buf, len)`        | Same for write                                              |
| `fs_seek(fd, offset, whence)`           | `f_fop->llseek`; updates `f_pos`                            |
| `fs_llseek(fd, hi, lo, result, whence)` | 64-bit seek; writes new pos to `*result`                    |
| `fs_close(fd)`                          | Clears fd slot; calls `fs_put_file` (may trigger `release`) |
| `fs_stat(path, s)`                      | `vfs_open` + `i_op->getattr` + `fs_put_file`                |
| `fs_fstat(fd, s)`                       | `i_op->getattr` on already-open fd                          |
| `fs_ioctl(fd, cmd, buf)`                | `f_fop->ioctl`                                              |
| `fs_fd_ready(fd, type)`                 | `f_fop->poll` — returns 0 if ready, -1 if not               |
| `fs_sync(fd)`                           | `f_fop->flush`                                              |
| `fs_chmod(path, mode)`                  | `vfs_open` + `i_op->setattr`                                |
| `fs_chown(path, uid, gid)`              | `vfs_open` + `i_op->chown`                                  |

### `fs_dup` / `fs_dup2`

`fs_dup(fd)`:
- Increments `f_count` via `fs_get_file`.
- Installs the same `file *` in the lowest free slot (clears `O_CLOEXEC`).

`fs_dup2(fd, newfd)`:
- If `newfd` is occupied: `fs_put_file` on the old file.
- Copies the fd slot; clears `O_CLOEXEC`.

### `fs_pipe(pipefd)`

Calls `pipe_open(fp[2])` to create two file objects (reader + writer), then installs them into the lowest two free fd slots.

---

## 5. ext4 backend (`root.c`)

### Structures

```c
typedef struct {
    char mp[PAGE_SIZE];  // lwext4 mount point, e.g. "/" or "/mnt/"
                         // always ends with '/'
} ext4_mount_info;
```

Stored in `sb->s_fs_info`. All VFS paths are translated to lwext4 paths by:

```c
sprintf(full, "%s%s", mi->mp, path[0] == '/' ? path + 1 : path);
// root:      "/" + "etc/hosts" → "/etc/hosts"
// secondary: "/mnt/" + "etc/hosts" → "/mnt/etc/hosts"
```

### `ext4_path_open` — symlink-aware open

Three cases handled:

1. **Trailing `/`**: open as `ext4_dir` via `ext4_dir_open`.
2. **Regular file or symlink**: open via `ext4_fopen2`, then follow symlinks in a loop (max `MAX_SYMLINK_DEPTH = 8`). Each symlink is read, resolved to absolute path, and re-opened.
3. **Final target is a directory**: close file, reopen as `ext4_dir`.

Symlink resolution (`fs_resolve_symlink_path`):
- Absolute target (starts with `/`): use as-is.
- Relative target: prepend the directory component of the symlink's own path.

### File vs directory objects

| Backend     | `file_operations`                                                   | `inode_operations`      |
| ----------- | ------------------------------------------------------------------- | ----------------------- |
| `ext4_file` | read, write, llseek, poll, release                                  | getattr, setattr, chown |
| `ext4_dir`  | read (returns `linux_dirent` records), llseek, poll, flush, release | getattr, setattr, chown |

Directory `read` fills a buffer with `linux_dirent` structs (compatible with `getdents` syscall):

```c
struct linux_dirent {
    unsigned long  d_ino;
    unsigned long  d_off;
    unsigned short d_reclen;
    char           d_name[];
};
```

### Root filesystem init (`KERNEL_INIT 3`)

```c
static void fs_mount_root(void)
{
    fs_register_type(&ext4_fs_type);        // register "ext4" in type list
    cur->root = ext4_get();                 // create root super_block (mp="/")
    ext4_mount(hdd_partitions[0].name, "/", 0); // mount first partition
    ext4_cache_write_back("/", true);       // enable write-back cache
}
```

After this, `cur->root` is the root `super_block` for the `kmain_process` task. All child tasks inherit it via `task_struct` copy in `do_fork`.

---

## 6. Loop block device (`src/dev/loop.c`)

**Source:** `src/dev/loop.c`  
**Header:** `include/dev/loopdev.h`

The loop device wraps a regular file as a lwext4 block device, enabling image files to be mounted as if they were physical partitions. Up to `LOOP_MAX_DEVS = 8` slots are supported (`/dev/loop0`..`/dev/loop7`), registered at boot with major number 7.

### Data structures

```c
/* Public descriptor (one per slot) — exposed via loop_devs[] */
typedef struct {
    char     name[16];      /* "loop0" … "loop7"; empty = slot free */
    char     backing[256];  /* absolute path of backing file        */
    uint64_t size_bytes;
} loop_dev_info;

/* Internal per-slot state */
typedef struct {
    file                    *fp;    /* open file backing the device  */
    struct ext4_blockdev_iface bdif; /* lwext4 block interface       */
    struct ext4_blockdev    bdev;   /* lwext4 block device           */
} loop_device;
```

The lwext4 bread/bwrite callbacks translate block-number requests into `f_fop->read`/`f_fop->write` calls on the backing `file *`, using `blk_id * 512` as the byte offset.

### Public API

| Function              | Description                                                                                                              |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| `loop_setup(path)`    | Open `path`, attach it to the first free slot, register with lwext4. Returns the device name (e.g. `"loop0"`) or `NULL`. |
| `loop_teardown(name)` | Unregister from lwext4, close the backing file, free the ph_bbuf. Must not be called while the device is still mounted.  |

### Usage modes

**Auto-loop (via `mount`):**

When `ext4_get_sb` is called with a `dev` path that is neither a known HDD partition nor an already-registered loop device, it treats `dev` as a file path and calls `loop_setup(dev)` automatically:

```
mount("/path/to/img", "/mnt", "ext3", 0, NULL)
  └─ ext4_get_sb: dev not found in hdd_partitions or loop_devs
       └─ loop_setup("/path/to/img") → "loop0"
       └─ ext4_mount("loop0", "/mnt/", false)
       └─ sb->s_devname = "/dev/loop0"   (shown in /proc/mounts)
       └─ mi->loop_name = "loop0"        (for teardown on umount)
```

On umount, `ext4_mount_info.loop_name` is checked and `loop_teardown` is called automatically.

**Manual (`losetup`-compatible):**

```
open("/dev/loop0", O_RDWR)          → loop_cdev_open (major=7)
ioctl(fd, LOOP_SET_FD, img_fd)      → loop_attach(minor, img_fp, NULL)
mount("/dev/loop0", "/mnt", "ext3") → ext4_get_sb finds loop_devs[0]
ioctl(fd, LOOP_CLR_FD, 0)          → loop_detach(minor)
```

Supported ioctls: `LOOP_SET_FD`, `LOOP_CLR_FD`, `LOOP_GET_STATUS`, `LOOP_SET_STATUS`, `LOOP_GET_STATUS64`, `LOOP_SET_STATUS64`.

### Boot registration

`loop_dev_register` runs via `DEV_INIT`:
1. Calls `cdev_register(S_IFBLK, 7, 0, LOOP_MAX_DEVS, loop_cdev_open)` to register the character device handler.
2. Creates `/dev/loop0`..`/dev/loop7` in devtmpfs via `vfs_mknod`.

---

## 7. Lifecycle summary

```
boot (KERNEL_INIT 2)
  └─ fs_register_type: proc, sysfs, tmpfs, devtmpfs, none

boot (KERNEL_INIT 3)
  └─ fs_mount_root
       └─ cur->root = ext4 super_block for "/"
       └─ ext4_mount("hda0", "/")

open("/etc/hosts", O_RDONLY)
  └─ sys_open → fs_open
       └─ fs_open_file → vfs_open(cur->root, "/etc/hosts")
            └─ sb_path_resolve: no child mounts match → target_sb = root sb
            └─ s_op->open(sb, "/etc/hosts") → ext4_open
                 └─ ext4_path_open("/etc/hosts")
                      └─ ext4_fopen2 → ext4_file *
                      └─ ext4_alloc_file → file *
       └─ fs_install_fd → fd N
  └─ return fd N

mount("/dev/hda1", "/mnt", "ext4")
  └─ sys_mount → fs_do_mount
       └─ fs_find_type("ext4") → &ext4_fs_type
       └─ ext4_get_sb("/dev/hda1", "/mnt") → sb_mnt
       └─ vfs_mount(cur->root, "/mnt", sb_mnt)
            └─ hash_insert(root->s_mounts, "/mnt", sb_mnt)

open("/mnt/data.txt", O_RDONLY)
  └─ vfs_open(root, "/mnt/data.txt")
       └─ sb_path_resolve: root->s_mounts["/mnt"] matches prefix
            → recurse: vfs_open(sb_mnt, "/data.txt")
       └─ sb_mnt->s_op->open(sb_mnt, "/data.txt")
            └─ ext4_open: mp="/mnt/" → full="/mnt/data.txt"
            └─ ext4_path_open("/mnt/data.txt")

read(fd, buf, len)
  └─ fs_read → fp->f_fop->read(fp, buf, len, &fp->f_pos)
       └─ ext4_file_read → ext4_fread

close(fd)
  └─ fs_close → clears fd slot → fs_put_file
       └─ f_count-- == 0 → f_fop->release
            └─ ext4_file_release → ext4_fclose, kfree

umount("/mnt")
  └─ sys_umount → fs_do_umount
       └─ vfs_umount(cur->root, "/mnt")
            └─ hash_remove_at → sb_entry_evict
                 └─ free(key), sb_put(sb_mnt)
                      └─ s_op->release → ext4_umount("/mnt/"), kfree(sb_mnt)
```
