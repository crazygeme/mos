# Disk Image (`rh9.qcow2`)

`rh9.qcow2` is a qcow2 disk image containing the Red Hat 9 userspace. It is the
primary disk used by QEMU (`run.sh`). A pre-built image is shipped as
`redhat9.img.zip` and is extracted automatically on the first `./run.sh`.

---

## Mount / unmount

```sh
./tools/mountdisk.sh    # attach via qemu-nbd, mount partition 1 → mnt/
# ... make changes ...
./tools/umountdisk.sh   # unmount and disconnect NBD device
```

**Always unmount before launching QEMU.** The kernel flushes dirty pages on
unmount; skipping this step can leave the image in an inconsistent state.

`mountdisk.sh` requires `qemu-utils` (Linux) or `qemu` (macOS) for `qemu-nbd`.

## Copy files into the image

```sh
./tools/mountdisk.sh
sudo cp your_binary mnt/bin/
./tools/umountdisk.sh
```

---

## Inspect without mounting

`debugfs` works without root and avoids the need to attach an NBD device:

```sh
# List a directory
debugfs -R 'ls -l /bin' /dev/nbd0p1

# Or using the raw qcow2 (requires converting first, or use guestfish)
guestfish --ro -a rh9.qcow2 -m /dev/sda1 ls /bin
```

---

## Recreate the image from scratch

```sh
# 1. Create a blank qcow2 image
qemu-img create -f qcow2 rh9.qcow2 2G

# 2. Convert to raw temporarily for partitioning
qemu-img convert -f qcow2 -O raw rh9.qcow2 rh9.raw

# 3. Partition (one primary partition, starting at sector 2048)
echo -e "n\np\n1\n2048\n\nw" | fdisk rh9.raw

# 4. Format as ext2 via loop
LOOP=$(sudo losetup --find --show --offset 1048576 rh9.raw)
sudo mkfs.ext2 "$LOOP"
sudo losetup -d "$LOOP"

# 5. Convert back to qcow2
qemu-img convert -f raw -O qcow2 rh9.raw rh9.qcow2
rm rh9.raw

# 6. Mount and populate
./tools/mountdisk.sh
sudo mkdir -p mnt/bin mnt/etc mnt/proc mnt/dev
# copy binaries...
./tools/umountdisk.sh
```
