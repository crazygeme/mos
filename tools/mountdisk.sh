#!/bin/bash

set -e

DISK="rh9.qcow2"
NBD_DEV="/dev/nbd0"
MNT="out/mnt"

if [ ! -f "$DISK" ]; then
    echo "Error: $DISK not found"
    exit 1
fi

sudo modprobe nbd max_part=8
sudo qemu-nbd -c "$NBD_DEV" "$DISK"

# Wait for the partition device to appear
for i in $(seq 10); do
    [ -b "${NBD_DEV}p1" ] && break
    sleep 0.3
done

if [ ! -b "${NBD_DEV}p1" ]; then
    echo "Error: ${NBD_DEV}p1 did not appear"
    sudo qemu-nbd -d "$NBD_DEV"
    exit 1
fi

mkdir -p "$MNT"
sudo mount "${NBD_DEV}p1" "$MNT"
echo "Mounted $DISK at $MNT/"
