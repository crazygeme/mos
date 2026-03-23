# Working with ffs.img

`ffs.img` is a 350 MB raw disk image containing a single ext2 partition
(partition 1, starting at sector 2048 / offset 1 048 576 bytes). It is used
as the primary disk by `rh6.sh` and can be mounted on the host to copy
binaries in or out.

Helper scripts in `tools/` wrap the mount/unmount steps.

---

## Mount

```sh
./tools/mountdisk.sh
```

This runs:

```sh
mkdir -p mnt
sudo mount -t ext2 -o loop,offset=1048576 ffs.img mnt
```

The image is now accessible at `mnt/`.

## Copy a binary

```sh
sudo cp your_binary mnt/bin/
```

## Unmount

```sh
./tools/umountdisk.sh
```

Always unmount before launching QEMU — the kernel flushes dirty pages on
unmount, so skipping this step can leave the image in an inconsistent state.

---

## Create a new ffs.img from scratch

If you need to recreate or resize the image:

```sh
# 1. Create a blank image (adjust size as needed)
qemu-img create ffs.img 350M

# 2. Partition it (one primary partition using the full disk)
#    'n p 1 2048 <default> w' via fdisk:
echo -e "n\np\n1\n2048\n\nw" | fdisk ffs.img

# 3. Format the partition as ext2
sudo losetup -o 1048576 --sizelimit $(( 349 * 1024 * 1024 )) /dev/loop0 ffs.img
sudo mkfs.ext2 /dev/loop0
sudo losetup -d /dev/loop0

# 4. Mount and populate
./tools/mountdisk.sh
sudo mkdir -p mnt/bin mnt/etc mnt/proc mnt/dev
# copy your binaries...
./tools/umountdisk.sh
```

### Using `losetup --find` (simpler, modern alternative)

```sh
# Attach the partition directly
LOOP=$(sudo losetup --find --show --offset 1048576 ffs.img)
sudo mkfs.ext2 "$LOOP"
sudo losetup -d "$LOOP"
```

---

## Inspect image contents without mounting

```sh
# List files
debugfs -R 'ls -l /bin' ffs.img?offset=1048576

# Extract a single file
debugfs -R 'dump /bin/mybin mybin' ffs.img?offset=1048576
```

`debugfs` works without root and is available via:

```sh
sudo apt install e2fsprogs   # Ubuntu/Debian
brew install e2fsprogs       # macOS
```
